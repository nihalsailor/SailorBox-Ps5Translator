add_library(sailor_media SHARED
    src/AvPlayer.cpp
    src/AvPlayerExports.cpp
)

target_include_directories(sailor_media PUBLIC
    include
)

target_link_libraries(sailor_media PRIVATE
    sailor_common
    mfplat
    mfreadwrite
    mfuuid
    ole32
)

target_compile_features(sailor_media PUBLIC cxx_std_20)
