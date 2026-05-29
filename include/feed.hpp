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
// Future Implementation Example: BinanceWebsocketFeed
// -------------------------------------------------------------
/*
class BinanceWebsocketFeed : public FeedHandler {
public:
    void start(SPSCQueue<Tick, 4096>& tick_q, std::atomic<bool>& running) override {
        // 1. Connect to wss://fstream.binance.com/ws/btcusdt@bookTicker
        // 2. Parse JSON (or binary) in a while(running) loop
        // 3. Push to tick_q
    }
};
*/