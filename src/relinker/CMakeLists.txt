add_library(sailor_relinker STATIC
    src/ElfReader.cpp
    src/DynamicLinker.cpp
    src/PeImportBuilder.cpp
    src/PeExportBuilder.cpp
    src/EntryStubBuilder.cpp
    src/PeWriter.cpp
    src/Relinker.cpp
)

target_include_directories(sailor_relinker PUBLIC
    include
)

target_link_libraries(sailor_relinker PUBLIC
    sailor_common
)

target_compile_features(sailor_relinker PUBLIC cxx_std_20)

add_executable(sailor_relink
    src/Main.cpp
)

target_link_libraries(sailor_relink PRIVATE
    sailor_relinker
)

target_compile_features(sailor_relink PRIVATE cxx_std_20)

add_executable(sailor_relink_test
    tests/TestRelinker.cpp
)

target_link_libraries(sailor_relink_test PRIVATE
    sailor_relinker
)

target_compile_features(sailor_relink_test PRIVATE cxx_std_20)
