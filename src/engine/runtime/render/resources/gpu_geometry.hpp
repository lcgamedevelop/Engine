#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/render/gpu_memory_tracker.hpp"
#include "runtime/resource/resource_reference.hpp"
#include "runtime/resource/types/texture_resource.hpp"

#include "runtime/render/resources/gpu_vertex.hpp"
#include "runtime/schema/mesh_generated.h"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace engine
{
class MeshRenderer;
class Resource;
} // namespace engine

namespace engine::render_detail
{

// 不可变源几何由绘制实例共享；资源强引用区分相同 GUID 的不同发布版本。
struct GpuGeometry
{
    struct StaticVertexPage
    {
        static constexpr std::uint32_t capacity = 65536;
        explicit StaticVertexPage(bool white_color) : white(white_color)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
        }
        ~StaticVertexPage() noexcept;
        StaticVertexPage(const StaticVertexPage&) = delete;
        StaticVertexPage& operator=(const StaticVertexPage&) = delete;

        [[nodiscard]] bool create(GpuMemoryTracker* tracker, SceneScopeId scope,
                                  std::uint64_t scene);
        [[nodiscard]] bool store(std::span<const GpuVertex> vertices, std::uint32_t& first);
        void release_range(std::uint32_t first, std::uint32_t count) noexcept;
        [[nodiscard]] std::uint64_t byte_size() const noexcept;

        const bool white;
        bgfx::DynamicVertexBufferHandle positions = BGFX_INVALID_HANDLE;
        bgfx::DynamicVertexBufferHandle attributes = BGFX_INVALID_HANDLE;
        [[no_unique_address]] GpuMemoryAllocation memory;

    private:
        struct Range
        {
            std::uint32_t first, count;
        };
        std::vector<Range> free_ranges_{{0, capacity}};
        std::uint16_t attribute_stride_ = 0;
    };
    using VertexPages = std::vector<std::weak_ptr<StaticVertexPage>>;

    GpuGeometry() = default;
    ~GpuGeometry() noexcept;
    GpuGeometry(const GpuGeometry&) = delete;
    GpuGeometry& operator=(const GpuGeometry&) = delete;

    [[nodiscard]] bool create(const MeshRenderer& renderer, VertexPages* pages,
                              GpuMemoryTracker* tracker, SceneScopeId scope, std::uint64_t scene);
    [[nodiscard]] bool create_base_indices(std::span<const std::uint32_t> source);
    [[nodiscard]] bool skinning_bounds(std::span<const std::array<float, 16>> palette,
                                       std::array<float, 3>& minimum,
                                       std::array<float, 3>& maximum) const noexcept;
    [[nodiscard]] std::uint32_t resource_count() const noexcept;
    [[nodiscard]] bgfx::IndexBufferHandle index_buffer(std::size_t level = 0) const noexcept;
    [[nodiscard]] std::uint32_t index_count(std::size_t level = 0) const noexcept;
    [[nodiscard]] bool index_uses_16_bits(std::size_t level, bool meshlet = false) const noexcept;
    [[nodiscard]] std::uint64_t index_buffer_bytes() const noexcept;
    [[nodiscard]] bool vertices_ready() const noexcept;
    void bind_vertices(bool positions_only = false, bool whole_page = false) const;
    void bind_position_buffer(std::uint8_t stage) const;
    [[nodiscard]] std::uint16_t position_buffer_index() const noexcept;
    [[nodiscard]] std::uint32_t first_vertex() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return first_vertex_;
    }
    [[nodiscard]] const StaticVertexPage* vertex_page() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return page_.get();
    }
    [[nodiscard]] std::uint16_t position_stride_bytes() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return page_ || bgfx::isValid(attributes_layout_) ? sizeof(GpuVertex::position)
                                                          : vertex_stride_;
    }
    [[nodiscard]] std::uint64_t vertex_buffer_bytes() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return page_ ? 0 : std::uint64_t{vertex_count} * vertex_stride_ + vertex_padding_;
    }
    [[nodiscard]] std::span<const schema::MeshSubmesh>
    submeshes(std::size_t level = 0) const noexcept;
    [[nodiscard]] std::size_t lod_count() const noexcept;
    [[nodiscard]] bool meshlets_ready(bool debug) const noexcept;
    [[nodiscard]] bool prepare_meshlets(bool debug);
    [[nodiscard]] std::uint64_t meshlet_upload_bytes(bool debug) const noexcept;
    void release_meshlet_debug() noexcept;
    void release_meshlets() noexcept;
    [[nodiscard]] bgfx::IndexBufferHandle meshlet_indices(std::size_t level) const noexcept;
    [[nodiscard]] bgfx::VertexBufferHandle meshlet_debug_offsets(std::size_t level) const noexcept;
    void release() noexcept;
    [[nodiscard]] static bgfx::VertexLayout vertex_layout();
    [[nodiscard]] static bgfx::VertexLayout vector_layout();
    [[nodiscard]] static bool
    pack_vertices(const schema::MeshDataT& data, std::span<const schema::MeshVertex> positions,
                  std::span<const schema::Vec4> tangents, std::vector<GpuVertex>& packed,
                  std::array<float, 3>& minimum, std::array<float, 3>& maximum);

    ResourceReference resource;
    bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle influences = BGFX_INVALID_HANDLE;
    std::uint32_t vertex_count = 0;
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};

private:
    [[nodiscard]] bool store_vertices(VertexPages& pages, std::span<const GpuVertex> packed,
                                      bool white, GpuMemoryTracker* tracker, SceneScopeId scope,
                                      std::uint64_t scene);
    [[no_unique_address]] GpuMemoryAllocation memory_;
    std::shared_ptr<StaticVertexPage> page_;
    std::uint32_t first_vertex_ = 0;
    bgfx::VertexLayoutHandle attributes_layout_ = BGFX_INVALID_HANDLE;
    std::uint32_t first_attribute_vertex_ = 0;
    std::uint16_t vertex_stride_ = sizeof(GpuVertex);
    std::uint16_t vertex_padding_ = 0;

    struct IndexBuffer
    {
        bgfx::IndexBufferHandle handle = BGFX_INVALID_HANDLE;
        std::uint32_t bytes = 0;
        std::uint32_t count = 0;
        bool index16 = false;

        [[nodiscard]] bool upload(std::span<const std::uint32_t> source);
        void release() noexcept;
    };
    [[nodiscard]] const IndexBuffer* index_storage(std::size_t level) const noexcept;
    [[nodiscard]] const IndexBuffer* meshlet_index_storage(std::size_t level) const noexcept;

    struct MeshletBuffers
    {
        IndexBuffer indices;
        bgfx::VertexBufferHandle debug_offsets = BGFX_INVALID_HANDLE;
        bool ready = false;
    };
    std::vector<MeshletBuffers> meshlets_;
    bool meshlet_debug_ready_ = false;
    struct Lod
    {
        IndexBuffer indices;
        std::vector<schema::MeshSubmesh> submeshes;
    };
    std::vector<Lod> lods_;
    std::vector<schema::MeshSubmesh> submeshes_;
    IndexBuffer indices_;
    struct JointBounds
    {
        std::array<float, 3> minimum{};
        std::array<float, 3> maximum{};
        bool used = false;
    };
    std::vector<JointBounds> joint_bounds_;
};

} // namespace engine::render_detail
