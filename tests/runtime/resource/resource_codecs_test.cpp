#include "resource_fixture.hpp"
#include "scene_snapshot.hpp"

#include "runtime/engine_config.hpp"
#include "runtime/resource/codecs/material_resource_codec.hpp"
#include "runtime/resource/codecs/mesh_resource_codec.hpp"
#include "runtime/resource/codecs/shader_resource_codec.hpp"
#include "runtime/resource/codecs/texture_chunks.hpp"
#include "runtime/resource/codecs/texture_resource_codec.hpp"
#include "runtime/resource/mesh_storage.hpp"
#include "runtime/resource/resource_codec.hpp"
#include "runtime/resource/resource_system.hpp"
#include "runtime/schema/movie_generated.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace engine;

bool expect(bool value, const char* message)
{
    if (!value)
    {
        std::fprintf(stderr, "resource codecs: %s\n", message);
    }
    return value;
}

template <typename T> ByteBuffer pack(const T& data, const char* identifier)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(T::TableType::Pack(builder, &data), identifier);
    const auto* bytes = reinterpret_cast<const std::byte*>(builder.GetBufferPointer());
    return {bytes, bytes + builder.GetSize()};
}

bool add_payload(test_support::ResourceFixture& fixture, std::string path, schema::AssetKind kind,
                 ByteView payload, std::vector<Guid> dependencies = {})
{
    auto entry = std::make_unique<schema::AssetEntryT>();
    std::array<char, 37> guid{};
    std::snprintf(guid.data(), guid.size(), "30000000-0000-4000-8000-%012zu",
                  fixture.catalog.entries.size() + 1);
    entry->guid = guid.data();
    entry->path = std::move(path);
    entry->kind = kind;
    if (kind == schema::AssetKind::script)
    {
        entry->module_path = "main.js";
    }
    entry->dependencies = std::move(dependencies);
    if (kind == schema::AssetKind::script)
    {
        entry->module_path = "scripts/main.js";
    }
    ByteBuffer entity;
    if (!ResourceSystem::encode_resource(entry->guid, kind, payload, entity, entry->header_size))
    {
        return false;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto value : entity)
    {
        hash = (hash ^ std::to_integer<std::uint8_t>(value)) * 1099511628211ULL;
    }
    std::array<char, 17> digest{};
    std::snprintf(digest.data(), digest.size(), "%016llx", static_cast<unsigned long long>(hash));
    entry->content_hash = digest.data();
    entry->content_size = entity.size();
    entry->location = std::make_unique<schema::FileLocationT>();
    entry->location->mount = "content";
    entry->location->path = "gen/" + entry->guid;
    entry->location->length = entity.size();
    std::ofstream stream(fixture.root / entry->location->path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(entity.data()),
                 static_cast<std::streamsize>(entity.size()));
    stream.close();
    if (!stream)
    {
        return false;
    }
    fixture.catalog.entries.push_back(std::move(entry));
    return true;
}

