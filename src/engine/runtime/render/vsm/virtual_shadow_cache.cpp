#include "runtime/render/vsm/virtual_shadow_cache.hpp"
#include "runtime/performance/profiling.hpp"

#include "runtime/render/render_submission.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/resource/resource.hpp"

#include <algorithm>
#include <bit>
#include <bx/math.h>
#include <cmath>
#include <limits>
#include <set>
#include <span>
#include <utility>

namespace engine::render_detail
{
namespace
{
double dot(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
    ENGINE_PROFILE_FUNCTION();
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
bool inside(const VsmPageKey& key, const VsmView& view)
{
    ENGINE_PROFILE_FUNCTION();
    if (key.level >= view.level_count)
    {
        return false;
    }
    const auto& level = view.levels[key.level];
    return key.x >= level.x && key.x < level.x + view.pages_per_axis && key.y >= level.y &&
           key.y < level.y + view.pages_per_axis;
}
} // namespace

std::uint32_t vsm_atlas_columns(std::uint32_t pages) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(pages))));
}

VsmViewUniforms vsm_pack_view(const VsmView& view, std::uint32_t dominant_level) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    VsmViewUniforms output{};
    for (unsigned i = 0; i < 3; ++i)
    {
        for (unsigned j = 0; j < 3; ++j)
        {
            output[i][j] = static_cast<float>(view.axes[i][j]);
        }
    }
    output[3] = {static_cast<float>(view.depth_max - dot(view.axes[2], view.origin)),
                 static_cast<float>(1.0 / (view.depth_max - view.depth_min)),
                 static_cast<float>(view.pages_per_axis), static_cast<float>(view.level_count)};
    output[0][3] = static_cast<float>(vsm_atlas_columns(view.physical_pages) * vsm_tile_size);
    output[1][3] = static_cast<float>(view.levels[0].page_world_size / vsm_page_size);
    output[2][3] = static_cast<float>(dominant_level);
    output[4] = {view.forward[0], view.forward[1], view.forward[2], view.near_clip};
    output[5] = {view.pixel_scale, view.orthographic ? 1.F : 0.F, view.lod_bias,
                 view.shadow_distance};
    for (std::uint32_t i = 0; i < view.level_count; ++i)
    {
        const auto& level = view.levels[i];
        output[6 + i] = {
            static_cast<float>(level.x * level.page_world_size - dot(view.axes[0], view.origin)),
            static_cast<float>(level.y * level.page_world_size - dot(view.axes[1], view.origin)),
            static_cast<float>(level.page_world_size), view.origin_bottom_left ? 1.F : -1.F};
    }
    return output;
}

bool VirtualShadowCache::configure(const schema::VsmSettingsT& settings, const RenderConfig& config,
                                   std::uint64_t generation)
{
    ENGINE_PROFILE_FUNCTION();
    if (generation == 0 || config.vsm_physical_pages == 0 ||
        config.vsm_physical_pages > vsm_max_physical_pages ||
        settings.virtual_resolution < 4096 || settings.virtual_resolution > 32768 ||
        !std::has_single_bit(settings.virtual_resolution) ||
        !std::isfinite(settings.first_clipmap_extent) || settings.first_clipmap_extent <= 0 ||
        !std::isfinite(settings.lod_bias) || settings.lod_bias < -2 || settings.lod_bias > 4)
    {
        return false;
    }
    if (generation_ != generation || capacity_ != config.vsm_physical_pages ||
        settings_.virtual_resolution != settings.virtual_resolution ||
        settings_.first_clipmap_extent != settings.first_clipmap_extent ||
        settings_.lod_bias != settings.lod_bias)
    {
        reset_scene();
        settings_ = settings;
        capacity_ = config.vsm_physical_pages;
        generation_ = generation;
    }
    return true;
}

void VirtualShadowCache::invalidate_all()
{
    ENGINE_PROFILE_FUNCTION();
    for (auto& [key, page] : pages_)
    {
        page.valid = false;
        ++page.version;
    }
    demand_.clear();
    status_.demand_view_revision = 0;
    ++status_.cache_revision;
}

