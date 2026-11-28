#include "runtime/render/resources/gpu_geometry.hpp"

#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/resource/types/mesh_resource.hpp"
#include "runtime/scene/mesh_renderer.hpp"
#include "runtime/scene/node.hpp"
#include "runtime/utils/mesh_utils.hpp"
#include "runtime/world/world_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace engine::render_detail
{
namespace
{
bool shares_meshlet_indices(const schema::MeshletDataT& meshlets,
                            mesh_storage::MeshIndexView regular)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    if (meshlets.uses_parent_indices)
    {
        return true;
    }
    const auto view = mesh_storage::meshlet_indices(meshlets, regular);
    if (view.size() != regular.size())
    {
        return false;
    }
    if (!view.narrow.empty())
    {
        return regular.narrow.empty() ? std::ranges::equal(view.narrow, regular.wide)
                                      : std::ranges::equal(view.narrow, regular.narrow);
    }
    return regular.narrow.empty() ? std::ranges::equal(view.wide, regular.wide)
                                  : std::ranges::equal(view.wide, regular.narrow);
}

enum class VertexStorage
{
    aligned,
    static_float,
    static_white
};

bgfx::VertexLayout make_vertex_layout(VertexStorage storage, bool include_position = true)
{
    ENGINE_PROFILE_FUNCTION();
    bgfx::VertexLayout layout;
    layout.begin();
    if (include_position)
    {
        layout.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float);
    }
    layout.add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Tangent, 4, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float);
    const bool white = storage == VertexStorage::static_white;
    layout
        .add(bgfx::Attrib::Color0, 4, white ? bgfx::AttribType::Uint8 : bgfx::AttribType::Float,
             white)
        .add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float);
    if (storage == VertexStorage::aligned)
    {
        layout.skip(sizeof(GpuVertex::padding));
    }
    layout.end();
    return layout;
}

void write_static_vertices(std::span<const GpuVertex> vertices, VertexStorage storage,
                           std::uint8_t* positions, std::size_t position_stride,
                           std::uint8_t* attributes, std::size_t attribute_stride,
                           std::size_t attribute_size)
{
    ENGINE_PROFILE_FUNCTION();
    constexpr auto position_bytes = sizeof(GpuVertex::position);
    for (std::size_t index = 0; index < vertices.size(); ++index)
    {
        const auto& vertex = vertices[index];
        const auto* source = reinterpret_cast<const std::uint8_t*>(&vertex);
        auto* attribute = attributes + index * attribute_stride;
        std::memcpy(positions + index * position_stride, source, position_bytes);
        if (storage == VertexStorage::static_white)
        {
            constexpr auto prefix = offsetof(GpuVertex, color) - position_bytes;
            constexpr std::size_t rgba8_bytes = 4;
            std::memcpy(attribute, source + position_bytes, prefix);
            std::memset(attribute + prefix, 0xff, rgba8_bytes);
            std::memcpy(attribute + prefix + rgba8_bytes, vertex.lightmap_texcoord,
                        sizeof(vertex.lightmap_texcoord));
        }
        else
        {
            std::memcpy(attribute, source + position_bytes, attribute_size);
        }
    }
}

bgfx::VertexLayout position_layout()
{
    ENGINE_PROFILE_FUNCTION();
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    return layout;
}

const bgfx::Memory* copy_vertex_data(std::span<const GpuVertex> vertices, VertexStorage storage,
                                     std::uint16_t stride, std::uint32_t attribute_offset)
{
    ENGINE_PROFILE_FUNCTION();
    if (storage == VertexStorage::aligned)
    {
        return bgfx::copy(vertices.data(), static_cast<std::uint32_t>(vertices.size() * stride));
    }
    constexpr auto position_bytes = sizeof(GpuVertex::position);
    const bool separate = attribute_offset != 0;
    const auto attribute_bytes = stride - position_bytes;
    const auto bytes =
        separate ? attribute_offset + vertices.size() * attribute_bytes : vertices.size() * stride;
    const auto* memory = bgfx::alloc(static_cast<std::uint32_t>(bytes));
    if (separate)
    {
        const auto end = vertices.size() * position_bytes;
        std::memset(memory->data + end, 0, attribute_offset - end);
    }
    write_static_vertices(vertices, storage, memory->data, separate ? position_bytes : stride,
                          memory->data + (separate ? attribute_offset : position_bytes),
                          separate ? attribute_bytes : stride, attribute_bytes);
    return memory;
}
} // namespace

