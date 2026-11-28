#include "plugins/runtime/resource_graph/resource_graph_system.hpp"

#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"

#include "runtime/resource/resource_system.hpp"
#include "runtime/task/task_system.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::plugins::resource_graph
{
namespace
{
bool empty(const ResourceHandle& value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return value.owner == 0 && value.id == 0 && value.generation == 0;
}

using Key = std::pair<std::uint64_t, std::string>;

std::uint64_t monotonic_nanoseconds()
{
    ENGINE_PROFILE_FUNCTION();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

struct DependencyIndex
{
    std::map<std::string, std::string, std::less<>> paths;
    std::set<std::string, std::less<>> guids;
};

using MemoryEntries = std::map<std::string, schema::AssetEntryT, std::less<>>;

struct LoadPlan
{
    ResourceCatalog catalog;
    std::shared_ptr<const MemoryEntries> memory;
    std::string root;
    std::vector<std::string> order;
    std::shared_ptr<DependencyIndex> index = std::make_shared<DependencyIndex>();
    std::shared_future<bool> planning;
    std::size_t users = 0;
    std::atomic<bool> cancelled{false};
    ResourceError error = ResourceError::none;
    std::string message;

    const schema::AssetEntryT* find(std::string_view guid) const
    {
        ENGINE_PROFILE_FUNCTION();
        const auto local = memory->find(guid);
        return local == memory->end() ? catalog.find(guid) : &local->second;
    }

    bool execute()
    {
        ENGINE_PROFILE_FUNCTION();
        struct Cursor
        {
            const schema::AssetEntryT* entry;
            std::size_t next = 0;
        };
        const auto* entry = find(root);
        if (!entry)
        {
            error = ResourceError::not_found;
            message = "Unknown resource: " + root;
            return false;
        }
        std::map<std::string_view, unsigned> visited;
        std::vector<Cursor> stack{{entry}};
        visited.emplace(entry->guid, 1);
        while (!stack.empty())
        {
            if (cancelled.load(std::memory_order_relaxed))
            {
                error = ResourceError::cancelled;
                return false;
            }
            auto& cursor = stack.back();
            if (cursor.next == cursor.entry->dependencies.size())
            {
                if (order.size() == std::numeric_limits<std::uint32_t>::max())
                {
                    error = ResourceError::budget_exceeded;
                    message = "Resource closure is too large";
                    return false;
                }
                visited[cursor.entry->guid] = 2;
                order.push_back(cursor.entry->guid);
                if (cursor.entry->guid != root)
                {
                    index->guids.insert(cursor.entry->guid);
                    index->paths.emplace(cursor.entry->path, cursor.entry->guid);
                }
                stack.pop_back();
                continue;
            }
            const auto& guid = cursor.entry->dependencies[cursor.next++];
            const auto* child = find(guid);
            if (!child)
            {
                error = ResourceError::dependency_failed;
                message = "Missing declared dependency: " + guid;
                return false;
            }
            auto [mark, inserted] = visited.emplace(child->guid, 1);
            if (inserted)
            {
                stack.push_back({child});
            }
            else if (mark->second == 1)
            {
                error = ResourceError::dependency_failed;
                message = "Resource dependency cycle: " + guid;
                return false;
            }
        }
        return true;
    }
};
} // namespace

struct ResourceGraphSystem::State : std::enable_shared_from_this<State>
{
    using Phase = ResourceMemoryPhase;
    struct Record
    {
        Key key;
        std::uint64_t id = 0;
        schema::AssetEntryT info;
        Phase phase = Phase::queued;
        bool memory = false;
        bool admitted = false;
        bool queued = false;
        std::size_t edge_cursor = 0;
        std::uint32_t unresolved_dependencies = 0;
        std::vector<std::pair<Key, std::uint64_t>> waiting_parents;
        std::vector<std::uint64_t> subscribers;
        std::shared_ptr<const DependencyIndex> index;
        std::unique_ptr<Resource> value;
        std::unique_ptr<Resource> candidate;
        std::unique_ptr<ResourceRead> read;
        std::vector<Record*> dependencies;
        std::uint64_t acquisitions = 0;
        std::uint64_t dependency_references = 0;
        std::uint64_t request_references = 0;
        ResourceError error = ResourceError::none;
        std::string message;
    };
    struct Acquisition
    {
        Record* record = nullptr;
        ResourceMemoryOwner memory_owner;
    };
    struct LoadState
    {
        std::weak_ptr<State> owner;
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
        ResourceMemoryOwner memory_owner;
        ResourceLoadStatus status = ResourceLoadStatus::loading;
        ResourceLoadProgress progress;
        ResourceLoadProgress observed;
        ResourceLoadStatus observed_status = ResourceLoadStatus::loading;
        void observe()
        {
            ENGINE_PROFILE_FUNCTION();
            if (progress.completed == observed.completed && progress.total == observed.total &&
                progress.discovery_complete == observed.discovery_complete &&
                progress.in_flight == observed.in_flight && status == observed_status)
            {
                return;
            }
            ++progress.progress_revision;
            progress.last_progress_ns = monotonic_nanoseconds();
            observed = progress;
            observed_status = status;
        }
        ResourceError error = ResourceError::none;
        std::string message;
        ResourceHandle result;
        std::shared_ptr<LoadPlan> plan;
        bool planned = false;
        bool attached = false;
        std::size_t admission_cursor = 0;
        std::vector<Record*> pins;
        std::unordered_set<Record*> pinned;
    };

    ResourceSystem& resources;
    TaskSystem& tasks;
    ResourceGraphConfig config;
    const std::uint64_t owner;
    const std::uint64_t memory_started_ns = monotonic_nanoseconds();
    bool active = false;
    std::uint64_t generation = 0;
    std::uint64_t last_acquisition = 0;
    std::uint64_t last_request = 0;
    std::uint64_t last_record = 0;
    mutable std::uint64_t memory_sequence = 0;
    std::map<Key, std::unique_ptr<Record>> records;
    std::map<Key, Record*> paths;
    std::map<std::uint64_t, Acquisition> acquisitions;
    std::vector<std::shared_ptr<LoadState>> operations;
    std::map<std::uint64_t, std::weak_ptr<LoadState>> requests;
    std::map<Key, std::weak_ptr<LoadPlan>> plans;
    std::shared_ptr<const MemoryEntries> memory_snapshot;
    std::deque<std::pair<Key, std::uint64_t>> pending_records;
    std::deque<std::unique_ptr<Record>> retired_records;
    ResourceError error = ResourceError::none;
    std::string message;

    State(ResourceSystem& resources, TaskSystem& tasks, ResourceGraphConfig config,
          std::uint64_t owner)
        : resources(resources), tasks(tasks), config(config), owner(owner)
    {
        ENGINE_PROFILE_FUNCTION();
    }

    bool fail(ResourceError value, std::string text)
    {
        ENGINE_PROFILE_FUNCTION();
        error = value;
        message = std::move(text);
        return false;
    }
    bool success()
    {
        ENGINE_PROFILE_FUNCTION();
        error = ResourceError::none;
        message.clear();
        return true;
    }

    bool can_acquire()
    {
        ENGINE_PROFILE_FUNCTION();
        return last_acquisition < std::numeric_limits<std::uint64_t>::max() ||
               fail(ResourceError::budget_exceeded, "Resource acquisition identities exhausted");
    }
    bool add_count(std::uint64_t& count)
    {
        ENGINE_PROFILE_FUNCTION();
        if (count == std::numeric_limits<std::uint64_t>::max())
        {
            return fail(ResourceError::budget_exceeded, "Resource reference count overflow");
        }
        ++count;
        return true;
    }
    Record* lookup(const ResourceHandle& handle) const noexcept
    {
        ENGINE_PROFILE_FUNCTION();
        if (!active || handle.owner != owner || handle.generation != 1 || handle.id == 0)
        {
            return nullptr;
        }
        const auto found = acquisitions.find(handle.id);
        return found == acquisitions.end() ? nullptr : found->second.record;
    }

    void collect(Record* record, bool deferred = false) noexcept
    {
        ENGINE_PROFILE_FUNCTION();
        // Iterative reverse release avoids stack growth for deep resource closures.
        std::vector<Record*> pending{record};
        while (!pending.empty())
        {
            auto* item = pending.back();
            pending.pop_back();
            if (item->acquisitions || item->dependency_references || item->request_references)
            {
                continue;
            }
            if (item->read)
            {
                (void)item->read->cancel();
                item->read.reset();
            }
            auto dependencies = std::move(item->dependencies);
            paths.erase({item->key.first, item->info.path});
            if (item->memory)
            {
                memory_snapshot.reset();
            }
            // Destroy the item before withdrawing its dependency ownership.
            const auto key = item->key;
            if (deferred)
            {
                item->value.reset();
                item->candidate.reset();
                item->dependencies = std::move(dependencies);
                retired_records.push_back(std::move(records.find(key)->second));
                records.erase(key);
                continue;
            }
            records.erase(key);
            for (auto* dependency : dependencies)
            {
                --dependency->dependency_references;
                if (!dependency->dependency_references && !dependency->acquisitions &&
                    !dependency->request_references)
                {
                    pending.push_back(dependency);
                }
            }
        }
    }

    bool acquire(Record& record, ResourceHandle& output, ResourceMemoryOwner memory_owner = {})
    {
        ENGINE_PROFILE_FUNCTION();
        if (!empty(output))
        {
            return fail(ResourceError::busy, "Resource handle output must be empty");
        }
        if (!can_acquire() || !add_count(record.acquisitions))
        {
            return false;
        }
        const auto id = ++last_acquisition;
        acquisitions.emplace(id, Acquisition{&record, memory_owner});
        output = {owner, id, 1};
        return success();
    }

    bool release(ResourceHandle& handle) noexcept
    {
        ENGINE_PROFILE_FUNCTION();
        if (empty(handle))
        {
            return success();
        }
        auto* record = lookup(handle);
        const auto id = handle.id;
        handle = {};
        if (!record)
        {
            error = ResourceError::invalid_handle;
            message.clear();
            return false;
        }
        acquisitions.erase(id);
        --record->acquisitions;
        collect(record);
        return success();
    }

    void unpin(LoadState& operation) noexcept
    {
        ENGINE_PROFILE_FUNCTION();
        // Pins are independent counts, so each record remains alive until its own pin is removed.
        auto pins = std::move(operation.pins);
        for (auto* record : pins)
        {
            --record->request_references;
            collect(record);
        }
        operation.pinned.clear();
    }
    void detach_plan(LoadState& operation)
    {
        ENGINE_PROFILE_FUNCTION();
        if (operation.attached)
        {
            operation.attached = false;
            if (--operation.plan->users == 0)
            {
                operation.plan->cancelled.store(true, std::memory_order_relaxed);
            }
        }
    }
    void finish(LoadState& operation, ResourceError value, std::string text)
    {
        ENGINE_PROFILE_FUNCTION();
        operation.status = ResourceLoadStatus::failed;
        operation.observe();
        operation.error = value;
        operation.message = std::move(text);
        detach_plan(operation);
    }
    bool cancel(LoadState& operation) noexcept
    {
        ENGINE_PROFILE_FUNCTION();
        if (operation.status == ResourceLoadStatus::delivered ||
            operation.status == ResourceLoadStatus::failed)
        {
            return false;
        }
        if (operation.status == ResourceLoadStatus::cancelled)
        {
            return true;
        }
        detach_plan(operation);
        if (!empty(operation.result))
        {
            (void)release(operation.result);
        }
        operation.status = ResourceLoadStatus::cancelled;
        operation.progress.in_flight = 0;
        operation.observe();
        operation.error = ResourceError::cancelled;
        return true;
    }

    void refresh_generation()
    {
        ENGINE_PROFILE_FUNCTION();
        if (!active || generation == resources.generation())
        {
            return;
        }
        generation = resources.generation();
        memory_snapshot.reset();
        for (const auto& operation : operations)
        {
            if (operation->status == ResourceLoadStatus::loading)
            {
                finish(*operation, ResourceError::generation_changed,
                       "Resource catalog changed during loading");
            }
        }
    }
    bool command()
    {
        ENGINE_PROFILE_FUNCTION();
        if (!active || !resources.is_initialized() || !tasks.is_initialized())
        {
            return fail(ResourceError::unavailable, "Resource loader is unavailable");
        }
        refresh_generation();
        return success();
    }
    bool resolve(std::string_view path_or_guid, std::string& guid)
    {
        ENGINE_PROFILE_FUNCTION();
        const auto found = paths.find({generation, std::string(path_or_guid)});
        if (found != paths.end())
        {
            guid = found->second->info.guid;
            return true;
        }
        if (GuidMap::valid_guid(path_or_guid))
        {
            guid = path_or_guid;
            if (records.contains({generation, guid}) || resources.catalog().find(guid))
            {
                return true;
            }
        }
        else if (resources.catalog().identities().path_to_guid(path_or_guid, guid))
        {
            return true;
        }
        return fail(ResourceError::not_found, "Unknown resource: " + std::string(path_or_guid));
    }
    std::shared_ptr<LoadPlan> plan(std::string guid)
    {
        ENGINE_PROFILE_FUNCTION();
        if (!memory_snapshot)
        {
            auto snapshot = std::make_shared<MemoryEntries>();
            for (const auto& [key, record] : records)
            {
                if (key.first == generation && record->memory)
                {
                    snapshot->emplace(record->info.guid, record->info);
                }
            }
            memory_snapshot = std::move(snapshot);
        }
        const auto found = plans.find({generation, guid});
        if (found != plans.end())
        {
            const auto previous = found->second.lock();
            if (previous && previous->memory == memory_snapshot &&
                !previous->cancelled.load(std::memory_order_relaxed))
            {
                return previous;
            }
        }
        std::erase_if(plans, [](const auto& item)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            return item.second.expired();
        });
        auto result = std::make_shared<LoadPlan>();
        result->catalog = resources.catalog();
        result->root = std::move(guid);
        result->memory = memory_snapshot;
        plans[{generation, result->root}] = result;
        return result;
    }
    Record* make_record(const schema::AssetEntryT& info)
    {
        ENGINE_PROFILE_FUNCTION();
        const Key key{generation, info.guid};
        const auto existing = records.find(key);
        if (existing != records.end())
        {
            return existing->second.get();
        }
        if (last_record == std::numeric_limits<std::uint64_t>::max())
        {
            (void)fail(ResourceError::budget_exceeded, "Resource record identities exhausted");
            return nullptr;
        }
        auto item = std::make_unique<Record>();
        item->key = key;
        item->id = ++last_record;
        item->info = info;
        auto* record = item.get();
        records.emplace(key, std::move(item));
        paths.emplace(Key{generation, info.path}, record);
        return record;
    }
    bool pin(LoadState& operation, Record& record)
    {
        ENGINE_PROFILE_FUNCTION();
        if (operation.pinned.contains(&record))
        {
            return true;
        }
        if (!add_count(record.request_references))
        {
            return false;
        }
        operation.pins.push_back(&record);
        operation.pinned.insert(&record);
        if (record.phase == Phase::ready)
        {
            ++operation.progress.completed;
        }
        else if (record.phase == Phase::failed)
        {
            operation.error = record.error;
            operation.message = record.message;
        }
        else if (operation.id != 0)
        {
            record.subscribers.push_back(operation.id);
        }
        return true;
    }
    void schedule(Record& record)
    {
        ENGINE_PROFILE_FUNCTION();
        if (!record.queued && record.phase != Phase::ready && record.phase != Phase::failed)
        {
            record.queued = true;
            pending_records.emplace_back(record.key, record.id);
        }
    }
    void notify(Record& record)
    {
        ENGINE_PROFILE_FUNCTION();
        for (const auto& [key, id] : record.waiting_parents)
        {
            const auto found = records.find(key);
            if (found == records.end() || found->second->id != id)
            {
                continue;
            }
            auto& parent = *found->second;
            if (record.phase == Phase::failed)
            {
                parent.error = ResourceError::dependency_failed;
                parent.message = record.message;
            }
            else
            {
                --parent.unresolved_dependencies;
            }
            schedule(parent);
        }
        record.waiting_parents.clear();
        for (const auto id : record.subscribers)
        {
            const auto found = requests.find(id);
            const auto operation = found == requests.end() ? nullptr : found->second.lock();
            if (!operation || operation->status != ResourceLoadStatus::loading ||
                !operation->pinned.contains(&record))
            {
                continue;
            }
            if (record.phase == Phase::ready)
            {
                ++operation->progress.completed;
            }
            else
            {
                operation->error = record.error;
                operation->message = record.message;
            }
        }
        record.subscribers.clear();
    }
    bool publish(Record& record, std::unique_ptr<Resource>& candidate)
    {
        ENGINE_PROFILE_FUNCTION();
        std::vector<Record*> dependencies;
        std::vector<const Resource*> values;
        for (const auto& guid : record.info.dependencies)
        {
            const auto found = records.find({record.key.first, guid});
            if (found == records.end() || found->second->phase != Phase::ready)
            {
                return fail(ResourceError::not_ready, "Resource dependency is not ready");
            }
            dependencies.push_back(found->second.get());
            values.push_back(found->second->value.get());
            if (!record.admitted &&
                found->second->dependency_references == std::numeric_limits<std::uint64_t>::max())
            {
                return fail(ResourceError::budget_exceeded, "Dependency reference count overflow");
            }
        }
        ResourceError status;
        std::string detail;
        if (!resources.validate_dependencies(*candidate, values, status, detail))
        {
            return fail(status, std::move(detail));
        }
        if (!record.admitted)
        {
            for (auto* dependency : dependencies)
            {
                ++dependency->dependency_references;
            }
            record.dependencies = std::move(dependencies);
        }
        record.value = std::move(candidate);
        record.phase = Phase::ready;
        notify(record);
        return true;
    }
    void fail_record(Record& record, ResourceError value, std::string text)
    {
        ENGINE_PROFILE_FUNCTION();
        record.phase = Phase::failed;
        record.error = value;
        record.message = std::move(text);
        record.candidate.reset();
        record.read.reset();
        notify(record);
    }

    const DependencyIndex& dependency_index(Record& root)
    {
        ENGINE_PROFILE_FUNCTION();
        if (!root.index)
        {
            auto index = std::make_shared<DependencyIndex>();
            std::vector<const Record*> pending(root.dependencies.begin(), root.dependencies.end());
            while (!pending.empty())
            {
                const auto* record = pending.back();
                pending.pop_back();
                if (index->guids.insert(record->info.guid).second)
                {
                    index->paths.emplace(record->info.path, record->info.guid);
                    pending.insert(pending.end(), record->dependencies.begin(),
                                   record->dependencies.end());
                }
            }
            root.index = std::move(index);
        }
        return *root.index;
    }

    bool attach_plan(LoadState& operation)
    {
        ENGINE_PROFILE_FUNCTION();
        const auto work = operation.plan;
        if (!work->planning.valid())
        {
            work->planning =
                tasks.submit("ResourceGraph.plan", [work]()
                {
                    ENGINE_PROFILE_FUNCTION_PROBE();
                    return work->execute();
                }).share();
            if (!work->planning.valid())
            {
                return fail(ResourceError::busy, "Resource planning task domain is full");
            }
        }
        ++work->users;
        operation.attached = true;
        return true;
    }

    bool admit(Record& record, std::chrono::steady_clock::time_point deadline)
    {
        ENGINE_PROFILE_FUNCTION();
        if (record.phase == Phase::ready || record.phase == Phase::failed || record.admitted)
        {
            return true;
        }
        const auto reject = [&](ResourceError value, std::string text)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            record.error = value;
            record.message = std::move(text);
            schedule(record);
            return true;
        };
        while (record.edge_cursor < record.info.dependencies.size())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            const auto& guid = record.info.dependencies[record.edge_cursor];
            const auto found = records.find({record.key.first, guid});
            if (found == records.end())
            {
                return reject(ResourceError::dependency_failed,
                              "Planned resource dependency is missing");
            }
            auto& dependency = *found->second;
            if (!add_count(dependency.dependency_references))
            {
                return reject(error, message);
            }
            record.dependencies.push_back(&dependency);
            ++record.edge_cursor;
            if (dependency.phase == Phase::failed)
            {
                return reject(ResourceError::dependency_failed, dependency.message);
            }
            if (dependency.phase != Phase::ready)
            {
                ++record.unresolved_dependencies;
                dependency.waiting_parents.emplace_back(record.key, record.id);
            }
        }
        record.admitted = true;
        schedule(record);
        return true;
    }

    void cleanup(std::chrono::steady_clock::time_point deadline)
    {
        ENGINE_PROFILE_FUNCTION();
        for (const auto& operation : operations)
        {
            if (operation->status == ResourceLoadStatus::loading)
            {
                continue;
            }
            while (!operation->pins.empty() && std::chrono::steady_clock::now() < deadline)
            {
                auto* record = operation->pins.back();
                operation->pins.pop_back();
                operation->pinned.erase(record);
                --record->request_references;
                collect(record, true);
            }
            if (operation->pins.empty())
            {
                operation->plan.reset();
            }
        }
        while (!retired_records.empty() && std::chrono::steady_clock::now() < deadline)
        {
            auto& record = retired_records.front();
            if (record->dependencies.empty())
            {
                retired_records.pop_front();
                continue;
            }
            auto* dependency = record->dependencies.back();
            record->dependencies.pop_back();
            --dependency->dependency_references;
            collect(dependency, true);
        }
        std::erase_if(operations,
                      [this](const auto& operation)
                      {
                          ENGINE_PROFILE_FUNCTION_PROBE();
                          if (operation->status == ResourceLoadStatus::loading ||
                              operation->status == ResourceLoadStatus::ready ||
                              !operation->pins.empty())
                          {
                              return false;
                          }
                          requests.erase(operation->id);
                          return true;
                      });
    }

    bool memory_inputs(const Resource& candidate, std::span<const ResourceHandle> handles,
                       std::vector<Record*>& dependencies)
    {
        ENGINE_PROFILE_FUNCTION();
        const auto& info = candidate.info();
        if (!GuidMap::valid_guid(info.guid) || !GuidMap::valid_path(info.path) ||
            info.kind > schema::AssetKind::MAX ||
            info.path.starts_with("editor://") || info.location || info.kind != candidate.kind())
        {
            return fail(ResourceError::invalid_data, "Invalid memory resource identity");
        }
        if (records.contains({generation, info.guid}) || paths.contains({generation, info.path}))
        {
            return fail(ResourceError::identity_conflict,
                        "Resource identity is already loaded or pending");
        }
        std::set<std::string_view> unique;
        for (const auto& handle : handles)
        {
            auto* dependency = lookup(handle);
            if (!dependency)
            {
                return fail(ResourceError::invalid_handle, "Invalid memory dependency handle");
            }
            if (dependency->key.first != generation)
            {
                return fail(ResourceError::generation_changed,
                            "Memory dependencies belong to another generation");
            }
            if (!unique.insert(dependency->info.guid).second ||
                std::find(info.dependencies.begin(), info.dependencies.end(),
                          dependency->info.guid) == info.dependencies.end())
            {
                return fail(ResourceError::dependency_failed,
                            "Duplicate or undeclared memory dependency");
            }
            dependencies.push_back(dependency);
        }
        if (handles.size() != info.dependencies.size())
        {
            return fail(ResourceError::dependency_failed,
                        "Memory candidate requires every direct dependency");
        }
        return true;
    }

    bool begin(std::string_view input, std::unique_ptr<ResourceLoad>& output,
               ResourceMemoryOwner memory_owner);
    bool begin_memory(std::unique_ptr<Resource>& candidate, std::span<const ResourceHandle> handles,
                      std::unique_ptr<ResourceLoad>& output);
    void advance();
    bool observe_memory(const ResourceMemoryQuery& query, ResourceMemorySnapshot& output) const;
};

