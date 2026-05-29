#pragma once
#include <cstdint>
#include "types.hpp"      // Assuming Tick, Bar, Signal are moved here
#include "indicators.hpp" // Assuming IndicatorEngine is moved here

/**
 * Dynamic Risk Parameters.
 * In a real system, you would load this from a JSON file on startup.
 */
struct RiskConfig
{
    float stop_loss_atr_mult = 1.5f;
    float take_profit_atr_mult = 2.5f;
    float max_drawdown_pct = 0.05f;
    int max_positions = 4;
    uint8_t min_signal_strength = 75;
};

/**
 * CRTP Base Class for Strategies (Zero-Overhead Polymorphism)
 * By using templates instead of 'virtual' functions, the compiler
 * perfectly inlines your strategy into the hot path.
 */
template <typename T>
class StrategyBase
{
public:
    explicit StrategyBase(RiskConfig cfg = {}) : risk(cfg) {}

    // Hot-path evaluation function.
    inline bool evaluate(const Bar &bar, const IndicatorEngine &ind, Signal &out) noexcept
    {
        // Delegate to the derived class implementation
        return static_cast<T *>(this)->evaluate_impl(bar, ind, out);
    }

    void update_risk(const RiskConfig &new_cfg) noexcept
    {
        risk = new_cfg;
    }

protected:
    RiskConfig risk;

    // Helper for branchless scoring
    inline uint8_t score_if(bool cond, uint8_t score) const noexcept
    {
        return static_cast<uint8_t>(score & -static_cast<uint8_t>(cond));
    }
};