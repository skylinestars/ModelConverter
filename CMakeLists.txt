cmake_minimum_required(VERSION 3.20)
project(ModelConverter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC: force UTF-8 source encoding for Chinese comments
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ============================================================
# GoogleTest (local, from thirdparty)
# ============================================================
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_build_tests OFF CACHE BOOL "" FORCE)
set(gtest_build_samples OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/thirdparty/assimp-6.0.5/contrib/googletest
    ${CMAKE_BINARY_DIR}/googletest
    EXCLUDE_FROM_ALL)

# ============================================================
# Include directories
# ============================================================
set(MC_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)
set(MC_SRC_DIR ${CMAKE_SOURCE_DIR}/src)

# ============================================================
# Common library (Logger)
# ============================================================
add_library(mc_common STATIC
    ${MC_SRC_DIR}/common/Logger.cpp
)
target_include_directories(mc_common PUBLIC ${MC_INCLUDE_DIR})
target_compile_features(mc_common PUBLIC cxx_std_17)

# ============================================================
# Core library (header-only data structures)
# ============================================================
add_library(mc_core INTERFACE)
target_include_directories(mc_core INTERFACE ${MC_INCLUDE_DIR})
target_link_libraries(mc_core INTERFACE mc_common)
target_compile_features(mc_core INTERFACE cxx_std_17)

# ============================================================
# PluginManager library
# ============================================================
add_library(mc_pluginmgr STATIC
    ${MC_SRC_DIR}/pluginmgr/PluginManager.cpp
)
target_include_directories(mc_pluginmgr PUBLIC ${MC_INCLUDE_DIR})
target_link_libraries(mc_pluginmgr PUBLIC mc_common mc_core)
target_compile_features(mc_pluginmgr PUBLIC cxx_std_17)

# ============================================================
# Pipeline library
# ============================================================
add_library(mc_pipeline STATIC
    ${MC_SRC_DIR}/pipeline/Pipeline.cpp
)
target_include_directories(mc_pipeline PUBLIC ${MC_INCLUDE_DIR})
target_link_libraries(mc_pipeline PUBLIC mc_common mc_core)
target_compile_features(mc_pipeline PUBLIC cxx_std_17)

# ============================================================
# Plugin DLLs
# ============================================================
add_subdirectory(plugins/dummy)

# ============================================================
# CLI executable
# ============================================================
add_executable(mc_cli
    ${MC_SRC_DIR}/app/main.cpp
)
target_link_libraries(mc_cli PRIVATE mc_common mc_core mc_pluginmgr mc_pipeline)
target_include_directories(mc_cli PRIVATE ${MC_INCLUDE_DIR})

# ============================================================
# Tests
# ============================================================
enable_testing()

add_executable(mc_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/LoggerTest.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/SceneCreateTest.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/SceneDestroyTest.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/SceneStressTest.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/PluginManagerTest.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/PipelinePassTest.cpp
)
target_link_libraries(mc_tests PRIVATE mc_common mc_core mc_pluginmgr mc_pipeline GTest::gtest GTest::gtest_main)
target_include_directories(mc_tests PRIVATE ${MC_INCLUDE_DIR})
target_compile_features(mc_tests PUBLIC cxx_std_17)

include(GoogleTest)
gtest_discover_tests(mc_tests)