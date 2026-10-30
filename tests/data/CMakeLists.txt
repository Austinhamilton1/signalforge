add_executable(test_data
    test_jitter.cpp
    test_ring.cpp
)

target_link_libraries(test_data
PRIVATE
    sf_data
    Catch2::Catch2WithMain
)

include(Catch)

catch_discover_tests(test_data
PROPERTIES
    LABELS "signalforge data"
)