GpuGeometry::StaticVertexPage::~StaticVertexPage() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    for (const auto buffer : {positions, attributes})
    {
        if (bgfx::isValid(buffer))
        {
            bgfx::destroy(buffer);
        }
    }
    memory.reset();
}

bool GpuGeometry::StaticVertexPage::create(GpuMemoryTracker* tracker, SceneScopeId scope,
                                            std::uint64_t scene)
{
    ENGINE_PROFILE_FUNCTION();
    const auto layout = make_vertex_layout(
        white ? VertexStorage::static_white : VertexStorage::static_float, false);
    positions =
        bgfx::createDynamicVertexBuffer(capacity, position_layout(), BGFX_BUFFER_COMPUTE_READ);
    if (!bgfx::isValid(positions))
    {
        return false;
    }
    // The read-only flag requests a dedicated bgfx allocation for this persistent stream.
    attributes = bgfx::createDynamicVertexBuffer(capacity, layout, BGFX_BUFFER_COMPUTE_READ);
    attribute_stride_ = layout.getStride();
    if (!bgfx::isValid(attributes))
    {
        return false;
    }
    if (tracker)
    {
        GpuMemoryEntry entry;
        entry.kind = GpuMemoryKind::static_vertex_page;
        entry.creation_scope = scope;
        entry.creation_scene = scene;
        entry.shared_backing_bytes = byte_size();
        memory.track(tracker, std::move(entry));
    }
    return true;
}

bool GpuGeometry::StaticVertexPage::store(std::span<const GpuVertex> vertices, std::uint32_t& first)
{
    ENGINE_PROFILE_FUNCTION();
    const auto range = std::ranges::find_if(free_ranges_, [&](const auto& free)
                                            {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return free.count >= vertices.size();
    });
    if (range == free_ranges_.end())
    {
        return false;
    }
    first = range->first;
    const auto count = static_cast<std::uint32_t>(vertices.size());
    constexpr auto position_stride = sizeof(GpuVertex::position);
    const auto* points = bgfx::alloc(static_cast<std::uint32_t>(count * position_stride));
    const auto* values = bgfx::alloc(count * attribute_stride_);
    write_static_vertices(
        vertices, white ? VertexStorage::static_white : VertexStorage::static_float, points->data,
        position_stride, values->data, attribute_stride_, attribute_stride_);
    bgfx::update(positions, first, points);
    bgfx::update(attributes, first, values);
    range->first += count;
    range->count -= count;
    if (!range->count)
    {
        free_ranges_.erase(range);
    }
    return true;
}

void GpuGeometry::StaticVertexPage::release_range(std::uint32_t first, std::uint32_t count) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    auto next = std::ranges::lower_bound(free_ranges_, first, {}, &Range::first);
    if (next != free_ranges_.end() && next->first == first + count)
    {
        count += next->count;
        next = free_ranges_.erase(next);
    }
    if (next != free_ranges_.begin() && (next - 1)->first + (next - 1)->count == first)
    {
        (next - 1)->count += count;
    }
    else
    {
        free_ranges_.insert(next, {first, count});
    }
}

std::uint64_t GpuGeometry::StaticVertexPage::byte_size() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return std::uint64_t{capacity} * (sizeof(GpuVertex::position) + attribute_stride_);
}

GpuGeometry::~GpuGeometry() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release();
}

