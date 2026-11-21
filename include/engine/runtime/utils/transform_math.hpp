#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/scene/scene_types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace engine::transform_math
{
// Column-major affine matrices. Subtract in double before conversion to GPU coordinates.
inline void relative(const double* world, const Vec3& origin, float* output) noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    for (std::size_t i = 0; i < 16; ++i)
    {
        if (i < 12 || i == 15)
        {
            output[i] = static_cast<float>(world[i]);
        }
    }
    output[12] = static_cast<float>(world[12] - origin.x());
    output[13] = static_cast<float>(world[13] - origin.y());
    output[14] = static_cast<float>(world[14] - origin.z());
}

inline void multiply(const double* a, const double* b, double* output) noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    std::array<double, 16> result{};
    for (std::size_t c = 0; c < 4; ++c)
    {
        for (std::size_t r = 0; r < 4; ++r)
        {
            for (std::size_t k = 0; k < 4; ++k)
            {
                result[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
            }
        }
    }
    std::copy(result.begin(), result.end(), output);
}

[[nodiscard]] inline bool inverse(const double* m, double* output) noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    std::array<double, 16> r{};
    const double d = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                     m[8] * (m[1] * m[6] - m[5] * m[2]);
    if (!std::isfinite(d) || std::abs(d) < 1e-20 || m[3] != 0 || m[7] != 0 || m[11] != 0 ||
        m[15] != 1)
    {
        return false;
    }
    r[0] = (m[5] * m[10] - m[9] * m[6]) / d;
    r[4] = (m[8] * m[6] - m[4] * m[10]) / d;
    r[8] = (m[4] * m[9] - m[8] * m[5]) / d;
    r[1] = (m[9] * m[2] - m[1] * m[10]) / d;
    r[5] = (m[0] * m[10] - m[8] * m[2]) / d;
    r[9] = (m[8] * m[1] - m[0] * m[9]) / d;
    r[2] = (m[1] * m[6] - m[5] * m[2]) / d;
    r[6] = (m[4] * m[2] - m[0] * m[6]) / d;
    r[10] = (m[0] * m[5] - m[4] * m[1]) / d;
    for (std::size_t i = 0; i < 3; ++i)
    {
        r[12 + i] = -(r[i] * m[12] + r[4 + i] * m[13] + r[8 + i] * m[14]);
    }
    r[15] = 1;
    if (!std::all_of(r.begin(), r.end(), [](double v)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return std::isfinite(v);
    }))
    {
        return false;
    }
    std::copy(r.begin(), r.end(), output);
    return true;
}

[[nodiscard]] inline Vec3 point(const double* m, const Vec3& value) noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    return {m[0] * value.x() + m[4] * value.y() + m[8] * value.z() + m[12],
            m[1] * value.x() + m[5] * value.y() + m[9] * value.z() + m[13],
            m[2] * value.x() + m[6] * value.y() + m[10] * value.z() + m[14]};
}
} // namespace engine::transform_math
