// Runs the validation suite on a folder of bar CSVs and writes a SIMULATION report.
// Usage: mc_backtest --data <dir> --out <dir> [--capital <rupees>]
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "mc/backtest.hpp"
#include "mc/replay.hpp"

using namespace mc;

namespace {
std::vector<StratSpec> default_strats() {
  std::vector<StratSpec> v;
  v.push_back({std::make_unique<Momentum>(10, 50, 60), 0.5});
  v.push_back({std::make_unique<MeanReversion>(20, 2.0, 10), 0.5});
  return v;
}
std::string metrics(const BtResult& r) {
  std::ostringstream o;
  o << "\"ret\":" << r.ret << ",\"max_dd\":" << r.max_dd << ",\"sharpe\":" << r.sharpe << ",\"final_equity\":" << r.final_equity
    << ",\"trades\":" << r.trades << ",\"wins\":" << r.wins << ",\"orders\":" << r.orders << ",\"fills\":" << r.fills
    << ",\"stops\":" << r.stops << ",\"fees\":" << r.fees << ",\"gross_profit\":" << r.gross_profit << ",\"gross_loss\":" << r.gross_loss
    << ",\"risk_rejects\":" << r.risk_rejects << ",\"venue_rejects\":" << r.venue_rejects << ",\"halts\":" << r.halts
    << ",\"locks\":" << r.locks << ",\"strat_halts\":" << r.strat_halts << ",\"unaffordable\":" << r.unaffordable
    << ",\"unprotected\":" << r.unprotected << ",\"oversell\":" << r.oversell << ",\"osm_errors\":" << r.osm_errors;
  return o.str();
}
void line(const char* name, const BtResult& r) {
  std::printf("%-28s ret %+8.2f%%  maxDD %6.2f%%  sharpe %+6.2f  trades %4d  win %5.1f%%  stops %4d  vetoes %4d\n", name,
              r.ret * 100, r.max_dd * 100, r.sharpe, r.trades, r.trades ? 100.0 * r.wins / r.trades : 0.0, r.stops, r.risk_rejects);
}
std::string wf_json(const char* name, const WfResult& w) {
  std::ostringstream o;
  o << "{\"name\":\"" << name << "\",\"oos_ret\":" << w.oos_ret << ",\"worst_dd\":" << w.worst_dd << ",\"oos_trades\":" << w.oos_trades << ",\"folds\":[";
  for (std::size_t i = 0; i < w.folds.size(); ++i) {
    const WfFold& f = w.folds[i];
    o << (i ? "," : "") << "{\"train_from\":" << f.train_from << ",\"train_to\":" << f.train_to << ",\"test_from\":" << f.test_from
      << ",\"test_to\":" << f.test_to << ",\"chosen\":\"" << f.chosen << "\",\"train_ret\":" << f.train_ret << ",\"test_ret\":" << f.test_ret
      << ",\"test_dd\":" << f.test_dd << ",\"test_trades\":" << f.test_trades << "}";
  }
  o << "]}";
  return o.str();
}
}  // namespace

