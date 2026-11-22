function(engine_configure_google_play_plugin)
    add_library(engine_google_play STATIC src/engine/plugins/editor/google_play/google_play_publisher.cpp)
    add_library(engine::google_play ALIAS engine_google_play)
    target_link_libraries(engine_google_play PUBLIC engine::editor nlohmann_json::nlohmann_json
                          PRIVATE engine::asset_pipeline engine::task)
    target_compile_definitions(engine_google_play PRIVATE JSON_NOEXCEPTION=1)
    engine_apply_options(engine_google_play)
    if("editor/mcp" IN_LIST ENGINE_SELECTED_PLUGINS)
        add_library(engine_google_play_mcp_tools STATIC
                    src/engine/plugins/editor/google_play/google_play_mcp_tools.cpp)
        target_link_libraries(engine_google_play_mcp_tools PUBLIC engine::project_mcp_tools)
        engine_apply_options(engine_google_play_mcp_tools)
    endif()
endfunction()
engine_register_plugin(NAME editor/google_play CONFIGURE engine_configure_google_play_plugin
                       OPTIONS ENGINE_ENABLE_EDITOR_DEPS ENGINE_ENABLE_ASSET_PIPELINE_DEPS)
