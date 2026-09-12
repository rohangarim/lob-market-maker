find_package(benchmark REQUIRED)

foreach(bench_name IN ITEMS bench_order_book bench_ring_buffer bench_pipeline bench_memory)
    add_executable(${bench_name} ${bench_name}.cpp)
    target_link_libraries(${bench_name} PRIVATE lob_core benchmark::benchmark)
endforeach()
