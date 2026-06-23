# Plugin_Assimp —— Phase06 静态模型导入插件
add_library(Plugin_Assimp SHARED
    Plugin_Assimp.cpp
    AssimpSceneConverter.cpp
)

target_include_directories(Plugin_Assimp PRIVATE
    ${MC_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/thirdparty/assimp-6.0.5/include
    ${CMAKE_BINARY_DIR}/thirdparty/assimp-6.0.5/include  # config.h 生成在此
)

target_link_libraries(Plugin_Assimp PRIVATE mc_common mc_core assimp)
target_compile_features(Plugin_Assimp PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(Plugin_Assimp PRIVATE /utf-8)
endif()

# 输出到与 mc_tests.exe 相同目录
set_target_properties(Plugin_Assimp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel"
)
