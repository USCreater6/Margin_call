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
std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
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
std::string ok(const std::string& why = "") {
  std::string w;
  for (char c : why) { if (c == '"' || c == '\\') w += '\\'; w += c; }
  return std::string("{\"ok\":") + (why.empty() ? "true" : "false") + ",\"why\":\"" + w + "\"}";
}
}  // namespace

int main(int argc, char** argv) {
  std::string web = "web", data, bind_addr = "127.0.0.1";
  int port = 8080;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--web")) web = argv[i + 1];
    else if (!std::strcmp(argv[i], "--port")) port = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--data")) data = argv[i + 1];
    else if (!std::strcmp(argv[i], "--bind")) bind_addr = argv[i + 1];
  }
  std::signal(SIGPIPE, SIG_IGN);

  LiveSim sim(100000.0, RiskLimits{}, static_cast<std::uint64_t>(wall_ms()));
  sim.set_wall(wall_ms());
  int loaded = 0;
  if (!data.empty() && std::filesystem::exists(data)) {
    std::vector<std::string> files;
    for (const auto& e : std::filesystem::directory_iterator(data))
      if (e.path().extension() == ".csv" && e.path().filename().string()[0] != '_') files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      const LoadResult r = load_bars(f);
      if (!r.ok) continue;
      const Price c = std::max<Price>(5, (r.bars.back().c / 5) * 5);
      sim.add_instrument(std::filesystem::path(f).stem().string(), r.synthetic ? "SYNTH" : "NSE", c, 5);
      ++loaded;
    }
  }
  if (!loaded) {   // placeholder instruments with placeholder prices
    sim.add_instrument("NIFTY-FUT", "NSE F&O", 2500000, 50);
    sim.add_instrument("RELIANCE", "NSE", 140000, 10);
    sim.add_instrument("HDFCBANK", "NSE", 100000, 10);
    sim.add_instrument("TCS", "NSE", 300000, 10);
    sim.add_instrument("GOLDM", "MCX", 10000000, 100);
    sim.add_instrument("CRUDEOIL", "MCX", 600000, 100);
  }

  const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET; a.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, bind_addr.c_str(), &a.sin_addr) != 1) { std::fprintf(stderr, "bad bind address\n"); return 2; }
  if (::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(ls, 16) != 0) { std::perror("bind/listen"); return 2; }
  std::printf("Margin_call terminal (SIMULATION) on http://%s:%d  instruments: %zu (%s)\n", bind_addr.c_str(), port, sim.instruments(),
              loaded ? "last close from data files; ticks are synthetic" : "placeholder prices; ticks are synthetic");
  std::fflush(stdout);

  Ts next_tick = mono_ns() + 1000000000LL;
  for (;;) {
    pollfd p{ls, POLLIN, 0};
    ::poll(&p, 1, 100);
    Ts now = mono_ns();
    while (now >= next_tick) { sim.set_wall(wall_ms()); sim.step(next_tick); next_tick += 1000000000LL; }
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
          std::size_t cl = req.find("Content-Length:");
          if (cl == std::string::npos) cl = req.find("content-length:");
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
    sim.set_wall(wall_ms());

    if (path == "/api/state") {
      std::size_t sel = 0;
      if (const auto s = query.find("sel="); s != std::string::npos) sel = static_cast<std::size_t>(std::atol(query.c_str() + s + 4));
      respond(fd, 200, "application/json", sim.state_json(sel, now));
    } else if (path.rfind("/api/", 0) == 0 && method == "POST") {
      std::string out = ok();
      if (path == "/api/order") {
        const int i = sim.find_symbol(jget(body, "sym"));
        const std::string side = jget(body, "side"), type = jget(body, "type");
        const double qty = std::atof(jget(body, "qty").c_str()), px = std::atof(jget(body, "px").c_str());
        if (i < 0) out = ok("Unknown instrument");
        else if ((side != "BUY" && side != "SELL") || (type != "MARKET" && type != "LIMIT")) out = ok("Side must be BUY or SELL; type MARKET or LIMIT");
        else if (!(qty >= 1) || qty > 1e9 || qty != std::floor(qty)) out = ok("Quantity must be a whole number of 1 or more");
        else out = ok(sim.submit(static_cast<std::size_t>(i), side == "BUY" ? Side::Buy : Side::Sell, type == "MARKET" ? OrdType::Market : OrdType::Limit,
                                 static_cast<Qty>(qty), static_cast<Price>(std::llround(px * 100.0)), "MANUAL", now));
      } else if (path == "/api/cancel") {
        const std::string id = jget(body, "id");
        const std::size_t d = id.find_first_of("0123456789");
        out = (d != std::string::npos && sim.cancel(static_cast<OrderId>(std::atoll(id.c_str() + d)), now)) ? ok() : ok("Order is not open");
      } else if (path == "/api/cancel_all") sim.cancel_all(now);
      else if (path == "/api/flatten") sim.flatten(now);
      else if (path == "/api/kill") sim.kill(jget(body, "flatten") == "true", now);
      else if (path == "/api/rearm") sim.rearm();
      else if (path == "/api/reset") sim.reset();
      else if (path == "/api/fund") out = sim.fund(std::atof(jget(body, "amount").c_str())) ? ok() : ok("Enter an amount above zero");
      else if (path == "/api/stub") sim.set_stub(jget(body, "on") == "true");
      else { respond(fd, 404, "application/json", ok("Unknown endpoint")); ::close(fd); continue; }
      respond(fd, 200, "application/json", out);
    } else if (method == "GET" && path.find("..") == std::string::npos) {
      const std::string file = web + (path == "/" ? "/index.html" : path);
      std::ifstream f(file, std::ios::binary);
      if (f && std::filesystem::is_regular_file(file)) {
        std::ostringstream ss; ss << f.rdbuf();
        respond(fd, 200, mime(file), ss.str());
      } else respond(fd, 404, "text/plain", "not found");
    } else respond(fd, 400, "text/plain", "bad request");
    ::close(fd);
  }
}