bool GpuGeometry::IndexBuffer::upload(mesh_storage::MeshIndexView source)
{
    ENGINE_PROFILE_FUNCTION();
    if (source.empty() || source.size() > UINT32_MAX / sizeof(std::uint16_t))
    {
        return false;
    }
    const bool compact =
        !source.narrow.empty() || *std::ranges::max_element(source.wide) <= UINT16_MAX;
    // Debug shaders read uint32 words, including the final pair of packed indices.
    const std::uint64_t padded_count = source.size() + (compact && source.size() % 2 != 0);
    const auto element_size = compact ? sizeof(std::uint16_t) : sizeof(std::uint32_t);
    const auto byte_count = padded_count * element_size;
    if (byte_count > UINT32_MAX)
    {
        return false;
    }
    const auto size = static_cast<std::uint32_t>(byte_count);
    const bgfx::Memory* memory;
    if (compact)
    {
        memory = bgfx::alloc(size);
        auto* packed = reinterpret_cast<std::uint16_t*>(memory->data);
        if (!source.narrow.empty())
        {
            std::memcpy(packed, source.narrow.data(), source.narrow.size_bytes());
        }
        else
        {
            std::ranges::transform(source.wide, packed,
                                   [](std::uint32_t index)
                                   {
                                       ENGINE_PROFILE_FUNCTION_PROBE();
                                       return static_cast<std::uint16_t>(index);
                                   });
        }
        if (padded_count != source.size())
        {
            packed[source.size()] = 0;
        }
    }
    else
    {
        memory = bgfx::copy(source.wide.data(), size);
    }
    handle = bgfx::createIndexBuffer(memory, BGFX_BUFFER_COMPUTE_READ |
                                                 (compact ? 0 : BGFX_BUFFER_INDEX32));
    if (!bgfx::isValid(handle))
    {
        return false;
    }
    bytes = size;
    count = static_cast<std::uint32_t>(source.size());
    index16 = compact;
    return true;
}

void GpuGeometry::IndexBuffer::release() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(handle))
    {
        bgfx::destroy(handle);
    }
    *this = {};
}

bool GpuGeometry::create_base_indices(mesh_storage::MeshIndexView source)
{
    ENGINE_PROFILE_FUNCTION();
    return !bgfx::isValid(indices_.handle) && indices_.upload(source);
}

bgfx::VertexLayout GpuGeometry::vertex_layout()
{
    ENGINE_PROFILE_FUNCTION();
    return make_vertex_layout(VertexStorage::aligned);
}

bool GpuGeometry::vertices_ready() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return page_ || bgfx::isValid(vertices);
}

void GpuGeometry::bind_vertices(bool positions_only, bool whole_page) const
{
    ENGINE_PROFILE_FUNCTION();
    if (page_)
    {
        const auto first = whole_page ? 0U : first_vertex_;
        const auto count = whole_page ? StaticVertexPage::capacity : vertex_count;
        bgfx::setVertexBuffer(0, page_->positions, first, count);
        if (!positions_only)
        {
            bgfx::setVertexBuffer(1, page_->attributes, first, count);
        }
        return;
    }
    bgfx::setVertexBuffer(0, vertices, 0, vertex_count);
    if (!positions_only && bgfx::isValid(attributes_layout_))
    {
        bgfx::setVertexBuffer(1, vertices, first_attribute_vertex_, vertex_count,
                              attributes_layout_);
    }
}

void GpuGeometry::bind_position_buffer(std::uint8_t stage) const
{
    ENGINE_PROFILE_FUNCTION();
    if (page_)
    {
        bgfx::setBuffer(stage, page_->positions, bgfx::Access::Read);
    }
    else
    {
        bgfx::setBuffer(stage, vertices, bgfx::Access::Read);
    }
}

std::uint16_t GpuGeometry::position_buffer_index() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return page_ ? page_->positions.idx : vertices.idx;
}

