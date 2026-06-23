# Plugin_Dummy —— Phase03 验收用桩插件
add_library(Plugin_Dummy SHARED
    Plugin_Dummy.cpp
)
target_include_directories(Plugin_Dummy PRIVATE ${MC_INCLUDE_DIR})
target_link_libraries(Plugin_Dummy PRIVATE mc_common)
target_compile_features(Plugin_Dummy PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(Plugin_Dummy PRIVATE /utf-8)
endif()

# 将 DLL 输出到与 mc_tests.exe 相同目录，确保 ctest 能居相对路径加载
set_target_properties(Plugin_Dummy PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel"
)