bool compact_mesh_tests()
{
    schema::MeshDataT source;
    source.indices = {0, 65535, 1};
    source.meshlets = std::make_unique<schema::MeshletDataT>();
    source.meshlets->indices = source.indices;
    auto lod = std::make_unique<schema::MeshLodDataT>();
    lod->indices = {0, 65536, 2};
    lod->meshlets = std::make_unique<schema::MeshletDataT>();
    lod->meshlets->indices = {1, 2, 0};
    source.lods.push_back(std::move(lod));
    source.texcoords.emplace_back(-0.0, std::numeric_limits<float>::denorm_min());
    source.tangents.emplace_back(1.0 + std::numeric_limits<double>::epsilon(), 0, 0, -1);
    source.lightmap_texcoords.emplace_back(0.25, 1);
    const auto original = pack(source, "EMS2");

    if (!expect(mesh_storage::compact(source) && mesh_storage::compact(source) &&
                    source.indices.capacity() == 0 && source.meshlets->indices.capacity() == 0 &&
                    source.indices16.size() == 3 && source.meshlets->uses_parent_indices &&
                    mesh_storage::meshlet_indices(*source.meshlets,
                                                  mesh_storage::indices(source))[1] == 65535 &&
                    source.lods[0]->indices.size() == 3 && source.lods[0]->indices16.empty() &&
                    source.lods[0]->meshlets->indices16.size() == 3 &&
                    !source.lods[0]->meshlets->uses_parent_indices &&
                    source.texcoords.capacity() == 0 && source.texcoords32.size() == 1 &&
                    source.lightmap_texcoords.capacity() == 0 &&
                    source.lightmap_texcoords32.size() == 1 && source.tangents.size() == 1 &&
                    source.tangents32.empty(),
                "compact mesh preserves wide precision and independent LOD topology"))
    {
        return false;
    }
    auto invalid = source;
    invalid.meshlets->indices16.push_back(0);
    auto both = source;
    both.indices.push_back(0);
    auto uv_conflict = source;
    uv_conflict.texcoords.emplace_back(0, 0);
    const auto before = pack(invalid, "EMS2");
    const auto conflicting_uv = pack(uv_conflict, "EMS2");
    std::unique_ptr<Resource> rejected;
    schema::AssetEntryT entry;
    entry.kind = schema::AssetKind::mesh;
    std::string error;
    if (!expect(!mesh_storage::compact(invalid) && !mesh_storage::expand(invalid) &&
                    pack(invalid, "EMS2") == before && !mesh_storage::valid_encoding(both) &&
                    !mesh_storage::valid_encoding(uv_conflict) &&
                    !MeshResourceCodec{}.decode(entry, conflicting_uv, rejected, error) &&
                    !rejected && !error.empty(),
                "conflicting compact mesh encodings are rejected atomically"))
    {
        return false;
    }
    const auto encoded = pack(source, "EMS2");
    std::unique_ptr<Resource> decoded;
    if (!MeshResourceCodec{}.decode(entry, encoded, decoded, error))
    {
        return false;
    }
    auto restored = *resource_detail::data<MeshResource>(decoded.get());
    if (!expect(restored.optimize_storage && mesh_storage::expand(restored) &&
                    mesh_storage::expand(restored) && !mesh_storage::is_compact(restored) &&
                    pack(restored, "EMS2") == original,
                "default mesh storage optimization exactly restores authored streams"))
    {
        return false;
    }
    restored.optimize_storage = false;
    source.optimize_storage = false;
    const auto wide = pack(restored, "EMS2");
    const auto compact_disabled = pack(source, "EMS2");
    for (const auto* disabled : {&wide, &compact_disabled})
    {
        std::unique_ptr<Resource> decoded_disabled;
        if (!MeshResourceCodec{}.decode(entry, *disabled, decoded_disabled, error))
        {
            return false;
        }
        const auto* expanded = resource_detail::data<MeshResource>(decoded_disabled.get());
        const auto wide_bytes = decoded_disabled->cpu_payload_capacity();
        const auto optimized_bytes = decoded->cpu_payload_capacity();
        if (!expect(expanded && !expanded->optimize_storage &&
                        !mesh_storage::is_compact(*expanded) && pack(*expanded, "EMS2") == wide &&
                        wide_bytes && optimized_bytes && *wide_bytes > *optimized_bytes,
                    "per-resource disabled policy retains exact wide streams"))
        {
            return false;
        }
    }
    return true;
}