bool ResourceGraphSystem::State::observe_memory(const ResourceMemoryQuery& query,
                                                ResourceMemorySnapshot& output) const
{
    ENGINE_PROFILE_ZONE("ResourceGraph.memorySnapshot");
    output = {};
    if (!active || memory_sequence == std::numeric_limits<std::uint64_t>::max() ||
        query.limit == 0 || query.limit > ResourceMemoryQuery::maximum_items ||
        (!query.guid.empty() && !GuidMap::valid_guid(query.guid)))
    {
        return false;
    }

    struct RankedRecord
    {
        const Record* record;
        Phase phase;
        std::uint64_t stored_bytes;
        std::optional<std::uint64_t> cpu_bytes;
        bool has_payload;
    };
    std::vector<RankedRecord> ranked;
    const auto measure = [&](const Record& record, Phase phase)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        const auto* payload = record.value ? record.value.get() : record.candidate.get();
        const auto capacity = payload ? payload->cpu_payload_capacity() : std::nullopt;
        auto& kind = output.kinds[static_cast<std::size_t>(record.info.kind)];
        if (capacity)
        {
            ++output.measured_payloads;
            output.measured_cpu_payload_bytes += *capacity;
            ++kind.measured_payloads;
            kind.measured_cpu_payload_bytes += *capacity;
        }
        else if (payload)
        {
            ++output.unmeasured_payloads;
            ++kind.unmeasured_payloads;
        }
        if (query.guid.empty() || query.guid == record.info.guid)
        {
            ranked.push_back({&record, phase, record.info.content_size - record.info.header_size,
                              capacity, payload != nullptr});
        }
    };
    for (const auto& [key, record] : records)
    {
        (void)key;
        measure(*record, record->phase);
    }
    for (const auto& record : retired_records)
    {
        measure(*record, Phase::retiring);
    }
    output.matching_entries = ranked.size();
    const auto items = std::min<std::size_t>(ranked.size(), query.limit);
    std::partial_sort(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(items),
                      ranked.end(),
                      [](const RankedRecord& left, const RankedRecord& right)
                      {
                          ENGINE_PROFILE_FUNCTION_PROBE();
                          const auto left_bytes = left.cpu_bytes.value_or(left.stored_bytes);
                          const auto right_bytes = right.cpu_bytes.value_or(right.stored_bytes);
                          return left_bytes != right_bytes ? left_bytes > right_bytes
                                                           : left.record->id < right.record->id;
                      });
    ranked.resize(items);

    std::unordered_map<const Record*, std::size_t> selected;
    selected.reserve(items);
    output.entries.reserve(items);
    for (const auto& row : ranked)
    {
        const auto& record = *row.record;
        selected.emplace(&record, output.entries.size());
        output.entries.push_back({
            .record_id = record.id,
            .catalog_generation = record.key.first,
            .guid = record.info.guid,
            .path = record.info.path,
            .kind = record.info.kind,
            .phase = row.phase,
            .stored_payload_bytes = row.stored_bytes,
            .cpu_payload_bytes = row.cpu_bytes,
            .has_payload = row.has_payload,
            .holders_truncated = false,
            .acquisitions = record.acquisitions,
            .dependency_references = record.dependency_references,
            .request_references = record.request_references,
            .holders = {},
        });
    }
    const auto holder = [&](const Record* record, ResourceMemoryHolderKind kind, std::uint64_t id,
                            ResourceMemoryOwner memory_owner, std::string_view guid, bool pending)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        const auto found = selected.find(record);
        if (found == selected.end())
        {
            return;
        }
        auto& row = output.entries[found->second];
        if (row.holders.size() == ResourceMemoryQuery::maximum_holders_per_item)
        {
            row.holders_truncated = true;
            return;
        }
        row.holders.push_back({kind, id, memory_owner, std::string(guid), pending});
    };
    for (const auto& [id, acquisition] : acquisitions)
    {
        holder(acquisition.record, ResourceMemoryHolderKind::acquisition, id,
               acquisition.memory_owner, {}, false);
    }
    const auto add_dependencies = [&](const Record& parent, bool retiring)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        for (const auto* dependency : parent.dependencies)
        {
            holder(dependency, ResourceMemoryHolderKind::dependency, parent.id, {},
                   parent.info.guid, retiring);
        }
    };
    for (const auto& [key, record] : records)
    {
        (void)key;
        add_dependencies(*record, false);
    }
    for (const auto& record : retired_records)
    {
        add_dependencies(*record, true);
    }
    for (const auto& operation : operations)
    {
        const std::string_view root = operation->plan ? operation->plan->root : std::string_view{};
        for (const auto* record : operation->pins)
        {
            holder(record, ResourceMemoryHolderKind::load_request, operation->id,
                   operation->memory_owner, root, operation->status != ResourceLoadStatus::loading);
        }
    }
    output.loader_id = owner;
    output.session_started_ns = memory_started_ns;
    output.sequence = ++memory_sequence;
    output.catalog_generation = generation;
    output.timestamp_ns = monotonic_nanoseconds();
    return true;
}

