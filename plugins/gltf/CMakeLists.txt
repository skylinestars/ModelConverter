# Plugin_Gltf —— Phase08 GLTF/GLB 导入插件（tinygltf header-only）
add_library(Plugin_Gltf SHARED
    Plugin_Gltf.cpp
    GltfSceneConverter.cpp
    GltfExporter.cpp
)

target_include_directories(Plugin_Gltf PRIVATE
    ${MC_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/thirdparty/tinygltf
)

target_link_libraries(Plugin_Gltf PRIVATE mc_common mc_core)
target_compile_features(Plugin_Gltf PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(Plugin_Gltf PRIVATE /utf-8 /wd4100 /wd4189 /wd4244 /wd4267)
endif()

# 输出到与 mc_tests.exe 相同目录
set_target_properties(Plugin_Gltf PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel"
)
