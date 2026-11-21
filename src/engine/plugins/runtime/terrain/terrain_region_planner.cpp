#include "plugins/runtime/terrain/terrain_regions.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"

#include "plugins/runtime/terrain/terrain_samples.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace engine::plugins::terrain
{
namespace
{
using namespace terrain_detail;
using Keys = std::set<TerrainRegionKey, TerrainRegionLess>;

double distance_to(Vec3 point, const Bounds3& box)
{
    ENGINE_PROFILE_FUNCTION();
    return std::hypot(
        std::max({box.minimum().x() - point.x(), point.x() - box.maximum().x(), 0.0}),
        std::max({box.minimum().y() - point.y(), point.y() - box.maximum().y(), 0.0}),
        std::max({box.minimum().z() - point.z(), point.z() - box.maximum().z(), 0.0}));
}

bool adjacent(const Bounds3& a, const Bounds3& b)
{
    ENGINE_PROFILE_FUNCTION();
    const std::array<double, 3> amin{a.minimum().x(), a.minimum().y(), a.minimum().z()};
    const std::array<double, 3> amax{a.maximum().x(), a.maximum().y(), a.maximum().z()};
    const std::array<double, 3> bmin{b.minimum().x(), b.minimum().y(), b.minimum().z()};
    const std::array<double, 3> bmax{b.maximum().x(), b.maximum().y(), b.maximum().z()};
    int touching = 0;
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        if (amax[axis] < bmin[axis] || bmax[axis] < amin[axis])
        {
            return false;
        }
        if (amax[axis] == bmin[axis] || bmax[axis] == amin[axis])
        {
            ++touching;
        }
    }
    return touching == 1;
}

class DistanceRegionPlanner final : public TerrainRegionPlanner
{
public:
    bool plan(const TerrainPlanInput& input, TerrainRegionPlan& output,
              std::string& error) const override
    {
        ENGINE_PROFILE_FUNCTION();
        output = {};
        error.clear();
        if (!input.source || input.views.size() > 16 || input.pins.size() > 64 ||
            input.budget.max_regions == 0 || input.budget.max_regions > 4096)
        {
            error = "Invalid terrain planning input or region budget";
            return false;
        }
        for (const auto& view : input.views)
        {
            if (!finite(view.center) || !std::isfinite(view.visual_radius) ||
                !std::isfinite(view.collision_radius) || view.visual_radius <= 0 ||
                view.collision_radius < 0 || view.collision_radius > view.visual_radius ||
                view.lod_bias < -15 || view.lod_bias > 15)
            {
                error = "Invalid terrain view";
                return false;
            }
        }
        const auto& source = *input.source;
        std::vector<Bounds3> pinned;
        for (const auto& pin : input.pins)
        {
            if (pin.owner_key == 0 || pin.regions.size() > input.budget.max_regions)
            {
                error = "Invalid terrain pin";
                return false;
            }
            for (auto key : pin.regions)
            {
                if (!entry(source, key) || (pin.collision_required && key.lod() != 0))
                {
                    error = "Terrain pin requires a known region at the requested quality";
                    return false;
                }
                pinned.push_back(region_bounds(source, key));
            }
        }
        Keys desired;
        const auto children = [&](TerrainRegionKey key)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            std::array<TerrainRegionKey, 8> result;
            for (int child = 0; child < 8; ++child)
            {
                result[child] = {key.x() * 2 + (child & 1), key.y() * 2 + ((child >> 1) & 1),
                                 key.z() * 2 + ((child >> 2) & 1),
                                 static_cast<std::uint8_t>(key.lod() - 1)};
            }
            return result;
        };
        const auto visit = [&](TerrainRegionKey key, auto&& self) -> bool
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            const auto bounds = region_bounds(source, key);
            if (!intersects(source.bounds, bounds))
            {
                return true;
            }
            const bool retained =
                std::any_of(input.resident.begin(), input.resident.end(),
                            [&](const auto& region)
                            {
                                ENGINE_PROFILE_FUNCTION_PROBE();
                                return region.key == key;
                            });
            bool needed = false;
            bool split = entry(source, key) == nullptr;
            for (const auto& view : input.views)
            {
                const double distance = distance_to(view.center, bounds);
                needed = needed || distance <= view.visual_radius * (retained ? 1.15 : 1.0);
                const double size = std::ldexp(source.voxel_size * source.block_cells, key.lod());
                split = split || distance <= view.collision_radius ||
                        distance < std::ldexp(size * (retained ? 1.8 : 2.0), -view.lod_bias);
            }
            for (const auto& pin : pinned)
            {
                if (intersects(bounds, pin))
                {
                    needed = true;
                    split = split || bounds.maximum().x() - bounds.minimum().x() >
                                         pin.maximum().x() - pin.minimum().x();
                }
            }
            if (!needed)
            {
                return true;
            }
            if (split && key.lod() > 0)
            {
                if (key.x() < INT32_MIN / 2 || key.x() > INT32_MAX / 2 || key.y() < INT32_MIN / 2 ||
                    key.y() > INT32_MAX / 2 || key.z() < INT32_MIN / 2 || key.z() > INT32_MAX / 2)
                {
                    error = "Terrain subdivision exceeds integer coordinates";
                    return false;
                }
                for (auto child : children(key))
                {
                    if (!self(child, self))
                    {
                        return false;
                    }
                }
                return true;
            }
            const auto* known = entry(source, key);
            if (!known || known->uniform == schema::TerrainUniformState::unknown)
            {
                error = "Terrain demand intersects an unknown region";
                return false;
            }
            desired.insert(key);
            if (desired.size() > input.budget.max_regions)
            {
                error = "Terrain demand exceeds the region budget";
                return false;
            }
            return true;
        };
        // Start with a small root grid, independent of the total number of source entries.
        std::uint8_t root_lod = 0;
        std::vector<TerrainRegionKey> roots;
        for (; root_lod < 15; ++root_lod)
        {
            if (enumerate(source, source.bounds, root_lod, 64, roots))
            {
                break;
            }
        }
        if (!enumerate(source, source.bounds, root_lod, 64, roots))
        {
            error = "Terrain bounds exceed supported integer coordinates";
            return false;
        }
        for (auto key : roots)
        {
            if (!visit(key, visit))
            {
                return false;
            }
        }
        // Refine only the coarse side until every shared face differs by at most one LOD.
        for (;;)
        {
            auto coarse = desired.end();
            for (auto first = desired.begin(); first != desired.end() && coarse == desired.end();
                 ++first)
            {
                for (auto second = std::next(first); second != desired.end(); ++second)
                {
                    if (std::abs(int(first->lod()) - int(second->lod())) > 1 &&
                        adjacent(region_bounds(source, *first), region_bounds(source, *second)))
                    {
                        coarse = first->lod() > second->lod() ? first : second;
                        break;
                    }
                }
            }
            if (coarse == desired.end())
            {
                break;
            }
            const auto parent = *coarse;
            if (parent.x() < INT32_MIN / 2 || parent.x() > INT32_MAX / 2 ||
                parent.y() < INT32_MIN / 2 || parent.y() > INT32_MAX / 2 ||
                parent.z() < INT32_MIN / 2 || parent.z() > INT32_MAX / 2)
            {
                error = "Terrain transition exceeds integer coordinates";
                return false;
            }
            desired.erase(coarse);
            for (auto child : children(parent))
            {
                if (!intersects(source.bounds, region_bounds(source, child)))
                {
                    continue;
                }
                const auto* known = entry(source, child);
                if (!known || known->uniform == schema::TerrainUniformState::unknown)
                {
                    error = "Terrain transition requires an unknown neighbor";
                    return false;
                }
                desired.insert(child);
            }
            if (desired.size() > input.budget.max_regions)
            {
                error = "Terrain transition neighbors exceed the region budget";
                return false;
            }
        }
        Keys resident_keys;
        for (const auto& region : input.resident)
        {
            resident_keys.insert(region.key);
            if (desired.contains(region.key))
            {
                output.retain.push_back(region.key);
            }
            else if (region.dirty || region.pin_count || region.state == SceneWorkState::working)
            {
                output = {};
                error =
                    "Terrain retirement is blocked by unsaved data, pins or an active operation";
                return false;
            }
            else
            {
                output.retire.push_back(region.key);
            }
        }
        for (auto key : desired)
        {
            if (!resident_keys.contains(key))
            {
                output.acquire.push_back(key);
            }
        }
        return true;
    }
};
} // namespace
std::unique_ptr<TerrainRegionPlanner> create_terrain_region_planner(std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    error.clear();
    return std::make_unique<DistanceRegionPlanner>();
}
} // namespace engine::plugins::terrain
