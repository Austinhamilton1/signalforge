add_executable(signalforge
    signalforge.cpp
)

add_executable(test_backend
    test_backend.cpp
)

target_link_libraries(signalforge
PRIVATE
    sf_audio
    sf_data
    sf_network
)

target_link_libraries(test_backend
PRIVATE
    sf_data
    sf_network
)