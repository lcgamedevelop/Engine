#pragma once

#include "runtime/render/vsm/vsm_types.hpp"

#include <map>

namespace engine
{
struct RenderConfig;
namespace render_detail
{
struct RenderFrameData;
class VirtualShadowCache final
{
public:
    [[nodiscard]] bool configure(const schema::VsmSettingsT& settings, const RenderConfig& config,
                                 std::uint64_t generation);
    [[nodiscard]] bool begin_frame(const VsmView& view, const RenderFrameData& frame);
    [[nodiscard]] bool request(const VsmPageDemand& demand);
    [[nodiscard]] bool plan(std::uint32_t budget, VsmPageBatch& output);
    [[nodiscard]] bool publish(const VsmPageBatch& batch, std::uint64_t submission_id);
    void cancel_batch() noexcept;
    [[nodiscard]] bool status(VsmStatus& output) const;
    void reset_scene() noexcept;

private:
    struct Page
    {
        std::uint32_t slot = 0;
        std::uint64_t version = 1, last_used = 0;
        bool valid = false, planned = false;
    };
    struct Source
    {
        std::uint64_t instance = 0, geometry = 0, deformation = 0;
        std::array<double, 16> world{};
        std::array<double, 6> bounds{};
        std::vector<std::uint64_t> material;
        bool operator==(const Source&) const = default;
    };
    void invalidate(const std::array<double, 6>& bounds);
    void invalidate_all();
    schema::VsmSettingsT settings_;
    std::uint32_t capacity_ = 0;
    VsmView view_;
    VsmStatus status_;
    std::map<VsmPageKey, Page> pages_;
    std::map<std::uint64_t, Source> sources_;
    std::vector<VsmPageKey> demand_;
    std::uint64_t generation_ = 0, batch_id_ = 0, pending_batch_ = 0, frame_ = 0;
    std::uint64_t demand_submission_ = 0;
    double depth_min_ = 0, depth_max_ = 0;
};
} // namespace render_detail
} // namespace engine
