add_library(sailor_tools STATIC
    src/GamePatch.cpp
    src/StubGenerator.cpp
)

target_include_directories(sailor_tools PUBLIC
    include
)

target_link_libraries(sailor_tools PUBLIC
    sailor_common
)

target_compile_features(sailor_tools PUBLIC cxx_std_20)

# sailor CLI Launcher (sailor.exe)
add_executable(sailor
    src/Main.cpp
)

target_include_directories(sailor PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/src/relinker/include
)

target_link_libraries(sailor PRIVATE
    sailor_tools
    sailor_relinker
    sailor_common
)

target_compile_features(sailor PRIVATE cxx_std_20)

# Phase 6 Developer Tooling Test Suite
add_executable(sailor_tools_test
    tests/TestPhase6.cpp
)

target_include_directories(sailor_tools_test PRIVATE
    include
)

target_link_libraries(sailor_tools_test PRIVATE
    sailor_tools
    sailor_common
)

target_compile_features(sailor_tools_test PRIVATE cxx_std_20)

add_executable(find_nid
    find_nid.cpp
)

target_include_directories(find_nid PRIVATE
    include
    ${CMAKE_SOURCE_DIR}/src/relinker/include
)

target_link_libraries(find_nid PRIVATE
    sailor_relinker
    sailor_common
)

target_compile_features(find_nid PRIVATE cxx_std_20)

