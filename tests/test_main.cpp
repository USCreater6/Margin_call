// Test suite. Plain asserts with a tiny harness so there are no external dependencies.
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
#include "mc/backtest.hpp"
#include "mc/latency.hpp"
#include "mc/order_book.hpp"
#include "mc/osm.hpp"
#include "mc/replay.hpp"
#include "mc/risk.hpp"

using namespace mc;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b) CHECK(std::fabs((a) - (b)) < 1e-6)

static std::vector<Trade> g_tr;
static void rec(const Trade& t) { g_tr.push_back(t); }

static void test_book_priority() {
  OrderBook b; g_tr.clear();
  CHECK(b.add_limit(1, Side::Sell, 1000, 10, rec) == 10);
  CHECK(b.add_limit(2, Side::Sell, 1000, 10, rec) == 10);   // same price, later
  CHECK(b.add_limit(3, Side::Sell, 995, 5, rec) == 5);      // better price
  CHECK(b.best_ask() == 995 && b.best_bid() == 0);
  CHECK(b.add_limit(4, Side::Buy, 1000, 12, rec) == 0);
  CHECK(g_tr.size() == 2);
  CHECK(g_tr[0].maker == 3 && g_tr[0].px == 995 && g_tr[0].qty == 5);   // price priority
  CHECK(g_tr[1].maker == 1 && g_tr[1].px == 1000 && g_tr[1].qty == 7);  // time priority
  CHECK(b.open_qty(1) == 3 && b.open_qty(2) == 10 && !b.has(3) && !b.has(4));
  CHECK(b.qty_at(Side::Sell, 1000) == 13);
}
static void test_book_partial_rest_cancel() {
  OrderBook b; g_tr.clear();
  b.add_limit(1, Side::Sell, 1000, 5, rec);
  CHECK(b.add_limit(2, Side::Buy, 1000, 8, rec) == 3);   // 5 trade, 3 rest
  CHECK(b.best_bid() == 1000 && b.best_ask() == 0 && b.open_qty(2) == 3);
  CHECK(b.cancel(2) && !b.cancel(2) && b.size() == 0 && b.best_bid() == 0);
  CHECK(b.add_limit(9, Side::Buy, 1000, 0, rec) == -1);   // bad qty
  b.add_limit(5, Side::Buy, 900, 1, rec);
  CHECK(b.add_limit(5, Side::Buy, 900, 1, rec) == -1);    // duplicate id
}
static void test_book_market_and_limit_bound() {
  OrderBook b; g_tr.clear();
  b.add_limit(1, Side::Buy, 990, 4, rec); b.add_limit(2, Side::Buy, 985, 4, rec);
  CHECK(b.add_market(3, Side::Sell, 10, rec) == 2);       // sweeps both levels, 2 unfilled, nothing rests
  CHECK(g_tr.size() == 2 && g_tr[0].px == 990 && g_tr[1].px == 985 && b.size() == 0);
  g_tr.clear();
  b.add_limit(4, Side::Sell, 1000, 5, rec); b.add_limit(5, Side::Sell, 1010, 5, rec);
  CHECK(b.add_limit(6, Side::Buy, 1005, 9, rec) == 4);    // must not trade through its limit
  CHECK(g_tr.size() == 1 && g_tr[0].px == 1000 && b.best_bid() == 1005 && b.best_ask() == 1010);
  CHECK(b.add_market(7, Side::Buy, 1, rec) == 0);
  OrderBook e; CHECK(e.add_market(1, Side::Buy, 5, rec) == 5);   // empty book
}
static void test_book_modify() {
  OrderBook b; g_tr.clear();
  b.add_limit(1, Side::Sell, 1000, 10, rec); b.add_limit(2, Side::Sell, 1000, 10, rec);
  CHECK(b.modify(1, 1000, 4, rec));          // decrease keeps priority
  b.add_market(3, Side::Buy, 4, rec);
  CHECK(g_tr.back().maker == 1 && !b.has(1));
  b.add_limit(4, Side::Sell, 1000, 10, rec); // order: 2, 4
  CHECK(b.modify(2, 1000, 20, rec));         // increase loses priority -> order: 4, 2
  g_tr.clear(); b.add_market(5, Side::Buy, 1, rec);
  CHECK(g_tr.back().maker == 4);
  CHECK(b.modify(2, 1005, 20, rec) && b.qty_at(Side::Sell, 1005) == 20 && b.qty_at(Side::Sell, 1000) == 9);
  CHECK(!b.modify(99, 1000, 1, rec));
}
// Property test: random order flow must keep the book uncrossed and conserve quantity.
static void test_book_property() {
  Rng rng(12345);
  OrderBook b;
  long long resting_in = 0, traded = 0, cancelled = 0, mkt_unfilled = 0, mkt_in = 0;
  std::vector<OrderId> live;
  OrderId id = 1;
  bool crossed = false, bad_px = false;
  for (int i = 0; i < 20000; ++i) {
    const auto r = rng.next() % 10;
    const Side s = (rng.next() & 1) ? Side::Buy : Side::Sell;
    const Qty q = 1 + static_cast<Qty>(rng.next() % 50);
    if (r < 6) {
      const Price px = 1000 + 5 * static_cast<Price>(rng.next() % 40);
      const OrderId me = id++;
      long long t = 0;
      const Qty rest = b.add_limit(me, s, px, q, [&](const Trade& tr) {
        t += tr.qty;
        if (s == Side::Buy ? tr.px > px : tr.px < px) bad_px = true;
      });
      traded += t; resting_in += rest; cancelled += 0;
      if (rest > 0) live.push_back(me);
      if (t + rest != q) bad_px = true;
    } else if (r < 8 && !live.empty()) {
      const std::size_t k = rng.next() % live.size();
      const Qty oq = b.open_qty(live[k]);
      if (b.cancel(live[k])) cancelled += oq;
      live[k] = live.back(); live.pop_back();
    } else {
      long long t = 0;
      const Qty unf = b.add_market(id++, s, q, [&](const Trade& tr) { t += tr.qty; });
      mkt_in += q; mkt_unfilled += unf; traded += t;
      if (t + unf != q) bad_px = true;
    }
    if (b.best_bid() && b.best_ask() && b.best_bid() >= b.best_ask()) crossed = true;
  }
  long long open = 0;
  for (OrderId x : live) open += b.open_qty(x);
  CHECK(!crossed);
  CHECK(!bad_px);
  // every resting unit is either still open, was cancelled, or was consumed as a maker
  long long maker_consumed = resting_in - open - cancelled;
  CHECK(maker_consumed >= 0);
  CHECK(maker_consumed == traded);
  (void)mkt_in; (void)mkt_unfilled;
}

