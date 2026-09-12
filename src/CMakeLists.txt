set(LOB_CORE_SOURCES
    replay/event_codec.cpp
    execution/paper_execution_engine.cpp
)

add_library(lob_core STATIC ${LOB_CORE_SOURCES})
target_include_directories(lob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lob_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(lob_core PUBLIC cxx_std_20)

if(BUILD_LIVE_FEED)
    target_sources(lob_core PRIVATE marketdata/finnhub_client.cpp)
    target_link_libraries(lob_core PUBLIC ixwebsocket)
    target_compile_definitions(lob_core PUBLIC LOB_BUILD_LIVE_FEED=1)
else()
    target_compile_definitions(lob_core PUBLIC LOB_BUILD_LIVE_FEED=0)
endif()

add_executable(market_engine apps/market_engine.cpp)
target_link_libraries(market_engine PRIVATE lob_core)

add_executable(generate_synthetic_data tools/synthetic_data_gen.cpp)
target_link_libraries(generate_synthetic_data PRIVATE lob_core)

if(BUILD_LIVE_FEED)
    add_executable(finnhub_recorder tools/finnhub_recorder.cpp)
    target_link_libraries(finnhub_recorder PRIVATE lob_core)
endif()
