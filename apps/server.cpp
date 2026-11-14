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
#include <string>
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
// symbol,venue,price,tick,lot,source[,date,open,high,low,close,prev_close,volume_units,volume_check]
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
  int port = 8080;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--web")) web = argv[i + 1];
    else if (!std::strcmp(argv[i], "--port")) port = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--data")) data = argv[i + 1];
    else if (!std::strcmp(argv[i], "--bind")) bind_addr = argv[i + 1];
    else if (!std::strcmp(argv[i], "--instruments")) instruments = argv[i + 1];
    else if (!std::strcmp(argv[i], "--state")) state_file = argv[i + 1];
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

  constexpr Ts kTick = 250000000LL;                 // 4 market updates per second
  Ts next_tick = mono_ns() + kTick, next_save = mono_ns() + 2000000000LL;
  for (;;) {
    pollfd p{ls, POLLIN, 0};
    ::poll(&p, 1, 50);
    Ts now = mono_ns();
    sim.set_wall_ns(wall_ns());
    if (now - next_tick > 5000000000LL) next_tick = now;   // after a pause (machine asleep): no catch-up burst
    while (now >= next_tick) { sim.step(next_tick); next_tick += kTick; }
    if (now >= next_save) { persist(); next_save = now + 2000000000LL; }
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
    } else if (path.rfind("/api/", 0) == 0 && method == "POST") {
      std::string out = ok();
      if (path == "/api/order") {
        const int i = sim.find_symbol(jget(body, "sym"));
        const std::string side = jget(body, "side"), type = jget(body, "type");
        const double qty = std::atof(jget(body, "qty").c_str()), px = std::atof(jget(body, "px").c_str());
        if (i < 0) out = ok("Unknown instrument");
        else if ((side != "BUY" && side != "SELL") || (type != "MARKET" && type != "LIMIT")) out = ok("Side must be BUY or SELL; type MARKET or LIMIT");
        else if (!(qty >= 1) || qty > 1e9 || qty != std::floor(qty)) out = ok("Quantity must be a whole number of 1 or more");
        else {
          const std::string why = sim.submit(static_cast<std::size_t>(i), side == "BUY" ? Side::Buy : Side::Sell, type == "MARKET" ? OrdType::Market : OrdType::Limit,
                                             static_cast<Qty>(qty), static_cast<Price>(std::llround(px * 100.0)), "MANUAL", now);
          out = ok(why, why.empty() ? "" : sim.orders().back().code);
        }
      } else if (path == "/api/cancel") {
        const std::string id = jget(body, "id");
        const std::size_t d = id.find_first_of("0123456789");
        out = (d != std::string::npos && sim.cancel(static_cast<OrderId>(std::atoll(id.c_str() + d)), now)) ? ok() : ok("Order is not open");
      } else if (path == "/api/cancel_all") sim.cancel_all(now);
      else if (path == "/api/flatten") sim.flatten(now);
      else if (path == "/api/kill") sim.kill(now);
      else if (path == "/api/close") {
        const auto ids = parse_ids(jget(body, "ids"));
        out = ids.empty() ? ok("Select at least one position") : sim.close_lots(ids, "MANUAL", now) ? ok() : ok("Already closing or closed");
      } else if (path == "/api/speed") out = sim.set_speed(std::atof(jget(body, "x").c_str())) ? ok() : ok("Speed must be 0, 1, 2, 6.25, 12.5 or 25");
      else if (path == "/api/restart") { const std::string w = sim.restart_replay(); out = ok(w, w.empty() ? "" : "NOT_FLAT"); }
      else if (path == "/api/rearm") sim.rearm();
      else if (path == "/api/reset") sim.reset();
      else if (path == "/api/fund") out = sim.fund(std::atof(jget(body, "amount").c_str())) ? ok() : ok("Enter an amount above zero");
      else if (path == "/api/stub") sim.set_stub(jget(body, "on") == "true");
      else { respond(fd, 404, "application/json", ok("Unknown endpoint")); ::close(fd); continue; }
      respond(fd, 200, "application/json", out);
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