void VirtualShadowCache::invalidate(const std::array<double, 6>& bounds)
{
    ENGINE_PROFILE_FUNCTION();
    for (auto& [key, page] : pages_)
    {
        const auto size =
            std::ldexp(settings_.first_clipmap_extent * 2, key.level) / view_.pages_per_axis;
        const double margin = size * vsm_border / vsm_page_size;
        if (bounds[0] <= (key.x + 1) * size + margin && bounds[3] >= key.x * size - margin &&
            bounds[1] <= (key.y + 1) * size + margin && bounds[4] >= key.y * size - margin)
        {
            page.valid = false;
            ++page.version;
        }
    }
    ++status_.cache_revision;
}

bool VirtualShadowCache::begin_frame(const VsmView& view, const RenderFrameData& frame)
{
    ENGINE_PROFILE_FUNCTION();
    if (generation_ == 0 || view.generation != generation_ || pending_batch_ ||
        view.shadow_distance <= 0 || view.width == 0 || view.height == 0)
    {
        return false;
    }
    const bool projection_changed = view_.axes != view.axes || view_.scope != view.scope ||
                                    view_.observer != view.observer ||
                                    view_.light_instance != view.light_instance ||
                                    view_.shadow_distance != view.shadow_distance;
    const bool changed = projection_changed || view_.origin != view.origin ||
                         view_.inverse_view_projection != view.inverse_view_projection ||
                         view_.width != view.width || view_.height != view.height;
    const auto revision = view_.revision + (changed ? 1 : 0);
    view_ = view;
    view_.revision = std::max<std::uint64_t>(1, revision);
    view_.pages_per_axis = settings_.virtual_resolution / vsm_page_size;
    view_.physical_pages = capacity_;
    view_.lod_bias = settings_.lod_bias;
    view_.level_count = 1;
    while (std::ldexp(settings_.first_clipmap_extent, view_.level_count - 1) < view.shadow_distance)
    {
        if (view_.level_count == vsm_max_levels)
        {
            return false;
        }
        ++view_.level_count;
    }
    for (std::uint32_t level = 0; level < view_.level_count; ++level)
    {
        auto& tile = view_.levels[level];
        tile.page_world_size =
            std::ldexp(settings_.first_clipmap_extent * 2, level) / view_.pages_per_axis;
        const double x = std::floor(dot(view_.axes[0], view.origin) / tile.page_world_size);
        const double y = std::floor(dot(view_.axes[1], view.origin) / tile.page_world_size);
        constexpr double limit = 9.0e15;
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > limit || std::abs(y) > limit)
        {
            return false;
        }
        tile.x = static_cast<std::int64_t>(x) - view_.pages_per_axis / 2;
        tile.y = static_cast<std::int64_t>(y) - view_.pages_per_axis / 2;
    }
    if (projection_changed)
    {
        invalidate_all();
        sources_.clear();
        depth_min_ = depth_max_ = 0;
    }
    std::map<std::uint64_t, Source> current;
    double minimum = dot(view_.axes[2], view.origin) - view.shadow_distance;
    double maximum = minimum + view.shadow_distance * 2;
    for (const auto& mesh : frame.meshes)
    {
        if (!mesh.cast_shadows || !mesh.any_opaque || !mesh.gpu)
        {
            continue;
        }
        const auto& gpu = *mesh.gpu;
        Source source;
        source.instance = gpu.instance_id;
        source.geometry = gpu.revision;
        source.deformation = gpu.deformation_revision;
        source.world = mesh.world;
        source.bounds = {INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY};
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            std::array<double, 3> point{};
            for (unsigned row = 0; row < 3; ++row)
            {
                point[row] = mesh.world[12 + row];
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    point[row] += mesh.world[axis * 4 + row] *
                                  ((corner & (1U << axis)) ? gpu.maximum[axis] : gpu.minimum[axis]);
                }
            }
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                const auto value = dot(view_.axes[axis], point);
                if (!std::isfinite(value))
                {
                    return false;
                }
                source.bounds[axis] = std::min(source.bounds[axis], value);
                source.bounds[axis + 3] = std::max(source.bounds[axis + 3], value);
            }
        }
        minimum = std::min(minimum, source.bounds[2]);
        maximum = std::max(maximum, source.bounds[5]);
        for (const auto& material : gpu.materials)
        {
            source.material.push_back(static_cast<std::uint64_t>(material.cull));
            source.material.push_back(static_cast<std::uint64_t>(material.blend));
            source.material.push_back(material.shadow_texture.idx);
            source.material.push_back(material.shadow_valid);
            for (const auto value : material.shadow_alpha)
            {
                source.material.push_back(std::bit_cast<std::uint32_t>(value));
            }
            for (const auto value : material.shadow_uv)
            {
                source.material.push_back(std::bit_cast<std::uint32_t>(value));
            }
        }
        for (const auto* texture : gpu.textures)
        {
            source.material.push_back(texture ? texture->generation() : 0);
        }
        const auto old = sources_.find(gpu.node_id);
        if (old == sources_.end() || old->second != source)
        {
            if (old != sources_.end())
            {
                invalidate(old->second.bounds);
            }
            invalidate(source.bounds);
        }
        current.emplace(gpu.node_id, std::move(source));
    }
    for (const auto& [id, source] : sources_)
    {
        if (!current.contains(id))
        {
            invalidate(source.bounds);
        }
    }
    sources_ = std::move(current);
    if (minimum < depth_min_ || maximum > depth_max_ || depth_min_ == depth_max_)
    {
        const double margin = std::max(1.0, (maximum - minimum) * 0.1);
        depth_min_ = std::min(depth_min_, minimum - margin);
        depth_max_ = std::max(depth_max_, maximum + margin);
        invalidate_all();
    }
    view_.depth_min = depth_min_;
    view_.depth_max = depth_max_;
    status_.scope = view.scope;
    status_.light_instance = view.light_instance;
    status_.device_generation = view.device_generation;
    status_.generation = generation_;
    status_.view_revision = view_.revision;
    status_.redrawn_pages = 0;
    ++frame_;
    return true;
}

