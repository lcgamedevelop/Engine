function(engine_configure_terrain_plugin)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/transvoxel/Transvoxel.cpp")
        message(FATAL_ERROR "Transvoxel is missing; run git submodule update --init third_party/transvoxel")
    endif()
    engine_add_system(plugins/runtime terrain)
    add_library(engine_terrain_data STATIC
        src/engine/plugins/runtime/terrain/terrain_volume.cpp
        src/engine/plugins/runtime/terrain/terrain_samples.cpp
        src/engine/plugins/runtime/terrain/terrain_region_planner.cpp
        src/engine/plugins/runtime/terrain/terrain_mesher.cpp
        src/engine/plugins/runtime/terrain/terrain_islands.cpp)
    add_library(engine::terrain_data ALIAS engine_terrain_data)
    target_include_directories(engine_terrain_data PRIVATE "${PROJECT_SOURCE_DIR}/src/engine")
    target_include_directories(engine_terrain_data SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/transvoxel")
    target_link_libraries(engine_terrain_data PUBLIC engine::scene_data engine::core)
    engine_apply_options(engine_terrain_data)
    target_compile_definitions(engine_terrain PRIVATE JSON_NOEXCEPTION=1)
    target_sources(engine_terrain PRIVATE src/engine/plugins/runtime/terrain/codecs/terrain_resource_codec.cpp)
    target_sources(engine_terrain PRIVATE src/engine/plugins/runtime/terrain/codecs/terrain_block_resource_codec.cpp)
    target_sources(engine_terrain PRIVATE
        src/engine/plugins/runtime/terrain/terrain_component.cpp
        src/engine/plugins/runtime/terrain/terrain_control.cpp
        src/engine/plugins/runtime/terrain/terrain_fragment_runtime.cpp
        src/engine/plugins/runtime/terrain/terrain_state_store.cpp)
    target_include_directories(engine_terrain SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/transvoxel")
    target_link_libraries(engine_terrain PUBLIC engine::world engine::physics engine::resource engine::terrain_data engine::geometry
                                        PRIVATE SDL3::SDL3 engine::task engine::scene_ecs nlohmann_json::nlohmann_json)
endfunction()

engine_register_plugin(NAME runtime/terrain CONFIGURE engine_configure_terrain_plugin
                       OPTIONS ENGINE_ENABLE_RUNTIME_DEPS)
