#pragma once
#include <cstdint>

// Prevent false sharing across CPU cores
constexpr int CACHE_LINE = 64;

// ─────────────────────────────────────────────────────────────
//  Market Data Types
// ─────────────────────────────────────────────────────────────

struct alignas(CACHE_LINE) Tick
{
    double price;
    double bid;
    double ask;
    double last_size;
    uint64_t ts_ns;
    uint32_t seq;
    uint16_t symbol_id;
    uint8_t side;
    uint8_t _pad[17];
};
static_assert(sizeof(Tick) == CACHE_LINE, "Tick must fit exactly in one cache line");

struct alignas(32) Bar
{
    float open, high, low, close;
    uint32_t volume;
    uint64_t ts_open_ns;
    uint64_t ts_close_ns;
};

// ─────────────────────────────────────────────────────────────
//  Signal & Strategy Types
// ─────────────────────────────────────────────────────────────

enum class SignalType : uint8_t
{
    NONE = 0,
    LONG_EMA_CROSS,
    SHORT_EMA_CROSS,
    LONG_RSI_OVERSOLD,
    SHORT_RSI_OVERBOUGHT,
    LONG_VWAP_RECLAIM,
    SHORT_VWAP_BREAK,
    LONG_MOMENTUM,
    SHORT_MOMENTUM,
};

inline const char *signal_name(SignalType t)
{
    switch (t)
    {
    case SignalType::LONG_EMA_CROSS:
        return "LONG_EMA_CROSS";
    case SignalType::SHORT_EMA_CROSS:
        return "SHORT_EMA_CROSS";
    case SignalType::LONG_RSI_OVERSOLD:
        return "LONG_RSI_OVERSOLD";
    case SignalType::SHORT_RSI_OVERBOUGHT:
        return "SHORT_RSI_OVERBOUGHT";
    case SignalType::LONG_VWAP_RECLAIM:
        return "LONG_VWAP_RECLAIM";
    case SignalType::SHORT_VWAP_BREAK:
        return "SHORT_VWAP_BREAK";
    case SignalType::LONG_MOMENTUM:
        return "LONG_MOMENTUM";
    case SignalType::SHORT_MOMENTUM:
        return "SHORT_MOMENTUM";
    default:
        return "NONE";
    }
}

struct alignas(32) Signal
{
    SignalType type;
    uint8_t strength;
    float entry_price;
    float stop_loss;
    float take_profit;
    uint64_t ts_ns;
    uint64_t tick_ts_ns;
};