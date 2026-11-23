add_library(sailor_pad SHARED
    src/Pad.cpp
    src/PadExports.cpp
)

target_include_directories(sailor_pad PUBLIC
    include
)

target_link_libraries(sailor_pad PRIVATE
    sailor_common
)

target_compile_features(sailor_pad PUBLIC cxx_std_20)