bool GpuGeometry::pack_vertices(const schema::MeshDataT& data,
                                std::span<const schema::MeshVertex> positions,
                                std::span<const schema::Vec4> tangents,
                                std::vector<GpuVertex>& packed, std::array<float, 3>& minimum,
                                std::array<float, 3>& maximum)
{
    ENGINE_PROFILE_ZONE("Render.geometryPackVertices");
    if (positions.size() != data.vertices.size() || tangents.size() != positions.size())
    {
        return false;
    }
    packed.resize(positions.size());
    minimum.fill(std::numeric_limits<float>::max());
    maximum.fill(std::numeric_limits<float>::lowest());
    const bool has_uv = mesh_storage::texcoord_count(data) != 0;
    const bool has_lightmap_uv = mesh_storage::lightmap_texcoord_count(data) != 0;
    for (std::size_t index = 0; index < packed.size(); ++index)
    {
        const auto& source = positions[index];
        const auto& tangent = tangents[index];
        const auto uv = has_uv ? mesh_storage::texcoord(data, index) : schema::Vec2{};
        const auto lightmap_uv =
            has_lightmap_uv ? mesh_storage::lightmap_texcoord(data, index) : schema::Vec2{};
        const auto color = data.colors.empty() ? schema::Color{1, 1, 1, 1} : data.colors[index];
        const double normal_length =
            std::hypot(static_cast<double>(source.nx()), static_cast<double>(source.ny()),
                       static_cast<double>(source.nz()));
        if (!(normal_length > 0.0))
        {
            return false;
        }
        const std::array<double, 3> normal = {
            source.nx() / normal_length, source.ny() / normal_length, source.nz() / normal_length};
        const std::array<double, 3> bitangent = {normal[1] * tangent.z() - normal[2] * tangent.y(),
                                                 normal[2] * tangent.x() - normal[0] * tangent.z(),
                                                 normal[0] * tangent.y() - normal[1] * tangent.x()};
        const std::array<double, 3> orthogonal = {
            bitangent[1] * normal[2] - bitangent[2] * normal[1],
            bitangent[2] * normal[0] - bitangent[0] * normal[2],
            bitangent[0] * normal[1] - bitangent[1] * normal[0]};
        const double tangent_length = std::hypot(orthogonal[0], orthogonal[1], orthogonal[2]);
        if (!(tangent_length > 0.0))
        {
            return false;
        }
        packed[index] = {{source.x(), source.y(), source.z()},
                         {static_cast<float>(normal[0]), static_cast<float>(normal[1]),
                          static_cast<float>(normal[2])},
                         {static_cast<float>(orthogonal[0] / tangent_length),
                          static_cast<float>(orthogonal[1] / tangent_length),
                          static_cast<float>(orthogonal[2] / tangent_length),
                          static_cast<float>(tangent.w())},
                         {static_cast<float>(uv.x()), static_cast<float>(uv.y())},
                         {color.r(), color.g(), color.b(), color.a()},
                         {static_cast<float>(lightmap_uv.x()), static_cast<float>(lightmap_uv.y())},
                         {}};
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            minimum[axis] = std::min(minimum[axis], packed[index].position[axis]);
            maximum[axis] = std::max(maximum[axis], packed[index].position[axis]);
        }
    }
    return true;
}

bgfx::VertexLayout GpuGeometry::vector_layout()
{
    ENGINE_PROFILE_FUNCTION();
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float).end();
    return layout;
}

bool GpuGeometry::store_vertices(VertexPages& pages, std::span<const GpuVertex> packed, bool white,
                                 GpuMemoryTracker* tracker, SceneScopeId scope,
                                 std::uint64_t scene)
{
    ENGINE_PROFILE_FUNCTION();
    std::erase_if(pages, [](const auto& page)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return page.expired();
    });
    for (const auto& candidate : pages)
    {
        auto page = candidate.lock();
        if (page && page->white == white && page->store(packed, first_vertex_))
        {
            page_ = std::move(page);
            return true;
        }
    }
    auto page = std::make_shared<StaticVertexPage>(white);
    if (!page->create(tracker, scope, scene) || !page->store(packed, first_vertex_))
    {
        return false;
    }
    pages.emplace_back(page);
    page_ = std::move(page);
    return true;
}

