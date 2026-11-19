#include "runtime/render/passes/planar_reflection_math.hpp"
#include "runtime/render/render_math.hpp"
#include "runtime/scene/camera_math.hpp"
#include <algorithm>
#include <bx/math.h>
#include <cmath>
#include <limits>
namespace engine::render_detail
{
bool prepare_planar_reflection(const float* plane_world, const float* camera_world,
                               const float* camera_projection, float clip_bias, bool homogeneous,
                               std::array<float, 16>& world, std::array<float, 16>& view,
                               std::array<float, 16>& projection,
                               std::array<float, 16>& view_projection)
{
    if (plane_world == nullptr || camera_world == nullptr || camera_projection == nullptr ||
        !finite_matrix(plane_world) || !finite_matrix(camera_world) ||
        !finite_matrix(camera_projection) || !std::isfinite(clip_bias) || clip_bias < 0.0F ||
        clip_bias > 1.0F)
    {
        return false;
    }
    // Reflector.js (Three.js bf8f297466d0a8f8ef141d83113c79fadbe1019c, MIT):
    // mirror the camera and use Lengyel's oblique near plane. The full reflection
    // matrix retains its negative determinant; the draw submission reverses winding.
    // Source/license: third_party/notices/threejs-reflections.md.
    float normals[16];
    if (!normal_matrix(plane_world, normals))
    {
        return false;
    }
    std::array<float, 3> n{normals[4], normals[5], normals[6]};
    const float length = std::hypot(n[0], n[1], n[2]);
    if (!(length > 0.0F))
    {
        return false;
    }
    for (auto& value : n)
    {
        value /= length;
    }
    float distance = 0;
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        distance += n[axis] * (camera_world[12 + axis] - plane_world[12 + axis]);
    }
    if (distance <= clip_bias)
    {
        return false;
    }
    float reflection[16];
    bx::mtxIdentity(reflection);
    float d = 0;
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        d -= n[axis] * plane_world[12 + axis];
    }
    for (std::size_t row = 0; row < 3; ++row)
    {
        for (std::size_t col = 0; col < 3; ++col)
        {
            reflection[col * 4 + row] -= 2 * n[col] * n[row];
        }
        reflection[12 + row] = -2 * d * n[row];
    }
    float camera_view[16]{};
    float normalized_camera_world[16]{};
    if (!camera_detail::view_matrix(camera_world, camera_view))
    {
        return false;
    }
    bx::mtxInverse(normalized_camera_world, camera_view);
    bx::mtxMul(world.data(), normalized_camera_world, reflection);
    // Rebuilding a look-at basis here would remove the reflection's negative determinant.
    bx::mtxInverse(view.data(), world.data());
    if (!finite_matrix(view.data()))
    {
        return false;
    }
    std::copy_n(camera_projection, 16, projection.begin());
    const std::array<float, 4> plane{n[0], n[1], n[2], d - clip_bias};
    std::array<float, 4> camera_plane{};
    for (std::size_t col = 0; col < 4; ++col)
    {
        for (std::size_t row = 0; row < 4; ++row)
        {
            camera_plane[col] += world[col * 4 + row] * plane[row];
        }
    }
    float inverse[16];
    bx::mtxInverse(inverse, projection.data());
    const std::array<float, 4> corner{camera_plane[0] < 0 ? -1.0F : 1.0F,
                                      camera_plane[1] < 0 ? -1.0F : 1.0F, 1, 1};
    std::array<float, 4> q{};
    float dot = 0;
    for (std::size_t row = 0; row < 4; ++row)
    {
        for (std::size_t col = 0; col < 4; ++col)
        {
            q[row] += inverse[col * 4 + row] * corner[col];
        }
        dot += camera_plane[row] * q[row];
    }
    if (!std::isfinite(dot) || std::abs(dot) < 0.000001F)
    {
        return false;
    }
    const float factor = (homogeneous ? 2.0F : 1.0F) / dot;
    for (std::size_t col = 0; col < 4; ++col)
    {
        projection[col * 4 + 2] =
            camera_plane[col] * factor - (homogeneous ? projection[col * 4 + 3] : 0.0F);
    }
    // An orthographic observer parallel to the mirror produces a singular
    // oblique volume. It has no usable reflection rays; the caller keeps its IBL path.
    float inverse_projection[16]{};
    bx::mtxInverse(inverse_projection, projection.data());
    if (!finite_matrix(inverse_projection))
    {
        return false;
    }
    bx::mtxMul(view_projection.data(), view.data(), projection.data());
    return finite_matrix(projection.data()) && finite_matrix(view_projection.data());
}

