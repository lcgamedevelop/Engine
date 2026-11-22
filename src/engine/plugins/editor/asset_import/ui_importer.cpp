#include "plugins/editor/asset_import/ui_importer.hpp"
#include "runtime/performance/profiling.hpp"

#include "editor/asset_pipeline/asset_io.hpp"
#include "editor/asset_pipeline/importer_utils.hpp"
#include "runtime/resource/codecs/ui_resource_codec.hpp"
#include "runtime/schema/ui_generated.h"

#include <flatbuffers/flatbuffers.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::plugins::asset_import
{
namespace
{
constexpr std::size_t maximum_markup_size = 1024U * 1024U;

bool whitespace(char value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool name_character(char value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '_' || value == ':';
}

std::string lowercase(std::string_view text)
{
    ENGINE_PROFILE_FUNCTION();
    std::string result(text);
    for (auto& value : result)
    {
        if (value >= 'A' && value <= 'Z')
        {
            value = static_cast<char>(value + ('a' - 'A'));
        }
    }
    return result;
}

bool resolve(std::string_view name, schema::AssetKind kind,
             const editor::asset_pipeline::ImportContext& context, Guid& guid, std::string& path,
             std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    if (GuidMap::valid_guid(name))
    {
        guid = name;
        if (!context.identities.guid_to_path(guid, path))
        {
            error = "UI resource GUID is missing: " + guid;
            return false;
        }
    }
    else
    {
        path = name;
        if (!context.identities.path_to_guid(path, guid))
        {
            error = "UI resource path is missing: " + path;
            return false;
        }
    }
    const auto entry = context.kinds.find(guid);
    if (entry == context.kinds.end() || entry->second != kind)
    {
        error = "UI resource has an incompatible type: " + path;
        return false;
    }
    return true;
}

bool read_references(const toml::table& settings, const char* field, schema::AssetKind kind,
                     const editor::asset_pipeline::ImportContext& context,
                     std::vector<std::string>& output, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    const auto* references = settings[field].as_array();
    const std::size_t maximum = kind == schema::AssetKind::font ? 64U : 256U;
    if (references == nullptr || references->size() > maximum)
    {
        error = "UI " + std::string(field) + " must be an array of at most " +
                std::to_string(maximum) + " resources";
        return false;
    }
    for (const auto& reference : *references)
    {
        const auto name = reference.value<std::string>();
        std::string path;
        Guid guid;
        if (!name || name->empty() || !resolve(*name, kind, context, guid, path, error))
        {
            if (error.empty())
            {
                error = "UI resource reference must be a nonempty string";
            }
            return false;
        }
        if (std::find(output.begin(), output.end(), guid) != output.end())
        {
            error = "UI resource is declared more than once: " + path;
            return false;
        }
        output.push_back(std::move(guid));
    }
    return true;
}

bool read_settings(const std::filesystem::path& source,
                   const editor::asset_pipeline::ImportContext& context,
                   schema::UiDocumentDataT& document, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    auto meta = source;
    meta += ".meta";
    toml::table root;
    if (!editor::asset_pipeline::read_table(meta, root, error))
    {
        return false;
    }
    const auto* table = root["ui"].as_table();
    if (table == nullptr)
    {
        error = "UI meta requires a [ui] table";
        return false;
    }
    for (const auto& [key, value] : *table)
    {
        static_cast<void>(value);
        if (key != "fonts" && key != "textures")
        {
            error = "unknown UI meta setting: " + std::string(key.str());
            return false;
        }
    }
    if (table->contains("textures") &&
        !read_references(*table, "textures", schema::AssetKind::texture, context,
                         document.texture_guids, error))
    {
        return false;
    }
    const auto* fonts = (*table)["fonts"].as_array();
    if (fonts == nullptr || fonts->empty() || fonts->size() > 64)
    {
        error = "UI requires 1..64 [[ui.fonts]] bindings";
        return false;
    }
    std::set<std::string> families;
    for (const auto& item : *fonts)
    {
        const auto* font = item.as_table();
        if (font == nullptr)
        {
            error = "UI font binding requires a table containing family, asset and optional shader";
            return false;
        }
        for (const auto& [key, value] : *font)
        {
            static_cast<void>(value);
            if (key != "family" && key != "asset" && key != "shader")
            {
                error = "unknown UI font binding setting: " + std::string(key.str());
                return false;
            }
        }
        const auto family = (*font)["family"].value<std::string>();
        const auto asset = (*font)["asset"].value<std::string>();
        auto binding = std::make_unique<schema::UiFontBindingT>();
        if (!family || !asset || asset->empty())
        {
            error = "UI font binding requires nonempty family and asset strings";
            return false;
        }
        binding->family = lowercase(*family);
        if (!UiResourceCodec::valid_family(binding->family) ||
            !families.insert(binding->family).second)
        {
            error = "UI font family aliases must be valid and unique without case sensitivity";
            return false;
        }
        std::string shader_name = "builtin://shaders/font";
        if (font->contains("shader"))
        {
            const auto name = (*font)["shader"].value<std::string>();
            if (!name || name->empty())
            {
                error = "UI font shader must be a nonempty resource path or GUID";
                return false;
            }
            shader_name = *name;
        }
        std::string path;
        Guid shader_guid;
        if (!resolve(*asset, schema::AssetKind::font, context, binding->font_guid, path, error) ||
            !resolve(shader_name, schema::AssetKind::shader, context, shader_guid,
                     binding->shader_path, error))
        {
            return false;
        }
        document.fonts.push_back(std::move(binding));
    }
    return true;
}

bool declared_texture(std::string_view source, const schema::UiDocumentDataT& document,
                      const editor::asset_pipeline::ImportContext& context)
{
    Guid guid;
    std::string path, error;
    return !source.empty() && source.find('&') == source.npos &&
           resolve(source, schema::AssetKind::texture, context, guid, path, error) &&
           std::find(document.texture_guids.begin(), document.texture_guids.end(), guid) !=
               document.texture_guids.end();
}

std::string_view trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == text.npos ? std::string_view{}
                              : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool validate_style(std::string_view text, const schema::UiDocumentDataT& document,
                    const editor::asset_pipeline::ImportContext& context, bool stylesheet,
                    std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    std::string css(text);
    for (auto begin = css.find("/*"); begin != css.npos; begin = css.find("/*", begin + 2))
    {
        const auto end = css.find("*/", begin + 2);
        if (end == css.npos)
        {
            error = "UI stylesheet comment is unterminated";
            return false;
        }
        std::fill(css.begin() + begin, css.begin() + end + 2, ' ');
    }
    const auto lower = lowercase(css);
    std::string normalized;
    for (const auto character : lower)
    {
        if (!whitespace(character))
        {
            normalized.push_back(character);
        }
    }
    for (const auto* external : {"@import", "@font-face", "url(", "image(", "fill-image:"})
    {
        if (normalized.find(external) != normalized.npos)
        {
            error = "UI external CSS resources are unsupported";
            return false;
        }
    }
    constexpr std::string_view sheet_rule = "@spritesheet";
    const std::string_view sheet(css);
    for (auto at = lower.find(sheet_rule); at != lower.npos; at = lower.find(sheet_rule, at))
    {
        const auto open = sheet.find_first_of("{}", at + sheet_rule.size());
        const auto end = open == sheet.npos ? sheet.npos : sheet.find_first_of("{}", open + 1);
        if (!stylesheet || open == sheet.npos || sheet[open] != '{' || end == sheet.npos ||
            sheet[end] != '}' ||
            trimmed(sheet.substr(at + sheet_rule.size(), open - at - sheet_rule.size())).empty())
        {
            error = "UI spritesheets require a named inline stylesheet block";
            return false;
        }
        auto fields = sheet.substr(open + 1, end - open - 1);
        bool found = false;
        while (!fields.empty())
        {
            const auto next = fields.find(';');
            const auto field = fields.substr(0, next);
            const auto colon = field.find(':');
            if (colon != field.npos && lowercase(trimmed(field.substr(0, colon))) == "src")
            {
                auto source = trimmed(field.substr(colon + 1));
                if (source.size() >= 2 && (source.front() == '\'' || source.front() == '"') &&
                    source.back() == source.front())
                {
                    source = source.substr(1, source.size() - 2);
                }
                if (found || !declared_texture(source, document, context))
                {
                    error =
                        "UI spritesheet src must identify one texture declared in [ui].textures";
                    return false;
                }
                found = true;
            }
            fields = next == fields.npos ? std::string_view{} : fields.substr(next + 1);
        }
        if (!found)
        {
            error = "UI spritesheet requires a declared src texture";
            return false;
        }
        at = end + 1;
    }
    return true;
}

bool validate_markup(const schema::UiDocumentDataT& document,
                     const editor::asset_pipeline::ImportContext& context, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    const std::string_view text = document.markup;
    if (text.empty() || text.size() > maximum_markup_size || text.find('\0') != std::string::npos)
    {
        error = "UI markup must be nonempty, at most 1 MiB and contain no NUL bytes";
        return false;
    }
    const auto lower = lowercase(text);
    bool root = false;
    bool body = false;
    for (std::size_t cursor = 0; cursor < text.size();)
    {
        const auto opening = text.find('<', cursor);
        if (opening == std::string_view::npos)
        {
            break;
        }
        cursor = opening + 1;
        if (text.substr(opening).starts_with("<!--"))
        {
            const auto end = text.find("-->", opening + 4);
            if (end == std::string_view::npos)
            {
                error = "UI comment is unterminated";
                return false;
            }
            cursor = end + 3;
            continue;
        }
        bool closing = cursor < text.size() && text[cursor] == '/';
        if (closing)
        {
            ++cursor;
        }
        const auto tag_begin = cursor;
        while (cursor < text.size() && name_character(text[cursor]))
        {
            ++cursor;
        }
        if (cursor == tag_begin)
        {
            error = "UI markup has an invalid tag; escape literal angle brackets";
            return false;
        }
        const auto tag = std::string_view(lower).substr(tag_begin, cursor - tag_begin);
        if (tag == "script" || tag == "link" || tag == "template")
        {
            error = "UI external scripts, stylesheets and templates are unsupported";
            return false;
        }
        root = root || (!closing && tag == "rml");
        body = body || (!closing && tag == "body");
        std::string_view image_source;
        bool has_source = false;
        while (cursor < text.size())
        {
            while (cursor < text.size() && whitespace(text[cursor]))
            {
                ++cursor;
            }
            if (cursor >= text.size() || text[cursor] == '>' || text[cursor] == '/')
            {
                break;
            }
            const auto attribute_begin = cursor;
            while (cursor < text.size() && name_character(text[cursor]))
            {
                ++cursor;
            }
            if (attribute_begin == cursor || closing)
            {
                error = "UI markup has an invalid attribute";
                return false;
            }
            const auto attribute =
                std::string_view(lower).substr(attribute_begin, cursor - attribute_begin);
            while (cursor < text.size() && whitespace(text[cursor]))
            {
                ++cursor;
            }
            std::string_view value;
            if (cursor < text.size() && text[cursor] == '=')
            {
                ++cursor;
                while (cursor < text.size() && whitespace(text[cursor]))
                {
                    ++cursor;
                }
                if (cursor >= text.size() || (text[cursor] != '\'' && text[cursor] != '"'))
                {
                    error = "UI attribute values must be quoted";
                    return false;
                }
                const char quote = text[cursor++];
                const auto end = text.find(quote, cursor);
                if (end == std::string_view::npos)
                {
                    error = "UI attribute value is unterminated";
                    return false;
                }
                value = text.substr(cursor, end - cursor);
                cursor = end + 1;
            }
            if (attribute == "src")
            {
                if (tag != "img" || has_source)
                {
                    error = "UI src is supported only once on an img element";
                    return false;
                }
                has_source = true;
                image_source = value;
            }
            if (attribute == "template" || attribute == "data-model")
            {
                error = "UI templates and script data models are not enabled";
                return false;
            }
            if (attribute == "style" && !validate_style(value, document, context, false, error))
            {
                return false;
            }
        }
        if (cursor < text.size() && text[cursor] == '/')
        {
            ++cursor;
        }
        if (cursor >= text.size() || text[cursor] != '>')
        {
            error = "UI tag is unterminated";
            return false;
        }
        ++cursor;
        if (!closing && tag == "img")
        {
            if (!has_source || !declared_texture(image_source, document, context))
            {
                error = "UI img src must identify a texture declared in [ui].textures";
                return false;
            }
        }
        if (!closing && tag == "style")
        {
            const auto end = lower.find("</style", cursor);
            if (end == std::string::npos ||
                !validate_style(text.substr(cursor, end - cursor), document, context, true, error))
            {
                if (error.empty())
                {
                    error = "UI style element is unterminated";
                }
                return false;
            }
            cursor = end;
        }
    }
    if (!root || !body)
    {
        error = "UI document requires rml and body elements";
        return false;
    }
    return true;
}

bool read_document(const editor::asset_pipeline::ImportAsset& asset,
                   const editor::asset_pipeline::ImportContext& context,
                   schema::UiDocumentDataT& document, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    std::vector<std::byte> bytes;
    if (!read_settings(asset.source, context, document, error) ||
        !editor::asset_pipeline::read_bytes(asset.source, bytes, error))
    {
        return false;
    }
    if (bytes.empty() || bytes.size() > maximum_markup_size)
    {
        error = "UI source must be nonempty and at most 1 MiB";
        return false;
    }
    document.markup.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return validate_markup(document, context, error);
}
} // namespace

std::string_view UiImporter::id() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return "ui";
}

schema::AssetKind UiImporter::kind() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return schema::AssetKind::ui;
}

