add_subdirectory(rdna_backend)

add_library(sailor_shader STATIC
    src/RdnaOpcodes.cpp
    src/RdnaDecoder.cpp
    src/ShaderIr.cpp
    src/SpirvBuilder.cpp
    src/ShaderCache.cpp
    src/ShaderRecompiler.cpp
)

target_include_directories(sailor_shader PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/src/common/include
    rdna_backend
    rdna_backend/RdnaDecoder/include
    rdna_backend/ControlFlow/include
    rdna_backend/IntermediateRepresentation/include
    rdna_backend/Translation/include
    rdna_backend/Optimization/include
    rdna_backend/SpirvBackend/include
    rdna_backend/prx/libc/include
)

target_link_libraries(sailor_shader PUBLIC
    sailor_common
    sailor_rdna_backend
)

# Phase 4 Test executable
add_executable(sailor_shader_test
    tests/TestShader.cpp
)

target_link_libraries(sailor_shader_test PRIVATE
    sailor_shader
    sailor_rdna_backend
    sailor_common
)
