// Terminal server: serves web/ and a small JSON API backed by the C++ simulation session.
// SIMULATION ONLY. Usage: mc_server [--web web] [--port 8080] [--data data/nse] [--bind 127.0.0.1]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iterator>
#include <map>
#include <string>
#include "mc/agent.hpp"
#include "mc/replay.hpp"
#include "mc/session.hpp"

using namespace mc;

namespace {
Ts mono_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::int64_t wall_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::int64_t wall_ms() { return wall_ns() / 1000000LL; }
Price paise(const std::string& s) { return static_cast<Price>(std::llround(std::atof(s.c_str()) * 100.0)); }
// Reads the value of a top-level key from a flat JSON object. Enough for this API's small bodies.
std::string jget(const std::string& body, const std::string& key) {
  const std::string k = "\"" + key + "\"";
  std::size_t p = body.find(k);
  if (p == std::string::npos) return "";
  p = body.find(':', p + k.size());
  if (p == std::string::npos) return "";
  ++p;
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
  if (p < body.size() && body[p] == '"') {
    const std::size_t e = body.find('"', p + 1);
    return e == std::string::npos ? "" : body.substr(p + 1, e - p - 1);
  }
  std::size_t e = p;
  while (e < body.size() && body[e] != ',' && body[e] != '}' && body[e] != ' ' && body[e] != '\n') ++e;
  return body.substr(p, e - p);
}
const char* mime(const std::string& path) {
  const auto ext = std::filesystem::path(path).extension().string();
  if (ext == ".html") return "text/html; charset=utf-8";
  if (ext == ".json") return "application/json";
  if (ext == ".js") return "text/javascript";
  if (ext == ".css") return "text/css";
  if (ext == ".svg") return "image/svg+xml";
  return "application/octet-stream";
}
void send_all(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return;
    off += static_cast<std::size_t>(n);
  }
}
void respond(int fd, int code, const char* type, const std::string& body) {
  std::ostringstream h;
  h << "HTTP/1.1 " << code << (code == 200 ? " OK" : code == 404 ? " Not Found" : " Bad Request") << "\r\nContent-Type: " << type
    << "\r\nContent-Length: " << body.size() << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  send_all(fd, h.str() + body);
}
std::string ok(const std::string& why = "", const std::string& code = "") {
  std::string w;
  for (char c : why) { if (c == '"' || c == '\\') w += '\\'; w += c; }
  return std::string("{\"ok\":") + (why.empty() ? "true" : "false") + ",\"why\":\"" + w + "\",\"code\":\"" + code + "\"}";
}
// Decodes %XX escapes and '+' in a URL path. Returns false on a malformed escape or an encoded NUL.
bool url_decode(const std::string& in, std::string& out) {
  out.clear();
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%') {
      if (i + 2 >= in.size() || !std::isxdigit(static_cast<unsigned char>(in[i + 1])) || !std::isxdigit(static_cast<unsigned char>(in[i + 2]))) return false;
      const char c = static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16));
      if (c == '\0') return false;
      out += c; i += 2;
    } else out += in[i];
  }
  return true;
}
std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
// Instruments file written by tools/fetch_latest.py. Lines starting with # are comments.
// symbol,venue,price,tick,lot,source[,date,open,high,low,close,prev_close,volume_units,volume_check[,kind]]
// kind = stock | gold-etf | index-future (decides the statutory charges); without it: F&O venue = index-future, else stock.
// Prices in rupees. With the day columns, the session replays that NSE trading day.
int load_instruments(LiveSim& sim, const std::string& path) {
  std::ifstream f(path);
  std::string line;
  int n = 0;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#' || line.rfind("symbol,", 0) == 0) continue;
    std::stringstream ss(line);
    std::vector<std::string> c;
    for (std::string x; std::getline(ss, x, ',');) c.push_back(x);
    if (c.size() < 6) { std::fprintf(stderr, "instruments: skipped short row: %s\n", line.c_str()); continue; }
    const double px = std::atof(c[2].c_str()), tick = std::atof(c[3].c_str());
    const long long lot = std::atoll(c[4].c_str());
    if (c[0].empty() || !(px > 0) || !(tick > 0)) { std::fprintf(stderr, "instruments: skipped bad row: %s\n", line.c_str()); continue; }
    const Price t = static_cast<Price>(std::llround(tick * 100.0));
    const Price p = std::max<Price>(t, static_cast<Price>(std::llround(px * 100.0 / static_cast<double>(t))) * t);
    sim.add_instrument(c[0], c[1], p, t, c[5], lot > 0 ? lot : 1);
    const std::string kind = c.size() >= 15 ? c[14] : "";
    sim.set_segment(sim.instruments() - 1, kind == "gold-etf" ? Seg::GoldEtf : kind == "index-future" || (kind.empty() && c[1].find("F&O") != std::string::npos) ? Seg::IndexFuture : Seg::Stock);
    if (c.size() >= 13) {
      LiveSim::Day d;
      d.date = c[6]; d.o = paise(c[7]); d.h = paise(c[8]); d.l = paise(c[9]); d.c = paise(c[10]); d.pc = paise(c[11]);
      d.vol_units = std::atof(c[12].c_str()); d.vol_note = c.size() >= 14 ? c[13] : "";
      if (!sim.set_day(sim.instruments() - 1, d)) std::fprintf(stderr, "instruments: day data rejected for %s\n", c[0].c_str());
    }
    ++n;
  }
  return n;
}
// Every NSE EQ share of the last official day (tools/fetch_latest.py: data/latest/universe.csv).
struct Uni { double o = 0, h = 0, l = 0, c = 0, pc = 0, vol = 0, tick = 0; std::string tick_check, kind; };
std::map<std::string, Uni> load_universe(const std::string& path, std::string& date) {
  std::map<std::string, Uni> u;
  std::ifstream f(path);
  for (std::string line; std::getline(f, line);) {
    if (line.rfind("# NSE CM daily file ", 0) == 0) { date = line.substr(20, 10); continue; }
    if (line.empty() || line[0] == '#' || line.rfind("symbol,", 0) == 0) continue;
    std::stringstream ss(line); std::vector<std::string> c;
    for (std::string x; std::getline(ss, x, ',');) c.push_back(x);
    if (c.size() < 10) continue;
    Uni x; x.o = std::atof(c[1].c_str()); x.h = std::atof(c[2].c_str()); x.l = std::atof(c[3].c_str()); x.c = std::atof(c[4].c_str());
    x.pc = std::atof(c[5].c_str()); x.vol = std::atof(c[6].c_str()); x.tick = std::atof(c[7].c_str()); x.tick_check = c[8]; x.kind = c[9];
    if (x.c > 0 && x.tick > 0 && x.l > 0 && x.h >= x.l) u[c[0]] = x;
  }
  return u;
}
// Adds one NSE share from the universe to the session (replays its official day). False if unknown.
bool add_share(LiveSim& sim, const std::string& sym, const std::map<std::string, Uni>& uni, const std::string& date) {
  if (sim.find_symbol(sym) >= 0) return true;
  const auto it = uni.find(sym);
  if (it == uni.end()) return false;
  const Uni& x = it->second;
  const Price t = static_cast<Price>(std::llround(x.tick * 100.0));
  const Price p = std::max<Price>(t, static_cast<Price>(std::llround(x.c * 100.0 / static_cast<double>(t))) * t);
  sim.add_instrument(sym, "NSE", p, t, "NSE close " + date + (x.tick_check == "master" ? "" : " (tick not verified)"), 1);
  const std::size_t i = sim.instruments() - 1;
  sim.set_segment(i, x.kind == "gold-etf" ? Seg::GoldEtf : Seg::Stock);
  LiveSim::Day d; d.date = date; d.o = paise(std::to_string(x.o)); d.h = paise(std::to_string(x.h)); d.l = paise(std::to_string(x.l));
  d.c = paise(std::to_string(x.c)); d.pc = paise(std::to_string(x.pc)); d.vol_units = x.vol; d.vol_note = "units (NSE daily file)";
  sim.set_day(i, d);
  return true;
}
// NIFTY options of the nearest expiry (tools/fetch_options.py: data/latest/options.csv). Each option's implied
// volatility is solved from its official close at the official index close; those it cannot be solved for are skipped.
int load_options(LiveSim& sim, const std::string& path) {
  std::ifstream f(path);
  const int fut = sim.find_symbol("NIFTY-FUT");
  if (!f || fut < 0) return 0;
  int n = 0; double ratio = 0;
  std::string fday;   // the file day, from the first comment line: "# NSE F&O daily file YYYY-MM-DD: ..."
  for (std::string line; std::getline(f, line);) {
    if (fday.empty() && line.rfind("# NSE F&O daily file ", 0) == 0) { fday = line.substr(21, 10); continue; }
    if (line.empty() || line[0] == '#' || line.rfind("symbol,", 0) == 0) continue;
    std::stringstream ss(line); std::vector<std::string> c;
    for (std::string x; std::getline(ss, x, ',');) c.push_back(x);
    if (c.size() < 18) continue;
    const double k = std::atof(c[2].c_str()), cl = std::atof(c[7].c_str()), und = std::atof(c[14].c_str()), tick = std::atof(c[16].c_str()), fc = std::atof(c[17].c_str());
    const long long lot = std::atoll(c[12].c_str());
    if (!(k > 0 && cl > 0 && und > 0 && tick > 0 && fc > 0 && lot > 0)) continue;
    ratio = und / fc;
    // T: from the file day's close (15:30) to the expiry (15:30)
    const double T = static_cast<double>(LiveSim::civil_day(c[1]) - LiveSim::civil_day(fday)) / 365.0;
    const bool call = c[3] == "CE";
    const double iv = LiveSim::implied_vol(cl, und, k, T, call);
    if (!(iv > 0) || !(T > 0)) { std::fprintf(stderr, "options: %s skipped (implied volatility not solvable)\n", c[0].c_str()); continue; }
    const Price t = static_cast<Price>(std::llround(tick * 100.0));
    sim.add_instrument(c[0], "NSE F&O", std::max<Price>(t, static_cast<Price>(std::llround(cl * 100.0 / static_cast<double>(t))) * t), t,
                       "NSE close " + fday + " " + c[13], lot);
    const std::size_t i = sim.instruments() - 1;
    LiveSim::Day d; d.date = fday; d.o = paise(c[4]); d.h = paise(c[5]); d.l = paise(c[6]); d.c = paise(c[7]); d.pc = paise(c[8]);
    d.vol_units = std::atof(c[9].c_str()) * static_cast<double>(lot); d.vol_note = "contracts x lot " + c[12];
    sim.set_day(i, d);
    sim.set_option(i, k, call, c[1], iv);
    ++n;
  }
  if (n) sim.set_index_link(static_cast<std::size_t>(fut), ratio);
  return n;
}
// Live prices written by tools/live_feed.py (data/live/quotes.tsv):
//   LIVE <fetched ms> <1|0> <message>  /  X <index name> <value> <prev close>  /  S <symbol> <last> <prev close>
struct Live { long long fetched = 0; bool ok = false; std::string msg; double nifty = 0; std::map<std::string, std::pair<double, double>> q; };
Live read_live(const std::string& path) {
  Live L; std::ifstream f(path);
  for (std::string line; std::getline(f, line);) {
    std::stringstream ss(line); std::vector<std::string> c;
    for (std::string x; std::getline(ss, x, '\t');) c.push_back(x);
    if (c.empty()) continue;
    if (c[0] == "LIVE" && c.size() >= 3) { L.fetched = std::atoll(c[1].c_str()); L.ok = c[2] == "1"; L.msg = c.size() >= 4 ? c[3] : ""; }
    else if (c[0] == "X" && c.size() >= 3 && c[1] == "NIFTY 50") L.nifty = std::atof(c[2].c_str());
    else if (c[0] == "S" && c.size() >= 3) L.q[c[1]] = {std::atof(c[2].c_str()), c.size() >= 4 ? std::atof(c[3].c_str()) : 0.0};
  }
  return L;
}
// Background job status for the terminal (connector build, price history, live feed): whether its process runs
// (found in /proc by its command line) and the last lines of its log in logs/.
std::string job_json(const std::string& name) {
  static const std::map<std::string, std::pair<std::string, std::string>> jobs = {
      {"connector", {"tools/build_connector.py", "logs/connector.log"}}, {"history", {"tools/fetch_latest.py --days", "logs/history.log"}},
      {"live", {"tools/live_feed.py", "logs/live_feed.log"}}, {"options", {"tools/fetch_options.py", "logs/fetch_options.log"}}};
  const auto it = jobs.find(name);
  if (it == jobs.end()) return "{\"ok\":false}";
  bool running = false;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator("/proc", ec)) {
    const std::string pid = e.path().filename().string();
    if (pid.empty() || !std::isdigit(static_cast<unsigned char>(pid[0]))) continue;
    std::ifstream f(e.path() / "cmdline", std::ios::binary); std::string cmd((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (char& c : cmd) if (c == '\0') c = ' ';
    if (cmd.find(it->second.first) != std::string::npos) { running = true; break; }
  }
  std::vector<std::string> tail;
  { std::ifstream f(it->second.second); for (std::string l; std::getline(f, l);) { tail.push_back(l); if (tail.size() > 8) tail.erase(tail.begin()); } }
  std::string log;
  for (const auto& l : tail) log += l + "\n";
  std::string esc_log;
  for (char c : log) { if (c == '"' || c == '\\') esc_log += '\\'; if (c == '\n') esc_log += "\\n"; else if (static_cast<unsigned char>(c) >= 32) esc_log += c; }
  return std::string("{\"ok\":true,\"running\":") + (running ? "true" : "false") + ",\"log\":\"" + esc_log + "\"}";
}
std::vector<OrderId> parse_ids(const std::string& list) {
  std::vector<OrderId> out;
  std::stringstream ss(list);
  for (std::string x; std::getline(ss, x, ',');) {
    const std::size_t d = x.find_first_of("0123456789");
    if (d != std::string::npos) out.push_back(static_cast<OrderId>(std::atoll(x.c_str() + d)));
  }
  return out;
}
// Atomic save: write a temporary file, then rename over the old one (a crash never leaves half a file).
bool save_text(const std::string& path, const std::string& text) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  const std::string tmp = path + ".tmp";
  { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return false; f << text; if (!f.flush()) return false; }
  std::filesystem::rename(tmp, path, ec);
  return !ec;
}
}  // namespace

