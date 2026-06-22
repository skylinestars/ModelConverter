# Plugin_Dummy —— Phase03 验收用桩插件
add_library(Plugin_Dummy SHARED
    Plugin_Dummy.cpp
)
target_include_directories(Plugin_Dummy PRIVATE ${MC_INCLUDE_DIR})
target_link_libraries(Plugin_Dummy PRIVATE mc_common)
target_compile_features(Plugin_Dummy PRIVATE cxx_std_17)

# 将 DLL 输出到与 mc_cli.exe 相同的目录，便于运行时加载
set_target_properties(Plugin_Dummy PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)