#include "editor/asset_pipeline/asset_importer.hpp"
#include "runtime/performance/profiling.hpp"

namespace engine::editor::asset_pipeline
{
std::string_view AssetImporter::id() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return schema::EnumNameAssetKind(kind());
}
bool AssetImporter::discover(const ImportAsset&, std::vector<ImportSubresource>& output,
                             std::string&) const
{
    ENGINE_PROFILE_FUNCTION();
    output.clear();
    return true;
}
std::string_view AssetImporter::default_meta() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return {};
}
std::string AssetImporter::module_path(const std::filesystem::path&) const
{
    ENGINE_PROFILE_FUNCTION();
    return {};
}
bool AssetImporter::inspect_batch(const ImportContext&, std::string& fingerprint, std::string&)
{
    ENGINE_PROFILE_FUNCTION();
    fingerprint.clear();
    return true;
}
bool AssetImporter::finalize_dependencies(ImportAsset&, const ImportContext&, std::string&) const
{
    ENGINE_PROFILE_FUNCTION();
    return true;
}

bool AssetImporter::scan_dependencies(ImportAsset& asset, const ImportContext&, std::string&) const
{
    ENGINE_PROFILE_FUNCTION();
    asset.build_dependencies.clear();
    asset.runtime_dependencies.clear();
    asset.stream_dependencies.clear();
    return true;
}
bool AssetImporter::prepare_batch(const ImportContext&, TaskDescription& task, std::string&) const
{
    ENGINE_PROFILE_FUNCTION();
    task = {};
    return true;
}
} // namespace engine::editor::asset_pipeline