class GraphResourceLoad final : public ResourceLoad
{
    using State = ResourceGraphSystem::State;

public:
    explicit GraphResourceLoad(std::shared_ptr<State::LoadState> state) : state_(std::move(state))
    {
        ENGINE_PROFILE_FUNCTION();
    }
    ~GraphResourceLoad() noexcept override
    {
        ENGINE_PROFILE_FUNCTION();
        (void)cancel();
    }
    ResourceLoadStatus status(ResourceLoadProgress& output) const noexcept override
    {
        ENGINE_PROFILE_FUNCTION();
        output = state_->progress;
        return state_->status;
    }
    bool take(ResourceHandle& output) override
    {
        ENGINE_PROFILE_FUNCTION();
        if (!empty(output) || state_->status != ResourceLoadStatus::ready)
        {
            return false;
        }
        const auto owner = state_->owner.lock();
        if (!owner || !owner->lookup(state_->result))
        {
            return false;
        }
        output = std::exchange(state_->result, {});
        state_->status = ResourceLoadStatus::delivered;
        state_->observe();
        return true;
    }
    bool cancel() noexcept override
    {
        ENGINE_PROFILE_FUNCTION();
        const auto owner = state_->owner.lock();
        if (owner)
        {
            return owner->cancel(*state_);
        }
        return state_->status == ResourceLoadStatus::cancelled;
    }
    ResourceError error() const noexcept override
    {
        ENGINE_PROFILE_FUNCTION();
        return state_->error;
    }
    std::string_view last_error() const noexcept override
    {
        ENGINE_PROFILE_FUNCTION();
        return state_->error == ResourceError::cancelled
                   ? std::string_view("Resource load cancelled")
                   : std::string_view(state_->message);
    }

private:
    std::shared_ptr<State::LoadState> state_;
};

