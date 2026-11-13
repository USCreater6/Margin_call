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
      if (e.path().extension() == ".csv") files.push_back(e.path().string());
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

  std::printf("\n== SIMULATION == walk-forward (train 500 bars, test 125 bars)\n");
  std::vector<ParamSet> mom, mr;
  for (int f : {5, 10, 20})
    for (int s : {50, 100}) {
      const std::string lb = "fast=" + std::to_string(f) + ",slow=" + std::to_string(s);
      mom.push_back({lb, [f, s] { std::vector<StratSpec> v; v.push_back({std::make_unique<Momentum>(f, s, 60), 1.0}); return v; }});
    }
  for (int w : {10, 20})
    for (double z : {1.5, 2.0, 2.5}) {
      const std::string lb = "window=" + std::to_string(w) + ",z=" + std::to_string(z).substr(0, 3);
      mr.push_back({lb, [w, z] { std::vector<StratSpec> v; v.push_back({std::make_unique<MeanReversion>(w, z, 10), 1.0}); return v; }});
    }
  const WfResult wm = walk_forward(insts, mom, cfg, 500, 125, 120);
  const WfResult wr = walk_forward(insts, mr, cfg, 500, 125, 120);
  std::printf("%-28s OOS ret %+8.2f%%  worst fold DD %6.2f%%  trades %4d  folds %zu\n", "momentum", wm.oos_ret * 100, wm.worst_dd * 100, wm.oos_trades, wm.folds.size());
  std::printf("%-28s OOS ret %+8.2f%%  worst fold DD %6.2f%%  trades %4d  folds %zu\n", "mean reversion", wr.oos_ret * 100, wr.worst_dd * 100, wr.oos_trades, wr.folds.size());

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
    << wf_json("momentum", wm) << "," << wf_json("mean_reversion", wr) << "]\n}\n";
  std::printf("\nreport: %s/report.json   audit trail: %s/audit.jsonl\n", out.c_str(), out.c_str());
  if (R.oversell || R.osm_errors) { std::fprintf(stderr, "INTEGRITY FAILURE: oversell=%d osm_errors=%d\n", R.oversell, R.osm_errors); return 1; }
  return 0;
}