bool VirtualShadowCache::request(const VsmPageDemand& demand)
{
    ENGINE_PROFILE_FUNCTION();
    const auto& v = demand.view;
    if (v.generation != generation_ || v.scope != view_.scope || v.observer != view_.observer ||
        v.device_generation != view_.device_generation ||
        v.light_instance != view_.light_instance || v.axes != view_.axes ||
        v.depth_min != depth_min_ || v.depth_max != depth_max_ || v.width != view_.width ||
        v.height != view_.height || v.pixel_scale != view_.pixel_scale ||
        v.shadow_distance != view_.shadow_distance || demand.submission_id < demand_submission_)
    {
        return false;
    }
    double distance_squared = 0;
    for (unsigned i = 0; i < 3; ++i)
    {
        distance_squared += (v.origin[i] - view_.origin[i]) * (v.origin[i] - view_.origin[i]);
    }
    if (distance_squared > double(view_.shadow_distance) * view_.shadow_distance)
    {
        return false;
    }
    std::vector<VsmPageKey> pages;
    for (const auto& key : demand.pages)
    {
        if (key.level >= view_.level_count)
        {
            return false;
        }
        if (inside(key, view_))
        {
            pages.push_back(key);
        }
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    demand_ = std::move(pages);
    demand_submission_ = demand.submission_id;
    status_.demand_view_revision = v.revision;
    ++status_.demand_revision;
    return true;
}

bool VirtualShadowCache::plan(std::uint32_t budget, VsmPageBatch& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    if (!generation_ || pending_batch_ || !view_.level_count || budget == 0 || budget > 16)
    {
        return false;
    }
    output.generation = generation_;
    output.batch_id = pending_batch_ = ++batch_id_;
    output.view = view_;
    const auto columns = vsm_atlas_columns(capacity_);
    auto next_free = static_cast<std::uint32_t>(pages_.size());
    for (const auto& key : demand_)
    {
        if (!inside(key, view_))
        {
            continue;
        }
        auto found = pages_.find(key);
        if (found != pages_.end() && found->second.valid)
        {
            found->second.last_used = frame_;
            continue;
        }
        if (pending_pages_ == budget)
        {
            continue;
        }
        Reservation candidate;
        candidate.key = key;
        if (found == pages_.end() && next_free < capacity_)
        {
            candidate.slot = next_free++;
            candidate.version = 1;
        }
        else
        {
            if (found == pages_.end())
            {
                const auto reserved = std::span(reserved_).first(pending_pages_);
                for (auto it = pages_.begin(); it != pages_.end(); ++it)
                {
                    if (!std::binary_search(demand_.begin(), demand_.end(), it->first) &&
                        !std::ranges::any_of(reserved, [&](const auto& r)
                                             { return r.slot == it->second.slot; }) &&
                        (found == pages_.end() || it->second.last_used < found->second.last_used))
                    {
                        found = it;
                    }
                }
                if (found == pages_.end())
                {
                    continue;
                }
            }
            candidate.previous = found->first;
            candidate.slot = found->second.slot;
            candidate.version = found->second.version + (found->first != key);
        }
        reserved_[pending_pages_++] = candidate;
        VsmPageDraw draw;
        draw.key = key;
        draw.slot = candidate.slot;
        draw.version = candidate.version;
        draw.view_id = vsm_page_views[output.pages.size()];
        draw.x = static_cast<std::uint16_t>((draw.slot % columns) * vsm_tile_size);
        draw.y = static_cast<std::uint16_t>((draw.slot / columns) * vsm_tile_size);
        draw.size = vsm_tile_size;
        bx::mtxIdentity(draw.view.data());
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            for (unsigned row = 0; row < 3; ++row)
            {
                draw.view[row * 4 + axis] = static_cast<float>(view_.axes[axis][row]);
            }
        }
        const double size = view_.levels[key.level].page_world_size;
        const double x = key.x * size - dot(view_.axes[0], view_.origin);
        const double y = key.y * size - dot(view_.axes[1], view_.origin);
        const double border = size * vsm_border / vsm_page_size;
        const auto origin_z = dot(view_.axes[2], view_.origin);
        bx::mtxOrtho(draw.projection.data(), float(x - border), float(x + size + border),
                     float(y - border), float(y + size + border), float(origin_z - depth_max_),
                     float(origin_z - depth_min_), 0, view_.homogeneous_depth,
                     bx::Handedness::Right);
        output.pages.push_back(draw);
    }
    const auto count = view_.pages_per_axis * view_.pages_per_axis * view_.level_count;
    output.page_table.resize((count + 3) / 4);
    const auto table_entry = [&](const VsmPageKey& key, std::uint32_t slot, std::uint32_t flags)
    {
        if (!inside(key, view_))
        {
            return;
        }
        const auto& level = view_.levels[key.level];
        const auto index =
            static_cast<std::uint32_t>(key.level * view_.pages_per_axis * view_.pages_per_axis +
                                       (key.y - level.y) * view_.pages_per_axis + key.x - level.x);
        if (flags & vsm_page_resident)
        {
            const auto row = slot / columns;
            const auto sample_row = view_.origin_bottom_left ? columns - 1U - row : row;
            flags |= ((sample_row * vsm_tile_size) << vsm_atlas_origin_bits) |
                     ((slot % columns) * vsm_tile_size);
        }
        output.page_table[index / 4][index % 4] = std::bit_cast<float>(flags);
    };
    for (const auto& [key, page] : pages_)
    {
        table_entry(key, page.slot, page.valid ? vsm_page_resident : vsm_page_dirty);
    }
    for (const auto& candidate : std::span(reserved_).first(pending_pages_))
    {
        if (candidate.previous && *candidate.previous != candidate.key)
        {
            table_entry(*candidate.previous, 0, 0);
        }
        table_entry(candidate.key, candidate.slot, vsm_page_resident | vsm_page_planned);
    }
    std::array<std::uint32_t, vsm_max_levels> level_demand{};
    for (const auto& key : demand_)
    {
        if (inside(key, view_))
        {
            ++level_demand[key.level];
        }
    }
    output.dominant_level = static_cast<std::uint32_t>(
        std::max_element(level_demand.begin(), level_demand.end()) - level_demand.begin());

    return true;
}