bool ResourceGraphSystem::State::begin(std::string_view input,
                                       std::unique_ptr<ResourceLoad>& output,
                                       ResourceMemoryOwner memory_owner)
{
    ENGINE_PROFILE_FUNCTION();
    if (!command())
    {
        return false;
    }
    if (output)
    {
        return fail(ResourceError::busy, "Resource operation output must be empty");
    }
    const auto count = std::count_if(operations.begin(), operations.end(),
                                     [](const auto& op)
                                     {
                                         ENGINE_PROFILE_FUNCTION_PROBE();
                                         return op->status == ResourceLoadStatus::loading ||
                                                op->status == ResourceLoadStatus::ready;
                                     });
    if (count >= config.max_requests || last_request == std::numeric_limits<std::uint64_t>::max())
    {
        return fail(ResourceError::busy, "Resource request budget or identities exhausted");
    }
    std::string guid;
    if (!resolve(input, guid))
    {
        return false;
    }
    auto operation = std::make_shared<LoadState>();
    operation->owner = shared_from_this();
    operation->id = ++last_request;
    operation->generation = generation;
    operation->memory_owner = memory_owner;
    const auto existing = records.find({generation, guid});
    if (existing != records.end() && existing->second->phase == Phase::ready)
    {
        auto& root = *existing->second;
        const auto& index = dependency_index(root);
        if (index.guids.size() >= std::numeric_limits<std::uint32_t>::max())
        {
            return fail(ResourceError::budget_exceeded, "Resource progress count is exhausted");
        }
        if (!acquire(root, operation->result, operation->memory_owner))
        {
            return false;
        }
        operation->progress.completed = operation->progress.total =
            static_cast<std::uint32_t>(index.guids.size() + 1);
        operation->status = ResourceLoadStatus::ready;
        operation->progress.discovery_complete = true;
        operation->observe();
    }
    else
    {
        operation->plan = plan(guid);
        if (!attach_plan(*operation))
        {
            return false;
        }
        auto* record = existing != records.end() ? existing->second.get()
                                                 : make_record(*operation->plan->find(guid));
        if (!record || !pin(*operation, *record))
        {
            detach_plan(*operation);
            if (record)
            {
                collect(record);
            }
            return false;
        }
    }
    requests.emplace(operation->id, operation);
    operations.push_back(operation);
    output = std::make_unique<GraphResourceLoad>(operation);
    return success();
}