bool GpuGeometry::create(const MeshRenderer& renderer, VertexPages* pages,
                         GpuMemoryTracker* tracker, SceneScopeId scope, std::uint64_t scene)
{
    ENGINE_PROFILE_ZONE("Render.geometryCreate");
    release();
    const auto* data = renderer.mesh_data();
    std::vector<schema::Vec4> tangents;
    std::vector<GpuVertex> packed;
    if (data == nullptr || !MeshUtils::build_tangents(*data, tangents) ||
        !pack_vertices(*data, data->vertices, tangents, packed, minimum, maximum))
    {
        return false;
    }
    const auto storage = !data->skin.empty()    ? VertexStorage::aligned
                         : data->colors.empty() ? VertexStorage::static_white
                                                : VertexStorage::static_float;
    auto layout = make_vertex_layout(storage);
    const auto stride = layout.getStride();
    const auto vertex_bytes = std::uint64_t{packed.size()} * stride;
    if (vertex_bytes > std::numeric_limits<std::uint32_t>::max())
    {
        return false;
    }
    std::vector<std::array<float, 4>> skin;
    if (!data->skin.empty())
    {
        if (data->skin.size() != packed.size())
        {
            return false;
        }
        std::size_t count = 0;
        for (const auto& influence : data->skin)
        {
            count = std::max(count, std::size_t{1} + std::max({influence.j0(), influence.j1(),
                                                               influence.j2(), influence.j3()}));
        }
        joint_bounds_.resize(count);
        skin.reserve(packed.size() * 2);
        for (std::size_t vertex = 0; vertex < packed.size(); ++vertex)
        {
            const auto& input = data->skin[vertex];
            const std::array<std::uint16_t, 4> joints = {input.j0(), input.j1(), input.j2(),
                                                         input.j3()};
            std::array<float, 4> weights = {input.w0(), input.w1(), input.w2(), input.w3()};
            const float sum = weights[0] + weights[1] + weights[2] + weights[3];
            if (!std::isfinite(sum) || std::abs(sum - 1.0F) > 0.0001F)
            {
                return false;
            }
            for (std::size_t item = 0; item < weights.size(); ++item)
            {
                if (!std::isfinite(weights[item]) || weights[item] < 0 || weights[item] > 1)
                {
                    return false;
                }
                weights[item] /= sum;
                if (weights[item] == 0)
                {
                    continue;
                }
                auto& bounds = joint_bounds_[joints[item]];
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    const auto value = packed[vertex].position[axis];
                    bounds.minimum[axis] =
                        bounds.used ? std::min(bounds.minimum[axis], value) : value;
                    bounds.maximum[axis] =
                        bounds.used ? std::max(bounds.maximum[axis], value) : value;
                }
                bounds.used = true;
            }
            skin.push_back({static_cast<float>(joints[0]), static_cast<float>(joints[1]),
                            static_cast<float>(joints[2]), static_cast<float>(joints[3])});
            skin.push_back(weights);
        }
        influences = bgfx::createVertexBuffer(
            bgfx::copy(skin.data(), static_cast<std::uint32_t>(skin.size() * sizeof(skin[0]))),
            vector_layout(), BGFX_BUFFER_COMPUTE_READ);
        if (!bgfx::isValid(influences))
        {
            return false;
        }
    }
    {
        // 这里只采集 CPU 拷贝与提交；GPU 执行时间由 GPU 时间戳表示。
        ENGINE_PROFILE_ZONE("Render.geometryBuffersSubmit");
        vertex_count = static_cast<std::uint32_t>(packed.size());
        const auto* caps = bgfx::getCaps();
        const bool split_streams = storage != VertexStorage::aligned && !packed.empty() &&
                                   caps->limits.maxVertexStreams >= 2;
        const bool use_page =
            split_streams && pages && data->meshlets &&
            packed.size() <= StaticVertexPage::capacity &&
            store_vertices(*pages, packed, storage == VertexStorage::static_white, tracker, scope,
                           scene);
        std::uint32_t attribute_offset = 0;
        if (!use_page && split_streams)
        {
            const auto attributes = make_vertex_layout(storage, false);
            const auto attribute_stride = attributes.getStride();
            const auto position_bytes = packed.size() * sizeof(GpuVertex::position);
            const auto first = (position_bytes + attribute_stride - 1) / attribute_stride;
            const auto offset = first * attribute_stride;
            const auto padding = offset - position_bytes;
            if (vertex_bytes + padding <= std::numeric_limits<std::uint32_t>::max())
            {
                attributes_layout_ = bgfx::createVertexLayout(attributes);
            }
            if (bgfx::isValid(attributes_layout_))
            {
                layout = position_layout();
                attribute_offset = static_cast<std::uint32_t>(offset);
                first_attribute_vertex_ = static_cast<std::uint32_t>(first);
                vertex_padding_ = static_cast<std::uint16_t>(padding);
            }
        }
        if (!use_page)
        {
            vertices = bgfx::createVertexBuffer(
                copy_vertex_data(packed, storage, stride, attribute_offset), layout,
                BGFX_BUFFER_COMPUTE_READ);
        }
        if (!vertices_ready() || !create_base_indices(mesh_storage::indices(*data)))
        {
            release();
            return false;
        }
    }
    submeshes_ = data->submeshes;
    for (const auto& level : data->lods)
    {
        Lod lod;
        lod.submeshes = level->submeshes;
        if (!lod.indices.upload(mesh_storage::indices(*level)))
        {
            release();
            return false;
        }
        lods_.push_back(std::move(lod));
    }
    if (!resource.retain(renderer.node().world()->resource_loader(), renderer.mesh_resource()))
    {
        release();
        return false;
    }
    vertex_stride_ = stride;
    if (tracker)
    {
        GpuMemoryEntry entry;
        entry.kind = GpuMemoryKind::mesh_geometry;
        entry.guid = resource.get()->info().guid;
        entry.creation_scope = scope;
        entry.creation_scene = scene;
        const auto geometry_vertex_bytes =
            page_ ? (page_->byte_size() / StaticVertexPage::capacity) * vertex_count
                  : vertex_buffer_bytes();
        entry.logical_bytes =
            geometry_vertex_bytes + index_buffer_bytes() + skin.size() * sizeof(skin[0]);
        entry.shared_backing_id = page_ ? page_->memory.id() : 0;
        memory_.track(tracker, std::move(entry));
    }
    return true;
}