static void test_osm() {
  State s = State::New, n = State::New;
  for (Ev e : {Ev::Submit, Ev::RiskPass, Ev::VenueAck, Ev::PartialFill, Ev::FullFill}) { CHECK(next_state(s, e, n)); s = n; }
  CHECK(s == State::Filled && is_terminal(s));
  for (int e = 0; e <= static_cast<int>(Ev::Expire); ++e)   // terminal states accept nothing
    for (State t : {State::Filled, State::Cancelled, State::RejectedRisk, State::RejectedVenue, State::Expired})
      CHECK(!next_state(t, static_cast<Ev>(e), n));
  CHECK(next_state(State::RiskCheck, Ev::RiskReject, n) && n == State::RejectedRisk);
  CHECK(next_state(State::Sent, Ev::VenueReject, n) && n == State::RejectedVenue);
  CHECK(next_state(State::Acked, Ev::CancelReq, n) && n == State::CancelPending);
  CHECK(next_state(State::CancelPending, Ev::CancelAck, n) && n == State::Cancelled);
  CHECK(next_state(State::CancelPending, Ev::FullFill, n) && n == State::Filled);   // fill wins the race
  CHECK(next_state(State::PartiallyFilled, Ev::ModifyReq, n) && n == State::ModifyPending);
  CHECK(next_state(State::ModifyPending, Ev::ModifyAck, n) && n == State::Acked);
  CHECK(next_state(State::Acked, Ev::Expire, n) && n == State::Expired);
  CHECK(!next_state(State::New, Ev::VenueAck, n));       // cannot skip the risk check
  CHECK(!next_state(State::New, Ev::RiskPass, n));
  CHECK(!next_state(State::RiskCheck, Ev::VenueAck, n));
  CHECK(!next_state(State::Sent, Ev::FullFill, n));
}

