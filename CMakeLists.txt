cmake_minimum_required(VERSION 3.20)
project(ModelConverter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC: force UTF-8 source encoding for Chinese comments
# 注意：不能全局 add_compile_options(/utf-8)，会与 assimp 的 /source-charset 冲突
# 改为在 assimp 子目录之后，对本工程每个 target 单独设置

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
# Assimp (source, built as shared DLL on Windows)
# ============================================================
# 临时开启 BUILD_SHARED_LIBS 仅供 assimp 使用，之后立即恢复
set(_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS         ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES      OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT          OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL            OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_IGNORE_GIT_HASH    ON  CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/thirdparty/assimp-6.0.5
    ${CMAKE_BINARY_DIR}/thirdparty/assimp-6.0.5
    EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS ${_SAVED_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)

# ============================================================
# MSVC UTF-8 helper（assimp 之后，仅本工程使用）
# ============================================================
if(MSVC)
    function(mc_enable_utf8 target)
        target_compile_options(${target} PRIVATE /utf-8)
    endfunction()
else()
    function(mc_enable_utf8 target)
    endfunction()
endif()

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
mc_enable_utf8(mc_common)

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
mc_enable_utf8(mc_pluginmgr)

# ============================================================
# Pipeline library
# ============================================================
add_library(mc_pipeline STATIC
    ${MC_SRC_DIR}/pipeline/Pipeline.cpp
    ${MC_SRC_DIR}/pipeline/ValidatePass.cpp
    ${MC_SRC_DIR}/pipeline/AxisConvertPass.cpp
    ${MC_SRC_DIR}/pipeline/UnitConvertPass.cpp
    ${MC_SRC_DIR}/pipeline/HandednessFlipPass.cpp
    ${MC_SRC_DIR}/pipeline/AnimationOptimizePass.cpp
)
target_include_directories(mc_pipeline PUBLIC ${MC_INCLUDE_DIR})
target_link_libraries(mc_pipeline PUBLIC mc_common mc_core)
target_compile_features(mc_pipeline PUBLIC cxx_std_17)
mc_enable_utf8(mc_pipeline)

# ============================================================
# Plugin DLLs
# ============================================================
add_subdirectory(plugins/dummy)
add_subdirectory(plugins/dummy_exporter)
add_subdirectory(plugins/assimp)
add_subdirectory(plugins/gltf)
add_subdirectory(plugins/fbx)

# ============================================================
# CLI executable
# ============================================================
add_executable(mc_cli
    ${MC_SRC_DIR}/app/main.cpp
)
target_link_libraries(mc_cli PRIVATE mc_common mc_core mc_pluginmgr mc_pipeline)
target_include_directories(mc_cli PRIVATE ${MC_INCLUDE_DIR})
target_compile_definitions(mc_cli PRIVATE
    PLUGIN_ASSIMP_FILENAME="$<TARGET_FILE_NAME:Plugin_Assimp>"
)
mc_enable_utf8(mc_cli)
add_dependencies(mc_cli Plugin_Assimp)
add_custom_command(TARGET mc_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:assimp>
        $<TARGET_FILE:Plugin_Assimp>
        $<TARGET_FILE_DIR:mc_cli>
)

# ============================================================
# Tests
# ============================================================
enable_testing()

add_executable(mc_tests
#    ${CMAKE_SOURCE_DIR}/tests/unit/LoggerTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/SceneCreateTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/SceneDestroyTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/SceneStressTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/PluginManagerTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/PipelinePassTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/ValidatePassTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/AssimpPluginTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/GltfPluginTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/FbxPluginTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/ExporterTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/GltfExporterTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/FbxExporterTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/ConvertPassTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/AnimationOptimizePassTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/GltfAnimationTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/FbxAnimationTest.cpp
#    ${CMAKE_SOURCE_DIR}/tests/unit/GltfAnimationExportTest.cpp
    ${CMAKE_SOURCE_DIR}/tests/unit/FbxAnimationExportTest.cpp
)
target_compile_definitions(mc_tests PRIVATE
    CMAKE_TESTS_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
    PLUGIN_ASSIMP_FILENAME="$<TARGET_FILE_NAME:Plugin_Assimp>"
    PLUGIN_GLTF_FILENAME="$<TARGET_FILE_NAME:Plugin_Gltf>"
    PLUGIN_FBX_FILENAME="$<TARGET_FILE_NAME:Plugin_Fbx>"
    PLUGIN_DUMMY_EXPORTER_FILENAME="$<TARGET_FILE_NAME:Plugin_DummyExporter>"
)
target_include_directories(mc_tests PRIVATE ${MC_INCLUDE_DIR})
target_link_libraries(mc_tests PRIVATE mc_common mc_core mc_pluginmgr mc_pipeline GTest::gtest GTest::gtest_main)
target_compile_features(mc_tests PUBLIC cxx_std_17)
mc_enable_utf8(mc_tests)
add_dependencies(mc_tests Plugin_Dummy Plugin_DummyExporter Plugin_Assimp Plugin_Gltf Plugin_Fbx)

# 将 assimp.dll 及 gtest DLL 并排到 mc_tests.exe 相同目录
add_custom_command(TARGET mc_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:assimp>
        $<TARGET_FILE:GTest::gtest>
        $<TARGET_FILE:GTest::gtest_main>
        $<TARGET_FILE_DIR:mc_tests>
)
add_custom_command(TARGET mc_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Plugin_Gltf>
        $<TARGET_FILE_DIR:mc_tests>
)
add_custom_command(TARGET mc_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Plugin_DummyExporter>
        $<TARGET_FILE_DIR:mc_tests>
)
add_custom_command(TARGET mc_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Plugin_Fbx>
        $<IF:$<CONFIG:Debug>,${CMAKE_SOURCE_DIR}/thirdparty/fbxsdk/2020.3.9/lib/x64/debug/libfbxsdk.dll,${CMAKE_SOURCE_DIR}/thirdparty/fbxsdk/2020.3.9/lib/x64/release/libfbxsdk.dll>
        $<TARGET_FILE_DIR:mc_tests>
)

include(GoogleTest)
gtest_discover_tests(mc_tests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:mc_tests>
)