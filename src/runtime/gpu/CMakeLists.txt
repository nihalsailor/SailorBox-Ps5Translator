add_library(sailor_gpu SHARED
    src/Pm4Decoder.cpp
    src/VulkanContext.cpp
    src/VideoOut.cpp
    src/TextureDetiler.cpp
    src/VulkanPipelineManager.cpp
    src/AgcDriver.cpp
    src/AgcExports.cpp
)

target_include_directories(sailor_gpu PUBLIC
    include
)

target_link_libraries(sailor_gpu PRIVATE
    sailor_common
    sailor_shader
)

target_compile_features(sailor_gpu PUBLIC cxx_std_20)

add_executable(sailor_gpu_test
    src/Pm4Decoder.cpp
    src/VulkanContext.cpp
    src/VideoOut.cpp
    src/TextureDetiler.cpp
    src/VulkanPipelineManager.cpp
    src/AgcDriver.cpp
    tests/TestGpu.cpp
)

target_include_directories(sailor_gpu_test PRIVATE
    include
)

target_link_libraries(sailor_gpu_test PRIVATE
    sailor_common
    sailor_shader
)

target_compile_features(sailor_gpu_test PRIVATE cxx_std_20)
