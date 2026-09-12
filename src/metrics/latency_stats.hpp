#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace lob {

// Collects raw latency samples (in nanoseconds) and computes percentiles on
// demand. Not designed for unbounded runs -- callers doing very long
// replays should periodically drain/report and clear() to bound memory.
class LatencyStats {
public:
    void add_ns(int64_t ns) { samples_.push_back(ns); }

    [[nodiscard]] size_t count() const noexcept { return samples_.size(); }
    void clear() { samples_.clear(); }

    // Nearest-rank percentile, p in [0, 100].
    [[nodiscard]] double percentile(double p) const {
        if (samples_.empty()) return 0.0;
        std::vector<int64_t> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        size_t rank = static_cast<size_t>((p / 100.0) * static_cast<double>(sorted.size() - 1));
        return static_cast<double>(sorted[rank]);
    }

    [[nodiscard]] double max_ns() const {
        if (samples_.empty()) return 0.0;
        return static_cast<double>(*std::max_element(samples_.begin(), samples_.end()));
    }

    [[nodiscard]] double mean_ns() const {
        if (samples_.empty()) return 0.0;
        double sum = 0.0;
        for (auto s : samples_) sum += static_cast<double>(s);
        return sum / static_cast<double>(samples_.size());
    }

    [[nodiscard]] const std::vector<int64_t>& raw_samples() const noexcept { return samples_; }

private:
    std::vector<int64_t> samples_;
};

}  // namespace lob
