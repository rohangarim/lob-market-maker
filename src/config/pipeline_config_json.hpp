#pragma once

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "common/pipeline.hpp"

namespace lob {

inline nlohmann::json load_json_config(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << "note: config file '" << path << "' not found, using built-in defaults\n";
        return nlohmann::json::object();
    }
    nlohmann::json j;
    ifs >> j;
    return j;
}

inline PipelineConfig build_pipeline_config(const nlohmann::json& cfg) {
    PipelineConfig pcfg;
    if (cfg.contains("risk")) {
        auto& r = cfg["risk"];
        if (r.contains("max_position")) pcfg.risk_limits.max_position = r["max_position"].get<Quantity>();
        if (r.contains("max_order_size")) pcfg.risk_limits.max_order_size = r["max_order_size"].get<Quantity>();
        if (r.contains("max_notional")) pcfg.risk_limits.max_notional = r["max_notional"].get<double>();
        if (r.contains("max_daily_loss")) pcfg.risk_limits.max_daily_loss = r["max_daily_loss"].get<double>();
        if (r.contains("max_open_orders")) pcfg.risk_limits.max_open_orders = r["max_open_orders"].get<size_t>();
    }
    if (cfg.contains("strategy")) {
        auto& s = cfg["strategy"];
        if (s.contains("base_half_spread_bps")) pcfg.strategy.base_half_spread_bps = s["base_half_spread_bps"].get<double>();
        if (s.contains("quote_size")) pcfg.strategy.quote_size = s["quote_size"].get<Quantity>();
        if (s.contains("inventory_skew_bps_per_unit")) pcfg.strategy.inventory_skew_bps_per_unit = s["inventory_skew_bps_per_unit"].get<double>();
        if (s.contains("max_inventory_for_skew")) pcfg.strategy.max_inventory_for_skew = s["max_inventory_for_skew"].get<Quantity>();
        if (s.contains("tick_size")) pcfg.strategy.tick_size = s["tick_size"].get<double>();
        if (s.contains("post_only")) pcfg.strategy.post_only = s["post_only"].get<bool>();
        if (s.contains("use_microprice")) pcfg.strategy.use_microprice = s["use_microprice"].get<bool>();
        if (s.contains("imbalance_skew_bps")) pcfg.strategy.imbalance_skew_bps = s["imbalance_skew_bps"].get<double>();
        if (s.contains("imbalance_levels")) pcfg.strategy.imbalance_levels = s["imbalance_levels"].get<int>();
        if (s.contains("requote_threshold_bps")) pcfg.strategy.requote_threshold_bps = s["requote_threshold_bps"].get<double>();
        if (s.contains("volatility_spread_multiplier")) pcfg.strategy.volatility_spread_multiplier = s["volatility_spread_multiplier"].get<double>();
    }
    if (cfg.contains("execution")) {
        auto& e = cfg["execution"];
        auto& f = pcfg.fill_assumptions;
        if (e.contains("fill_model")) {
            std::string m = e["fill_model"].get<std::string>();
            if (m == "queue") f.fill_model = FillModel::QueuePosition;
            else if (m == "trade_through") f.fill_model = FillModel::TradeThrough;
            else throw std::runtime_error("unknown execution.fill_model '" + m + "' (expected 'queue' or 'trade_through')");
        }
        if (e.contains("order_latency_us")) f.order_latency = std::chrono::microseconds(e["order_latency_us"].get<int64_t>());
        if (e.contains("cancel_latency_us")) f.cancel_latency = std::chrono::microseconds(e["cancel_latency_us"].get<int64_t>());
        if (e.contains("maker_fee_per_share")) f.maker_fee_per_share = e["maker_fee_per_share"].get<double>();
        if (e.contains("taker_fee_per_share")) f.taker_fee_per_share = e["taker_fee_per_share"].get<double>();
        if (e.contains("partial_fill_probability")) f.partial_fill_probability = e["partial_fill_probability"].get<double>();
        if (e.contains("rng_seed")) f.rng_seed = e["rng_seed"].get<uint64_t>();
    }
    return pcfg;
}


}  // namespace lob
