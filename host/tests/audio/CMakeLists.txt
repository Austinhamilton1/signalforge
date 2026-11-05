add_executable(test_audio
    test_audio.cpp
)

target_link_libraries(test_audio
PRIVATE
    sf_audio
)