int main(int argc, char** argv) {
  std::string data = "data", out = "reports";
  BtConfig cfg;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--data")) data = argv[i + 1];
    else if (!std::strcmp(argv[i], "--out")) out = argv[i + 1];
    else if (!std::strcmp(argv[i], "--capital")) cfg.capital = std::atof(argv[i + 1]);
  }
  std::filesystem::create_directories(out);

  std::vector<std::string> files;
  if (std::filesystem::exists(data))
    for (const auto& e : std::filesystem::directory_iterator(data))
      if (e.path().extension() == ".csv" && e.path().filename().string()[0] != '_') files.push_back(e.path().string());
  std::sort(files.begin(), files.end());
  std::vector<Instrument> insts;
  std::ostringstream dq;
  bool any_synth = false;
  std::printf("== SIMULATION == data quality\n");
  for (const auto& f : files) {
    const LoadResult r = load_bars(f);
    const std::string sym = std::filesystem::path(f).stem().string();
    if (!r.ok) { std::printf("%-18s UNREADABLE, skipped\n", sym.c_str()); continue; }
    any_synth = any_synth || r.synthetic;
    std::printf("%-18s bars %5zu  %d..%d  dup %d  out-of-order %d  gaps %d  bad %d%s\n", sym.c_str(), r.bars.size(), r.bars.front().date,
                r.bars.back().date, r.duplicates, r.out_of_order, r.gaps, r.bad_rows, r.synthetic ? "  [SYNTHETIC]" : "");
    dq << (insts.empty() ? "" : ",") << "{\"sym\":\"" << sym << "\",\"bars\":" << r.bars.size() << ",\"from\":" << r.bars.front().date
       << ",\"to\":" << r.bars.back().date << ",\"duplicates\":" << r.duplicates << ",\"out_of_order\":" << r.out_of_order
       << ",\"gaps\":" << r.gaps << ",\"bad_rows\":" << r.bad_rows << ",\"synthetic\":" << (r.synthetic ? "true" : "false") << "}";
    insts.push_back({sym, r.bars, 1});
  }
  if (insts.empty()) { std::fprintf(stderr, "no usable CSV files in %s\n", data.c_str()); return 2; }

  std::printf("\n== SIMULATION == full period, both strategies, capital %.0f\n", cfg.capital);
  BtConfig full = cfg;
  full.audit_path = out + "/audit.jsonl";
  auto st = default_strats();
  const BtResult R = run_backtest(insts, st, full);
  line("ALL INSTRUMENTS", R);

  std::printf("\n== SIMULATION == per instrument (regime check)\n");
  std::ostringstream per;
  for (std::size_t i = 0; i < insts.size(); ++i) {
    std::vector<Instrument> one{insts[i]};
    auto s1 = default_strats();
    const BtResult r = run_backtest(one, s1, cfg);
    line(insts[i].sym.c_str(), r);
    per << (i ? "," : "") << "{\"sym\":\"" << insts[i].sym << "\"," << metrics(r) << "}";
  }

  std::printf("\n== SIMULATION == fee and slippage sensitivity\n");
  std::ostringstream sens;
  bool first = true;
  for (int fm : {1, 2, 4})
    for (int sm : {1, 2, 4}) {
      BtConfig c = cfg;
      c.ex.fee_bps *= fm; c.ex.slippage_ticks *= sm; c.ex.half_spread_ticks *= sm;
      auto s2 = default_strats();
      const BtResult r = run_backtest(insts, s2, c);
      char nm[64];
      std::snprintf(nm, sizeof nm, "fee x%d, slippage x%d", fm, sm);
      line(nm, r);
      sens << (first ? "" : ",") << "{\"fee_mult\":" << fm << ",\"slip_mult\":" << sm << "," << metrics(r) << "}";
      first = false;
    }

  // ---- validation of every candidate strategy family ----
  // Pass criteria were fixed before any of these were run (approved 14 Nov 2026):
  //   1. out-of-sample (walk-forward) return > 0 with fees, spread and slippage all doubled
  //   2. worst drawdown inside any out-of-sample period < 15%
  //   3. at least 55% of out-of-sample periods positive (6 of 11 when there are 11)
  std::printf("\n== SIMULATION == walk-forward validation (train 500 bars, test 125 bars), each family alone\n");
  auto one = [](auto make) { return [make] { std::vector<StratSpec> v; v.push_back({make(), 1.0}); return v; }; };
  struct Family { std::string name; std::vector<ParamSet> ps; };
  std::vector<Family> fams(6);
  fams[0].name = "momentum"; fams[1].name = "momentum+regime";
  for (int f : {5, 10, 20})
    for (int sl : {50, 100}) {
      const std::string lb = "fast=" + std::to_string(f) + ",slow=" + std::to_string(sl);
      fams[0].ps.push_back({lb, one([f, sl] { return std::make_unique<Momentum>(f, sl, 60, false); })});
      fams[1].ps.push_back({lb, one([f, sl] { return std::make_unique<Momentum>(f, sl, 60, true); })});
    }
  fams[2].name = "rotation"; fams[3].name = "rotation+regime";
  for (int lbk : {63, 126, 189})
    for (int rb : {5, 10}) {
      const std::string lb = "lookback=" + std::to_string(lbk) + ",rebal=" + std::to_string(rb) + ",top=3";
      fams[2].ps.push_back({lb, one([lbk, rb] { return std::make_unique<Rotation>(lbk, 3, rb, false); })});
      fams[3].ps.push_back({lb, one([lbk, rb] { return std::make_unique<Rotation>(lbk, 3, rb, true); })});
    }
  fams[4].name = "meanrev"; fams[5].name = "meanrev-uptrend";
  for (int w : {10, 20})
    for (double z : {1.5, 2.0, 2.5}) {
      const std::string lb = "window=" + std::to_string(w) + ",z=" + std::to_string(z).substr(0, 3);
      fams[4].ps.push_back({lb, one([w, z] { return std::make_unique<MeanReversion>(w, z, 10, false); })});
      fams[5].ps.push_back({lb, one([w, z] { return std::make_unique<MeanReversion>(w, z, 10, true); })});
    }
  bool has_proxy = false;
  for (const auto& in : insts) has_proxy = has_proxy || in.sym == "NIFTYBEES";
  if (!has_proxy) std::printf("NOTE: NIFTYBEES is not in the data, so the +regime filters are INACTIVE in this run.\n");
  BtConfig cfg2 = cfg;
  cfg2.ex.fee_bps *= 2; cfg2.ex.slippage_ticks *= 2; cfg2.ex.half_spread_ticks *= 2;
  std::ostringstream cand;
  int n_pass = 0;
  std::printf("%-18s %10s %10s %10s %9s %7s  %s\n", "family", "OOS ret", "OOS 2xcost", "worst DD", "pos folds", "trades", "verdict");
  for (std::size_t k = 0; k < fams.size(); ++k) {
    const WfResult w1 = walk_forward(insts, fams[k].ps, cfg, 500, 125, 260);
    const WfResult w2 = walk_forward(insts, fams[k].ps, cfg2, 500, 125, 260);
    int pos = 0;
    for (const auto& f : w2.folds) pos += f.test_ret > 0;
    const std::size_t nf = w2.folds.size();
    const bool c1 = w2.oos_ret > 0, c2 = w2.worst_dd < 0.15, c3 = nf > 0 && pos * 100 >= static_cast<int>(nf) * 55;
    const bool regime_family = fams[k].name.find("regime") != std::string::npos;
    const bool pass = c1 && c2 && c3 && (!regime_family || has_proxy);
    n_pass += pass;
    std::printf("%-18s %+9.2f%% %+9.2f%% %9.2f%% %5d/%-3zu %7d  %s%s%s%s\n", fams[k].name.c_str(), w1.oos_ret * 100, w2.oos_ret * 100, w2.worst_dd * 100,
                pos, nf, w2.oos_trades, pass ? "PASS" : "FAIL", c1 ? "" : " [loses at 2x cost]", c2 ? "" : " [drawdown >= 15%]", c3 ? "" : " [too few positive periods]");
    cand << (k ? "," : "") << "{\"family\":\"" << fams[k].name << "\",\"oos_ret\":" << w1.oos_ret << ",\"oos_ret_2x\":" << w2.oos_ret
         << ",\"worst_dd_2x\":" << w2.worst_dd << ",\"pos_folds\":" << pos << ",\"folds\":" << nf << ",\"trades_2x\":" << w2.oos_trades
         << ",\"pass\":" << (pass ? "true" : "false") << ",\"c_cost\":" << (c1 ? "true" : "false") << ",\"c_dd\":" << (c2 ? "true" : "false")
         << ",\"c_folds\":" << (c3 ? "true" : "false") << ",\"filter_active\":" << ((!regime_family || has_proxy) ? "true" : "false")
         << ",\"wf\":" << wf_json(fams[k].name.c_str(), w2) << "}";
  }
  std::printf("%d of %zu families pass.%s\n", n_pass, fams.size(),
              any_synth ? " Input is SYNTHETIC: this only tests the engine." : " Universe is today's index members (survivorship bias: companies that left the index are missing).");
  const WfResult wm = walk_forward(insts, fams[0].ps, cfg, 500, 125, 260);
  const WfResult wr = walk_forward(insts, fams[4].ps, cfg, 500, 125, 260);

  std::ofstream o(out + "/report.json");
  o.precision(8);
  char ts[32];
  const std::time_t t = std::time(nullptr);
  std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
  o << "{\n\"label\":\"SIMULATION\",\n\"generated\":\"" << ts << "\",\n\"capital\":" << cfg.capital
    << ",\n\"data_is_synthetic\":" << (any_synth ? "true" : "false") << ",\n\"notes\":["
    << "\"All results are simulated on daily bars with a synthetic order book.\","
    << "\"Fee model is a flat placeholder; the real fee and tax schedule is not yet verified.\","
    << (any_synth ? "\"Input data is SYNTHETIC. These results say nothing about real markets.\"," : "")
    << "\"Risk checks run at the daily close because the data is daily.\"],\n\"data\":[" << dq.str() << "],\n\"full\":{" << metrics(R)
    << ",\"strat_pnl\":[";
  for (std::size_t i = 0; i < R.strat_pnl.size(); ++i) o << (i ? "," : "") << R.strat_pnl[i];
  o << "],\"rejects\":{";
  for (std::size_t i = 1; i < R.rejects.size(); ++i) o << (i > 1 ? "," : "") << "\"" << to_string(static_cast<Reject>(i)) << "\":" << R.rejects[i];
  o << "},\"equity\":[";
  const std::size_t step = std::max<std::size_t>(1, R.equity.size() / 400);
  for (std::size_t i = 0; i < R.equity.size(); i += step) o << (i ? "," : "") << "[" << R.dates[i] << "," << R.equity[i] << "]";
  o << "]},\n\"per_instrument\":[" << per.str() << "],\n\"sensitivity\":[" << sens.str() << "],\n\"walk_forward\":["
    << wf_json("momentum", wm) << "," << wf_json("mean_reversion", wr) << "],\n\"criteria\":\"OOS return > 0 at 2x costs; worst OOS-period drawdown < 15%; >= 55% of OOS periods positive\",\n\"candidates\":[" << cand.str() << "]\n}\n";
  std::printf("\nreport: %s/report.json   audit trail: %s/audit.jsonl\n", out.c_str(), out.c_str());
  if (R.oversell || R.osm_errors) { std::fprintf(stderr, "INTEGRITY FAILURE: oversell=%d osm_errors=%d\n", R.oversell, R.osm_errors); return 1; }
  return 0;
}
