#pragma once

#include "plugins/runtime/script/vm/script_vm.hpp"
#include "plugins/runtime/script/vm/text_encoding.hpp"

namespace engine::plugins::script::bindings::binding_detail
{
[[nodiscard]] inline bool decode_text(ByteView bytes, std::string& output)
{
    output.clear();
    if (bytes.size() > vm::maximum_text_size)
    {
        return false;
    }
    if (bytes.empty())
    {
        return true;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!vm::valid_utf8(text))
    {
        return false;
    }
    output.assign(text);
    return true;
}
} // namespace engine::plugins::script::bindings::binding_detail
