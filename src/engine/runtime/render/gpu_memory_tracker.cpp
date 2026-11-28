#include "runtime/render/gpu_memory_tracker.hpp"

#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

namespace engine::render_detail
{
namespace
{
GpuMemoryPoint memory_point(std::uint32_t frame) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    using namespace std::chrono;
    return {static_cast<std::uint64_t>(duration_cast<nanoseconds>(steady_clock::now()
                                                                  .time_since_epoch()).count()),
            frame};
}

void largest(std::vector<const GpuMemoryEntry*>& matches, std::uint32_t limit,
             std::vector<GpuMemoryEntry>& output)
{
    ENGINE_PROFILE_FUNCTION();
    const auto count = std::min<std::size_t>(matches.size(), limit);
    const auto end = matches.begin() + static_cast<std::ptrdiff_t>(count);
    std::partial_sort(matches.begin(), end, matches.end(),
                      [](const auto* left, const auto* right)
                      {
                          ENGINE_PROFILE_FUNCTION_PROBE();
                          const auto left_bytes =
                              std::max(left->logical_bytes, left->shared_backing_bytes);
                          const auto right_bytes =
                              std::max(right->logical_bytes, right->shared_backing_bytes);
                          return left_bytes == right_bytes ? left->id < right->id
                                                           : left_bytes > right_bytes;
                      });
    output.reserve(count);
    for (auto item = matches.begin(); item != end; ++item)
    {
        output.push_back(**item);
    }
}
} // namespace

GpuMemoryTracker::GpuMemoryTracker(std::uint64_t device_generation, bool threaded) noexcept
    : started_ns_(memory_point(0).timestamp_ns), device_generation_(device_generation),
      threaded_(threaded)
{
    ENGINE_PROFILE_FUNCTION();
}

std::uint64_t GpuMemoryTracker::track(GpuMemoryEntry entry)
{
    ENGINE_PROFILE_FUNCTION();
    if (next_id_ == std::numeric_limits<std::uint64_t>::max())
    {
        return 0;
    }
    entry.id = ++next_id_;
    entry.created = memory_point(frame_);
    ++totals_.live_count;
    totals_.live_logical_bytes += entry.logical_bytes;
    totals_.live_shared_backing_bytes += entry.shared_backing_bytes;
    totals_.peak_logical_bytes = std::max(totals_.peak_logical_bytes,
                                         totals_.live_logical_bytes +
                                             totals_.retiring_logical_bytes);
    totals_.peak_shared_backing_bytes = std::max(totals_.peak_shared_backing_bytes,
                                                totals_.live_shared_backing_bytes +
                                                    totals_.retiring_shared_backing_bytes);
    live_.emplace(next_id_, std::move(entry));
    return next_id_;
}

void GpuMemoryTracker::release_requested(std::uint64_t id) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto found = live_.find(id);
    if (found != live_.end() && !found->second.release_requested.timestamp_ns)
    {
        auto& entry = found->second;
        entry.release_requested = memory_point(frame_);
        if (entry.phase == GpuMemoryPhase::live)
        {
            entry.phase = GpuMemoryPhase::waiting_retirement;
        }
    }
}

void GpuMemoryTracker::retain(std::uint64_t id) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto found = live_.find(id);
    if (found != live_.end() && found->second.phase == GpuMemoryPhase::waiting_retirement)
    {
        found->second.release_requested = {};
        found->second.phase = GpuMemoryPhase::live;
    }
}

void GpuMemoryTracker::wait_for_readback(std::uint64_t id) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto found = live_.find(id);
    if (found != live_.end())
    {
        found->second.phase = GpuMemoryPhase::waiting_readback;
    }
}

void GpuMemoryTracker::readback_ready(std::uint64_t id) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto found = live_.find(id);
    if (found != live_.end())
    {
        found->second.readback_ready = memory_point(frame_);
    }
}

