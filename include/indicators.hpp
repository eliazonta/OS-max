#pragma once
#include <cmath>
#include <algorithm>
#include "types.hpp"

// Load AVX intrinsics safely only on x86 platforms
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

// ─────────────────────────────────────────────────────────────
//  Bar Builder
// ─────────────────────────────────────────────────────────────
class BarBuilder
{
public:
    // Defaulting to 100ms for active testing. Change to 5'000'000'000ULL for 5s bars in production.
    explicit BarBuilder(uint64_t dur_ns = 100'000'000ULL) : dur_ns_(dur_ns) {}

    bool on_tick(const Tick &tick, Bar &out) noexcept
    {
        if (bar_start_ns_ == 0)
        {
            bar_start_ns_ = tick.ts_ns;
            current_ = {tick.price, tick.price,
                        tick.price, tick.price,
                        0, tick.ts_ns, tick.ts_ns};
        }

        current_.high = std::max(current_.high, tick.price);
        current_.low = std::min(current_.low, tick.price);
        current_.close = tick.price;
        current_.volume += tick.last_size;
        current_.ts_close_ns = tick.ts_ns;

        if (tick.ts_ns - bar_start_ns_ >= dur_ns_)
        {
            out = current_;
            bar_start_ns_ = tick.ts_ns;
            current_ = {current_.close, current_.close, current_.close, current_.close,
                        0, tick.ts_ns, tick.ts_ns};
            return true;
        }
        return false;
    }

private:
    uint64_t dur_ns_;
    uint64_t bar_start_ns_{0};
    Bar current_{};
};

// ─────────────────────────────────────────────────────────────
//  Math Engine (EMA, RSI, VWAP)
// ─────────────────────────────────────────────────────────────
class alignas(CACHE_LINE) IndicatorEngine
{
public:
    explicit IndicatorEngine(int ema_short = 9, int ema_long = 21, int rsi_period = 14)
        : k_short_(PRICE_SCALE * 2 / (ema_short + 1)), 
          k_long_(PRICE_SCALE * 2 / (ema_long + 1)), 
          rsi_period_(rsi_period) {}

    void update(const Bar &bar) noexcept
    {
        int64_t c = bar.close;

        ema_short_ = ema_short_ == 0 ? c : (c * k_short_ + ema_short_ * (PRICE_SCALE - k_short_)) / PRICE_SCALE;
        ema_long_ = ema_long_ == 0 ? c : (c * k_long_ + ema_long_ * (PRICE_SCALE - k_long_)) / PRICE_SCALE;

        if (prev_close_ != 0)
        {
            int64_t diff = c - prev_close_;
            int64_t gain = diff > 0 ? diff : 0;
            int64_t loss = diff < 0 ? -diff : 0;
            avg_gain_ = (avg_gain_ * (rsi_period_ - 1) + gain) / rsi_period_;
            avg_loss_ = (avg_loss_ * (rsi_period_ - 1) + loss) / rsi_period_;
        }
        prev_close_ = c;
        
        if (avg_loss_ == 0) {
            rsi_ = 100 * PRICE_SCALE;
        } else {
            rsi_ = (100LL * PRICE_SCALE * avg_gain_) / (avg_gain_ + avg_loss_);
        }

        int64_t tp = (bar.high + bar.low + bar.close) / 3;
        vwap_pv_ += (unsigned __int128)tp * bar.volume;
        vwap_v_ += bar.volume;
        vwap_ = vwap_v_ > 0 ? (int64_t)(vwap_pv_ / vwap_v_) : c;

        if (prev_high_ != 0)
        {
            int64_t tr = std::max(bar.high - bar.low,
                                  std::max(std::abs(bar.high - prev_close_),
                                           std::abs(bar.low - prev_close_)));
            atr_ = atr_ == 0 ? tr : (tr * k_short_ + atr_ * (PRICE_SCALE - k_short_)) / PRICE_SCALE;
        }
        prev_high_ = bar.high;
        prev_low_ = bar.low;
        bar_count_++;
    }

    // Removed AVX batch processing for now due to fixed point arithmetic transition
    void batch_ema_avx2(const float *, int) noexcept {}

    int64_t ema_short() const noexcept { return ema_short_; }
    int64_t ema_long() const noexcept { return ema_long_; }
    int64_t rsi() const noexcept { return rsi_; }
    int64_t vwap() const noexcept { return vwap_; }
    int64_t atr() const noexcept { return atr_; }
    int bar_count() const noexcept { return bar_count_; }
    bool ready() const noexcept { return bar_count_ >= rsi_period_ + 2; }

private:
    const int64_t k_short_, k_long_;
    const int rsi_period_;
    int64_t ema_short_{0}, ema_long_{0};
    int64_t rsi_{50 * PRICE_SCALE};
    int64_t avg_gain_{0}, avg_loss_{0};
    int64_t vwap_{0};
    unsigned __int128 vwap_pv_{0};
    uint64_t vwap_v_{0};
    int64_t atr_{0};
    int64_t prev_close_{0}, prev_high_{0}, prev_low_{0};
    int bar_count_{0};
};