// Bar-driven backtest engine and walk-forward validation. Everything produced here is SIMULATION.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "mc/exchange.hpp"
#include "mc/portfolio.hpp"
#include "mc/risk.hpp"
#include "mc/strategy.hpp"

namespace mc {

struct Instrument {
  std::string sym;
  std::vector<Bar> bars;
  Qty lot = 1;   // contract lot size; real lot sizes must be loaded from exchange files
};

struct StratSpec {
  std::unique_ptr<Strategy> s;
  double alloc = 0.5;   // share of account equity this strategy may use
};

struct BtConfig {
  double capital = 100000.0;
  RiskLimits risk;
  ExchConfig ex;
  double per_trade = 0.5;        // share of the strategy allocation used per trade
  std::int32_t from = 0, to = 99999999;
  std::int32_t trade_from = 0;   // bars before this date only warm up the strategies
  std::string audit_path;        // optional JSONL audit trail
};

struct BtResult {
  std::vector<std::int32_t> dates;
  std::vector<double> equity;
  std::vector<double> strat_pnl;
  double ret = 0, max_dd = 0, sharpe = 0, gross_profit = 0, gross_loss = 0, fees = 0, final_equity = 0;
  int orders = 0, fills = 0, trades = 0, wins = 0, stops = 0, halts = 0, locks = 0, strat_halts = 0;
  int risk_rejects = 0, venue_rejects = 0, unaffordable = 0, unprotected = 0, oversell = 0, osm_errors = 0;
  std::array<int, static_cast<std::size_t>(Reject::COUNT)> rejects{};
};

inline void finalize(BtResult& r, double capital) {
  r.final_equity = r.equity.empty() ? capital : r.equity.back();
  r.ret = r.final_equity / capital - 1.0;
  double peak = capital, prev = capital, sum = 0, sum2 = 0;
  for (double e : r.equity) {
    peak = std::max(peak, e);
    r.max_dd = std::max(r.max_dd, 1.0 - e / peak);
    const double x = e / prev - 1.0;
    sum += x; sum2 += x * x; prev = e;
  }
  const double n = static_cast<double>(r.equity.size());
  if (n > 1) {
    const double mean = sum / n, var = sum2 / n - mean * mean;
    r.sharpe = var > 1e-18 ? mean / std::sqrt(var) * std::sqrt(252.0) : 0.0;
  }
}

inline BtResult run_backtest(const std::vector<Instrument>& insts, std::vector<StratSpec>& strats, const BtConfig& cfg) {
  const std::size_t NI = insts.size(), NS = strats.size();
  BtResult R;
  R.strat_pnl.assign(NS, 0.0);
  for (auto& s : strats) s.s->reset(NI);

  std::vector<std::int32_t> dates;
  for (const auto& in : insts)
    for (const Bar& b : in.bars) if (b.date >= cfg.from && b.date <= cfg.to) dates.push_back(b.date);
  std::sort(dates.begin(), dates.end());
  dates.erase(std::unique(dates.begin(), dates.end()), dates.end());

  Portfolio pf(cfg.capital, NS);
  RiskEngine risk(cfg.risk);
  SimExchange ex(cfg.ex, NI);
  std::vector<Price> marks(NI, 0);
  std::vector<std::size_t> cur(NI, 0);
  std::vector<char> has(NI, 0), strat_halted(NS, 0);
  std::vector<double> snap(NS, 0.0);
  struct Slot { int held = 0; OrderId stop = 0, exit = 0, entry = 0; double trade_pnl = 0; };
  std::vector<Slot> slots(NS * NI);
  OrderId next_id = 1;
  double equity_now = cfg.capital;
  Ts now = 0;
  std::int32_t today = 0;
  const Ts kHour = 3600LL * 1000000000LL;

  std::ofstream audit;
  if (!cfg.audit_path.empty()) audit.open(cfg.audit_path);
  auto log = [&](const char* ev, const Order& o, const char* extra) {
    if (!audit.is_open()) return;
    audit << "{\"sim\":true,\"date\":" << today << ",\"ev\":\"" << ev << "\",\"id\":" << o.id << ",\"sym\":\"" << insts[o.inst].sym
          << "\",\"strat\":" << o.strat << ",\"side\":\"" << to_string(o.side) << "\",\"type\":\"" << to_string(o.type)
          << "\",\"px\":" << rupees(o.px) << ",\"qty\":" << o.qty << ",\"info\":\"" << extra << "\"}\n";
  };

  auto on_fill = [&](const Fill& f) {
    const double fee = ex.fee(f.px, f.qty);
    const Position before = pf.pos(f.strat, f.inst);
    if (f.side == Side::Sell && f.qty > before.qty) ++R.oversell;   // must never happen: long-only
    const double rl = pf.apply(f, fee);
    Slot& sl = slots[f.strat * NI + f.inst];
    sl.trade_pnl += rl - fee;
    ++R.fills;
    if (audit.is_open())
      audit << "{\"sim\":true,\"date\":" << f.date << ",\"ev\":\"FILL\",\"id\":" << f.id << ",\"sym\":\"" << insts[f.inst].sym
            << "\",\"side\":\"" << to_string(f.side) << "\",\"px\":" << rupees(f.px) << ",\"qty\":" << f.qty << ",\"fee\":" << fee << "}\n";
    if (before.qty != 0 && pf.pos(f.strat, f.inst).qty == 0) {
      ++R.trades;
      if (sl.trade_pnl > 0) { ++R.wins; R.gross_profit += sl.trade_pnl; } else R.gross_loss += -sl.trade_pnl;
      sl.trade_pnl = 0; sl.held = 0;
    }
  };

  // The only path by which an order can reach the exchange: state machine -> risk engine -> venue.
  auto send = [&](Order o, double ref_px) -> bool {
    State st = o.st;
    next_state(o.st, Ev::Submit, st); o.st = st;
    RiskCtx c;
    c.equity = equity_now; c.gross = pf.gross(marks); c.open_orders = ex.live(); c.positions = pf.open_positions();
    c.pos_qty = pf.pos(o.strat, o.inst).qty; c.ref_px = ref_px; c.strat_halted = strat_halted[o.strat] != 0;
    c.cash = pf.cash();
    const Reject rj = risk.check(o, c, now);
    if (rj != Reject::None) {
      ++R.risk_rejects; ++R.rejects[static_cast<std::size_t>(rj)];
      log("RISK_VETO", o, to_string(rj));
      return false;
    }
    next_state(o.st, Ev::RiskPass, st); o.st = st;
    ++R.orders;
    if (!ex.submit(o)) { ++R.venue_rejects; log("VENUE_REJECT", o, ""); return false; }
    log("ORDER", o, "");
    return true;
  };
  auto cancel = [&](OrderId id) { risk.take_rate_token(now); ex.cancel(id); };   // cancels count against the rate cap

  // Keeps exactly one protective stop per open position, sized to the position.
  auto sync_stops = [&] {
    for (std::size_t s = 0; s < NS; ++s)
      for (std::size_t i = 0; i < NI; ++i) {
        Slot& sl = slots[s * NI + i];
        const Position p = pf.pos(static_cast<std::uint16_t>(s), static_cast<std::uint32_t>(i));
        const Order* st = sl.stop ? ex.find(sl.stop) : nullptr;
        if (!st) sl.stop = 0;
        if (p.qty <= 0) { if (st) { cancel(sl.stop); sl.stop = 0; } continue; }
        if (sl.exit && ex.find(sl.exit)) continue;   // an exit is already working
        sl.exit = 0;
        if (st && st->qty - st->filled == p.qty) continue;
        if (st) { cancel(sl.stop); sl.stop = 0; }
        Order o;
        o.id = next_id++; o.inst = static_cast<std::uint32_t>(i); o.strat = static_cast<std::uint16_t>(s);
        o.side = Side::Sell; o.type = OrdType::Stop; o.qty = p.qty; o.date = today;
        const Price raw = static_cast<Price>(p.avg * 100.0 * (1.0 - cfg.risk.stop_pct));
        o.px = std::max(cfg.ex.tick, (raw / cfg.ex.tick) * cfg.ex.tick);
        if (send(o, p.avg)) sl.stop = o.id; else ++R.unprotected;
      }
  };
  auto strat_value = [&](std::size_t s) {
    const auto u = static_cast<std::uint16_t>(s);
    return pf.realized(u) - pf.fees(u) + pf.unrealized(u, marks);
  };

  for (std::size_t di = 0; di < dates.size(); ++di) {
    today = dates[di];
    for (std::size_t i = 0; i < NI; ++i) {
      const auto& bars = insts[i].bars;
      while (cur[i] < bars.size() && bars[cur[i]].date < today) ++cur[i];
      has[i] = cur[i] < bars.size() && bars[cur[i]].date == today;
    }
    if (today < cfg.trade_from) {   // warm-up only: strategies see the bar, nothing trades
      for (std::size_t s = 0; s < NS; ++s)
        for (std::size_t i = 0; i < NI; ++i)
          if (has[i]) strats[s].s->on_bar(static_cast<std::uint32_t>(i), insts[i].bars[cur[i]], false, 0);
      continue;
    }
    risk.start_day(equity_now);
    for (std::size_t s = 0; s < NS; ++s) { snap[s] = strat_value(s); strat_halted[s] = 0; }
    now = static_cast<Ts>(di) * 24 * kHour;

    for (std::size_t i = 0; i < NI; ++i) if (has[i]) ex.on_open(static_cast<std::uint32_t>(i), insts[i].bars[cur[i]], on_fill);
    now += kHour; sync_stops();
    for (std::size_t i = 0; i < NI; ++i) if (has[i]) ex.on_range(static_cast<std::uint32_t>(i), insts[i].bars[cur[i]], on_fill);
    for (std::size_t i = 0; i < NI; ++i) if (has[i]) marks[i] = insts[i].bars[cur[i]].c;
    now += kHour; sync_stops();

    equity_now = pf.equity(marks);
    const bool was_h = risk.halted(), was_l = risk.locked();
    risk.on_equity(equity_now);
    if (!was_h && risk.halted()) {   // stop opening risk; protective stops stay
      ++R.halts;
      ex.cancel_if([](const Order& o) { return o.side == Side::Buy; });
    }
    if (!was_l && risk.locked()) ++R.locks;
    for (std::size_t s = 0; s < NS; ++s)
      if (!strat_halted[s] && strat_value(s) - snap[s] <= -cfg.risk.strat_daily_loss * risk.day_start()) {
        strat_halted[s] = 1; ++R.strat_halts;
      }

    now += kHour;
    for (std::size_t s = 0; s < NS; ++s)
      for (std::size_t i = 0; i < NI; ++i) {
        if (!has[i]) continue;
        const Bar& b = insts[i].bars[cur[i]];
        Slot& sl = slots[s * NI + i];
        const Position p = pf.pos(static_cast<std::uint16_t>(s), static_cast<std::uint32_t>(i));
        const bool in_pos = p.qty > 0;
        if (in_pos) ++sl.held;
        const Signal sig = strats[s].s->on_bar(static_cast<std::uint32_t>(i), b, in_pos, sl.held);
        Order o;
        o.inst = static_cast<std::uint32_t>(i); o.strat = static_cast<std::uint16_t>(s); o.type = OrdType::Market; o.date = today;
        if (sig == Signal::EnterLong && !in_pos && !(sl.entry && ex.find(sl.entry))) {
          const double budget = std::min(strats[s].alloc * equity_now * cfg.per_trade, cfg.risk.max_order_notional * equity_now);
          const Qty lots = static_cast<Qty>(budget / rupees(b.c)) / insts[i].lot;
          if (lots < 1) { ++R.unaffordable; continue; }
          o.id = next_id++; o.side = Side::Buy; o.qty = lots * insts[i].lot;
          if (send(o, rupees(b.c))) sl.entry = o.id;
        } else if (sig == Signal::Exit && in_pos && !(sl.exit && ex.find(sl.exit))) {
          o.id = next_id++; o.side = Side::Sell; o.qty = p.qty;
          if (send(o, rupees(b.c))) {
            sl.exit = o.id;
            if (sl.stop) { cancel(sl.stop); sl.stop = 0; }
          }
        }
      }
    sync_stops();

    R.dates.push_back(today);
    R.equity.push_back(equity_now);
    for (const Order& o : ex.closed()) if (o.type == OrdType::Stop && o.st == State::Filled) ++R.stops;
    ex.closed().clear();
  }
  for (std::size_t s = 0; s < NS; ++s) R.strat_pnl[s] = strat_value(s);
  R.fees = pf.total_fees();
  R.osm_errors = ex.osm_errors();
  finalize(R, cfg.capital);
  return R;
}

// ---------- walk-forward ----------

struct ParamSet {
  std::string label;
  std::function<std::vector<StratSpec>()> make;
};
struct WfFold {
  std::int32_t train_from = 0, train_to = 0, test_from = 0, test_to = 0;
  std::string chosen;
  double train_ret = 0, test_ret = 0, test_dd = 0;
  int test_trades = 0;
};
struct WfResult {
  std::vector<WfFold> folds;
  double oos_ret = 0, worst_dd = 0;
  int oos_trades = 0;
};

// Rolling walk-forward: pick the best parameter set on each training window, then trade it,
// untouched, on the following test window. Only test-window results count as out-of-sample.
inline WfResult walk_forward(const std::vector<Instrument>& insts, const std::vector<ParamSet>& ps, BtConfig cfg,
                             std::size_t train_bars, std::size_t test_bars, std::size_t warmup) {
  std::vector<std::int32_t> dates;
  for (const auto& in : insts) for (const Bar& b : in.bars) dates.push_back(b.date);
  std::sort(dates.begin(), dates.end());
  dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
  WfResult W;
  double comp = 1.0;
  cfg.audit_path.clear();
  for (std::size_t a = 0; a + train_bars + test_bars <= dates.size(); a += test_bars) {
    WfFold f;
    const std::size_t t0 = a + train_bars, t1 = t0 + test_bars;
    f.train_from = dates[a]; f.train_to = dates[t0 - 1]; f.test_from = dates[t0]; f.test_to = dates[t1 - 1];
    std::size_t best = 0;
    double best_sh = -1e300, best_ret = -1e300;
    for (std::size_t k = 0; k < ps.size(); ++k) {
      auto st = ps[k].make();
      BtConfig c = cfg; c.from = f.train_from; c.trade_from = f.train_from; c.to = f.train_to;
      const BtResult r = run_backtest(insts, st, c);
      if (r.sharpe > best_sh || (r.sharpe == best_sh && r.ret > best_ret)) { best_sh = r.sharpe; best_ret = r.ret; best = k; }
    }
    auto st = ps[best].make();
    BtConfig c = cfg; c.from = dates[t0 > warmup ? t0 - warmup : 0]; c.trade_from = f.test_from; c.to = f.test_to;
    const BtResult r = run_backtest(insts, st, c);
    f.chosen = ps[best].label; f.train_ret = best_ret; f.test_ret = r.ret; f.test_dd = r.max_dd; f.test_trades = r.trades;
    comp *= 1.0 + r.ret;
    W.worst_dd = std::max(W.worst_dd, r.max_dd);
    W.oos_trades += r.trades;
    W.folds.push_back(f);
  }
  W.oos_ret = comp - 1.0;
  return W;
}

}  // namespace mc
