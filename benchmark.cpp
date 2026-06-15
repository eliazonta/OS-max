#include "include/spsc_queue.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

using namespace std::chrono;

struct DummyMsg {
    uint64_t data[8];
};

void run_spsc_benchmark() {
    SPSCQueue<DummyMsg, 65536> queue;
    const uint64_t MSG_COUNT = 10'000'000;
    
    std::atomic<bool> start_flag{false};
    
    std::thread consumer([&]() {
        DummyMsg msg;
        while (!start_flag.load(std::memory_order_acquire)) {}
        
        for (uint64_t i = 0; i < MSG_COUNT; ++i) {
            while (!queue.pop(msg)) {}
        }
    });
    
    DummyMsg msg{};
    start_flag.store(true, std::memory_order_release);
    auto t0 = high_resolution_clock::now();
    
    for (uint64_t i = 0; i < MSG_COUNT; ++i) {
        while (!queue.push(msg)) {}
    }
    
    consumer.join();
    auto t1 = high_resolution_clock::now();
    
    double elapsed = duration_cast<duration<double>>(t1 - t0).count();
    std::cout << "[Benchmark] SPSCQueue Throughput: " 
              << (MSG_COUNT / elapsed) / 1e6 << " million msgs/sec\n";
}

int main() {
    std::cout << "Running SPSCQueue Throughput Benchmark...\n";
    run_spsc_benchmark();
    return 0;
}
