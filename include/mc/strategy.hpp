// Strategy interface and the two launch strategies. Long-only, daily bars.
// Strategies only emit intents; sizing, stops and every limit are enforced by the engine and risk engine.
#pragma once
#include <cmath>
#include <algorithm>
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
  // Symbol of each instrument, in instrument order. Called once after reset().
  virtual void set_universe(const std::vector<std::string>& syms) { (void)syms; }
  // Called once per date, before on_bar, with every instrument's bar for that date (nullptr if none).
  virtual void on_day(std::int32_t date, const std::vector<const Bar*>& bars) { (void)date; (void)bars; }
  // Called once per instrument per bar, after the close. `held` = bars the position has been open.
  virtual Signal on_bar(std::uint32_t inst, const Bar& b, bool in_pos, int held) = 0;
};

// Market regime: ON when the index proxy (NIFTYBEES, the NSE Nifty 50 ETF) closes above its
// `n`-day average. If the proxy is not in the universe the filter cannot be applied: has_index() is false
// and the regime is reported as ON, so callers must report that the filter was inactive.
class Regime {
 public:
  explicit Regime(int n = 200, std::string proxy = "NIFTYBEES") : n_(n), proxy_(std::move(proxy)) {}
  void set_universe(const std::vector<std::string>& syms) {
    idx_ = -1; px_.clear(); on_ = false;
    for (std::size_t i = 0; i < syms.size(); ++i) if (syms[i] == proxy_) idx_ = static_cast<int>(i);
  }
  void on_day(const std::vector<const Bar*>& bars) {
    if (idx_ < 0 || !bars[static_cast<std::size_t>(idx_)]) return;
    px_.push_back(rupees(bars[static_cast<std::size_t>(idx_)]->c));
    if (px_.size() < static_cast<std::size_t>(n_)) { on_ = false; return; }
    double s = 0;
    for (std::size_t i = px_.size() - static_cast<std::size_t>(n_); i < px_.size(); ++i) s += px_[i];
    on_ = px_.back() > s / n_;
  }
  bool has_index() const { return idx_ >= 0; }
  int index() const { return idx_; }
  bool on() const { return idx_ < 0 || on_; }
 private:
  int n_;
  std::string proxy_;
  int idx_ = -1;
  std::vector<double> px_;
  bool on_ = false;
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
  Momentum(int fast, int slow, int max_hold, bool regime = false) : fast_(fast), slow_(slow), max_hold_(max_hold), use_regime_(regime) {}
  std::string name() const override {
    return std::string(use_regime_ ? "momentum+regime" : "momentum") + "(fast=" + std::to_string(fast_) + ",slow=" + std::to_string(slow_) + ",hold=" + std::to_string(max_hold_) + ")";
  }
  void reset(std::size_t n) override { px_.assign(n, {}); armed_.assign(n, 1); }
  void set_universe(const std::vector<std::string>& syms) override { reg_.set_universe(syms); }
  void on_day(std::int32_t, const std::vector<const Bar*>& bars) override { if (use_regime_) reg_.on_day(bars); }
  Signal on_bar(std::uint32_t inst, const Bar& b, bool in_pos, int held) override {
    if (use_regime_ && static_cast<int>(inst) == reg_.index()) return Signal::None;   // the proxy is a filter, not a trade
    auto& v = px_[inst];
    v.push_back(rupees(b.c));
    if (v.size() < static_cast<std::size_t>(slow_)) return Signal::None;
    const double f = detail::sma(v, static_cast<std::size_t>(fast_)), s = detail::sma(v, static_cast<std::size_t>(slow_));
    if (f <= s) armed_[inst] = 1;
    if (in_pos) return (f < s || held >= max_hold_ || (use_regime_ && !reg_.on())) ? Signal::Exit : Signal::None;
    if (f > s && armed_[inst] && (!use_regime_ || reg_.on())) { armed_[inst] = 0; return Signal::EnterLong; }
    return Signal::None;
  }
  bool filter_active() const { return !use_regime_ || reg_.has_index(); }
 private:
  int fast_, slow_, max_hold_;
  bool use_regime_;
  Regime reg_;
  std::vector<std::vector<double>> px_;
  std::vector<char> armed_;
};