bool GpuGeometry::skinning_bounds(std::span<const std::array<float, 16>> palette,
                                  std::array<float, 3>& output_minimum,
                                  std::array<float, 3>& output_maximum) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    output_minimum = {};
    output_maximum = {};
    if (joint_bounds_.empty() || palette.size() < joint_bounds_.size())
    {
        return false;
    }
    std::array<double, 3> low;
    std::array<double, 3> high;
    low.fill(std::numeric_limits<double>::max());
    high.fill(std::numeric_limits<double>::lowest());
    for (std::size_t joint = 0; joint < joint_bounds_.size(); ++joint)
    {
        const auto& bounds = joint_bounds_[joint];
        if (!bounds.used)
        {
            continue;
        }
        const auto& matrix = palette[joint];
        for (unsigned int corner = 0; corner < 8; ++corner)
        {
            const double x = (corner & 1) ? bounds.maximum[0] : bounds.minimum[0];
            const double y = (corner & 2) ? bounds.maximum[1] : bounds.minimum[1];
            const double z = (corner & 4) ? bounds.maximum[2] : bounds.minimum[2];
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                const double value = matrix[axis] * x + matrix[axis + 4] * y +
                                     matrix[axis + 8] * z + matrix[axis + 12];
                if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                {
                    return false;
                }
                low[axis] = std::min(low[axis], value);
                high[axis] = std::max(high[axis], value);
            }
        }
    }
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        // 留出浮点混合误差，边界必须保守覆盖 GPU 计算结果。
        const double margin = 1.0e-5 * std::max({1.0, std::abs(low[axis]), std::abs(high[axis])});
        if (low[axis] - margin < -std::numeric_limits<float>::max() ||
            high[axis] + margin > std::numeric_limits<float>::max())
        {
            return false;
        }
        output_minimum[axis] = static_cast<float>(low[axis] - margin);
        output_maximum[axis] = static_cast<float>(high[axis] + margin);
    }
    return true;
}

