add_executable(test_network
    test_serializer.cpp
    test_server.cpp
)

target_link_libraries(test_network
PRIVATE
    sf_network
    Catch2::Catch2WithMain
)

include(Catch)

catch_discover_tests(test_network
PROPERTIES
    LABELS "signalforge data"
)