#include "editor/asset_pipeline/importers/source_importer.hpp"
#include "runtime/performance/profiling.hpp"

#include "editor/asset_pipeline/asset_io.hpp"

namespace engine::editor::asset_pipeline
{
schema::AssetKind SourceImporter::kind() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return schema::AssetKind::source;
}
bool SourceImporter::matches(const std::filesystem::path&) const
{
    ENGINE_PROFILE_FUNCTION();
    return true;
}
bool SourceImporter::inspect_batch(const ImportContext&, std::string& fingerprint, std::string&)
{
    ENGINE_PROFILE_FUNCTION();
    fingerprint = "source-copy-2";
    return true;
}
bool SourceImporter::convert(const ImportAsset& asset, const ImportContext& context,
                             std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    std::vector<std::byte> bytes;
    return read_bytes(asset.source, bytes, error) &&
           write_bytes(context.output / asset.guid, bytes, error);
}
} // namespace engine::editor::asset_pipeline
