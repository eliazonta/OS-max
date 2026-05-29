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
            current_ = {(float)tick.price, (float)tick.price,
                        (float)tick.price, (float)tick.price,
                        0, tick.ts_ns, tick.ts_ns};
        }

        current_.high = std::max(current_.high, (float)tick.price);
        current_.low = std::min(current_.low, (float)tick.price);
        current_.close = (float)tick.price;
        current_.volume += (uint32_t)tick.last_size;
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
        : k_short_(2.f / (ema_short + 1)), k_long_(2.f / (ema_long + 1)), rsi_period_(rsi_period) {}

    void update(const Bar &bar) noexcept
    {
        float c = bar.close;

        ema_short_ = ema_short_ == 0.f ? c : c * k_short_ + ema_short_ * (1.f - k_short_);
        ema_long_ = ema_long_ == 0.f ? c : c * k_long_ + ema_long_ * (1.f - k_long_);

        if (prev_close_ != 0.f)
        {
            float diff = c - prev_close_;
            float gain = diff > 0.f ? diff : 0.f;
            float loss = diff < 0.f ? -diff : 0.f;
            avg_gain_ = (avg_gain_ * (rsi_period_ - 1) + gain) / rsi_period_;
            avg_loss_ = (avg_loss_ * (rsi_period_ - 1) + loss) / rsi_period_;
        }
        prev_close_ = c;
        rsi_ = avg_loss_ < 1e-9f ? 100.f : 100.f - 100.f / (1.f + avg_gain_ / avg_loss_);

        float tp = (bar.high + bar.low + bar.close) / 3.f;
        vwap_pv_ += tp * static_cast<float>(bar.volume);
        vwap_v_ += static_cast<float>(bar.volume);
        vwap_ = vwap_v_ > 0.f ? vwap_pv_ / vwap_v_ : c;

        if (prev_high_ != 0.f)
        {
            float tr = std::max(bar.high - bar.low,
                                std::max(std::abs(bar.high - prev_close_),
                                         std::abs(bar.low - prev_close_)));
            atr_ = atr_ == 0.f ? tr : tr * k_short_ + atr_ * (1.f - k_short_);
        }
        prev_high_ = bar.high;
        prev_low_ = bar.low;
        bar_count_++;
    }

    void batch_ema_avx2(const float *closes, int n) noexcept
    {
#if defined(__AVX2__) && (defined(__x86_64__) || defined(_M_X64))
        __m256 k_s = _mm256_set1_ps(k_short_);
        __m256 k_l = _mm256_set1_ps(k_long_);
        __m256 one = _mm256_set1_ps(1.f);
        __m256 e_s = _mm256_set1_ps(closes[0]);
        __m256 e_l = _mm256_set1_ps(closes[0]);
        for (int b = 0; b < n / 8; b++)
        {
            __m256 c = _mm256_loadu_ps(closes + b * 8);
            e_s = _mm256_fmadd_ps(c, k_s, _mm256_mul_ps(e_s, _mm256_sub_ps(one, k_s)));
            e_l = _mm256_fmadd_ps(c, k_l, _mm256_mul_ps(e_l, _mm256_sub_ps(one, k_l)));
        }
        float buf_s[8], buf_l[8];
        _mm256_storeu_ps(buf_s, e_s);
        _mm256_storeu_ps(buf_l, e_l);
        ema_short_ = buf_s[7];
        ema_long_ = buf_l[7];
#else
        for (int i = 0; i < n; i++)
        {
            ema_short_ = closes[i] * k_short_ + ema_short_ * (1.f - k_short_);
            ema_long_ = closes[i] * k_long_ + ema_long_ * (1.f - k_long_);
        }
#endif
    }

    float ema_short() const noexcept { return ema_short_; }
    float ema_long() const noexcept { return ema_long_; }
    float rsi() const noexcept { return rsi_; }
    float vwap() const noexcept { return vwap_; }
    float atr() const noexcept { return atr_; }
    int bar_count() const noexcept { return bar_count_; }
    bool ready() const noexcept { return bar_count_ >= rsi_period_ + 2; }

private:
    const float k_short_, k_long_;
    const int rsi_period_;
    float ema_short_{0.f}, ema_long_{0.f};
    float rsi_{50.f};
    float avg_gain_{0.f}, avg_loss_{0.f};
    float vwap_{0.f}, vwap_pv_{0.f}, vwap_v_{0.f};
    float atr_{0.f};
    float prev_close_{0.f}, prev_high_{0.f}, prev_low_{0.f};
    int bar_count_{0};
};