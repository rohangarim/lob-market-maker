#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace lob {

// Constant-memory latency recorder (nanoseconds). Values < 32 ns get exact
// buckets; above that each power of two is split into 16 linear sub-buckets,
// so a reported percentile is the bucket's lower bound and is within ~6% of
// the true sample. count, sum and max are exact. Unlike storing raw samples
// this never grows, so it is safe for arbitrarily long runs.
class LatencyStats {
public:
    void add_ns(int64_t ns) {
        uint64_t v = ns < 0 ? 0 : static_cast<uint64_t>(ns);
        ++buckets_[bucket_of(v)];
        ++count_;
        sum_ += static_cast<double>(v);
        if (v > max_) max_ = v;
    }

    [[nodiscard]] size_t count() const noexcept { return count_; }

    void clear() {
        buckets_.fill(0);
        count_ = 0;
        sum_ = 0.0;
        max_ = 0;
    }

    // Nearest-rank percentile, p in [0, 100]; returns the bucket lower bound.
    [[nodiscard]] double percentile(double p) const {
        if (count_ == 0) return 0.0;
        uint64_t target = static_cast<uint64_t>((p / 100.0) * static_cast<double>(count_) + 0.999999);
        if (target < 1) target = 1;
        if (target > count_) target = count_;
        uint64_t cumulative = 0;
        for (size_t i = 0; i < buckets_.size(); ++i) {
            cumulative += buckets_[i];
            if (cumulative >= target) return static_cast<double>(lower_bound_of(i));
        }
        return static_cast<double>(max_);
    }

    [[nodiscard]] double max_ns() const noexcept { return static_cast<double>(max_); }
    [[nodiscard]] double mean_ns() const noexcept {
        return count_ ? sum_ / static_cast<double>(count_) : 0.0;
    }

private:
    static constexpr size_t kBucketCount = 32 + 59 * 16;

    static size_t bucket_of(uint64_t v) noexcept {
        if (v < 32) return static_cast<size_t>(v);
        int m = 63 - std::countl_zero(v);
        size_t sub = static_cast<size_t>((v >> (m - 4)) & 15);
        return 32 + static_cast<size_t>(m - 5) * 16 + sub;
    }

    static uint64_t lower_bound_of(size_t idx) noexcept {
        if (idx < 32) return idx;
        size_t k = idx - 32;
        int m = 5 + static_cast<int>(k / 16);
        uint64_t sub = k % 16;
        return (16 + sub) << (m - 4);
    }

    std::array<uint64_t, kBucketCount> buckets_{};
    uint64_t count_ = 0;
    double sum_ = 0.0;
    uint64_t max_ = 0;
};

}  // namespace lob
