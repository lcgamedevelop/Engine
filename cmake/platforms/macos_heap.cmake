include_guard(GLOBAL)

function(engine_configure_macos_heap_observation target)
    add_library(engine_macos_heap_hooks SHARED platforms/macos/performance/heap_interpose.cpp)
    target_include_directories(engine_macos_heap_hooks PRIVATE "${PROJECT_SOURCE_DIR}/platforms")
    target_compile_features(engine_macos_heap_hooks PRIVATE cxx_std_20)
    set_target_properties(engine_macos_heap_hooks PROPERTIES
        CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden OUTPUT_NAME engine-macos-heap-hooks)
    engine_apply_compiler_cache(engine_macos_heap_hooks)
    target_compile_options(engine_macos_heap_hooks PRIVATE
        -Wall -Wextra -Wpedantic -fno-exceptions -fno-builtin)
    if(ENGINE_WARNINGS_AS_ERRORS)
        target_compile_options(engine_macos_heap_hooks PRIVATE -Werror)
    endif()
    target_sources(${target} PRIVATE platforms/macos/performance/heap_observer.cpp)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/platforms")
    target_compile_definitions(${target} PRIVATE ENGINE_ENABLE_MACOS_HEAP_OBSERVATION=1)
    target_link_libraries(${target} PRIVATE engine_macos_heap_hooks)
endfunction()