std::uint32_t GpuGeometry::resource_count() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    std::uint32_t meshlet_count = 0;
    for (const auto& buffers : meshlets_)
    {
        meshlet_count +=
            bgfx::isValid(buffers.indices.handle) + bgfx::isValid(buffers.debug_offsets);
    }
    return meshlet_count + static_cast<std::uint32_t>(bgfx::isValid(vertices)) +
           static_cast<std::uint32_t>(bgfx::isValid(indices_.handle)) +
           static_cast<std::uint32_t>(bgfx::isValid(influences)) +
           static_cast<std::uint32_t>(lods_.size());
}

const GpuGeometry::IndexBuffer* GpuGeometry::index_storage(std::size_t level) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return level == 0 ? &indices_ : level <= lods_.size() ? &lods_[level - 1].indices : nullptr;
}

bgfx::IndexBufferHandle GpuGeometry::index_buffer(std::size_t level) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* storage = index_storage(level);
    return storage ? storage->handle : bgfx::IndexBufferHandle{bgfx::kInvalidHandle};
}

std::uint32_t GpuGeometry::index_count(std::size_t level) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* storage = index_storage(level);
    return storage ? storage->count : 0;
}

bool GpuGeometry::index_uses_16_bits(std::size_t level, bool meshlet) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* storage = meshlet ? meshlet_index_storage(level) : index_storage(level);
    return storage && storage->index16;
}

std::uint64_t GpuGeometry::index_buffer_bytes() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    std::uint64_t bytes = indices_.bytes;
    for (const auto& lod : lods_)
    {
        bytes += lod.indices.bytes;
    }
    for (const auto& buffers : meshlets_)
    {
        bytes += buffers.indices.bytes;
    }
    return bytes;
}

std::span<const schema::MeshSubmesh> GpuGeometry::submeshes(std::size_t level) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (level == 0)
    {
        return submeshes_;
    }
    return level <= lods_.size() ? std::span<const schema::MeshSubmesh>(lods_[level - 1].submeshes)
                                 : std::span<const schema::MeshSubmesh>{};
}

std::size_t GpuGeometry::lod_count() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return lods_.size() + 1;
}

void GpuGeometry::release() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release_meshlets();
    for (auto& lod : lods_)
    {
        lod.indices.release();
    }
    lods_.clear();
    submeshes_.clear();
    for (auto* handle : {&vertices, &influences})
    {
        if (bgfx::isValid(*handle))
        {
            bgfx::destroy(*handle);
            *handle = BGFX_INVALID_HANDLE;
        }
    }
    if (bgfx::isValid(attributes_layout_))
    {
        bgfx::destroy(attributes_layout_);
        attributes_layout_ = BGFX_INVALID_HANDLE;
    }
    if (page_)
    {
        page_->release_range(first_vertex_, vertex_count);
        page_.reset();
    }
    first_vertex_ = 0;
    indices_.release();
    memory_.reset();
    resource.reset();
    joint_bounds_.clear();
    vertex_count = 0;
    first_attribute_vertex_ = 0;
    vertex_stride_ = sizeof(GpuVertex);
    vertex_padding_ = 0;
}

std::uint64_t GpuGeometry::meshlet_upload_bytes(bool debug) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* mesh = resource.data<MeshResource>();
    if (!mesh)
    {
        return 0;
    }
    const auto bytes =
        [&](mesh_storage::MeshIndexView regular, const schema::MeshletDataT* meshlets)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (!meshlets)
        {
            return std::uint64_t{0};
        }
        const auto index_bytes =
            meshlets_.empty() && !shares_meshlet_indices(*meshlets, regular)
                ? mesh_storage::meshlet_indices(*meshlets, regular).size() * sizeof(std::uint32_t)
                : 0;
        const auto debug_bytes =
            debug && !meshlet_debug_ready_ ? meshlets->meshlets.size() * sizeof(std::uint32_t) : 0;
        return static_cast<std::uint64_t>(index_bytes) + debug_bytes;
    };
    auto total = bytes(mesh_storage::indices(*mesh), mesh->meshlets.get());
    for (const auto& lod : mesh->lods)
    {
        total += bytes(mesh_storage::indices(*lod), lod->meshlets.get());
    }
    return total;
}

