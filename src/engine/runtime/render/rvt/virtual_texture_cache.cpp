#include "runtime/render/rvt/virtual_texture_cache.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>

namespace engine::render_detail
{
bool VirtualTextureCache::configure(const schema::RvtSettingsT& settings)
{
    if (!valid_rvt_settings(settings))
    {
        return false;
    }
    if (configured_ && settings_ == settings)
    {
        return true;
    }
    release();
    settings_ = settings;
    configured_ = true;
    touched_.resize(settings.physical_pages);
    versions_.resize(settings.physical_pages, 1);
    return true;
}
std::uint8_t VirtualTextureCache::maximum_mip() const noexcept
{
    return static_cast<std::uint8_t>(
        std::bit_width(settings_.virtual_resolution / settings_.page_size) - 1);
}
Bounds3 VirtualTextureCache::bounds(RvtPageKey key) const noexcept
{
    const double n = double(settings_.virtual_resolution / settings_.page_size) / (1U << key.mip);
    const auto& low = settings_.bounds.minimum();
    const auto& high = settings_.bounds.maximum();
    const double x = (high.x() - low.x()) / n, z = (high.z() - low.z()) / n;
    return {{low.x() + key.x * x, low.y(), low.z() + key.y * z},
            {low.x() + (key.x + 1) * x, high.y(), low.z() + (key.y + 1) * z}};
}
void VirtualTextureCache::begin_frame(std::uint64_t device, std::uint32_t frame) noexcept
{
    if (device_ != device)
    {
        pages_.clear();
        demand_.clear();
        batch_ = {};
        std::fill(touched_.begin(), touched_.end(), 0);
        ++cache_revision_;
    }
    device_ = device;
    frame_ = frame;
}
bool VirtualTextureCache::request(const RvtPageDemand& request)
{
    if (!configured_ || request.mip > maximum_mip())
    {
        return false;
    }
    const auto& low = settings_.bounds.minimum();
    const auto& high = settings_.bounds.maximum();
    const auto& a = request.bounds.minimum();
    const auto& b = request.bounds.maximum();
    if (!std::isfinite(a.x()) || !std::isfinite(a.y()) || !std::isfinite(a.z()) ||
        !std::isfinite(b.x()) || !std::isfinite(b.y()) || !std::isfinite(b.z()) || a.x() > b.x() ||
        a.y() > b.y() || a.z() > b.z())
    {
        return false;
    }
    std::set<RvtPageKey> wanted;
    // Coarse ancestors guarantee a bounded fallback before finer pages are available.
    for (int mip = maximum_mip(); mip >= int(request.mip) && a.x() < high.x() && b.x() > low.x() &&
                                  a.z() < high.z() && b.z() > low.z();
         --mip)
    {
        const auto count = (settings_.virtual_resolution / settings_.page_size) >> mip;
        const auto cell = [&](double value, double origin, double size)
        { return std::clamp(int(std::floor((value - origin) / size * count)), 0, int(count) - 1); };
        const auto x0 = cell(a.x(), low.x(), high.x() - low.x()),
                   x1 = cell(b.x(), low.x(), high.x() - low.x());
        const auto y0 = cell(a.z(), low.z(), high.z() - low.z()),
                   y1 = cell(b.z(), low.z(), high.z() - low.z());
        for (int y = y0; y <= y1; ++y)
        {
            for (int x = x0; x <= x1; ++x)
            {
                wanted.insert({std::uint32_t(x), std::uint32_t(y), std::uint8_t(mip)});
            }
        }
        if (wanted.size() > settings_.physical_pages)
        {
            return false;
        }
    }
    std::vector<RvtPageKey> next(wanted.begin(), wanted.end());
    std::sort(next.begin(), next.end(),
              [](auto a, auto b) { return a.mip != b.mip ? a.mip > b.mip : a < b; });
    if (next != demand_ || view_ != request.view_identity)
    {
        ++demand_revision_;
        demand_ = std::move(next);
        view_ = request.view_identity;
    }
    for (auto key : demand_)
    {
        const auto found = pages_.find(key);
        if (found != pages_.end())
        {
            touched_[found->second.slot] = frame_;
        }
    }
    return true;
}
void VirtualTextureCache::invalidate(const Bounds3& dirty, std::uint64_t revision)
{
    bool changed = false;
    std::erase_if(pages_,
                  [&](const auto& entry)
                  {
                      const auto area = bounds(entry.first);
                      if (area.maximum().x() <= dirty.minimum().x() ||
                          area.minimum().x() >= dirty.maximum().x() ||
                          area.maximum().z() <= dirty.minimum().z() ||
                          area.minimum().z() >= dirty.maximum().z())
                      {
                          return false;
                      }
                      versions_[entry.second.slot] =
                          std::max(versions_[entry.second.slot] + 1, revision);
                      changed = true;
                      return true;
                  });
    if (changed)
    {
        ++cache_revision_;
    }
}
SceneWorkState VirtualTextureCache::prepare(const SceneWorkBudget& budget)
{
    batch_ = {};
    batch_.device_generation = device_;
    batch_.demand_revision = demand_revision_;
    if (!configured_ || !settings_.enabled)
    {
        return SceneWorkState::not_required;
    }
    const std::uint64_t tile = settings_.page_size + 2 * settings_.page_border;
    const auto limit = std::min<std::uint64_t>(
        {settings_.pages_per_frame, budget.items, budget.upload_bytes / (tile * tile * 24)});
    std::set<std::uint32_t> used;
    for (auto key : demand_)
    {
        if (pages_.contains(key))
        {
            continue;
        }
        if (batch_.pages.size() >= limit)
        {
            break;
        }
        std::uint32_t selected = UINT32_MAX, oldest = UINT32_MAX;
        for (std::uint32_t slot = 0; slot < settings_.physical_pages; ++slot)
        {
            if (used.contains(slot))
            {
                continue;
            }
            const auto occupant = std::find_if(pages_.begin(), pages_.end(), [&](const auto& value)
                                               { return value.second.slot == slot; });
            if (occupant != pages_.end() &&
                std::find(demand_.begin(), demand_.end(), occupant->first) != demand_.end())
            {
                continue;
            }
            if (touched_[slot] != 0 && std::int32_t(frame_ - touched_[slot]) < 3)
            {
                continue;
            }
            if (selected == UINT32_MAX || touched_[slot] < oldest)
            {
                selected = slot;
                oldest = touched_[slot];
            }
        }
        if (selected == UINT32_MAX)
        {
            break;
        }
        std::erase_if(pages_, [&](const auto& value) { return value.second.slot == selected; });
        ++cache_revision_;
        used.insert(selected);
        batch_.pages.push_back({key, selected, ++versions_[selected]});
    }
    const bool ready =
        std::all_of(demand_.begin(), demand_.end(), [&](auto key) { return pages_.contains(key); });
    return ready ? SceneWorkState::ready : SceneWorkState::working;
}
bool VirtualTextureCache::publish(const RvtPageBatch& batch)
{
    if (batch.device_generation != device_ || batch.demand_revision != demand_revision_)
    {
        return false;
    }
    for (const auto& page : batch.pages)
    {
        if (page.slot >= versions_.size() || page.version != versions_[page.slot])
        {
            return false;
        }
    }
    for (const auto& page : batch.pages)
    {
        pages_[page.key] = page;
        touched_[page.slot] = frame_;
    }
    if (!batch.pages.empty())
    {
        ++cache_revision_;
    }
    return true;
}
bool VirtualTextureCache::status(RvtStatus& output) const
{
    output = {};
    output.requested = configured_ && settings_.enabled;
    output.device_generation = device_;
    output.demand_revision = demand_revision_;
    output.cache_revision = cache_revision_;
    output.required_pages = std::uint32_t(demand_.size());
    for (auto key : demand_)
    {
        if (pages_.contains(key))
        {
            ++output.ready_pages;
        }
    }
    return configured_;
}
void VirtualTextureCache::release() noexcept
{
    pages_.clear();
    demand_.clear();
    touched_.clear();
    versions_.clear();
    batch_ = {};
    configured_ = false;
    ++demand_revision_;
    ++cache_revision_;
}
} // namespace engine::render_detail
