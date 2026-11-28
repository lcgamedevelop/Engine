#pragma once

#include "runtime/render/frame_graph_plan.hpp"
#include "runtime/render/render_constants.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace engine::render_detail
{
inline constexpr std::size_t ssr_max_reductions = ssr_trace_view - ssr_hiz_view - 1;

// The depth borrowed from the surface pass is level zero. Odd and even reduced levels live in
// separate textures. Each entry names just the mip attached to that level's framebuffer.
struct SsrResourceVersions
{
    FrameResourceId scene, ibl, depth, normal, specular, lut;
    std::array<FrameResourceId, ssr_max_reductions> reduced;
    FrameResourceId reflection, color;
};

inline constexpr SsrResourceVersions ssr_local_versions = []
{
    SsrResourceVersions versions{0, 1, 2, 3, 4, 5, {}, 6, 7};
    for (std::size_t i = 0; i < versions.reduced.size(); ++i)
    {
        versions.reduced[i] = static_cast<FrameResourceId>(8 + i);
    }
    return versions;
}();

// The owner may assign IDs from a larger graph. All returned spans are backed by this object.
class SsrGraphDeclaration final
{
public:
    SsrGraphDeclaration(const SsrResourceVersions& resources, std::uint16_t levels,
                        bool reuse_depth, bool has_lut)
        : versions_(resources)
    {
        if (levels == 0 || levels > ssr_max_reductions + 1)
        {
            return;
        }
        // Logical mip zero is already included among the trace's sampled inputs.
        const std::size_t reduced_count = levels - 1;
        const std::array sampled{versions_.scene,  versions_.ibl,      versions_.depth,
                                 versions_.normal, versions_.specular, versions_.lut};
        const std::size_t inputs = has_lut ? sampled.size() : sampled.size() - 1;
        std::copy_n(sampled.begin(), inputs, trace_reads_.begin());
        std::copy_n(versions_.reduced.begin(), reduced_count, trace_reads_.begin() + inputs);
        import_count_ = inputs + (reuse_depth ? reduced_count : 0);

        if (!reuse_depth)
        {
            for (std::size_t i = 0; i < reduced_count; ++i)
            {
                const auto& source = i == 0 ? versions_.depth : versions_.reduced[i - 1];
                passes_[count_++] = {static_cast<FramePassId>(ssr_hiz_view + 1 + i),
                                     {&source, 1},
                                     {&versions_.reduced[i], 1},
                                     {},
                                     false};
            }
        }
        passes_[count_++] = {ssr_trace_view,
                             std::span(trace_reads_).first(inputs + reduced_count),
                             {&versions_.reflection, 1},
                             {},
                             false};
        composite_reads_ = {versions_.scene,  versions_.ibl,        versions_.depth,
                            versions_.normal, versions_.reflection, versions_.specular};
        passes_[count_++] = {
            ssr_composite_view, composite_reads_, {&versions_.color, 1}, {}, false};
    }

    SsrGraphDeclaration(const SsrGraphDeclaration&) = delete;
    SsrGraphDeclaration& operator=(const SsrGraphDeclaration&) = delete;

    [[nodiscard]] std::span<const FramePassDeclaration> passes() const noexcept
    {
        return std::span(passes_).first(count_);
    }

    [[nodiscard]] std::span<const FrameResourceId> imports() const noexcept
    {
        return std::span(trace_reads_).first(import_count_);
    }

private:
    SsrResourceVersions versions_;
    std::array<FrameResourceId, 6 + ssr_max_reductions> trace_reads_{};
    std::array<FrameResourceId, 6> composite_reads_{};
    std::array<FramePassDeclaration, 2 + ssr_max_reductions> passes_{};
    std::size_t count_ = 0;
    std::size_t import_count_ = 0;
};
} // namespace engine::render_detail