void GpuMemoryTracker::queue_destroy(std::uint64_t id) noexcept
{
    ENGINE_PROFILE_ZONE("Render.gpuMemoryQueueDestroy");
    auto node = live_.extract(id);
    if (node.empty())
    {
        return;
    }
    auto& entry = node.mapped();
    const auto now = memory_point(frame_);
    if (!entry.release_requested.timestamp_ns)
    {
        entry.release_requested = now;
    }
    entry.phase = GpuMemoryPhase::destroy_queued;
    entry.destroy_queued = now;
    --totals_.live_count;
    totals_.live_logical_bytes -= entry.logical_bytes;
    totals_.live_shared_backing_bytes -= entry.shared_backing_bytes;
    ++totals_.retiring_count;
    totals_.retiring_logical_bytes += entry.logical_bytes;
    totals_.retiring_shared_backing_bytes += entry.shared_backing_bytes;
    queued_.insert(std::move(node));
}

void GpuMemoryTracker::reclaim(GpuMemoryPoint observed, bool device_close) noexcept
{
    ENGINE_PROFILE_ZONE("Render.gpuMemoryReclaim");
    for (auto& [id, entry] : submitted_)
    {
        (void)id;
        --totals_.retiring_count;
        totals_.retiring_logical_bytes -= entry.logical_bytes;
        totals_.retiring_shared_backing_bytes -= entry.shared_backing_bytes;
        entry.phase = GpuMemoryPhase::backend_reclaimed;
        entry.backend_reclaimed = observed;
        entry.drained_on_device_close = device_close;
        recent_[recent_next_] = std::move(entry);
        recent_next_ = (recent_next_ + 1) % recent_.size();
        if (recent_count_ < recent_.size())
        {
            ++recent_count_;
        }
        else
        {
            ++overwritten_history_;
        }
    }
    submitted_.clear();
}

void GpuMemoryTracker::frame_submitted(std::uint32_t frame) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    frame_ = frame;
    if (!submitted_.empty() || !queued_.empty())
    {
        const auto now = memory_point(frame);
        // The pinned bgfx frame waits for the previous render thread's post commands.
        // Its single-thread path executes this submission's post commands inline.
        reclaim(now, false);
        for (auto& [id, entry] : queued_)
        {
            (void)id;
            entry.phase = GpuMemoryPhase::destroy_submitted;
            entry.destroy_submitted = now;
        }
        submitted_.swap(queued_);
        if (!threaded_)
        {
            reclaim(now, false);
        }
    }
    plot();
}

void GpuMemoryTracker::device_drained() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto now = memory_point(frame_);
    submitted_.merge(queued_);
    reclaim(now, true);
    plot();
}

void GpuMemoryTracker::plot() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (ENGINE_PROFILE_CONNECTED())
    {
        ENGINE_PROFILE_PLOT("Render.gpuLogicalLiveBytes",
                            static_cast<std::int64_t>(totals_.live_logical_bytes));
        ENGINE_PROFILE_PLOT("Render.gpuSharedBackingLiveBytes",
                            static_cast<std::int64_t>(totals_.live_shared_backing_bytes));
        ENGINE_PROFILE_PLOT("Render.gpuLogicalRetiringBytes",
                            static_cast<std::int64_t>(totals_.retiring_logical_bytes));
        ENGINE_PROFILE_PLOT("Render.gpuSharedBackingRetiringBytes",
                            static_cast<std::int64_t>(totals_.retiring_shared_backing_bytes));
    }
}

