// Positions, cash and PnL. Positions are kept per (strategy, instrument) for attribution.
#pragma once
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>
#include "mc/types.hpp"

namespace mc {

struct Position { Qty qty = 0; double avg = 0; };  // avg in rupees

class Portfolio {
 public:
  Portfolio(double cash, std::size_t n_strat) : cash_(cash), realized_(n_strat, 0.0), fees_(n_strat, 0.0) {}

  static std::uint64_t key(std::uint16_t strat, std::uint32_t inst) {
    return (static_cast<std::uint64_t>(strat) << 32) | inst;
  }
  Position pos(std::uint16_t strat, std::uint32_t inst) const {
    auto it = pos_.find(key(strat, inst));
    return it == pos_.end() ? Position{} : it->second;
  }

  // Applies a fill. Returns the realized PnL of this fill (before fee), in rupees.
  double apply(const Fill& f, double fee) {
    const double px = rupees(f.px);
    const Qty sq = f.side == Side::Buy ? f.qty : -f.qty;
    cash_ += (f.side == Side::Buy ? -1.0 : 1.0) * px * static_cast<double>(f.qty) - fee;
    fees_[f.strat] += fee;
    Position& p = pos_[key(f.strat, f.inst)];
    double realized = 0;
    if (p.qty == 0 || (p.qty > 0) == (sq > 0)) {
      const double a = static_cast<double>(std::llabs(p.qty)), b = static_cast<double>(f.qty);
      p.avg = (a * p.avg + b * px) / (a + b);
      p.qty += sq;
    } else {
      const Qty closed = std::min<Qty>(f.qty, std::llabs(p.qty));
      realized = (px - p.avg) * static_cast<double>(closed) * (p.qty > 0 ? 1.0 : -1.0);
      p.qty += sq;
      if (p.qty != 0 && (p.qty > 0) == (sq > 0)) p.avg = px;   // flipped through zero
    }
    realized_[f.strat] += realized;
    if (p.qty == 0) pos_.erase(key(f.strat, f.inst));
    return realized;
  }

  double cash() const { return cash_; }
  // Restores a saved account (used by the terminal after a restart). Strategy 0 only.
  void restore(double cash, double realized, double fees, const std::vector<std::pair<std::uint32_t, Position>>& positions) {
    cash_ = cash; realized_.assign(realized_.size(), 0.0); fees_.assign(fees_.size(), 0.0);
    if (!realized_.empty()) { realized_[0] = realized; fees_[0] = fees; }
    pos_.clear();
    for (const auto& [inst, p] : positions) if (p.qty != 0) pos_[key(0, inst)] = p;
  }
  void deposit(double amount) { cash_ += amount; }
  double realized(std::uint16_t s) const { return realized_[s]; }
  double fees(std::uint16_t s) const { return fees_[s]; }
  double total_fees() const { double t = 0; for (double f : fees_) t += f; return t; }
  int open_positions() const { return static_cast<int>(pos_.size()); }

  double equity(const std::vector<Price>& marks) const {
    double e = cash_;
    for (const auto& [k, p] : pos_) e += static_cast<double>(p.qty) * rupees(marks[k & 0xffffffffu]);
    return e;
  }
  double gross(const std::vector<Price>& marks) const {
    double g = 0;
    for (const auto& [k, p] : pos_) g += std::fabs(static_cast<double>(p.qty) * rupees(marks[k & 0xffffffffu]));
    return g;
  }
  double unrealized(std::uint16_t strat, const std::vector<Price>& marks) const {
    double u = 0;
    for (const auto& [k, p] : pos_)
      if ((k >> 32) == strat) u += (rupees(marks[k & 0xffffffffu]) - p.avg) * static_cast<double>(p.qty);
    return u;
  }

 private:
  double cash_;
  std::vector<double> realized_, fees_;
  std::unordered_map<std::uint64_t, Position> pos_;
};

}  // namespace mc
