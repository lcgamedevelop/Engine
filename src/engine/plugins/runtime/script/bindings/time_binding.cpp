#include "plugins/runtime/script/bindings/time_binding.hpp"
#include "runtime/performance/profiling.hpp"

#include "plugins/runtime/script/bindings/binding_context.hpp"

namespace engine::plugins::script::bindings
{
namespace
{
constexpr std::string_view javascript = R"js(
export class Time {
    constructor() { throw new TypeError('Time is static'); }
    static get deltaTime() { return native.DeltaTime(); }
}
Object.freeze(Time.prototype);
Object.freeze(Time);
)js";

void delta_time(const Arguments& arguments)
{
    ENGINE_PROFILE_FUNCTION();
    auto* binding = BindingContext::current(arguments);
    arguments.set_return(binding == nullptr ? 0.0 : binding->delta_time());
}
} // namespace

std::string_view TimeBinding::name() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return "Time";
}

std::string_view TimeBinding::source() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return javascript;
}

bool TimeBinding::register_functions(vm::ScriptVm& vm) const
{
    ENGINE_PROFILE_FUNCTION();
    return vm.function("DeltaTime", delta_time);
}

} // namespace engine::plugins::script::bindings
