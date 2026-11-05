add_executable(test_sink
    test_sink.cpp
)

add_executable(test_capture
    test_capture.cpp
)

target_link_libraries(test_sink
PRIVATE
    sf_audio
)

target_link_libraries(test_capture
PRIVATE
    sf_audio
)