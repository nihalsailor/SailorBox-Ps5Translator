cmake_minimum_required(VERSION 3.20)

file(GLOB_RECURSE RDNA_BACKEND_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/RdnaDecoder/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ControlFlow/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/IntermediateRepresentation/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Translation/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Optimization/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SpirvBackend/src/*.cpp"
)

list(APPEND RDNA_BACKEND_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Recompiler.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/StencilReplay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ShaderDiskCache.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ShaderCachePack.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ShaderCacheVersion.cpp"
)

# Filter out SpirvOptimizer.cpp since that requires external SPIRV-Tools library
list(FILTER RDNA_BACKEND_SOURCES EXCLUDE REGEX "SpirvOptimizer\\.cpp$")

add_library(sailor_rdna_backend STATIC
    ${RDNA_BACKEND_SOURCES}
)

target_include_directories(sailor_rdna_backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/RdnaDecoder/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/ControlFlow/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/IntermediateRepresentation/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Translation/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/Optimization/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/SpirvBackend/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/prx/libc/include"
)

target_compile_features(sailor_rdna_backend PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(sailor_rdna_backend PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4127 /wd4702 /wd4324 /bigobj /Zc:__cplusplus
    )
endif()