bool UiImporter::matches(const std::filesystem::path& source) const
{
    ENGINE_PROFILE_FUNCTION();
    return editor::asset_pipeline::source_extension(source) == ".rml";
}

std::string_view UiImporter::default_meta() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return "\n[ui]\ntextures = []\n\n[[ui.fonts]]\nfamily = \"body\"\nasset = \"\"\n"
           "shader = \"builtin://shaders/font\"\n";
}

bool UiImporter::inspect_batch(const editor::asset_pipeline::ImportContext&,
                               std::string& fingerprint, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    if (!editor::asset_pipeline::schema_fingerprint({"runtime/ui.fbs"}, fingerprint, error))
    {
        return false;
    }
    fingerprint += ":ui-importer-3:rmlui";
    return true;
}

bool UiImporter::scan_dependencies(editor::asset_pipeline::ImportAsset& asset,
                                   const editor::asset_pipeline::ImportContext& context,
                                   std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    asset.build_dependencies.clear();
    asset.runtime_dependencies.clear();
    schema::UiDocumentDataT document;
    if (!read_document(asset, context, document, error))
    {
        return false;
    }
    asset.build_dependencies = document.texture_guids;
    for (const auto& binding : document.fonts)
    {
        Guid guid;
        if (!context.identities.path_to_guid(binding->shader_path, guid))
        {
            error = "UI font shader disappeared from the import catalog";
            return false;
        }
        asset.build_dependencies.push_back(binding->font_guid);
        asset.build_dependencies.push_back(std::move(guid));
    }
    std::sort(asset.build_dependencies.begin(), asset.build_dependencies.end());
    asset.build_dependencies.erase(
        std::unique(asset.build_dependencies.begin(), asset.build_dependencies.end()),
        asset.build_dependencies.end());
    asset.runtime_dependencies = asset.build_dependencies;
    return true;
}

bool UiImporter::convert(const editor::asset_pipeline::ImportAsset& asset,
                         const editor::asset_pipeline::ImportContext& context,
                         std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    schema::UiDocumentDataT document;
    if (!read_document(asset, context, document, error))
    {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    schema::FinishUiDocumentDataBuffer(builder, schema::UiDocumentData::Pack(builder, &document));
    return editor::asset_pipeline::write_bytes(
        context.output / asset.guid,
        std::as_bytes(std::span(builder.GetBufferPointer(), builder.GetSize())), error);
}

} // namespace engine::plugins::asset_import
