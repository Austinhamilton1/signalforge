add_executable(signalforge
    signalforge.cpp
)

target_link_libraries(signalforge
PRIVATE
    sf_data
    sf_network
)