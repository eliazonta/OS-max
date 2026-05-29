#pragma once
#include "../../include/strategy.hpp"

/**
 * ============================================================
 * EMA Crossover & Momentum Strategy
 * ============================================================
 */
class SwingMomentumStrategy : public StrategyBase<SwingMomentumStrategy>
{
private:
    // Keep track of previous states to detect crossovers
    float prev_es = 0.f;
    float prev_el = 0.f;

public:
    using StrategyBase::StrategyBase;

    inline bool evaluate_impl(const Bar &bar, const IndicatorEngine &ind, Signal &out) noexcept
    {
        if (!ind.ready())
            return false;

        const float c = bar.close;
        const float es = ind.ema_short();
        const float el = ind.ema_long();
        const float rv = ind.rsi();
        const float at = ind.atr();

        // 1. Detect Crossovers
        bool bullish_cross = (prev_es <= prev_el) && (es > el);
        bool bearish_cross = (prev_es >= prev_el) && (es < el);

        // Save current state for the next bar
        prev_es = es;
        prev_el = el;

        // 2. Score the logic (Bullish cross + RSI has room to grow)
        uint8_t long_score = score_if(bullish_cross && rv > 40.f && rv < 70.f, 85);
        uint8_t short_score = score_if(bearish_cross && rv < 60.f && rv > 30.f, 82);

        // 3. Filter out weak signals based on Risk Config
        uint8_t best_score = std::max(long_score, short_score);
        if (best_score < risk.min_signal_strength)
            return false;

        // 4. Populate the Signal struct
        bool is_long = (long_score > short_score);

        out.type = is_long ? SignalType::LONG_EMA_CROSS : SignalType::SHORT_EMA_CROSS;
        out.strength = best_score;
        out.entry_price = c;
        out.ts_ns = 0; // Filled by the engine layer

        // 5. Apply dynamic risk metrics
        if (is_long)
        {
            out.stop_loss = c - (at * risk.stop_loss_atr_mult);
            out.take_profit = c + (at * risk.take_profit_atr_mult);
        }
        else
        {
            out.stop_loss = c + (at * risk.stop_loss_atr_mult);
            out.take_profit = c - (at * risk.take_profit_atr_mult);
        }

        return true;
    }
};