bool GpuGeometry::meshlets_ready(bool debug) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return !meshlets_.empty() && (!debug || meshlet_debug_ready_);
}

bool GpuGeometry::prepare_meshlets(bool debug)
{
    ENGINE_PROFILE_FUNCTION();
    if (meshlets_ready(debug))
    {
        return true;
    }
    const auto* data = resource.data<MeshResource>();
    if (!data)
    {
        return false;
    }
    const bool needs_indices = meshlets_.empty();
    meshlets_.resize(data->lods.size() + 1);
    for (std::size_t level = 0; level < meshlets_.size(); ++level)
    {
        const auto* source =
            level == 0 ? data->meshlets.get() : data->lods[level - 1]->meshlets.get();
        if (!source)
        {
            continue;
        }
        auto& buffers = meshlets_[level];
        const auto regular = level == 0 ? mesh_storage::indices(*data)
                                        : mesh_storage::indices(*data->lods[level - 1]);
        if (needs_indices && !shares_meshlet_indices(*source, regular) &&
            !buffers.indices.upload(mesh_storage::meshlet_indices(*source, regular)))
        {
            release_meshlets();
            return false;
        }
        buffers.ready = true;
    }
    if (!debug)
    {
        return true;
    }

    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::TexCoord0, 1, bgfx::AttribType::Float).end();
    for (std::size_t level = 0; level < meshlets_.size(); ++level)
    {
        const auto* source =
            level == 0 ? data->meshlets.get() : data->lods[level - 1]->meshlets.get();
        if (!source)
        {
            continue;
        }
        const auto size = source->meshlets.size() * sizeof(std::uint32_t);
        if (size > UINT32_MAX)
        {
            release_meshlet_debug();
            return false;
        }
        const auto* memory = bgfx::alloc(static_cast<std::uint32_t>(size));
        auto* offsets = reinterpret_cast<std::uint32_t*>(memory->data);
        std::ranges::transform(source->meshlets, offsets,
                               [](const auto& cluster)
                               {
                                   ENGINE_PROFILE_FUNCTION_PROBE();
                                   return cluster.first_index();
                               });
        auto& handle = meshlets_[level].debug_offsets;
        handle = bgfx::createVertexBuffer(memory, layout, BGFX_BUFFER_COMPUTE_READ);
        if (!bgfx::isValid(handle))
        {
            release_meshlet_debug();
            return false;
        }
    }
    meshlet_debug_ready_ = true;
    return true;
}

void GpuGeometry::release_meshlet_debug() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    for (auto& buffers : meshlets_)
    {
        if (bgfx::isValid(buffers.debug_offsets))
        {
            bgfx::destroy(buffers.debug_offsets);
            buffers.debug_offsets = BGFX_INVALID_HANDLE;
        }
    }
    meshlet_debug_ready_ = false;
}

void GpuGeometry::release_meshlets() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release_meshlet_debug();
    for (auto& buffers : meshlets_)
    {
        buffers.indices.release();
    }
    meshlets_.clear();
}

const GpuGeometry::IndexBuffer* GpuGeometry::meshlet_index_storage(std::size_t level) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (level >= meshlets_.size() || !meshlets_[level].ready)
    {
        return nullptr;
    }
    const auto& storage = meshlets_[level].indices;
    return bgfx::isValid(storage.handle) ? &storage : index_storage(level);
}

bgfx::IndexBufferHandle GpuGeometry::meshlet_indices(std::size_t level) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* storage = meshlet_index_storage(level);
    return storage ? storage->handle : bgfx::IndexBufferHandle{bgfx::kInvalidHandle};
}

bgfx::VertexBufferHandle GpuGeometry::meshlet_debug_offsets(std::size_t level) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return level < meshlets_.size() ? meshlets_[level].debug_offsets
                                    : bgfx::VertexBufferHandle{bgfx::kInvalidHandle};
}

} // namespace engine::render_detail