bool GpuMemoryTracker::snapshot(std::string_view guid, std::uint32_t limit,
                                GpuMemorySnapshot& output) const
{
    ENGINE_PROFILE_ZONE("Render.gpuMemorySnapshot");
    output = {};
    if (limit == 0 || limit > GpuMemorySnapshot::maximum_items)
    {
        return false;
    }
    output.session_started_ns = started_ns_;
    output.device_generation = device_generation_;
    output.sequence = ++sequence_;
    output.sampled = memory_point(frame_);
    output.totals = totals_;
    output.overwritten_history = overwritten_history_;
    std::vector<const GpuMemoryEntry*> live;
    std::vector<const GpuMemoryEntry*> pending;
    const auto collect = [&](const Entries& entries, bool owns_handles)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        for (const auto& [id, entry] : entries)
        {
            (void)id;
            if (owns_handles)
            {
                auto& usage = output.live_by_kind[static_cast<std::size_t>(entry.kind)];
                ++usage.count;
                usage.logical_bytes += entry.logical_bytes;
                usage.shared_backing_bytes += entry.shared_backing_bytes;
            }
            if (guid.empty() || entry.guid == guid)
            {
                (entry.phase == GpuMemoryPhase::live ? live : pending).push_back(&entry);
            }
        }
    };
    collect(live_, true);
    collect(queued_, false);
    collect(submitted_, false);
    output.matching_live = live.size();
    output.matching_pending = pending.size();
    largest(live, limit, output.live);
    largest(pending, limit, output.pending);
    for (std::size_t offset = 0; offset < recent_count_; ++offset)
    {
        const auto index = (recent_next_ + recent_.size() - 1 - offset) % recent_.size();
        const auto& entry = recent_[index];
        if (guid.empty() || entry.guid == guid)
        {
            ++output.matching_reclaims;
            if (output.recent_reclaims.size() < limit)
            {
                output.recent_reclaims.push_back(entry);
            }
        }
    }
    return true;
}

GpuMemoryAllocation::~GpuMemoryAllocation() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    reset();
}

GpuMemoryAllocation::GpuMemoryAllocation(GpuMemoryAllocation&& other) noexcept
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    : tracker_(std::exchange(other.tracker_, nullptr)), id_(std::exchange(other.id_, 0))
#endif
{
    ENGINE_PROFILE_FUNCTION();
    (void)other;
}

GpuMemoryAllocation& GpuMemoryAllocation::operator=(GpuMemoryAllocation&& other) noexcept
{
    ENGINE_PROFILE_FUNCTION();
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    if (this != &other)
    {
        reset();
        tracker_ = std::exchange(other.tracker_, nullptr);
        id_ = std::exchange(other.id_, 0);
    }
#else
    (void)other;
#endif
    return *this;
}

void GpuMemoryAllocation::track(GpuMemoryTracker* tracker, GpuMemoryEntry entry)
{
    ENGINE_PROFILE_FUNCTION();
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    reset();
    if (tracker)
    {
        tracker_ = tracker;
        id_ = tracker->track(std::move(entry));
    }
#else
    (void)tracker;
    (void)entry;
#endif
}

void GpuMemoryAllocation::release_requested() noexcept
{
    ENGINE_PROFILE_FUNCTION();
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    if (id_)
    {
        tracker_->release_requested(id_);
    }
#endif
}

void GpuMemoryAllocation::retain() noexcept
{
    ENGINE_PROFILE_FUNCTION();
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    if (id_)
    {
        tracker_->retain(id_);
    }
#endif
}

void GpuMemoryAllocation::wait_for_readback() noexcept
{
    ENGINE_PROFILE_FUNCTION();
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    if (id_)
    {
        tracker_->wait_for_readback(id_);
    }
#endif
}

void GpuMemoryAllocation::readback_ready() noexcept
{
    ENGINE_PROFILE_FUNCTION();
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    if (id_)
    {
        tracker_->readback_ready(id_);
    }
#endif
}

void GpuMemoryAllocation::reset() noexcept
{
    ENGINE_PROFILE_FUNCTION();
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    if (id_)
    {
        tracker_->queue_destroy(id_);
        id_ = 0;
        tracker_ = nullptr;
    }
#endif
}

std::uint64_t GpuMemoryAllocation::id() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    return id_;
#else
    return 0;
#endif
}
} // namespace engine::render_detail