static Order mk(Side s, Qty q) { Order o; o.id = 1; o.side = s; o.qty = q; return o; }
static void test_risk() {
  RiskLimits L;
  RiskCtx c; c.equity = 100000; c.ref_px = 100;
  { RiskEngine r(L); r.start_day(100000);
    CHECK(r.check(mk(Side::Buy, 100), c, 0) == Reject::None);
    CHECK(r.check(mk(Side::Buy, 251), c, 0) == Reject::Notional);         // > 25% of equity
    CHECK(r.check(mk(Side::Buy, 0), c, 0) == Reject::BadOrder);
    RiskCtx d = c; d.open_orders = 6; CHECK(r.check(mk(Side::Buy, 1), d, 0) == Reject::OpenOrders);
    d = c; d.positions = 3; CHECK(r.check(mk(Side::Buy, 1), d, 0) == Reject::Positions);
    d.pos_qty = 5; CHECK(r.check(mk(Side::Buy, 1), d, 0) == Reject::None);  // adding to an existing position
    d = c; d.gross = 90000; CHECK(r.check(mk(Side::Buy, 200), d, 0) == Reject::Gross);   // no leverage
    d = c; d.strat_halted = true; CHECK(r.check(mk(Side::Buy, 1), d, 0) == Reject::StratHalted);
  }
  { RiskEngine r(L); r.start_day(100000);   // daily loss halt
    CHECK(!r.on_equity(97500)); CHECK(!r.halted());
    CHECK(r.on_equity(97000)); CHECK(r.halted());
    CHECK(r.check(mk(Side::Buy, 1), c, 0) == Reject::Halted);
    RiskCtx d = c; d.pos_qty = 10;
    CHECK(r.check(mk(Side::Sell, 10), d, 0) == Reject::None);              // reducing is still allowed
    CHECK(r.check(mk(Side::Sell, 11), d, 0) == Reject::Halted);            // but not flipping short
    r.start_day(97000); CHECK(!r.halted());
  }
  { RiskEngine r(L); r.start_day(100000);   // profit lockout
    CHECK(r.on_equity(110000)); CHECK(r.locked() && !r.halted());
    CHECK(r.check(mk(Side::Buy, 1), c, 0) == Reject::ProfitLock);
  }
  { RiskEngine r(L); r.start_day(100000);   // kill switch blocks everything
    r.kill(); RiskCtx d = c; d.pos_qty = 10;
    CHECK(r.check(mk(Side::Buy, 1), c, 0) == Reject::Killed);
    CHECK(r.check(mk(Side::Sell, 10), d, 0) == Reject::Killed);
    r.rearm(); CHECK(r.check(mk(Side::Buy, 1), c, 0) == Reject::None);
  }
  { RiskEngine r(L); r.start_day(100000);   // 8 per second
    int ok = 0;
    for (int i = 0; i < 20; ++i) ok += r.check(mk(Side::Buy, 1), c, 0) == Reject::None;
    CHECK(ok == 8);
    CHECK(r.check(mk(Side::Buy, 1), c, 124000000) == Reject::RateLimit);   // 0.124 s later: still none
    CHECK(r.check(mk(Side::Buy, 1), c, 126000000) == Reject::None);        // 0.126 s later: one token back
    ok = 0;
    for (int i = 0; i < 20; ++i) ok += r.check(mk(Side::Buy, 1), c, 5000000000LL) == Reject::None;
    CHECK(ok == 8);                                                        // never more than the burst
    for (int i = 0; i < 5; ++i) r.check(mk(Side::Buy, 251), c, 5000000000LL);   // vetoed orders use no token
    CHECK(r.check(mk(Side::Buy, 1), c, 5125000001LL) == Reject::None);
  }
}