bool VirtualShadowCache::begin_page_write(std::uint64_t batch_id, const VsmPageDraw& draw)
{
    const auto candidates = std::span(reserved_).first(pending_pages_);
    const auto candidate = std::ranges::find(candidates, draw.slot, &Reservation::slot);
    if (!pending_batch_ || batch_id != pending_batch_ || pending_submission_ ||
        candidate == candidates.end() || candidate->key != draw.key ||
        candidate->version != draw.version || candidate->started)
    {
        return false;
    }
    if (candidate->previous)
    {
        const auto old = pages_.find(*candidate->previous);
        if (old == pages_.end() || old->second.slot != draw.slot)
        {
            return false;
        }
        if (std::exchange(old->second.valid, false))
        {
            ++status_.cache_revision;
        }
    }
    candidate->started = true;
    return true;
}

bool VirtualShadowCache::publish(const VsmPageBatch& batch, std::uint64_t submission_id)
{
    ENGINE_PROFILE_FUNCTION();
    if (!pending_batch_ || pending_submission_ || batch.batch_id != pending_batch_ ||
        batch.generation != generation_)
    {
        return false;
    }
    if (!submission_id || batch.pages.size() != pending_pages_)
    {
        cancel_batch();
        return false;
    }
    for (std::size_t i = 0; i < batch.pages.size(); ++i)
    {
        const auto& draw = batch.pages[i];
        const auto& candidate = reserved_[i];
        if (!candidate.started || draw.key != candidate.key || draw.version != candidate.version ||
            draw.slot != candidate.slot)
        {
            cancel_batch();
            return false;
        }
    }
    pending_submission_ = submission_id;
    return true;
}

