#include "runtime/resource/builtin_font.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"

#include <font8x8_basic.h>

namespace engine::resource_detail
{
const schema::FontDataT& diagnostic_font()
{
    ENGINE_PROFILE_FUNCTION();
    static const schema::FontDataT data = []
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        schema::FontDataT font;
        font.family = "Engine Diagnostic";
        font.style = "Regular";
        font.units_per_em = 16;
        font.ascender = 1;
        font.descender = -0.125F;
        font.underline_position = -0.0625F;
        font.underline_thickness = 0.0625F;
        const auto line = [&](float x0, float y0, float x1, float y1)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            font.curves.emplace_back(x0, y0, (x0 + x1) * 0.5F, (y0 + y1) * 0.5F, x1, y1);
        };
        for (std::uint32_t code = 0; code < 127; ++code)
        {
            if (code != 0 && code < 32)
            {
                continue;
            }
            auto glyph = std::make_unique<schema::FontGlyphT>();
            glyph->codepoint = code;
            glyph->glyph_index = code;
            glyph->advance_x = 0.75F;
            glyph->max_x = 0.75F;
            glyph->max_y = 1;
            glyph->curve_offset = static_cast<std::uint32_t>(font.curves.size());
            const auto& bitmap = font8x8_basic[code == 0 ? '?' : code];
            for (unsigned y = 0; y < 8; ++y)
            {
                for (unsigned x = 0; x < 8; ++x)
                {
                    if (!(bitmap[y] & (1U << x)))
                    {
                        continue;
                    }
                    const float left = x * 0.75F / 8.F, right = (x + 1) * 0.75F / 8.F;
                    const float top = 1 - y / 8.F, bottom = 1 - (y + 1) / 8.F;
                    line(left, bottom, right, bottom);
                    line(right, bottom, right, top);
                    line(right, top, left, top);
                    line(left, top, left, bottom);
                }
            }
            glyph->curve_count =
                static_cast<std::uint32_t>(font.curves.size()) - glyph->curve_offset;
            font.glyphs.push_back(std::move(glyph));
        }
        return font;
    }();
    return data;
}
} // namespace engine::resource_detail