bool ResourceGraphSystem::State::begin_memory(std::unique_ptr<Resource>& candidate,
                                              std::span<const ResourceHandle> handles,
                                              std::unique_ptr<ResourceLoad>& output)
{
    ENGINE_PROFILE_FUNCTION();
    if (!command())
    {
        return false;
    }
    if (output)
    {
        return fail(ResourceError::busy, "Resource operation output must be empty");
    }
    if (!candidate)
    {
        return fail(ResourceError::invalid_data, "Memory resource is empty");
    }
    const auto count = std::count_if(operations.begin(), operations.end(),
                                     [](const auto& op)
                                     {
                                         ENGINE_PROFILE_FUNCTION_PROBE();
                                         return op->status == ResourceLoadStatus::loading ||
                                                op->status == ResourceLoadStatus::ready;
                                     });
    if (count >= config.max_requests || last_request == std::numeric_limits<std::uint64_t>::max())
    {
        return fail(ResourceError::busy, "Resource request budget or identities exhausted");
    }
    std::vector<Record*> dependencies;
    if (!memory_inputs(*candidate, handles, dependencies))
    {
        return false;
    }
    auto operation = std::make_shared<LoadState>();
    operation->owner = shared_from_this();
    operation->id = ++last_request;
    operation->generation = generation;
    operation->plan = plan(candidate->info().guid);
    auto memory = std::make_shared<MemoryEntries>(*operation->plan->memory);
    memory->emplace(candidate->info().guid, candidate->info());
    operation->plan->memory = memory;
    if (!attach_plan(*operation))
    {
        return false;
    }
    for (auto* dependency : dependencies)
    {
        if (!pin(*operation, *dependency))
        {
            detach_plan(*operation);
            unpin(*operation);
            return false;
        }
    }
    auto* record = make_record(candidate->info());
    if (!record || !pin(*operation, *record))
    {
        detach_plan(*operation);
        unpin(*operation);
        if (record)
        {
            collect(record);
        }
        return false;
    }
    record->memory = true;
    record->candidate = std::move(candidate);
    memory_snapshot = std::move(memory);
    requests.emplace(operation->id, operation);
    operations.push_back(operation);
    output = std::make_unique<GraphResourceLoad>(operation);
    return success();
}