void VirtualShadowCache::finish_frame(bool success, VsmStatus& output)
{
    if (!pending_batch_)
    {
        return;
    }
    if (success && pending_submission_)
    {
        for (const auto& candidate : std::span(reserved_).first(pending_pages_))
        {
            if (candidate.previous && *candidate.previous != candidate.key)
            {
                pages_.erase(*candidate.previous);
            }
            pages_[candidate.key] = {candidate.slot, candidate.version, frame_, true};
        }
        status_.submission_id = pending_submission_;
        status_.redrawn_pages = static_cast<std::uint32_t>(pending_pages_);
        status_.cache_revision += pending_pages_ != 0;
    }
    cancel_batch();
    const auto bytes = output.resident_bytes;
    (void)status(output);
    output.resident_bytes = bytes;
}

void VirtualShadowCache::cancel_batch() noexcept
{
    pending_pages_ = 0;
    pending_submission_ = pending_batch_ = 0;
}

bool VirtualShadowCache::status(VsmStatus& output, bool preview) const
{
    ENGINE_PROFILE_FUNCTION();
    if (preview && !pending_submission_)
    {
        return false;
    }
    output = status_;
    output.resident_pages = output.ready_pages = output.dirty_pages = 0;
    output.demanded_pages = static_cast<std::uint32_t>(demand_.size());
    for (const auto& [key, page] : pages_)
    {
        output.resident_pages += page.valid;
        output.dirty_pages += !page.valid;
    }
    for (const auto& key : demand_)
    {
        const auto it = pages_.find(key);
        output.ready_pages += it != pages_.end() && it->second.valid;
    }
    // The overlay describes this frame's encoded table. Public status is confirmed at frame end.
    if (preview)
    {
        const auto count = static_cast<std::uint32_t>(pending_pages_);
        output.resident_pages += count;
        output.ready_pages += count;
        output.redrawn_pages = count;
        output.submission_id = pending_submission_;
        output.cache_revision += count != 0;
        for (const auto& candidate : std::span(reserved_).first(pending_pages_))
        {
            output.dirty_pages -= candidate.previous.has_value();
        }
    }
    output.fallback_pages = output.demanded_pages - output.ready_pages;
    output.state = generation_ == 0             ? VsmState::disabled
                   : demand_.size() > capacity_ ? VsmState::budget_limited
                   : output.demand_revision && output.demand_view_revision == view_.revision &&
                           output.fallback_pages == 0
                       ? VsmState::ready
                       : VsmState::warming;
    return true;
}
void VirtualShadowCache::reset_scene() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    pages_.clear();
    sources_.clear();
    demand_.clear();
    status_ = {};
    view_ = {};
    cancel_batch();
    generation_ = demand_submission_ = 0;
    depth_min_ = depth_max_ = 0;
}
} // namespace engine::render_detail
