add_subdirectory(kernel)
add_subdirectory(input)
add_subdirectory(audio)
add_subdirectory(gpu)
add_subdirectory(media)
add_subdirectory(services)

# Phase 5 Test Executable
add_executable(sailor_phase5_test
    tests/TestPhase5.cpp
)

target_include_directories(sailor_phase5_test PRIVATE
    input/include
    audio/include
    media/include
    ${CMAKE_SOURCE_DIR}/src/common/include
)

target_link_libraries(sailor_phase5_test PRIVATE
    sailor_pad
    sailor_audio
    sailor_media
    sailor_common
)

# Stage 7 Test Executable (Audio, Tempest 3D, Video Demuxer & Core SCE Services)
add_executable(sailor_stage7_test
    tests/TestStage7.cpp
)

target_include_directories(sailor_stage7_test PRIVATE
    audio/include
    media/include
    services/include
    kernel/include
    ${CMAKE_SOURCE_DIR}/src/common/include
)

target_link_libraries(sailor_stage7_test PRIVATE
    sailor_audio
    sailor_media
    sailor_services
    sailor_kernel
    sailor_common
)
