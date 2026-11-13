// Strategy interface and the two launch strategies. Long-only, daily bars.
// Strategies only emit intents; sizing, stops and every limit are enforced by the engine and risk engine.
#pragma once
#include <cmath>
#include <string>
#include <vector>
#include "mc/types.hpp"

namespace mc {

enum class Signal : std::uint8_t { None, EnterLong, Exit };

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual std::string name() const = 0;
  virtual void reset(std::size_t n_inst) = 0;
  // Called once per instrument per bar, after the close. `held` = bars the position has been open.
  virtual Signal on_bar(std::uint32_t inst, const Bar& b, bool in_pos, int held) = 0;
};

namespace detail {
inline double sma(const std::vector<double>& v, std::size_t n) {
  double s = 0;
  for (std::size_t i = v.size() - n; i < v.size(); ++i) s += v[i];
  return s / static_cast<double>(n);
}
}  // namespace detail

// Momentum: enter when the fast average crosses above the slow one; exit when it crosses back or after max_hold.
class Momentum final : public Strategy {
 public:
  Momentum(int fast, int slow, int max_hold) : fast_(fast), slow_(slow), max_hold_(max_hold) {}
  std::string name() const override {
    return "momentum(fast=" + std::to_string(fast_) + ",slow=" + std::to_string(slow_) + ",hold=" + std::to_string(max_hold_) + ")";
  }
  void reset(std::size_t n) override { px_.assign(n, {}); armed_.assign(n, 1); }
  Signal on_bar(std::uint32_t inst, const Bar& b, bool in_pos, int held) override {
    auto& v = px_[inst];
    v.push_back(rupees(b.c));
    if (v.size() < static_cast<std::size_t>(slow_)) return Signal::None;
    const double f = detail::sma(v, static_cast<std::size_t>(fast_)), s = detail::sma(v, static_cast<std::size_t>(slow_));
    if (f <= s) armed_[inst] = 1;
    if (in_pos) return (f < s || held >= max_hold_) ? Signal::Exit : Signal::None;
    if (f > s && armed_[inst]) { armed_[inst] = 0; return Signal::EnterLong; }
    return Signal::None;
  }
 private:
  int fast_, slow_, max_hold_;
  std::vector<std::vector<double>> px_;
  std::vector<char> armed_;
};

// Mean reversion: enter when the close is `entry_z` deviations below its average; exit at the average or after max_hold.
class MeanReversion final : public Strategy {
 public:
  MeanReversion(int window, double entry_z, int max_hold) : n_(window), z_(entry_z), max_hold_(max_hold) {}
  std::string name() const override {
    return "meanrev(window=" + std::to_string(n_) + ",z=" + std::to_string(z_).substr(0, 4) + ",hold=" + std::to_string(max_hold_) + ")";
  }
  void reset(std::size_t n) override { px_.assign(n, {}); }
  Signal on_bar(std::uint32_t inst, const Bar& b, bool in_pos, int held) override {
    auto& v = px_[inst];
    v.push_back(rupees(b.c));
    if (v.size() < static_cast<std::size_t>(n_)) return Signal::None;
    const double m = detail::sma(v, static_cast<std::size_t>(n_));
    double var = 0;
    for (std::size_t i = v.size() - static_cast<std::size_t>(n_); i < v.size(); ++i) var += (v[i] - m) * (v[i] - m);
    const double sd = std::sqrt(var / static_cast<double>(n_));
    if (sd <= 0) return Signal::None;
    const double z = (v.back() - m) / sd;
    if (in_pos) return (z >= 0 || held >= max_hold_) ? Signal::Exit : Signal::None;
    return z <= -z_ ? Signal::EnterLong : Signal::None;
  }
 private:
  int n_;
  double z_;
  int max_hold_;
  std::vector<std::vector<double>> px_;
};

}  // namespace mc
