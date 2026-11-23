add_library(sailor_kernel SHARED
    src/DirectMemory.cpp
    src/ThreadManager.cpp
    src/SyncPrimitives.cpp
    src/VirtualFileSystem.cpp
    src/FiberManager.cpp
    src/Decompressor.cpp
    src/PadManager.cpp
    src/SaveDataManager.cpp
    src/AudioOutManager.cpp
    src/KernelExports.cpp
)

target_include_directories(sailor_kernel PUBLIC
    include
)

target_link_libraries(sailor_kernel PRIVATE
    sailor_common
    ws2_32
)

target_compile_features(sailor_kernel PUBLIC cxx_std_20)
if(MSVC)
    target_link_options(sailor_kernel PRIVATE "/MAP")
endif()

add_executable(sailor_kernel_test
    src/DirectMemory.cpp
    src/ThreadManager.cpp
    src/SyncPrimitives.cpp
    src/VirtualFileSystem.cpp
    src/FiberManager.cpp
    src/Decompressor.cpp
    src/PadManager.cpp
    src/SaveDataManager.cpp
    src/AudioOutManager.cpp
    tests/TestKernel.cpp
)

target_include_directories(sailor_kernel_test PRIVATE
    include
)

target_link_libraries(sailor_kernel_test PRIVATE
    sailor_common
)

target_compile_features(sailor_kernel_test PRIVATE cxx_std_20)