int main(int argc, char** argv) {
  std::string web = "web", data, bind_addr = "127.0.0.1", instruments = "data/latest/instruments.csv", state_file = "data/session/state.tsv";
  std::string agent_log = "data/agent/decisions.jsonl";   // training record of the G.R.I.T agent; data/ is never committed
  int port = 8080;
  bool live_any_time = false;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--web")) web = argv[i + 1];
    else if (!std::strcmp(argv[i], "--port")) port = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--data")) data = argv[i + 1];
    else if (!std::strcmp(argv[i], "--bind")) bind_addr = argv[i + 1];
    else if (!std::strcmp(argv[i], "--instruments")) instruments = argv[i + 1];
    else if (!std::strcmp(argv[i], "--state")) state_file = argv[i + 1];
    else if (!std::strcmp(argv[i], "--agent-log")) agent_log = argv[i + 1];
    else if (!std::strcmp(argv[i], "--live-any-time")) live_any_time = std::atoi(argv[i + 1]) != 0;   // tests only: live outside market hours
  }
  std::signal(SIGPIPE, SIG_IGN);

  RiskLimits lim;
  lim.max_order_notional = 0;   // removed by the owner on 14 Nov 2026 (cash, gross 100% and the other limits stay)
  LiveSim sim(100000.0, lim, static_cast<std::uint64_t>(wall_ms()));
  sim.set_wall_ns(wall_ns());
  int loaded = load_instruments(sim, instruments);
  if (!loaded && !data.empty() && std::filesystem::exists(data)) {
    std::vector<std::string> files;
    for (const auto& e : std::filesystem::directory_iterator(data))
      if (e.path().extension() == ".csv" && e.path().filename().string()[0] != '_') files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      const LoadResult r = load_bars(f);
      if (!r.ok) continue;
      const Price c = std::max<Price>(5, (r.bars.back().c / 5) * 5);
      sim.add_instrument(std::filesystem::path(f).stem().string(), r.synthetic ? "SYNTH" : "NSE", c, 5,
                         r.synthetic ? "synthetic data" : "last close in " + data);
      ++loaded;
    }
  }
  const std::size_t base_n = sim.instruments();   // the original watchlist: the agent and the stub trade only these
  std::string uni_date;
  const std::string latest_dir = std::filesystem::path(instruments).parent_path().string();
  const auto uni = load_universe(latest_dir + "/universe.csv", uni_date);
  std::vector<std::string> nifty50, watch;
  { std::ifstream f(latest_dir + "/nifty50.txt"); for (std::string x; std::getline(f, x);) if (!x.empty()) nifty50.push_back(x); }
  const std::string watch_file = std::filesystem::path(state_file).parent_path().string() + "/watch.txt";
  { std::ifstream f(watch_file); for (std::string x; std::getline(f, x);) if (!x.empty()) watch.push_back(x); }
  int n50 = 0, nw = 0;
  if (loaded) {
    for (const auto& s : nifty50) n50 += add_share(sim, s, uni, uni_date);
    for (const auto& s : watch) nw += add_share(sim, s, uni, uni_date);
  }
  const int nopt = loaded ? load_options(sim, latest_dir + "/options.csv") : 0;
  sim.set_stub_universe(base_n);
  std::printf("instruments: %zu base, %d NIFTY 50, %d watched, %d NIFTY options; universe %zu shares (%s)\n", base_n, n50, nw, nopt, uni.size(), uni_date.c_str());
  auto save_watch = [&]() { std::string t; for (const auto& s : watch) t += s + "\n"; save_text(watch_file, t); };
  if (!loaded) {   // no data at all: placeholder instruments with placeholder prices, clearly labelled
    sim.add_instrument("NIFTY-FUT", "NSE F&O", 2500000, 50);
    sim.add_instrument("RELIANCE", "NSE", 140000, 10);
    sim.add_instrument("HDFCBANK", "NSE", 100000, 10);
    sim.add_instrument("TCS", "NSE", 300000, 10);
  }

  // the demo account survives restarts: wallet, positions, orders, ledger
  std::string last_saved;
  if (state_file != "-") {
    std::ifstream sf(state_file, std::ios::binary);
    if (sf) {
      std::ostringstream ss; ss << sf.rdbuf();
      const std::string why = sim.restore(ss.str(), mono_ns());
      if (why.empty()) { last_saved = ss.str(); std::printf("account restored from %s\n", state_file.c_str()); }
      else {
        std::fprintf(stderr, "account NOT restored (%s); the file is kept as %s.bad\n", why.c_str(), state_file.c_str());
        std::error_code ec; std::filesystem::copy_file(state_file, state_file + ".bad", std::filesystem::copy_options::overwrite_existing, ec);
      }
    }
  }
  // G.R.I.T agent: OFF at every start; the owner turns it on. It judges each instrument from a year of
  // daily closes (web/data/history/SYMBOL.csv, written by tools/fetch_latest.py) when that file exists.
  Agent agent;
  agent.resize(sim.instruments());
  for (std::size_t i = 0; i < base_n; ++i) {
    const std::string hf = web + "/data/history/" + sim.sym(i) + ".csv";
    if (std::filesystem::exists(hf)) { const LoadResult r = load_bars(hf); if (r.ok) agent.set_history(i, r.bars); }
  }
  std::printf("G.R.I.T agent: OFF; daily history for %d of %zu instruments (it trades the original watchlist only)\n", agent.with_history(), base_n);
  // On a fresh Codespace the year of history is still downloading when the engine starts: look again every
  // minute until every instrument that can have a history file has one.
  auto reload_history = [&]() {
    for (std::size_t i = 0; i < base_n; ++i) {
      const std::string hf = web + "/data/history/" + sim.sym(i) + ".csv";
      if (std::filesystem::exists(hf)) { const LoadResult r = load_bars(hf); if (r.ok) agent.set_history(i, r.bars); }
    }
  };
  auto write_agent_log = [&]() {
    auto lines = agent.drain_log();
    if (lines.empty() || agent_log == "-") return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(agent_log).parent_path(), ec);
    std::ofstream f(agent_log, std::ios::app);
    for (const auto& l : lines) f << l << '\n';
  };
  auto persist = [&]() {
    if (state_file == "-") return;
    std::string snap = sim.snapshot();
    if (snap != last_saved && save_text(state_file, snap)) last_saved = std::move(snap);
  };

  const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET; a.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, bind_addr.c_str(), &a.sin_addr) != 1) { std::fprintf(stderr, "bad bind address\n"); return 2; }
  if (::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(ls, 16) != 0) { std::perror("bind/listen"); return 2; }
  std::printf("Margin_call terminal (SIMULATION) on http://%s:%d  instruments: %zu (%s)\n", bind_addr.c_str(), port, sim.instruments(),
              loaded ? "from data files" : "PLACEHOLDER prices, held flat");
  std::fflush(stdout);

  // live data: market hours (09:15-15:30 IST, Monday-Friday) and a fresh feed file (fetched in the last 150 s)
  const std::string live_file = "data/live/quotes.tsv";
  std::filesystem::file_time_type live_mtime{};
  Live live;
  auto update_live = [&]() {
    std::error_code ec;
    const auto mt = std::filesystem::last_write_time(live_file, ec);
    if (!ec && mt != live_mtime) { live_mtime = mt; live = read_live(live_file); }
    const std::int64_t ms = wall_ms(), ist = ms + 19800000LL, day = ist / 86400000LL;
    const double sec = static_cast<double>(ist % 86400000LL) / 1000.0;
    const int wd = static_cast<int>((day + 4) % 7);   // 0 = Sunday (1970-01-01 was a Thursday)
    const bool open = live_any_time || (wd >= 1 && wd <= 5 && sec >= 33300.0 && sec < 55800.0);
    const bool fresh = live.ok && ms - live.fetched < 150000 && (live.nifty > 0 || !live.q.empty());
    if (open && fresh) {
      const std::int64_t z = day + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
      const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      const std::int64_t mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2);
      char date[48]; std::snprintf(date, sizeof date, "%04lld-%02lld-%02lld", static_cast<long long>(y), static_cast<long long>(m), static_cast<long long>(d));
      for (const auto& [sym, v] : live.q) { const int i = sim.find_symbol(sym); if (i >= 0) sim.set_live_quote(static_cast<std::size_t>(i), static_cast<Price>(std::llround(v.first * 100.0)), static_cast<Price>(std::llround(v.second * 100.0))); }
      sim.set_live_index(live.nifty);
      sim.set_live(true, date, sec - 33300.0);
    } else sim.set_live(false, "", 0);
  };
  Ts next_live = 0;

  constexpr Ts kTick = 250000000LL;                 // 4 market updates per second
  Ts next_tick = mono_ns() + kTick, next_save = mono_ns() + 2000000000LL, next_hist = mono_ns() + 60000000000LL;
  for (;;) {
    pollfd p{ls, POLLIN, 0};
    ::poll(&p, 1, 50);
    Ts now = mono_ns();
    sim.set_wall_ns(wall_ns());
    if (now >= next_live) { update_live(); next_live = now + 1000000000LL; }
    if (now - next_tick > 5000000000LL) next_tick = now;   // after a pause (machine asleep): no catch-up burst
    while (now >= next_tick) { sim.step(next_tick); agent.tick(sim, next_tick); next_tick += kTick; }
    write_agent_log();
    if (now >= next_save) { persist(); next_save = now + 2000000000LL; }
    if (now >= next_hist) { if (static_cast<std::size_t>(agent.with_history()) < base_n) reload_history(); next_hist = now + 60000000000LL; }
    sim.pump(now);   // queued cancels and exits go out as rate tokens free up (8 per second)
    if (!(p.revents & POLLIN)) continue;
    const int fd = ::accept(ls, nullptr, nullptr);
    if (fd < 0) continue;
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string req;
    char buf[4096];
    std::size_t hdr_end = std::string::npos, need = 0;
    while (req.size() < 65536) {
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) break;
      req.append(buf, static_cast<std::size_t>(n));
      if (hdr_end == std::string::npos) {
        hdr_end = req.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
          const std::string head = lower(req.substr(0, hdr_end));
          const std::size_t cl = head.find("content-length:");
          need = (cl != std::string::npos && cl < hdr_end) ? static_cast<std::size_t>(std::atol(req.c_str() + cl + 15)) : 0;
        }
      }
      if (hdr_end != std::string::npos && req.size() >= hdr_end + 4 + need) break;
    }
    if (hdr_end == std::string::npos) { ::close(fd); continue; }
    std::istringstream rl(req.substr(0, req.find("\r\n")));
    std::string method, target;
    rl >> method >> target;
    const std::string body = req.substr(hdr_end + 4);
    std::string path = target, query;
    if (const auto q = target.find('?'); q != std::string::npos) { path = target.substr(0, q); query = target.substr(q + 1); }
    now = mono_ns();
    sim.set_wall_ns(wall_ns());

    if (path == "/api/state") {
      std::size_t sel = 0;
      if (const auto s = query.find("sel="); s != std::string::npos) sel = static_cast<std::size_t>(std::atol(query.c_str() + s + 4));
      respond(fd, 200, "application/json", sim.state_json(sel, now, query.find("lite=1") != std::string::npos));
    } else if (path == "/api/quote" && method == "GET") {   // what a market buy of qty would get now from the visible book
      std::string sym, q;
      for (const std::string& kv : {std::string("sym="), std::string("qty=")}) {
        const auto s = query.find(kv);
        if (s == std::string::npos) continue;
        const auto e = query.find('&', s);
        std::string v = query.substr(s + kv.size(), e == std::string::npos ? std::string::npos : e - s - kv.size()), dv;
        if (!url_decode(v, dv)) dv.clear();
        (kv == "sym=" ? sym : q) = dv;
      }
      const int i = sim.find_symbol(sym);
      const long long qty = std::atoll(q.c_str());
      if (i < 0 || qty <= 0) respond(fd, 200, "application/json", ok("Unknown instrument or quantity"));
      else {
        const auto [got, avg] = sim.fillable_buy(static_cast<std::size_t>(i), qty);
        std::ostringstream o; o.precision(12);
        o << "{\"ok\":true,\"qty\":" << qty << ",\"fillable\":" << got << ",\"avg\":" << avg << ",\"depth\":" << sim.ask_depth(static_cast<std::size_t>(i)) << "}";
        respond(fd, 200, "application/json", o.str());
      }
    } else if (path == "/api/candles" && method == "GET") {   // ?sym=...&tf=60
      std::string sym, tfs;
      for (const std::string& kv : {std::string("sym="), std::string("tf=")}) {
        const auto s = query.find(kv);
        if (s == std::string::npos) continue;
        const auto e = query.find('&', s);
        std::string v = query.substr(s + kv.size(), e == std::string::npos ? std::string::npos : e - s - kv.size()), dv;
        for (char& ch : v) if (ch == '+') ch = ' ';
        if (!url_decode(v, dv)) dv.clear();
        (kv == "sym=" ? sym : tfs) = dv;
      }
      const int i = sim.find_symbol(sym);
      respond(fd, 200, "application/json", i < 0 ? std::string("{\"bars\":[]}") : sim.candles_json(static_cast<std::size_t>(i), std::max(1, std::atoi(tfs.c_str()))));
    } else if (path == "/api/jobs" && method == "GET") {
      const auto s = query.find("name=");
      respond(fd, 200, "application/json", job_json(s == std::string::npos ? "" : query.substr(s + 5, query.find('&', s) == std::string::npos ? std::string::npos : query.find('&', s) - s - 5)));
    } else if (path == "/api/watchlist" && method == "GET") {
      std::ostringstream o; o << "{\"base\":[";
      for (std::size_t i = 0; i < base_n; ++i) o << (i ? "," : "") << "\"" << sim.sym(i) << "\"";
      o << "],\"watch\":[";
      for (std::size_t k = 0; k < watch.size(); ++k) o << (k ? "," : "") << "\"" << watch[k] << "\"";
      o << "],\"nifty50\":[";
      bool f1 = true; for (const auto& x : nifty50) if (sim.find_symbol(x) >= 0) { o << (f1 ? "" : ",") << "\"" << x << "\""; f1 = false; }
      o << "],\"universe_date\":\"" << uni_date << "\",\"live\":{\"ok\":" << (live.ok ? "true" : "false") << ",\"fetched\":" << live.fetched
        << ",\"n\":" << live.q.size() << ",\"nifty\":" << live.nifty << ",\"msg\":\"";
      for (char ch : live.msg) { if (ch == '"' || ch == '\\') o << '\\'; if (static_cast<unsigned char>(ch) >= 32) o << ch; }
      o << "\"}}";
      respond(fd, 200, "application/json", o.str());
    } else if (path == "/api/agent" && method == "GET") {
      respond(fd, 200, "application/json", agent.json(sim));
    } else if (path.rfind("/api/", 0) == 0 && method == "POST") {
      std::string out = ok();
      if (path == "/api/order") {
        const int i = sim.find_symbol(jget(body, "sym"));
        const std::string side = jget(body, "side"), type = jget(body, "type");
        const double qty = std::atof(jget(body, "qty").c_str()), px = std::atof(jget(body, "px").c_str());
        const double sl = std::atof(jget(body, "sl").c_str()), tp = std::atof(jget(body, "tp").c_str());   // % of entry
        const bool split = jget(body, "split") == "true";
        if (i < 0) out = ok("Unknown instrument");
        else if ((side != "BUY" && side != "SELL") || (type != "MARKET" && type != "LIMIT")) out = ok("Side must be BUY or SELL; type MARKET or LIMIT");
        else if (!(qty >= 1) || qty > 1e9 || qty != std::floor(qty)) out = ok("Quantity must be a whole number of 1 or more");
        else {
          const std::size_t before = sim.orders().size();   // the new order's index (a split order's slices come after it)
          const std::string why = sim.submit(static_cast<std::size_t>(i), side == "BUY" ? Side::Buy : Side::Sell, type == "MARKET" ? OrdType::Market : OrdType::Limit,
                                             static_cast<Qty>(qty), static_cast<Price>(std::llround(px * 100.0)), "MANUAL", now, sl, tp, split);
          // the reply says what actually happened: filled quantity and average price, not just "accepted"
          const auto& r = sim.orders()[before];
          std::ostringstream ex;
          ex.precision(15);
          ex << ",\"id\":" << r.o.id << ",\"qty\":" << r.o.qty << ",\"filled\":" << r.o.filled << ",\"avg\":" << r.avg_fill() << ",\"st\":\"" << to_string(r.o.st)
             << "\",\"split\":" << (r.split ? "true" : "false");
          out = ok(why, why.empty() ? "" : r.code);
          out.insert(out.size() - 1, ex.str());
        }
      } else if (path == "/api/cancel") {
        const std::string id = jget(body, "id");
        const std::size_t d = id.find_first_of("0123456789");
        out = (d != std::string::npos && sim.cancel(static_cast<OrderId>(std::atoll(id.c_str() + d)), now)) ? ok() : ok("Order is not open");
      } else if (path == "/api/exits") {   // owner sets / clears the stop loss and take profit of one position
        const std::string id = jget(body, "id");
        const std::size_t d = id.find_first_of("0123456789");
        out = d == std::string::npos ? ok("Unknown position")
                                     : ok(sim.set_exits(static_cast<OrderId>(std::atoll(id.c_str() + d)), std::atof(jget(body, "sl").c_str()), std::atof(jget(body, "tp").c_str())));
      } else if (path == "/api/cancel_all") sim.cancel_all(now);
      else if (path == "/api/flatten") sim.flatten(now);
      else if (path == "/api/kill") { sim.kill(now); agent.on_kill(sim, now); }
      else if (path == "/api/agent") {   // the owner sets the agent's mode; the agent has no access to this
        const std::string m = jget(body, "mode");
        if (m != "off" && m != "shadow" && m != "sim") out = ok("Mode must be off, shadow or sim");
        else out = ok(agent.set_mode(sim, m == "sim" ? AgentMode::Sim : m == "shadow" ? AgentMode::Shadow : AgentMode::Off, now));
      }
      else if (path == "/api/close") {
        const auto ids = parse_ids(jget(body, "ids"));
        out = ids.empty() ? ok("Select at least one position") : sim.close_lots(ids, "MANUAL", now) ? ok() : ok("Already closing or closed");
      } else if (path == "/api/speed") out = sim.set_speed(std::atof(jget(body, "x").c_str())) ? ok() : ok("Speed must be 0, 1, 2, 6.25, 12.5 or 25");
      else if (path == "/api/restart") { const std::string w = sim.restart_replay(); out = ok(w, w.empty() ? "" : "NOT_FLAT"); if (w.empty()) agent.new_day(); }
      else if (path == "/api/rearm") sim.rearm();
      else if (path == "/api/reset") { sim.reset(); agent.new_day(); }
      else if (path == "/api/fund") {
        const double amt = std::atof(jget(body, "amount").c_str());
        out = sim.fund(amt) ? ok() : ok(amt > 0 ? "One deposit can be at most 1,00,000 crore (1e12)" : "Enter an amount above zero");
      }
      else if (path == "/api/stub") sim.set_stub(jget(body, "on") == "true");
      else if (path == "/api/watch") {   // add a share (or an option already loaded) to the owner's list
        const std::string sym = jget(body, "sym");
        if (sym.empty()) out = ok("No symbol");
        else if (std::find(watch.begin(), watch.end(), sym) != watch.end()) out = ok();
        else if (sim.find_symbol(sym) >= 0 || add_share(sim, sym, uni, uni_date)) { watch.push_back(sym); save_watch(); agent.resize(sim.instruments()); }
        else out = ok("Not in the NSE daily file of " + uni_date + " (EQ series)");
      } else if (path == "/api/unwatch") {
        const std::string sym = jget(body, "sym");
        watch.erase(std::remove(watch.begin(), watch.end(), sym), watch.end()); save_watch();
      }
      else { respond(fd, 404, "application/json", ok("Unknown endpoint")); ::close(fd); continue; }
      respond(fd, 200, "application/json", out);
      write_agent_log();
      persist();   // every account action is on disk before the next one
    } else if (std::string dp; method == "GET" && url_decode(path, dp) && dp.find("..") == std::string::npos && dp.find('\\') == std::string::npos) {
      const std::string file = web + (dp == "/" ? "/index.html" : dp);
      std::ifstream f(file, std::ios::binary);
      if (f && std::filesystem::is_regular_file(file)) {
        std::ostringstream ss; ss << f.rdbuf();
        respond(fd, 200, mime(file), ss.str());
      } else respond(fd, 404, "text/plain", "not found");
    } else respond(fd, 400, "text/plain", "bad request");
    ::close(fd);
  }
}