void ResourceGraphSystem::State::advance()
{
    ENGINE_PROFILE_FUNCTION();
    refresh_generation();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(config.coordination_budget_seconds));
    std::uint32_t completions = config.completion_budget;
    // Reclaim cancelled work first so it cannot indefinitely occupy the read window.
    cleanup(deadline);
    for (const auto& operation : operations)
    {
        if (completions == 0 || std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }
        if (operation->status != ResourceLoadStatus::loading || operation->planned)
        {
            continue;
        }
        auto& work = *operation->plan;
        if (work.planning.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            continue;
        }
        if (!work.planning.get())
        {
            finish(*operation, work.error, work.message);
            --completions;
            continue;
        }
        operation->progress.total = static_cast<std::uint32_t>(work.order.size());
        operation->progress.discovery_complete = true;
        while (operation->admission_cursor < work.order.size() &&
               std::chrono::steady_clock::now() < deadline)
        {
            const auto& guid = work.order[operation->admission_cursor];
            auto* record = make_record(*work.find(guid));
            if (!record || !pin(*operation, *record))
            {
                if (record)
                {
                    collect(record, true);
                }
                finish(*operation, error, message);
                --completions;
                break;
            }
            if (operation->error != ResourceError::none)
            {
                finish(*operation, operation->error, operation->message);
                --completions;
                break;
            }
            if (!admit(*record, deadline))
            {
                break;
            }
            ++operation->admission_cursor;
        }
        if (operation->status == ResourceLoadStatus::loading &&
            operation->admission_cursor == work.order.size())
        {
            operation->planned = true;
            records.find({operation->generation, work.root})->second->index = work.index;
        }
    }

    // Only pending records are visited. No record scans the request list or a pin array.
    auto remaining = pending_records.size();
    while (remaining-- && !pending_records.empty() && completions != 0 &&
           std::chrono::steady_clock::now() < deadline)
    {
        const auto [key, identity] = std::move(pending_records.front());
        pending_records.pop_front();
        const auto found = records.find(key);
        if (found == records.end() || found->second->id != identity)
        {
            continue;
        }
        auto& record = *found->second;
        record.queued = false;
        if (key.first != generation || record.phase == Phase::ready ||
            record.phase == Phase::failed)
        {
            continue;
        }
        if (record.error != ResourceError::none)
        {
            fail_record(record, record.error, record.message);
            --completions;
            continue;
        }
        if (!record.admitted)
        {
            continue;
        }
        if (record.phase == Phase::queued)
        {
            const bool accepted = record.memory
                                      ? resources.begin_read_memory(record.candidate, record.read)
                                      : resources.begin_read(record.info.guid, record.read);
            if (!accepted)
            {
                if (resources.error() == ResourceError::busy)
                {
                    schedule(record);
                }
                else
                {
                    fail_record(record, resources.error(), std::string(resources.last_error()));
                    --completions;
                }
                continue;
            }
            record.phase = Phase::reading;
        }
        if (record.phase == Phase::reading)
        {
            if (record.read->status() == ResourceReadStatus::reading)
            {
                schedule(record);
                continue;
            }
            if (record.read->status() != ResourceReadStatus::ready ||
                !record.read->take(record.candidate))
            {
                fail_record(record, record.read->error(), std::string(record.read->last_error()));
                --completions;
                continue;
            }
            record.read.reset();
            record.phase = Phase::decoded;
        }
        if (record.phase == Phase::decoded && record.unresolved_dependencies == 0)
        {
            if (!publish(record, record.candidate))
            {
                fail_record(record, error, message);
            }
            --completions;
        }
    }

    for (const auto& operation : operations)
    {
        if (completions == 0 || std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }
        if (operation->status != ResourceLoadStatus::loading)
        {
            continue;
        }
        if (operation->error != ResourceError::none)
        {
            finish(*operation, operation->error, operation->message);
            --completions;
        }
        else if (operation->planned && operation->progress.completed == operation->progress.total)
        {
            const auto root = records.find({operation->generation, operation->plan->root});
            if (root == records.end())
            {
                finish(*operation, ResourceError::not_loaded, "Loaded root record is missing");
            }
            else if (!acquire(*root->second, operation->result, operation->memory_owner))
            {
                finish(*operation, error, message);
            }
            else
            {
                operation->status = ResourceLoadStatus::ready;
                operation->progress.discovery_complete = true;
                operation->observe();
                detach_plan(*operation);
            }
            --completions;
        }
    }
    for (const auto& operation : operations)
    {
        operation->progress.in_flight = 0;
        if (operation->status == ResourceLoadStatus::loading)
        {
            for (const auto* pin : operation->pins)
            {
                operation->progress.in_flight += pin->phase == Phase::reading;
            }
        }
        operation->observe();
    }
    cleanup(deadline);
}