static void test_portfolio() {
  Portfolio p(100000, 2);
  Fill f; f.strat = 0; f.inst = 0; f.side = Side::Buy; f.px = 10000; f.qty = 10;
  NEAR(p.apply(f, 3.0), 0.0);
  f.px = 11000; p.apply(f, 0);
  NEAR(p.pos(0, 0).avg, 105.0); CHECK(p.pos(0, 0).qty == 20);
  std::vector<Price> m{12000};
  NEAR(p.equity(m), 100000 - 1000 - 1100 - 3 + 20 * 120.0);
  NEAR(p.unrealized(0, m), 20 * 15.0); NEAR(p.gross(m), 2400.0);
  f.side = Side::Sell; f.px = 12000; f.qty = 5;
  NEAR(p.apply(f, 2.0), 75.0);
  NEAR(p.realized(0), 75.0); NEAR(p.total_fees(), 5.0); CHECK(p.pos(0, 0).qty == 15);
  f.qty = 15; p.apply(f, 0);
  CHECK(p.open_positions() == 0); NEAR(p.realized(0), 300.0);
  NEAR(p.cash(), 100000 + 300 - 5);
  CHECK(p.pos(1, 0).qty == 0); NEAR(p.realized(1), 0.0);      // attribution is per strategy
  f.strat = 1; f.side = Side::Sell; f.qty = 4; f.px = 10000; p.apply(f, 0);   // short, then flip long
  f.side = Side::Buy; f.qty = 10; f.px = 9000; NEAR(p.apply(f, 0), 40.0);
  CHECK(p.pos(1, 0).qty == 6); NEAR(p.pos(1, 0).avg, 90.0);
}

static Order sent(OrderId id, Side s, OrdType t, Price px, Qty q) {
  Order o; o.id = id; o.side = s; o.type = t; o.px = px; o.qty = q; o.st = State::Sent; return o;
}
static void test_exchange() {
  ExchConfig c; c.depth_per_level = 10; c.levels = 3; c.participation = 0;
  std::vector<Fill> fl;
  auto cb = [&](const Fill& f) { fl.push_back(f); };
  Bar b; b.date = 20240102; b.o = 10000; b.h = 10200; b.l = 9900; b.c = 10100; b.vol = 1000;
  { SimExchange ex(c, 1);   // market buy: best ask = open + 1 tick, plus 1 tick slippage
    CHECK(ex.submit(sent(1, Side::Buy, OrdType::Market, 0, 4)) && ex.live() == 1);
    ex.on_open(0, b, cb);
    CHECK(fl.size() == 1 && fl[0].px == 10010 && fl[0].qty == 4 && ex.live() == 0);
    CHECK(ex.closed().back().st == State::Filled);
  }
  { SimExchange ex(c, 1); fl.clear();   // market order larger than depth: walks levels, remainder cancelled
    ex.submit(sent(1, Side::Buy, OrdType::Market, 0, 35));
    ex.on_open(0, b, cb);
    Qty q = 0; for (auto& f : fl) q += f.qty;
    CHECK(fl.size() == 3 && q == 30 && fl[2].px == 10020);
    CHECK(ex.closed().back().st == State::Cancelled && ex.closed().back().filled == 30);
  }
  { SimExchange ex(c, 1); fl.clear();   // resting limit needs the bar to trade strictly through it
    ex.submit(sent(1, Side::Buy, OrdType::Limit, 9900, 5));   // low == limit: no fill (worst case)
    ex.submit(sent(2, Side::Buy, OrdType::Limit, 9905, 5));   // low < limit: fill at limit
    ex.on_open(0, b, cb); CHECK(fl.empty());
    ex.on_range(0, b, cb);
    CHECK(fl.size() == 1 && fl[0].id == 2 && fl[0].px == 9905 && ex.live() == 1);
    CHECK(ex.cancel(1) && ex.live() == 0 && ex.closed().back().st == State::Cancelled);
  }
  { SimExchange ex(c, 1); fl.clear();   // stop: touched intrabar -> stop minus slippage
    ex.submit(sent(1, Side::Sell, OrdType::Stop, 9900, 5));
    ex.on_open(0, b, cb); CHECK(fl.empty());
    ex.on_range(0, b, cb);
    CHECK(fl.size() == 1 && fl[0].px == 9895);
  }
  { SimExchange ex(c, 1); fl.clear();   // stop gapped through at the open -> fills at the worse open price
    ex.submit(sent(1, Side::Sell, OrdType::Stop, 10500, 5));
    ex.on_open(0, b, cb);
    CHECK(fl.size() == 1 && fl[0].px == 9990);   // best bid (open - 1 tick) - 1 tick slippage, far below the stop
  }
  { ExchConfig p = c; p.participation = 0.01; SimExchange ex(p, 1); fl.clear();   // participation cap -> partial fill
    ex.submit(sent(1, Side::Buy, OrdType::Limit, 9950, 25));
    ex.on_open(0, b, cb); ex.on_range(0, b, cb);
    CHECK(fl.size() == 1 && fl[0].qty == 10 && ex.live() == 1 && ex.find(1)->st == State::PartiallyFilled);
  }
  { ExchConfig r = c; r.reject_prob = 1.0; SimExchange ex(r, 1);   // injected venue reject
    CHECK(!ex.submit(sent(1, Side::Buy, OrdType::Market, 0, 1)) && ex.live() == 0);
    CHECK(ex.closed().back().st == State::RejectedVenue);
  }
  { SimExchange ex(c, 1); CHECK(ex.osm_errors() == 0);   // an order that skipped the risk check is flagged
    Order o = sent(1, Side::Buy, OrdType::Market, 0, 1); o.st = State::New; ex.submit(o);
    CHECK(ex.osm_errors() == 1);
  }
}