PlanarReflectionScissor planar_receiver_scissor(const float* model, const float* view_projection,
                                                const std::array<float, 3>& minimum,
                                                const std::array<float, 3>& maximum,
                                                float distortion, std::uint16_t width,
                                                std::uint16_t height)
{
    if (!model || !view_projection || !finite_matrix(model) || !finite_matrix(view_projection) ||
        !std::isfinite(distortion) || distortion < 0 || width == 0 || height == 0)
    {
        return {};
    }
    std::array<double, 3> local_magnitude{};
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
            minimum[axis] > maximum[axis])
        {
            return {};
        }
        local_magnitude[axis] = std::max(std::abs(minimum[axis]), std::abs(maximum[axis]));
    }
    std::array<double, 3> world_magnitude{};
    for (std::size_t row = 0; row < 3; ++row)
    {
        world_magnitude[row] = std::abs(model[12 + row]);
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            world_magnitude[row] += std::abs(model[axis * 4 + row]) * local_magnitude[axis];
        }
    }
    constexpr std::array<std::size_t, 3> clip_rows{0, 1, 3};
    std::array<double, 3> clip_error{};
    for (std::size_t axis = 0; axis < clip_rows.size(); ++axis)
    {
        const auto row = clip_rows[axis];
        double magnitude = std::abs(view_projection[12 + row]);
        for (std::size_t column = 0; column < 3; ++column)
        {
            magnitude += std::abs(view_projection[column * 4 + row]) * world_magnitude[column];
        }
        // Allow for FP32 vertex transformation, interpolation and projection on the GPU.
        clip_error[axis] = magnitude * (64 * std::numeric_limits<float>::epsilon());
    }
    std::array<double, 2> low, high;
    low.fill(std::numeric_limits<double>::infinity());
    high.fill(-std::numeric_limits<double>::infinity());
    // When w is positive throughout the box, the extrema of x/w and y/w are at corners.
    for (unsigned corner = 0; corner < 8; ++corner)
    {
        std::array<double, 3> world{};
        for (std::size_t row = 0; row < 3; ++row)
        {
            world[row] = model[12 + row];
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                const double position = (corner & (1u << axis)) ? maximum[axis] : minimum[axis];
                world[row] += model[axis * 4 + row] * position;
            }
        }
        std::array<double, 3> clip{};
        for (std::size_t axis = 0; axis < clip_rows.size(); ++axis)
        {
            const auto row = clip_rows[axis];
            clip[axis] = view_projection[12 + row];
            for (std::size_t column = 0; column < 3; ++column)
            {
                clip[axis] += view_projection[column * 4 + row] * world[column];
            }
        }
        const double w = clip[2] - clip_error[2];
        if (w <= 0.0001)
        {
            return {};
        }
        for (std::size_t axis = 0; axis < 2; ++axis)
        {
            const double ndc = clip[axis] / clip[2];
            const double error = (clip_error[axis] + std::abs(ndc) * clip_error[2]) / w;
            low[axis] = std::min(low[axis], ndc - error);
            high[axis] = std::max(high[axis], ndc + error);
        }
    }
    // The frame texture is clamp-to-edge and bilinear, without mipmaps. Also keep
    // the neighboring texels for raster derivatives and multisample edges.
    const double pad_x = double{distortion} * width + 2.0;
    const double pad_y = double{distortion} * height + 2.0;
    const auto pixel = [](double value, double limit)
    { return static_cast<std::uint16_t>(std::clamp(value, 0.0, limit)); };
    const auto left = pixel(std::floor((1.0 + low[0]) * 0.5 * width - pad_x), width);
    const auto right = pixel(std::ceil((1.0 + high[0]) * 0.5 * width + pad_x), width);
    const auto top = pixel(std::floor((1.0 - high[1]) * 0.5 * height - pad_y), height);
    const auto bottom = pixel(std::ceil((1.0 - low[1]) * 0.5 * height + pad_y), height);
    if (right <= left || bottom <= top ||
        (left == 0 && top == 0 && right == width && bottom == height))
    {
        return {};
    }
    return {left, top, static_cast<std::uint16_t>(right - left),
            static_cast<std::uint16_t>(bottom - top)};
}

std::array<float, 16> planar_culling_projection(const std::array<float, 16>& projection,
                                                const PlanarReflectionScissor& scissor,
                                                std::uint16_t width, std::uint16_t height)
{
    auto result = projection;
    const float sx = static_cast<float>(width) / scissor.width;
    const float sy = static_cast<float>(height) / scissor.height;
    const float ox = (static_cast<float>(width) - (2 * scissor.x + scissor.width)) / scissor.width;
    const float oy =
        ((2 * scissor.y + scissor.height) - static_cast<float>(height)) / scissor.height;
    for (std::size_t column = 0; column < 16; column += 4)
    {
        result[column] = sx * projection[column] + ox * projection[column + 3];
        result[column + 1] = sy * projection[column + 1] + oy * projection[column + 3];
    }
    return result;
}
} // namespace engine::render_detail
