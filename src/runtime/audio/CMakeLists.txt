add_library(sailor_audio SHARED
    src/AudioOut.cpp
    src/Audio3d.cpp
    src/AudioExports.cpp
)

target_include_directories(sailor_audio PUBLIC
    include
)

target_link_libraries(sailor_audio PRIVATE
    sailor_common
    ole32
)

target_compile_features(sailor_audio PUBLIC cxx_std_20)
