# Plugin_Fbx —— Phase09 FBX SDK 静态模型导入插件
set(FBXSDK_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/fbxsdk/2020.3.9")

add_library(Plugin_Fbx SHARED
    Plugin_Fbx.cpp
    FbxSceneConverter.cpp
    FbxExporter.cpp
    FbxMeshHelper.cpp
)

target_include_directories(Plugin_Fbx PRIVATE
    ${MC_INCLUDE_DIR}
    ${FBXSDK_ROOT}/include
)

# Debug 用 debug/ 下的库，Release 用 release/ 下的库
target_link_libraries(Plugin_Fbx PRIVATE
    mc_common
    mc_core
    $<$<CONFIG:Debug>:${FBXSDK_ROOT}/lib/x64/debug/libfbxsdk.lib>
    $<$<CONFIG:Release>:${FBXSDK_ROOT}/lib/x64/release/libfbxsdk.lib>
    $<$<CONFIG:RelWithDebInfo>:${FBXSDK_ROOT}/lib/x64/release/libfbxsdk.lib>
    $<$<CONFIG:MinSizeRel>:${FBXSDK_ROOT}/lib/x64/release/libfbxsdk.lib>
)

target_compile_features(Plugin_Fbx PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(Plugin_Fbx PRIVATE /utf-8 /wd4100 /wd4189 /wd4244 /wd4267)
    # FBX SDK 要求 FBXSDK_SHARED 宏以使用 DLL 版本
    target_compile_definitions(Plugin_Fbx PRIVATE FBXSDK_SHARED)
endif()

# 将 libfbxsdk.dll 复制到输出目录
set(FBXSDK_DLL_DEBUG   "${FBXSDK_ROOT}/lib/x64/debug/libfbxsdk.dll")
set(FBXSDK_DLL_RELEASE "${FBXSDK_ROOT}/lib/x64/release/libfbxsdk.dll")

add_custom_command(TARGET Plugin_Fbx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<IF:$<CONFIG:Debug>,${FBXSDK_DLL_DEBUG},${FBXSDK_DLL_RELEASE}>
        $<TARGET_FILE_DIR:Plugin_Fbx>
)

# 输出到与 mc_tests.exe 相同目录
set_target_properties(Plugin_Fbx PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel"
)
