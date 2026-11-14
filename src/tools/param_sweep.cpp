// Parameter sweep: replays one event log under a Cartesian grid of strategy /
// execution settings in parallel and writes one CSV row per configuration.
//
//   param_sweep --data data/AAPL_synthetic.bin --threads 8 --out sweep.csv \
//       --half-spread-bps 2,5,10 --imbalance-skew-bps 0,5,10 --fill-model queue,trade_through
//
// Every list flag is optional; an omitted flag keeps the value from --config.
// Results are deterministic: the same inputs give the same rows in the same
// order for any --threads (only the events/s column is wall-clock dependent).

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "common/cli_args.hpp"
#include "config/pipeline_config_json.hpp"
#include "replay/event_codec.hpp"
#include "replay/sweep.hpp"

using namespace lob;
using json = nlohmann::json;

namespace {

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

// Expands one dimension of the grid: for every point so far, one copy per value.
template <typename Apply>
void expand(std::vector<SweepPoint>& points, const CliArgs& args, const std::string& flag,
            const std::string& short_name, Apply apply) {
    auto v = args.get(flag);
    if (!v) return;
    std::vector<std::string> values = split_csv(*v);
    if (values.empty()) return;
    std::vector<SweepPoint> next;
    next.reserve(points.size() * values.size());
    for (const auto& pt : points) {
        for (const auto& val : values) {
            SweepPoint np = pt;
            apply(np.config, val);
            np.label += (np.label.empty() ? "" : " ") + short_name + "=" + val;
            next.push_back(std::move(np));
        }
    }
    points = std::move(next);
}

}  // namespace

int main(int argc, char** argv) {
    CliArgs args(argc, argv);
    std::string data = args.get_or("data", "");
    if (data.empty()) {
        std::cerr << "usage: param_sweep --data <events.bin> [--threads N] [--out file.csv] [--symbol S]\n"
                     "       [--config cfg.json] [--half-spread-bps a,b] [--skew-bps-per-unit a,b]\n"
                     "       [--imbalance-skew-bps a,b] [--use-microprice 0,1] [--requote-bps a,b]\n"
                     "       [--fill-model queue,trade_through] [--order-latency-us a,b] [--cancel-latency-us a,b]\n"
                     "       [--quote-size a,b] [--max-daily-loss a,b] [--post-only 0,1] [--maker-fee a,b] [--taker-fee a,b]\n";
        return 1;
    }
    std::string symbol = args.get_or("symbol", "AAPL");
    size_t threads = static_cast<size_t>(
        args.get_long_or("threads", std::max(1u, std::thread::hardware_concurrency())));

    json cfg = load_json_config(args.get_or("config", "config/config.example.json"));
    std::vector<SweepPoint> points{SweepPoint{"", build_pipeline_config(cfg)}};

    expand(points, args, "half-spread-bps", "half_spread_bps",
           [](PipelineConfig& c, const std::string& v) { c.strategy.base_half_spread_bps = std::stod(v); });
    expand(points, args, "skew-bps-per-unit", "skew_bps_per_unit",
           [](PipelineConfig& c, const std::string& v) { c.strategy.inventory_skew_bps_per_unit = std::stod(v); });
    expand(points, args, "imbalance-skew-bps", "imb_skew_bps",
           [](PipelineConfig& c, const std::string& v) { c.strategy.imbalance_skew_bps = std::stod(v); });
    expand(points, args, "use-microprice", "microprice",
           [](PipelineConfig& c, const std::string& v) { c.strategy.use_microprice = (v == "1" || v == "true"); });
    expand(points, args, "requote-bps", "requote_bps",
           [](PipelineConfig& c, const std::string& v) { c.strategy.requote_threshold_bps = std::stod(v); });
    expand(points, args, "max-daily-loss", "max_daily_loss",
           [](PipelineConfig& c, const std::string& v) { c.risk_limits.max_daily_loss = std::stod(v); });
    expand(points, args, "post-only", "post_only",
           [](PipelineConfig& c, const std::string& v) { c.strategy.post_only = (v == "1" || v == "true"); });
    expand(points, args, "quote-size", "quote_size",
           [](PipelineConfig& c, const std::string& v) { c.strategy.quote_size = std::stol(v); });
    expand(points, args, "fill-model", "fill_model", [](PipelineConfig& c, const std::string& v) {
        if (v == "queue") c.fill_assumptions.fill_model = FillModel::QueuePosition;
        else if (v == "trade_through") c.fill_assumptions.fill_model = FillModel::TradeThrough;
        else throw std::runtime_error("unknown fill model: " + v);
    });
    expand(points, args, "order-latency-us", "order_lat_us", [](PipelineConfig& c, const std::string& v) {
        c.fill_assumptions.order_latency = std::chrono::microseconds(std::stol(v));
    });
    expand(points, args, "cancel-latency-us", "cancel_lat_us", [](PipelineConfig& c, const std::string& v) {
        c.fill_assumptions.cancel_latency = std::chrono::microseconds(std::stol(v));
    });
    expand(points, args, "maker-fee", "maker_fee",
           [](PipelineConfig& c, const std::string& v) { c.fill_assumptions.maker_fee_per_share = std::stod(v); });
    expand(points, args, "taker-fee", "taker_fee",
           [](PipelineConfig& c, const std::string& v) { c.fill_assumptions.taker_fee_per_share = std::stod(v); });
    for (auto& pt : points) {
        if (pt.label.empty()) pt.label = "base";
    }

    std::cerr << "Loading " << data << "...\n";
    std::vector<MarketEvent> events = read_events_from_file(data);
    std::cerr << events.size() << " events, " << points.size() << " configurations, " << threads
              << " threads\n";

    auto t0 = std::chrono::steady_clock::now();
    std::vector<SweepResult> results = run_sweep(events, Symbol(symbol), points, threads);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::ostringstream csv;
    csv << std::fixed << std::setprecision(6);
    size_t horizons = results.empty() ? 0 : results[0].avg_markout.size();
    csv << "config,events,orders,cancels,fills,taker_fills,risk_rejects,final_pos,max_abs_pos,mean_abs_pos,"
           "realized_pnl,fees,total_pnl,max_drawdown,filled_qty,avg_edge";
    for (size_t h = 0; h < horizons; ++h) csv << ",markout_h" << h;
    csv << ",events_per_sec\n";
    for (const auto& r : results) {
        csv << '"' << r.label << '"' << ',' << r.events << ',' << r.orders_submitted << ','
            << r.orders_canceled << ',' << r.fills << ',' << r.taker_fills << ',' << r.risk_rejects << ','
            << r.final_position << ',' << r.max_abs_position << ',' << r.mean_abs_position << ','
            << r.realized_pnl << ',' << r.fees_paid << ',' << r.total_pnl << ',' << r.max_drawdown << ','
            << r.filled_qty << ',' << r.avg_edge;
        for (double m : r.avg_markout) csv << ',' << m;
        csv << ',' << std::setprecision(0) << r.events_per_second << std::setprecision(6) << '\n';
    }
    if (auto out = args.get("out")) {
        std::ofstream(*out) << csv.str();
        std::cerr << "Wrote " << results.size() << " rows to " << *out << "\n";
    } else {
        std::cout << csv.str();
    }
    std::cerr << "Sweep wall time: " << secs << " s\n";
    return 0;
}
