#pragma once
#include <array>
#include <cstdint>

namespace engine::render_detail
{
struct PlanarReflectionScissor
{
    std::uint16_t x = 0, y = 0, width = 0, height = 0;
};

// Pure camera math shared by the GPU pass; all matrices use the renderer's column-major convention.
[[nodiscard]] bool prepare_planar_reflection(const float* plane_world, const float* camera_world,
                                             const float* camera_projection, float clip_bias,
                                             bool homogeneous, std::array<float, 16>& world,
                                             std::array<float, 16>& view,
                                             std::array<float, 16>& projection,
                                             std::array<float, 16>& view_projection);

// A zero rectangle keeps the entire reflection target.
[[nodiscard]] PlanarReflectionScissor
planar_receiver_scissor(const float* model, const float* view_projection,
                        const std::array<float, 3>& minimum, const std::array<float, 3>& maximum,
                        float distortion, std::uint16_t width, std::uint16_t height);

// Use the cropped projection only for visibility. Rasterization retains the original viewport.
[[nodiscard]] std::array<float, 16>
planar_culling_projection(const std::array<float, 16>& projection,
                          const PlanarReflectionScissor& scissor, std::uint16_t width,
                          std::uint16_t height);
} // namespace engine::render_detail