static void test_replay() {
  std::istringstream in(
      "# SYNTHETIC test\ndate,open,high,low,close,volume\n"
      "20240102,100,101,99,100.5,1000\n20240103,100.5,102,100,101,1100\n20240103,1,2,1,1,5\n"   // duplicate date
      "20240101,99,100,98,99.5,900\n"                                                           // out of order
      "20240104,101,100,102,101,10\n"                                                           // high < low
      "20240105,abc\n"                                                                          // malformed
      "20240120,101,103,100,102,1200\n");                                                       // gap
  const LoadResult r = parse_bars(in);
  CHECK(r.ok && r.synthetic && r.bars.size() == 4);
  CHECK(r.duplicates == 1 && r.out_of_order == 1 && r.bad_rows == 2 && r.gaps == 1);
  CHECK(r.bars[0].date == 20240101 && r.bars[2].date == 20240103 && r.bars[2].c == 10100);   // first copy kept
  CHECK(r.bars[1].o == 10000 && r.bars[1].c == 10050);
  CHECK(days_from_civil(19700101) == 0 && days_from_civil(20240301) - days_from_civil(20240228) == 2);
}

static std::vector<Instrument> make_data(std::uint64_t seed, int n, double drift) {
  Rng rng(seed);
  Instrument in; in.sym = "TEST";
  double c = 500;
  for (int i = 0; i < n; ++i) {
    const double o = c * (1 + (rng.uni() - 0.5) * 0.004);
    c = o * (1 + drift + (rng.uni() - 0.5) * 0.03);
    Bar b; b.date = 20200101 + i;   // dates only need to be increasing here
    b.o = static_cast<Price>(o * 100); b.c = static_cast<Price>(c * 100);
    b.h = std::max(b.o, b.c) + 50; b.l = std::min(b.o, b.c) - 50; b.vol = 100000;
    in.bars.push_back(b);
  }
  return {in};
}
static std::vector<StratSpec> both() {
  std::vector<StratSpec> v;
  v.push_back({std::make_unique<Momentum>(5, 20, 30), 0.5});
  v.push_back({std::make_unique<MeanReversion>(10, 1.5, 8), 0.5});
  return v;
}
static void test_backtest() {
  const auto data = make_data(99, 600, 0.0004);
  BtConfig cfg;
  auto s1 = both(); const BtResult a = run_backtest(data, s1, cfg);
  auto s2 = both(); const BtResult b = run_backtest(data, s2, cfg);
  CHECK(a.equity == b.equity && a.trades == b.trades && a.fills == b.fills);   // deterministic
  CHECK(a.trades > 0 && a.orders > 0);
  CHECK(a.oversell == 0 && a.osm_errors == 0 && a.unprotected == 0);
  CHECK(a.equity.size() == 600);
  NEAR(a.final_equity - cfg.capital, a.strat_pnl[0] + a.strat_pnl[1]);       // attribution adds up
  NEAR(a.ret, a.final_equity / cfg.capital - 1.0);

  BtConfig costly = cfg; costly.ex.fee_bps = 50;
  auto s3 = both(); const BtResult c = run_backtest(data, s3, costly);
  CHECK(c.fees > a.fees && c.final_equity < a.final_equity);                  // costs hurt

  BtConfig rej = cfg; rej.ex.reject_prob = 1.0;
  auto s4 = both(); const BtResult d = run_backtest(data, s4, rej);
  CHECK(d.trades == 0 && d.fills == 0 && d.venue_rejects > 0);
  NEAR(d.final_equity, cfg.capital);

  BtConfig poor = cfg; poor.capital = 1000;                                   // one share costs ~500: budget is 250
  auto s5 = both(); const BtResult e = run_backtest(data, s5, poor);
  CHECK(e.trades == 0 && e.unaffordable > 0);

  BtConfig warm = cfg; warm.trade_from = data[0].bars[300].date;
  auto s6 = both(); const BtResult w = run_backtest(data, s6, warm);
  CHECK(w.equity.size() == 300);
}
// A position must be closed by its protective stop; a gap-down day must trip the daily loss halt.
// The stop is fixed 2% below entry (not trailing), so the gap comes a few bars after entry.
static void test_backtest_stops_and_halt() {
  Instrument in; in.sym = "DROP";
  double c = 1000;
  for (int i = 0; i < 80; ++i) {
    Bar b; b.date = 20210101 + i;
    const double o = (i == 15) ? c * 0.80 : c;                    // day 15 opens 20% lower (gap)
    c = (i < 15) ? o * 1.01 : (i == 15 ? o : o * 0.995);          // rally, gap, slow bleed
    b.o = static_cast<Price>(o * 100); b.c = static_cast<Price>(c * 100);
    b.h = std::max(b.o, b.c); b.l = std::min(b.o, b.c); b.vol = 100000;
    in.bars.push_back(b);
  }
  std::vector<Instrument> data{in};
  std::vector<StratSpec> st;
  st.push_back({std::make_unique<Momentum>(3, 10, 100), 1.0});
  BtConfig cfg; cfg.per_trade = 0.25;
  const BtResult r = run_backtest(data, st, cfg);
  CHECK(r.trades >= 1 && r.stops >= 1);
  CHECK(r.halts >= 1);                 // 25% of equity losing 20% in a day is -5% > 3% limit
  CHECK(r.oversell == 0 && r.osm_errors == 0);
  CHECK(r.max_dd > 0.03);              // the gap blew through the stop: honest worst case
}
static void test_walk_forward() {
  const auto data = make_data(7, 500, 0.0005);
  std::vector<ParamSet> ps;
  for (int f : {3, 5})
    ps.push_back({"fast=" + std::to_string(f), [f] { std::vector<StratSpec> v; v.push_back({std::make_unique<Momentum>(f, 20, 30), 1.0}); return v; }});
  const WfResult w = walk_forward(data, ps, BtConfig{}, 200, 100, 40);
  CHECK(w.folds.size() == 3);
  double comp = 1;
  for (const auto& f : w.folds) {
    CHECK(f.train_to < f.test_from && f.test_from <= f.test_to);   // no look-ahead: test strictly after train
    CHECK(!f.chosen.empty());
    comp *= 1 + f.test_ret;
  }
  NEAR(w.oos_ret, comp - 1);
  const WfResult w2 = walk_forward(data, ps, BtConfig{}, 200, 100, 40);
  CHECK(w2.oos_ret == w.oos_ret);
}
static void test_latency() {
  LatencyRecorder r(1000);
  for (int i = 1; i <= 1000; ++i) r.add(i);
  const LatencyStats s = r.stats();
  CHECK(s.n == 1000 && s.p50 == 501 && s.p99 == 991 && s.max == 1000 && s.p999 == 1000);
}

int main() {
  struct T { const char* name; void (*fn)(); };
  const T tests[] = {
      {"order book: price-time priority", test_book_priority}, {"order book: partial fill, rest, cancel", test_book_partial_rest_cancel},
      {"order book: market orders, limit bound", test_book_market_and_limit_bound}, {"order book: modify", test_book_modify},
      {"order book: random-flow invariants", test_book_property}, {"order-state machine", test_osm},
      {"risk engine", test_risk}, {"portfolio and PnL", test_portfolio}, {"demo exchange fill model", test_exchange},
      {"data loader quality checks", test_replay}, {"backtest: determinism, costs, rejects", test_backtest},
      {"backtest: stop-loss and daily halt", test_backtest_stops_and_halt}, {"walk-forward", test_walk_forward},
      {"latency recorder", test_latency},
  };
  int failed_tests = 0;
  for (const T& t : tests) {
    const int before = g_fail;
    t.fn();
    std::printf("[%s] %s\n", g_fail == before ? "PASS" : "FAIL", t.name);
    failed_tests += g_fail != before;
  }
  std::printf("\n%zu tests, %d failed; %d checks, %d failed\n", sizeof tests / sizeof tests[0], failed_tests, g_checks, g_fail);
  return g_fail ? 1 : 0;
}