ResourceGraphSystem::ResourceGraphSystem(ResourceSystem& resources, TaskSystem& tasks,
                                         const ResourceGraphConfig& config)
    : state_(std::make_shared<State>(resources, tasks, config, owner_id()))
{
    ENGINE_PROFILE_FUNCTION();
}
ResourceGraphSystem::~ResourceGraphSystem() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    shutdown();
}

bool ResourceGraphSystem::on_initialize(const EngineConfig&)
{
    ENGINE_PROFILE_FUNCTION();
    auto& state = *state_;
    if (state.owner == 0 || !state.resources.is_initialized() || !state.tasks.is_initialized() ||
        state.config.max_requests == 0 || state.config.completion_budget == 0 ||
        !std::isfinite(state.config.coordination_budget_seconds) ||
        state.config.coordination_budget_seconds <= 0)
    {
        return state.fail(ResourceError::unavailable, "Invalid resource graph services or budgets");
    }
    state.active = true;
    state.generation = state.resources.generation();
    return state.success();
}
bool ResourceGraphSystem::on_tick(double)
{
    ENGINE_PROFILE_ZONE("ResourceGraph.tick");
    if (!state_->command())
    {
        return false;
    }
    state_->advance();
    ENGINE_PROFILE_PLOT("ResourceGraph.records", static_cast<std::int64_t>(state_->records.size()));
    ENGINE_PROFILE_PLOT("ResourceGraph.acquisitions",
                        static_cast<std::int64_t>(state_->acquisitions.size()));
    ENGINE_PROFILE_PLOT("ResourceGraph.requests",
                        static_cast<std::int64_t>(state_->requests.size()));
    return true;
}
void ResourceGraphSystem::on_shutdown() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    auto& state = *state_;
    for (const auto& operation : state.operations)
    {
        (void)state.cancel(*operation);
    }
    state.cleanup(std::chrono::steady_clock::time_point::max());
    state.operations.clear();
    state.requests.clear();
    while (!state.acquisitions.empty())
    {
        ResourceHandle handle{state.owner, state.acquisitions.begin()->first, 1};
        (void)state.release(handle);
    }
    state.active = false;
    (void)state.tasks.wait();
    state.paths.clear();
    state.records.clear();
    state.pending_records.clear();
    state.retired_records.clear();
    state.plans.clear();
    state.memory_snapshot.reset();
}

bool ResourceGraphSystem::load(std::string_view input, ResourceHandle& output)
{
    ENGINE_PROFILE_FUNCTION();
    auto& state = *state_;
    if (!state.command())
    {
        return false;
    }
    if (!empty(output))
    {
        return state.fail(ResourceError::busy, "Resource handle output must be empty");
    }
    if (!state.can_acquire())
    {
        return false;
    }
    std::string guid;
    if (!state.resolve(input, guid))
    {
        return false;
    }
    const auto current = state.records.find({state.generation, guid});
    if (current != state.records.end() && current->second->phase == State::Phase::ready)
    {
        return state.acquire(*current->second, output);
    }
    if (current != state.records.end())
    {
        return state.fail(ResourceError::busy, "Resource has asynchronous work in flight");
    }
    const auto plan = state.plan(guid);
    if (!plan->execute())
    {
        return state.fail(plan->error, plan->message);
    }
    for (const auto& identity : plan->order)
    {
        const auto existing = state.records.find({state.generation, identity});
        if (existing != state.records.end() && existing->second->phase != State::Phase::ready)
        {
            return state.fail(ResourceError::busy,
                              "Required resource has asynchronous work in flight");
        }
    }
    State::LoadState scope;
    for (const auto& identity : plan->order)
    {
        auto* record = state.make_record(*plan->find(identity));
        if (!record || !state.pin(scope, *record))
        {
            if (record)
            {
                state.collect(record);
            }
            state.unpin(scope);
            return false;
        }
        if (record->phase == State::Phase::ready)
        {
            continue;
        }
        std::unique_ptr<Resource> value;
        if (!state.resources.read(identity, value))
        {
            state.fail(state.resources.error(), std::string(state.resources.last_error()));
            state.unpin(scope);
            return false;
        }
        if (!state.publish(*record, value))
        {
            state.unpin(scope);
            return false;
        }
    }
    const auto root = state.records.find({state.generation, guid});
    if (root != state.records.end())
    {
        root->second->index = plan->index;
    }
    const bool success = root != state.records.end() ? state.acquire(*root->second, output)
                                                     : state.fail(ResourceError::not_loaded,
                                                                  "Loaded root record is missing");
    state.unpin(scope);
    return success;
}
bool ResourceGraphSystem::begin_load(std::string_view input, std::unique_ptr<ResourceLoad>& output,
                                     ResourceMemoryOwner memory_owner)
{
    ENGINE_PROFILE_FUNCTION();
    return state_->begin(input, output, memory_owner);
}

