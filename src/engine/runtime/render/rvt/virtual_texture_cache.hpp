#pragma once
#include "runtime/render/rvt.hpp"
#include "runtime/scene/scene_loading.hpp"
#include <compare>
#include <map>
#include <span>
#include <vector>

namespace engine::render_detail
{
struct RvtPageKey
{
    std::uint32_t x = 0, y = 0;
    std::uint8_t mip = 0;
    auto operator<=>(const RvtPageKey&) const = default;
};
struct RvtPageDemand
{
    Bounds3 bounds;
    std::uint8_t mip = 0;
    std::uint64_t view_identity = 0;
};
struct RvtPage
{
    RvtPageKey key;
    std::uint32_t slot = 0;
    std::uint64_t version = 0;
};
struct RvtPageBatch
{
    std::uint64_t device_generation = 0, demand_revision = 0;
    std::vector<RvtPage> pages;
};
class VirtualTextureCache final
{
public:
    [[nodiscard]] bool configure(const schema::RvtSettingsT& settings);
    [[nodiscard]] bool request(const RvtPageDemand& demand);
    void invalidate(const Bounds3& bounds, std::uint64_t revision);
    [[nodiscard]] SceneWorkState prepare(const SceneWorkBudget& budget);
    [[nodiscard]] bool publish(const RvtPageBatch& batch);
    [[nodiscard]] bool status(RvtStatus& output) const;
    void release() noexcept;
    [[nodiscard]] const RvtPageBatch& batch() const noexcept
    {
        return batch_;
    }
    [[nodiscard]] const schema::RvtSettingsT& settings() const noexcept
    {
        return settings_;
    }
    [[nodiscard]] const std::map<RvtPageKey, RvtPage>& pages() const noexcept
    {
        return pages_;
    }
    [[nodiscard]] const std::vector<RvtPageKey>& demand() const noexcept
    {
        return demand_;
    }
    [[nodiscard]] Bounds3 bounds(RvtPageKey key) const noexcept;
    [[nodiscard]] std::uint8_t maximum_mip() const noexcept;
    void begin_frame(std::uint64_t device, std::uint32_t frame) noexcept;

private:
    schema::RvtSettingsT settings_;
    std::map<RvtPageKey, RvtPage> pages_;
    std::vector<RvtPageKey> demand_;
    std::vector<std::uint32_t> touched_;
    std::vector<std::uint64_t> versions_;
    RvtPageBatch batch_;
    std::uint64_t device_ = 0, demand_revision_ = 0, cache_revision_ = 0, view_ = 0;
    std::uint32_t frame_ = 0;
    bool configured_ = false;
};
} // namespace engine::render_detail
