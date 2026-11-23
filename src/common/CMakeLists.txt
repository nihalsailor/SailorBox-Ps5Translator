add_library(sailor_common STATIC
    src/Logger.cpp
    src/Nid.cpp
)

target_include_directories(sailor_common PUBLIC
    include
)

target_compile_features(sailor_common PUBLIC cxx_std_20)
