#pragma once
#include "types.hpp"
#include "spsc_queue.hpp"
#include <thread>
#include <atomic>

/**
 * Standard interface for Market Data Feeds.
 * This runs on the dedicated Feed Core (Core 2).
 */
class FeedHandler
{
public:
    virtual ~FeedHandler() = default;

    // Starts the network listening loop
    virtual void start(SPSCQueue<Tick, 4096> &tick_q, std::atomic<bool> &running) = 0;
};

// -------------------------------------------------------------
// Random Generator Feed (for testing)
// -------------------------------------------------------------
class RandomFeed : public FeedHandler {
public:
    void start(SPSCQueue<Tick, 4096>& tick_q, std::atomic<bool>& running) override {
        int64_t price = 445LL * PRICE_SCALE;
        while (running.load(std::memory_order_relaxed)) {
            price += (int64_t)(((double)rand() / RAND_MAX - 0.5) * 10.0 * PRICE_SCALE);
            Tick t{};
            t.price = price;
            t.bid = price - 5000;
            t.ask = price + 5000;
            t.last_size = 100 + (rand() % 900);
            t.ts_ns = 0; // engine will set now_ns() or we can set it here
            while (!tick_q.push(t) && running.load(std::memory_order_relaxed)) { /* spin */ }
            usleep(10);
        }
    }
};

// -------------------------------------------------------------
// CSV Historical Replay Feed
// -------------------------------------------------------------
class CsvReplayFeed : public FeedHandler {
    std::string filename_;
    uint32_t throttle_us_;
public:
    explicit CsvReplayFeed(std::string filename, uint32_t throttle_us = 10) 
        : filename_(std::move(filename)), throttle_us_(throttle_us) {}

    void start(SPSCQueue<Tick, 4096>& tick_q, std::atomic<bool>& running) override {
        FILE* fp = fopen(filename_.c_str(), "r");
        if (!fp) {
            fprintf(stderr, "[Feed] Could not open %s, falling back to random...\n", filename_.c_str());
            RandomFeed fallback;
            fallback.start(tick_q, running);
            return;
        }

        char line[256];
        if (fgets(line, sizeof(line), fp)) {} // skip header

        while (running.load(std::memory_order_relaxed) && fgets(line, sizeof(line), fp)) {
            uint64_t ts_ns = 0;
            double p = 0.0, b = 0.0, a = 0.0;
            uint32_t size = 0;
            
            if (sscanf(line, "%llu,%lf,%lf,%lf,%u", &ts_ns, &p, &b, &a, &size) == 5) {
                Tick t{};
                t.ts_ns = ts_ns;
                t.price = (int64_t)(p * PRICE_SCALE);
                t.bid = (int64_t)(b * PRICE_SCALE);
                t.ask = (int64_t)(a * PRICE_SCALE);
                t.last_size = size;
                
                while (!tick_q.push(t) && running.load(std::memory_order_relaxed)) { /* spin */ }
                if (throttle_us_ > 0) usleep(throttle_us_);
            }
        }
        fclose(fp);
        // keep alive until killed
        while(running.load(std::memory_order_relaxed)) usleep(100000);
    }
};