bool codec_tests()
{
    ResourceSystem resources;
    if (!compact_mesh_tests() || !test_support::register_resource_codecs(resources) ||
        !resources.initialize({}))
    {
        return false;
    }
    test_support::ResourceFixture fixture;
    const auto builtin_mesh = test_support::builtin<MeshResourceCodec>("builtin://meshes/cube");
    const auto builtin_texture =
        test_support::builtin<TextureResourceCodec>("builtin://textures/white-srgb");
    const auto builtin_shader = test_support::builtin<ShaderResourceCodec>("builtin://shaders/pbr");
    const auto builtin_material =
        test_support::builtin<MaterialResourceCodec>("builtin://materials/default-pbr");
    if (!builtin_mesh || !builtin_texture || !builtin_shader || !builtin_material)
    {
        return false;
    }
    schema::MeshDataT mesh(*builtin_mesh);
    mesh.path = "cube.mesh";
    schema::MeshDataT mesh_storage;
    mesh_storage.texcoords.reserve(13);
    mesh_storage.meshlets = std::make_unique<schema::MeshletDataT>();
    mesh_storage.meshlets->indices.reserve(31);
    auto lod = std::make_unique<schema::MeshLodDataT>();
    lod->meshlets = std::make_unique<schema::MeshletDataT>();
    lod->meshlets->meshlets.reserve(7);
    mesh_storage.lods.push_back(std::move(lod));
    const auto capacity =
        mesh_storage.texcoords.capacity() * sizeof(schema::Vec2) +
        mesh_storage.meshlets->indices.capacity() * sizeof(std::uint32_t) +
        mesh_storage.lods.capacity() * sizeof(mesh_storage.lods.front()) +
        mesh_storage.lods.front()->meshlets->meshlets.capacity() * sizeof(schema::Meshlet);
    const MeshResource storage_probe({}, std::move(mesh_storage));
    if (!expect(storage_probe.cpu_payload_capacity() == capacity,
                "mesh payload counts allocated stream, meshlet and LOD capacities"))
    {
        return false;
    }
    schema::TextureDataT texture(*builtin_texture);
    texture.path = "white.png";
    std::vector<std::uint8_t> scratch;
    const auto borrowed = texture_storage::native_subresource(texture, texture.subresources.front(), scratch);
    if (!expect(borrowed.data() == texture.data.data() && borrowed.size() == texture.data.size() &&
                    scratch.empty(), "borrows an unpacked texture without a staging allocation"))
    {
        return false;
    }
    auto chunked = texture;
    chunked.width = 4;
    chunked.height = 2;
    chunked.subresources = {schema::TextureSubresourceData(0, 0, 0, 32)};
    // A Zstandard single-segment frame carrying a literal block of six shuffled RGBA texels.
    chunked.data = {0, 1, 2, 3, 4, 5, 6, 7, 0x28, 0xb5, 0x2f, 0xfd, 0x20, 24, 193, 0, 0};
    for (unsigned lane = 0; lane < 4; ++lane)
    {
        for (unsigned i = 0; i < 6; ++i)
        {
            chunked.data.push_back(static_cast<std::uint8_t>(8 + i * 4 + lane));
        }
    }
    chunked.chunks.emplace_back(0, 8, 0, 8, schema::TextureChunkEncoding::raw);
    chunked.chunks.emplace_back(8, 24, 8, 33, schema::TextureChunkEncoding::shuffled_zstd);
    std::array<std::uint8_t, 16> subset{};
    if (!expect(TextureResourceCodec::validate(chunked) &&
                    texture_storage::read_native(chunked, 4, subset),
                "reads portions of raw and shuffled chunks spanning an upload region"))
    {
        return false;
    }
    for (std::size_t i = 0; i < subset.size(); ++i)
    {
        if (!expect(subset[i] == i + 4, "restores every requested native byte in order"))
        {
            return false;
        }
    }
    const auto saved_chunk = chunked.chunks.back();
    chunked.chunks.back() = schema::TextureChunk(4, 24, 8, 33,
                                                 schema::TextureChunkEncoding::shuffled_zstd);
    if (!expect(!TextureResourceCodec::validate(chunked), "rejects overlapping native chunks"))
    {
        return false;
    }
    chunked.chunks.back() = saved_chunk;
    // A reserved block type is an invalid frame even with an intact outer byte count.
    chunked.data[14] = 199;
    if (!expect(!texture_storage::read_native(chunked, 8, subset),
                "propagates corrupt frame failures during partial reads"))
    {
        return false;
    }
    chunked.data[14] = 193;
    chunked.data.pop_back();
    if (!expect(!TextureResourceCodec::validate(chunked), "rejects a truncated native chunk"))
    {
        return false;
    }
    auto array_texture = texture;
    array_texture.array_layers = 2;
    if (!expect(!TextureResourceCodec::validate(array_texture),
                "arrays require every layer payload"))
    {
        return false;
    }
    array_texture.subresources.emplace_back(0, 1, array_texture.data.size(), texture.data.size());
    array_texture.data.insert(array_texture.data.end(), texture.data.begin(), texture.data.end());
    if (!expect(TextureResourceCodec::validate(array_texture),
                "native array layers share a complete mip layout"))
    {
        return false;
    }
    array_texture.cube_map = true;
    if (!expect(!TextureResourceCodec::validate(array_texture),
                "cube arrays are explicitly unsupported"))
    {
        return false;
    }
    schema::ShaderDataT shader(*builtin_shader);
    shader.path = "lit.shader";
    schema::MaterialDataT material(*builtin_material);
    material.type_path = "lit.material";
    material.shader_path = shader.path;
    auto editor_mesh = mesh;
    auto editor_texture = texture;
    auto editor_shader = shader;
    auto editor_material = material;
    editor_mesh.path = "editor://preview.mesh";
    editor_texture.path = "editor://preview.png";
    editor_shader.path = "editor://preview.shader";
    editor_material.type_path = "editor://preview.material";
    if (!expect(!MeshResourceCodec::validate(editor_mesh) &&
                    !TextureResourceCodec::validate(editor_texture) &&
                    !ShaderResourceCodec::validate(editor_shader) &&
                    !MaterialResourceCodec::validate(editor_material, &shader),
                "runtime codecs reject the reserved editor namespace"))
    {
        return false;
    }
    auto roughness = std::make_unique<schema::MaterialParameterT>();
    roughness->name = "roughness";
    schema::FloatValueT roughness_value;
    roughness_value.value = 0.4;
    roughness->value.Set(roughness_value);
    material.parameters.push_back(std::move(roughness));
    if (!fixture.add(mesh) || !fixture.add(texture) || !fixture.add(shader) ||
        !fixture.add(material))
    {
        return false;
    }
    schema::SceneDataT scene;
    scene.schema_version = 6;
    scene.info = std::make_unique<schema::SceneInfoT>();
    auto node = std::make_unique<schema::NodeDataT>();
    node->info = test_support::node_info();
    node->info->properties->id = 1;
    auto component = test_support::component_data();
    schema::MeshRendererDataT renderer;
    renderer.mesh_guid = fixture.catalog.entries[0]->guid;
    renderer.material_guid = fixture.catalog.entries[3]->guid;
    component->payload.Set(renderer);
    node->components.push_back(std::move(component));
    scene.nodes.push_back(std::move(node));
    const std::array raw{std::byte{0}, std::byte{42}, std::byte{255}};
    constexpr std::string_view json = "{\n  \"weapon\": \"carbine\", \"damage\": 24\n}\n";
    if (!add_payload(fixture, "main.scene", schema::AssetKind::scene, pack(scene, "ESCN"),
                     {renderer.mesh_guid, renderer.material_guid}) ||
        !add_payload(fixture, "data", schema::AssetKind::source, raw) ||
        !add_payload(fixture, "main.ts", schema::AssetKind::script, {}) ||
        !add_payload(fixture, "config/weapon.json", schema::AssetKind::source,
                     std::as_bytes(std::span(json.data(), json.size()))) ||
        !fixture.publish(resources))
    {
        return false;
    }
    ResourceError dependency_error{};
    std::string message;
    std::unique_ptr<Resource> mesh_resource;
    std::unique_ptr<Resource> texture_resource;
    std::unique_ptr<Resource> shader_resource;
    std::unique_ptr<Resource> material_resource;
    std::unique_ptr<Resource> scene_resource;
    std::unique_ptr<Resource> binary_resource;
    std::unique_ptr<Resource> script_resource;
    std::unique_ptr<Resource> json_resource;
    if (!fixture.read(resources, mesh.path, mesh_resource))
    {
        return false;
    }
    const auto* loaded_mesh = resource_detail::data<MeshResource>(mesh_resource.get());
    if (!expect(loaded_mesh && mesh_storage::is_compact(*loaded_mesh),
                "file meshes use compact storage"))
    {
        return false;
    }
    auto expanded_mesh = *loaded_mesh;
    if (!expect(mesh_storage::expand(expanded_mesh) && expanded_mesh == mesh &&
                    fixture.read(resources, texture.path, texture_resource) &&
                    resource_detail::data<TextureResource>(texture_resource.get()) &&
                    *resource_detail::data<TextureResource>(texture_resource.get()) == texture &&
                    fixture.read(resources, shader.path, shader_resource) &&
                    resource_detail::data<ShaderResource>(shader_resource.get()) &&
                    *resource_detail::data<ShaderResource>(shader_resource.get()) == shader &&
                    fixture.read(resources, material.type_path, material_resource) &&
                    resource_detail::data<MaterialResource>(material_resource.get()) &&
                    resource_detail::data<MaterialResource>(material_resource.get())->shader_path ==
                        shader.path &&
                    fixture.read(resources, "main.scene", scene_resource) &&
                    resource_detail::data<SceneResource>(scene_resource.get()) &&
                    *resource_detail::data<SceneResource>(scene_resource.get()) == scene,
                "all structured kinds decode to their FlatBuffers models") ||
        !expect(fixture.read(resources, "data", binary_resource) &&
                    (*resource_detail::data<SourceResource>(binary_resource.get())).size() == 3 &&
                    (*resource_detail::data<SourceResource>(binary_resource.get()))[0] == raw[0] &&
                    (*resource_detail::data<SourceResource>(binary_resource.get()))[2] == raw[2] &&
                    fixture.read(resources, "main.ts", script_resource) &&
                    (*resource_detail::data<ScriptResource>(script_resource.get())).empty(),
                "raw source retains embedded zeros and empty script is a valid byte payload") ||
        !expect(fixture.read(resources, "config/weapon.json", json_resource) &&
                    json_resource->info().kind == schema::AssetKind::source &&
                    json_resource->info().dependencies.empty() &&
                    (*resource_detail::data<SourceResource>(json_resource.get())).size() ==
                        json.size() &&
                    std::string_view(
                        reinterpret_cast<const char*>(
                            (*resource_detail::data<SourceResource>(json_resource.get())).data()),
                        (*resource_detail::data<SourceResource>(json_resource.get())).size()) ==
                        json,
                "JSON configuration is a source resource retaining its original text bytes") ||
        !expect(resources.validate_dependencies(
                    *scene_resource, std::array{mesh_resource.get(), material_resource.get()},
                    dependency_error, message) &&
                    !resources.validate_dependencies(
                        *scene_resource, std::array{mesh_resource.get(), texture_resource.get()},
                        dependency_error, message),
                "type codec validates only explicitly supplied scene references") ||
        !expect(!resources.add<SourceResourceCodec>() && !resources.add<ScriptResourceCodec>(),
                "codec registration is explicit and frozen after initialization"))
    {
        return false;
    }

    for (const auto* uniform :
         {"u_shadow_matrices", "u_shadow_rects", "u_shadow_splits", "u_shadow_params",
          "u_ambient_occlusion", "s_ambient_occlusion", "u_assao_params", "u_assao_rect",
          "u_hdr_params", "u_hdr_texel", "s_hdr_scene", "s_hdr_bloom"})
    {
        schema::ShaderDataT reserved(*builtin_shader);
        reserved.path = "builtin://shaders/reserved-renderer";
        reserved.parameters.front()->uniform_name = uniform;
        if (!expect(!ShaderResourceCodec::validate(reserved),
                    "material parameters cannot claim renderer uniforms"))
        {
            return false;
        }
        reserved.parameters.front()->uniform_name =
            builtin_shader->parameters.front()->uniform_name;
        reserved.samplers.front()->uniform_name = uniform;
        if (!expect(!ShaderResourceCodec::validate(reserved),
                    "material samplers cannot claim renderer uniforms"))
        {
            return false;
        }
    }

    schema::ShaderDataT sampler_boundary(*builtin_shader);
    sampler_boundary.samplers.resize(1);
    sampler_boundary.samplers.front()->stage = 4;
    if (!expect(ShaderResourceCodec::validate(sampler_boundary),
                "material sampler slot 4 remains available"))
    {
        return false;
    }
    for (std::uint8_t stage = 5; stage < 16; ++stage)
    {
        sampler_boundary.samplers.front()->stage = stage;
        if (!expect(!ShaderResourceCodec::validate(sampler_boundary),
                    "material samplers cannot occupy engine frame resource slots"))
        {
            return false;
        }
    }

    auto& dynamic_sampler = *sampler_boundary.samplers.front();
    dynamic_sampler.source = schema::ShaderTextureSource::scene_color;
    dynamic_sampler.stage = 9;
    dynamic_sampler.default_texture.clear();
    dynamic_sampler.exposed = false;
    if (!expect(ShaderResourceCodec::validate(sampler_boundary),
                "scene color uses its fixed slot without an asset dependency"))
    {
        return false;
    }
    dynamic_sampler.stage = 8;
    if (!expect(!ShaderResourceCodec::validate(sampler_boundary),
                "scene color cannot occupy the planar reflection slot"))
    {
        return false;
    }
    dynamic_sampler.source = schema::ShaderTextureSource::planar_reflection;
    if (!expect(ShaderResourceCodec::validate(sampler_boundary), "planar reflection uses slot 8"))
    {
        return false;
    }
    dynamic_sampler.exposed = true;
    if (!expect(!ShaderResourceCodec::validate(sampler_boundary),
                "dynamic scene inputs cannot be overridden as resource textures"))
    {
        return false;
    }
    dynamic_sampler.exposed = false;
    dynamic_sampler.default_texture = "builtin://textures/white";
    if (!expect(!ShaderResourceCodec::validate(sampler_boundary),
                "dynamic scene inputs cannot declare a fallback asset dependency"))
    {
        return false;
    }

    // Catalog 和信封有效，但各类型实体错误；加载不能发布部分候选。
    mesh.path = "invalid.mesh";
    mesh.indices[0] = static_cast<std::uint32_t>(mesh.vertices.size());
    texture.path = "invalid.png";
    texture.width = 0;
    shader.path = "invalid.shader";
    shader.parameters.front()->uniform_name = "u_view";
    material.type_path = "invalid.material";
    material.parameters.push_back(
        std::make_unique<schema::MaterialParameterT>(*material.parameters.front()));
    scene.schema_version = 1;
    if (!fixture.add(mesh) || !fixture.add(texture) || !fixture.add(shader) ||
        !fixture.add(material) ||
        !add_payload(fixture, "invalid.scene", schema::AssetKind::scene, pack(scene, "ESCN"),
                     {renderer.mesh_guid, renderer.material_guid}) ||
        !add_payload(fixture, "wrong-kind.mesh", schema::AssetKind::mesh, pack(texture, "ETEX")) ||
        !fixture.publish(resources))
    {
        return false;
    }
    for (const auto* path : {"invalid.mesh", "invalid.png", "invalid.shader", "invalid.material",
                             "invalid.scene", "wrong-kind.mesh"})
    {
        std::unique_ptr<Resource> output;
        if (!expect(!fixture.read(resources, path, output) && !output &&
                        !resources.last_error().empty(),
                    "invalid typed entity clears the output resource"))
        {
            return false;
        }
    }
    if (!expect(resource_detail::data<MeshResource>(mesh_resource.get()) &&
                    resource_detail::data<MeshResource>(mesh_resource.get())->path == "cube.mesh" &&
                    mesh_resource->generation() < resources.generation() &&
                    resource_detail::data<SceneResource>(scene_resource.get())->schema_version == 6,
                "failed new-generation decoding never mutates old live models"))
    {
        return false;
    }

    schema::AssetEntryT memory_info;
    memory_info.guid = "40000000-0000-4000-8000-000000000001";
    memory_info.path = "builtin://meshes/rejected";
    memory_info.kind = schema::AssetKind::mesh;
    mesh.path = memory_info.path;
    MeshResource invalid_memory(memory_info, mesh);
    return expect(!resources.validate_memory(invalid_memory),
                  "invalid native memory payload is rejected before graph admission");
}

