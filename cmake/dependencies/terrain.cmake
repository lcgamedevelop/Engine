if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/fastnoise2/CMakeLists.txt")
    message(FATAL_ERROR "FastNoise2 is missing; run git submodule update --init")
endif()
set(FASTNOISE2_TOOLS OFF CACHE BOOL "Build the upstream noise editor" FORCE)
set(FASTNOISE2_TESTS OFF CACHE BOOL "Build upstream noise tests" FORCE)
set(FASTNOISE2_UTILITY OFF CACHE BOOL "Build upstream noise utilities" FORCE)
set(FASTNOISE2_STRICT_FP ON CACHE BOOL "Use reproducible terrain noise" FORCE)
if(NOT CPM_SOURCE_CACHE)
    set(CPM_SOURCE_CACHE "${PROJECT_SOURCE_DIR}/build/dependencies/cpm" CACHE PATH
        "Shared immutable upstream source cache")
endif()
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/fastnoise2"
                 "${CMAKE_CURRENT_BINARY_DIR}/fastnoise2" EXCLUDE_FROM_ALL)
get_target_property(_terrain_noise_includes FastNoise INTERFACE_INCLUDE_DIRECTORIES)
set_property(TARGET FastNoise APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
             "${_terrain_noise_includes}")