bool ResourceGraphSystem::load_memory(std::unique_ptr<Resource>& candidate,
                                      std::span<const ResourceHandle> handles,
                                      ResourceHandle& output)
{
    ENGINE_PROFILE_FUNCTION();
    auto& state = *state_;
    if (!state.command())
    {
        return false;
    }
    if (!empty(output))
    {
        return state.fail(ResourceError::busy, "Resource handle output must be empty");
    }
    if (!candidate)
    {
        return state.fail(ResourceError::invalid_data, "Memory resource is empty");
    }
    if (!state.can_acquire())
    {
        return false;
    }
    std::vector<State::Record*> dependencies;
    if (!state.memory_inputs(*candidate, handles, dependencies))
    {
        return false;
    }
    if (!state.resources.validate_memory(*candidate))
    {
        return state.fail(state.resources.error(), std::string(state.resources.last_error()));
    }
    auto* record = state.make_record(candidate->info());
    if (!record)
    {
        return false;
    }
    record->memory = true;
    state.memory_snapshot.reset();
    if (!state.publish(*record, candidate))
    {
        state.collect(record);
        return false;
    }
    const bool success = state.acquire(*record, output);
    if (!success)
    {
        candidate = std::move(record->value);
        state.collect(record);
    }
    return success;
}
bool ResourceGraphSystem::begin_load_memory(std::unique_ptr<Resource>& candidate,
                                            std::span<const ResourceHandle> handles,
                                            std::unique_ptr<ResourceLoad>& output)
{
    ENGINE_PROFILE_FUNCTION();
    return state_->begin_memory(candidate, handles, output);
}

bool ResourceGraphSystem::acquire_loaded(std::string_view input, ResourceHandle& output)
{
    ENGINE_PROFILE_FUNCTION();
    auto& state = *state_;
    if (!state.command())
    {
        return false;
    }
    if (!empty(output))
    {
        return state.fail(ResourceError::busy, "Resource handle output must be empty");
    }
    std::string guid;
    if (!state.resolve(input, guid))
    {
        return false;
    }
    const auto found = state.records.find({state.generation, guid});
    if (found == state.records.end())
    {
        return state.fail(ResourceError::not_loaded, "Resource has not been loaded: " + guid);
    }
    if (found->second->phase != State::Phase::ready)
    {
        return state.fail(ResourceError::not_ready, "Resource is not ready: " + guid);
    }
    return state.acquire(*found->second, output);
}
bool ResourceGraphSystem::retain(const ResourceHandle& handle, ResourceHandle& output)
{
    ENGINE_PROFILE_FUNCTION();
    if (!state_->command())
    {
        return false;
    }
    auto* record = state_->lookup(handle);
    if (!record)
    {
        return state_->fail(ResourceError::invalid_handle, "Invalid resource acquisition");
    }
    return state_->acquire(*record, output);
}
bool ResourceGraphSystem::retain_dependency(const ResourceHandle& handle,
                                            std::string_view path_or_guid, ResourceHandle& output)
{
    ENGINE_PROFILE_FUNCTION();
    if (!state_->command())
    {
        return false;
    }
    if (!empty(output))
    {
        return state_->fail(ResourceError::busy, "Resource handle output must be empty");
    }
    auto* root = state_->lookup(handle);
    if (!root)
    {
        return state_->fail(ResourceError::invalid_handle, "Invalid resource owner acquisition");
    }
    const auto& index = state_->dependency_index(*root);
    std::string_view guid = path_or_guid;
    if (!GuidMap::valid_guid(guid))
    {
        const auto path = index.paths.find(path_or_guid);
        if (path == index.paths.end())
        {
            return state_->fail(ResourceError::not_found,
                                "Dependency path is absent from the owner's loaded closure");
        }
        guid = path->second;
    }
    if (index.guids.contains(guid))
    {
        const auto found = state_->records.find({root->key.first, std::string(guid)});
        if (found != state_->records.end() && found->second->phase == State::Phase::ready)
        {
            return state_->acquire(*found->second, output);
        }
    }
    return state_->fail(ResourceError::not_found,
                        "Dependency is absent from the owner's loaded closure");
}
bool ResourceGraphSystem::release(ResourceHandle& handle) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    state_->refresh_generation();
    return state_->release(handle);
}
ResourceError ResourceGraphSystem::get(const ResourceHandle& handle,
                                       const Resource*& output) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    output = nullptr;
    if (empty(handle))
    {
        return ResourceError::not_loaded;
    }
    const auto* record = state_->lookup(handle);
    if (!record)
    {
        return ResourceError::invalid_handle;
    }
    output = record->value.get();
    return ResourceError::none;
}
ResourceError ResourceGraphSystem::error() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return state_->error;
}
std::string_view ResourceGraphSystem::last_error() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (state_->error == ResourceError::invalid_handle && state_->message.empty())
    {
        return "Invalid resource acquisition";
    }
    return state_->message;
}

void ResourceGraphSystem::set_memory_owner(const ResourceHandle& handle,
                                            ResourceMemoryOwner owner) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (state_->lookup(handle))
    {
        state_->acquisitions.find(handle.id)->second.memory_owner = owner;
    }
}

bool ResourceGraphSystem::memory_snapshot(const ResourceMemoryQuery& query,
                                          ResourceMemorySnapshot& output) const
{
    ENGINE_PROFILE_FUNCTION();
    return state_->observe_memory(query, output);
}
} // namespace engine::plugins::resource_graph