bool movie_codec_tests()
{
    ResourceSystem resources;
    if (!test_support::register_resource_codecs(resources) || !resources.initialize({}))
    {
        return false;
    }
    test_support::ResourceFixture fixture;
    schema::MovieDataT movie;
    movie.video = std::make_unique<schema::MovieVideoDataT>();
    auto& video = *movie.video;
    video.width = 2;
    video.height = 2;
    video.frame_rate_num = 30;
    video.frame_rate_den = 1;
    video.time_base_num = 1;
    video.time_base_den = 30;
    video.duration_ticks = 1;
    video.frame_count = 1;
    video.color = std::make_unique<schema::MovieColorInfo>();
    // A bounded IVF envelope; VP9 bitstream validity is checked by the decoder, not this codec.
    video.payload = {'D', 'K', 'I', 'F', 0, 0, 32, 0, 'V', 'P', '9', '0', 2, 0, 2,
                     0,   30,  0,   0,   0, 1, 0,  0, 0,   1,   0,   0,   0, 0, 0,
                     0,   0,   1,   0,   0, 0, 0,  0, 0,   0,   0,   0,   0, 0, 42};
    if (!add_payload(fixture, "valid.mov", schema::AssetKind::movie, pack(movie, "EMOV")))
    {
        return false;
    }
    video.payload[32] = 2; // Claims two bytes for a one-byte frame.
    if (!add_payload(fixture, "truncated.mov", schema::AssetKind::movie, pack(movie, "EMOV")))
    {
        return false;
    }
    video.payload[32] = 1;
    video.payload[36] = 1; // The first timestamp must be zero.
    if (!add_payload(fixture, "bad-time.mov", schema::AssetKind::movie, pack(movie, "EMOV")) ||
        !fixture.publish(resources))
    {
        return false;
    }
    ResourceError dependency_error{};
    std::string message;
    std::unique_ptr<Resource> value;
    return expect(fixture.read(resources, "valid.mov", value) && value &&
                      resource_detail::data<MovieResource>(value.get()) != nullptr,
                  "valid movie envelope loads") &&
           expect(resources.validate_dependencies(*value, {}, dependency_error, message),
                  "silent movie has no dependencies") &&
           expect((value.reset(), !fixture.read(resources, "truncated.mov", value)) && !value,
                  "truncated IVF frame is rejected") &&
           expect(!fixture.read(resources, "bad-time.mov", value) && !value,
                  "invalid IVF timestamp is rejected");
}

} // namespace

int main()
{
    return codec_tests() && movie_codec_tests() ? 0 : 1;
}
