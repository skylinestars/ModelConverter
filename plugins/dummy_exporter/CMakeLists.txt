# Plugin_DummyExporter —— Phase10 验收用桩导出插件
add_library(Plugin_DummyExporter SHARED
    Plugin_DummyExporter.cpp
)
target_include_directories(Plugin_DummyExporter PRIVATE ${MC_INCLUDE_DIR})
target_link_libraries(Plugin_DummyExporter PRIVATE mc_common mc_core)
target_compile_features(Plugin_DummyExporter PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(Plugin_DummyExporter PRIVATE /utf-8)
endif()

set_target_properties(Plugin_DummyExporter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel"
)
