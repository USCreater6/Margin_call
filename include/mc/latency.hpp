// Latency histogram: stores samples in a preallocated buffer and reports percentiles.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace mc {

struct LatencyStats { std::int64_t p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0, max = 0; std::size_t n = 0; };

class LatencyRecorder {
 public:
  explicit LatencyRecorder(std::size_t capacity) { s_.reserve(capacity); }
  void add(std::int64_t ns) { if (s_.size() < s_.capacity()) s_.push_back(ns); }
  LatencyStats stats() {
    LatencyStats r;
    r.n = s_.size();
    if (s_.empty()) return r;
    std::sort(s_.begin(), s_.end());
    auto q = [&](double p) { return s_[std::min(s_.size() - 1, static_cast<std::size_t>(p * static_cast<double>(s_.size())))]; };
    r.p50 = q(0.50); r.p90 = q(0.90); r.p95 = q(0.95); r.p99 = q(0.99); r.p999 = q(0.999); r.max = s_.back();
    return r;
  }
 private:
  std::vector<std::int64_t> s_;
};

}  // namespace mc
