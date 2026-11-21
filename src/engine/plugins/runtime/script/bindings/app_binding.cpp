#include "plugins/runtime/script/bindings/app_binding.hpp"
#include "runtime/performance/profiling.hpp"

#include "plugins/runtime/script/bindings/binding_context.hpp"
#include "runtime/app/app.hpp"
#include "runtime/utils/path_utils.hpp"
#include "runtime/window/window_system.hpp"

namespace engine::plugins::script::bindings
{
namespace
{
constexpr std::string_view javascript = R"js(
export class App {
    constructor() { throw new TypeError('App is static'); }
    static get assetsPath() { return native.assetsPath; }
    static get persistentDataPath() { return native.persistentDataPath; }
    static get cachePath() { return native.cachePath; }
    static get gameLanguage() { return native.gameLanguage; }
    static RequestQuit() { return arguments.length === 0 && native.AppRequestQuit(); }
    static SupportsVibration() { return arguments.length === 0 && native.AppSupportsVibration(); }
    static Vibrate(durationMs) { return arguments.length === 1 && native.AppVibrate(durationMs); }
}
Object.freeze(App.prototype);
Object.freeze(App);
)js";

void request_quit(const Arguments& arguments)
{
    ENGINE_PROFILE_FUNCTION();
    arguments.set_return(false);
    auto* context = BindingContext::current(arguments);
    auto* window = context != nullptr ? context->window() : nullptr;
    if (window != nullptr && arguments.count() == 0)
    {
        arguments.set_return(window->request_exit());
    }
}

void supports_vibration(const Arguments& arguments)
{
    ENGINE_PROFILE_FUNCTION();
    arguments.set_return(arguments.count() == 0 && App::supports_vibration());
}

void vibrate(const Arguments& arguments)
{
    ENGINE_PROFILE_FUNCTION();
    arguments.set_return(BindingContext::current(arguments) != nullptr &&
                         arguments.count() == 1 && arguments.is_uint32(0) &&
                         App::vibrate(arguments.unsigned_integer(0)));
}

} // namespace

std::string_view AppBinding::name() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return "App";
}

std::string_view AppBinding::source() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return javascript;
}

bool AppBinding::register_functions(vm::ScriptVm& vm) const
{
    ENGINE_PROFILE_FUNCTION();
    return vm.string("assetsPath", PathUtils::to_utf8(App::assets_path())) &&
           vm.string("persistentDataPath", PathUtils::to_utf8(App::persistent_data_path())) &&
           vm.string("cachePath", PathUtils::to_utf8(App::cache_path())) &&
           vm.string("gameLanguage", App::game_language()) &&
           vm.function("AppRequestQuit", request_quit) &&
           vm.function("AppSupportsVibration", supports_vibration) &&
           vm.function("AppVibrate", vibrate);
}

} // namespace engine::plugins::script::bindings