// Mean reversion: enter when the close is `entry_z` deviations below its average; exit at the average or after max_hold.
class MeanReversion final : public Strategy {
 public:
  // uptrend_only: enter only when the close is above the instrument's own 200-day average.
  MeanReversion(int window, double entry_z, int max_hold, bool uptrend_only = false)
      : n_(window), z_(entry_z), max_hold_(max_hold), up_(uptrend_only) {}
  std::string name() const override {
    return std::string(up_ ? "meanrev-uptrend" : "meanrev") + "(window=" + std::to_string(n_) + ",z=" + std::to_string(z_).substr(0, 4) + ",hold=" + std::to_string(max_hold_) + ")";
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
    if (up_ && (v.size() < 200 || v.back() <= detail::sma(v, 200))) return Signal::None;
    return z <= -z_ ? Signal::EnterLong : Signal::None;
  }
 private:
  int n_;
  double z_;
  int max_hold_;
  bool up_;
  std::vector<std::vector<double>> px_;
};

// Cross-sectional momentum rotation: every `rebal` trading days, rank instruments by their `lookback`-day
// return and hold the top `top` with a positive return. With `regime`, hold nothing while the index proxy is
// below its 200-day average. Between rebalances positions are kept; a stopped-out position is not re-entered
// until the next rebalance.
class Rotation final : public Strategy {
 public:
  Rotation(int lookback, int top, int rebal, bool regime) : lb_(lookback), top_(top), rebal_(rebal), use_regime_(regime) {}
  std::string name() const override {
    return std::string(use_regime_ ? "rotation+regime" : "rotation") + "(lookback=" + std::to_string(lb_) + ",top=" + std::to_string(top_) + ",rebal=" + std::to_string(rebal_) + ")";
  }
  void reset(std::size_t n) override { px_.assign(n, {}); target_.assign(n, 0); day_ = 0; rebal_today_ = false; }
  void set_universe(const std::vector<std::string>& syms) override { reg_.set_universe(syms); }
  void on_day(std::int32_t, const std::vector<const Bar*>& bars) override {
    reg_.on_day(bars);
    for (std::size_t i = 0; i < bars.size(); ++i) if (bars[i]) px_[i].push_back(rupees(bars[i]->c));
    rebal_today_ = (day_++ % rebal_) == 0;
    if (!rebal_today_) return;
    std::vector<std::pair<double, std::size_t>> score;
    for (std::size_t i = 0; i < px_.size(); ++i) {
      if (static_cast<int>(i) == reg_.index() || !bars[i]) continue;   // the proxy is a filter, not a holding
      const auto& v = px_[i];
      if (v.size() <= static_cast<std::size_t>(lb_)) continue;
      const double r = v.back() / v[v.size() - 1 - static_cast<std::size_t>(lb_)] - 1.0;
      if (r > 0) score.push_back({r, i});
    }
    std::sort(score.begin(), score.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    std::fill(target_.begin(), target_.end(), 0);
    if (use_regime_ && !reg_.on()) return;
    for (std::size_t k = 0; k < score.size() && k < static_cast<std::size_t>(top_); ++k) target_[score[k].second] = 1;
  }
  Signal on_bar(std::uint32_t inst, const Bar&, bool in_pos, int) override {
    if (!rebal_today_) return Signal::None;
    if (in_pos && !target_[inst]) return Signal::Exit;
    if (!in_pos && target_[inst]) return Signal::EnterLong;
    return Signal::None;
  }
  bool filter_active() const { return !use_regime_ || reg_.has_index(); }
 private:
  int lb_, top_, rebal_;
  bool use_regime_;
  Regime reg_;
  std::vector<std::vector<double>> px_;
  std::vector<char> target_;
  long day_ = 0;
  bool rebal_today_ = false;
};

}  // namespace mc
