#include "android/native/memory_counters.hpp"

#include "runtime/app/runtime_host.hpp"
#include "runtime/performance/memory_snapshot.hpp"
#include "runtime/render/gpu_memory.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/resource/resource_loader.hpp"
#include "runtime/world/world_system.hpp"

#include <android/trace.h>
#include <dlfcn.h>

#include <array>
#include <cstdint>
#include <cstdio>

namespace engine::host_detail
{
namespace
{
using SetCounter = void (*)(const char*, std::int64_t);

void named_counter(SetCounter write, const char* prefix, const char* name, std::uint64_t value)
{
    char label[96];
    const auto size = std::snprintf(label, sizeof(label), "%s%s", prefix, name);
    if (size > 0 && static_cast<std::size_t>(size) < sizeof(label))
    {
        write(label, static_cast<std::int64_t>(value));
    }
}
} // namespace

void publish_android_memory_counters(RuntimeHost& host)
{
    // ATrace is available on API 23. Counters arrived in API 29; keep the API 28 app loadable.
    if (!ATrace_isEnabled())
    {
        return;
    }
    static const auto write =
        reinterpret_cast<SetCounter>(dlsym(RTLD_DEFAULT, "ATrace_setCounter"));
    if (write == nullptr)
    {
        return;
    }

    ATrace_beginSection("Engine.Memory.Sample");
    const auto cpu = memory_profile::snapshot();
    for (std::size_t i = 0; i < cpu.pools.size(); ++i)
    {
        named_counter(write, "Engine.CPU.PoolBytes.", memory_profile::pool_keys[i],
                      cpu.pools[i].current_bytes);
    }

    auto* world = host.world();
    const auto* loader = world ? world->resource_loader() : nullptr;
    ResourceMemorySnapshot resources;
    ResourceMemoryQuery query;
    query.limit = 1;
    if (loader && loader->memory_snapshot(query, resources))
    {
        for (const auto kind : schema::EnumValuesAssetKind())
        {
            const auto& usage = resources.kinds[static_cast<std::size_t>(kind)];
            named_counter(write, "Engine.CPU.PayloadBytes.", schema::EnumNameAssetKind(kind),
                          usage.measured_cpu_payload_bytes);
        }
        write("Engine.CPU.UnmeasuredPayloadCount", resources.unmeasured_payloads);
    }

    auto* renderer = host.renderer();
    GpuMemorySnapshot gpu;
    if (renderer && renderer->memory_snapshot({}, 1, gpu))
    {
        static constexpr std::array kinds{"Texture", "MeshGeometry", "StaticVertexPage",
                                          "FrameOutput", "FrameReadback"};
        static_assert(kinds.size() == static_cast<std::size_t>(GpuMemoryKind::count));
        for (std::size_t i = 0; i < kinds.size(); ++i)
        {
            named_counter(write, "Engine.GPU.LogicalBytes.", kinds[i],
                          gpu.live_by_kind[i].logical_bytes);
            named_counter(write, "Engine.GPU.SharedBackingBytes.", kinds[i],
                          gpu.live_by_kind[i].shared_backing_bytes);
        }
        if (gpu.bgfx_texture_estimated_bytes)
        {
            write("Engine.GPU.BgfxTextureBytes", *gpu.bgfx_texture_estimated_bytes);
        }
        if (gpu.bgfx_render_target_estimated_bytes)
        {
            write("Engine.GPU.BgfxRenderTargetBytes", *gpu.bgfx_render_target_estimated_bytes);
        }
        write("Engine.CPU.TextureStagingReservedBytes", gpu.texture_staging_reserved_bytes);
        write("Engine.Transfer.VsmFeedbackReadbackBytesTotal",
              gpu.vsm_feedback_readback_bytes_total);
        RvtStatus rvt;
        if (renderer->rvt_status(rvt))
        {
            write("Engine.GPU.RvtResidentEstimateBytes", rvt.resident_bytes);
        }
        VsmStatus vsm;
        if (renderer->vsm_status(vsm))
        {
            write("Engine.GPU.VsmResidentEstimateBytes", vsm.resident_bytes);
        }
    }
    ATrace_endSection();
}
} // namespace engine::host_detail
