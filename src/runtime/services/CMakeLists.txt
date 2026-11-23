add_library(sailor_services SHARED
    src/UserService.cpp
    src/SaveData.cpp
    src/NpTrophy.cpp
    src/PlayGo.cpp
    src/ServiceExports.cpp
)

target_include_directories(sailor_services PUBLIC
    include
)

target_link_libraries(sailor_services PRIVATE
    sailor_common
)

target_compile_features(sailor_services PUBLIC cxx_std_20)
