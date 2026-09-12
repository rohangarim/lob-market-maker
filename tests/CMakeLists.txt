find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(lob_tests
    test_order_book.cpp
    test_microstructure.cpp
    test_ring_buffer.cpp
    test_risk_engine.cpp
    test_execution_engine.cpp
    test_portfolio.cpp
    test_strategy.cpp
    test_event_codec.cpp
    test_integration_pipeline.cpp
    test_random_order_book.cpp
    test_replay_determinism.cpp
)

target_link_libraries(lob_tests PRIVATE lob_core GTest::gtest GTest::gtest_main)

gtest_discover_tests(lob_tests)
