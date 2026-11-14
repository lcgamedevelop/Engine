#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace engine::plugins::script::vm
{
[[nodiscard]] inline bool valid_utf8(std::string_view text) noexcept
{
    std::size_t index = 0;
    while (index < text.size())
    {
        const auto first = static_cast<unsigned char>(text[index++]);
        if (first < 0x80)
        {
            continue;
        }
        const unsigned count = first >= 0xc2 && first <= 0xdf   ? 1
                               : first >= 0xe0 && first <= 0xef ? 2
                               : first >= 0xf0 && first <= 0xf4 ? 3
                                                                : 0;
        if (count == 0 || text.size() - index < count)
        {
            return false;
        }
        std::uint32_t scalar = first & (0x7fU >> count);
        for (unsigned part = 0; part < count; ++part)
        {
            const auto next = static_cast<unsigned char>(text[index++]);
            if ((next & 0xc0) != 0x80)
            {
                return false;
            }
            scalar = (scalar << 6U) | (next & 0x3fU);
        }
        if ((count == 1 && scalar < 0x80) || (count == 2 && scalar < 0x800) ||
            (count == 3 && scalar < 0x10000) || scalar > 0x10ffff ||
            (scalar >= 0xd800 && scalar <= 0xdfff))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool valid_utf16(std::span<const std::uint16_t> text) noexcept
{
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        const auto unit = text[index];
        if (unit >= 0xd800 && unit <= 0xdbff)
        {
            if (++index == text.size() || text[index] < 0xdc00 || text[index] > 0xdfff)
            {
                return false;
            }
        }
        else if (unit >= 0xdc00 && unit <= 0xdfff)
        {
            return false;
        }
    }
    return true;
}
} // namespace engine::plugins::script::vm
