// Matching-engine latency benchmark. Usage: mc_bench <out_json>
// Numbers are only meaningful for the machine they were measured on.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include "mc/exchange.hpp"
#include "mc/latency.hpp"
#include "mc/order_book.hpp"

namespace {
std::string cpu_model() {
  std::ifstream f("/proc/cpuinfo");
  std::string l;
  while (std::getline(f, l))
    if (l.rfind("model name", 0) == 0) { const auto p = l.find(':'); return p == std::string::npos ? l : l.substr(p + 2); }
  return "unknown";
}
void row(std::ofstream& o, const char* name, mc::LatencyStats s, bool last) {
  o << "    {\"op\":\"" << name << "\",\"n\":" << s.n << ",\"p50\":" << s.p50 << ",\"p90\":" << s.p90 << ",\"p95\":" << s.p95
    << ",\"p99\":" << s.p99 << ",\"p999\":" << s.p999 << ",\"max\":" << s.max << "}" << (last ? "\n" : ",\n");
  std::printf("%-22s n=%zu  p50=%lld  p90=%lld  p95=%lld  p99=%lld  p99.9=%lld  max=%lld ns\n", name, s.n,
              static_cast<long long>(s.p50), static_cast<long long>(s.p90), static_cast<long long>(s.p95),
              static_cast<long long>(s.p99), static_cast<long long>(s.p999), static_cast<long long>(s.max));
}
}  // namespace

int main(int argc, char** argv) {
  using clk = std::chrono::steady_clock;
  const std::size_t N = 200000;
  mc::OrderBook book(1 << 18);
  mc::Rng rng(7);
  mc::LatencyRecorder add(N), match(N), cancel(N);
  long long traded = 0;
  auto on_trade = [&](const mc::Trade& t) { traded += t.qty; };
  auto ns = [](clk::time_point a, clk::time_point b) { return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count(); };
  mc::OrderId id = 1;
  for (std::size_t i = 0; i < N; ++i) {   // resting adds on both sides, never crossing
    const bool buy = (i & 1) == 0;
    const mc::Price px = buy ? 100000 - 5 * static_cast<mc::Price>(rng.next() % 50) : 100005 + 5 * static_cast<mc::Price>(rng.next() % 50);
    const auto a = clk::now();
    book.add_limit(id++, buy ? mc::Side::Buy : mc::Side::Sell, px, 10, on_trade);
    add.add(ns(a, clk::now()));
  }
  for (std::size_t i = 0; i < N / 2; ++i) {   // cancels of resting orders
    const mc::OrderId victim = 1 + (rng.next() % (N - 1));
    const auto a = clk::now();
    book.cancel(victim);
    cancel.add(ns(a, clk::now()));
  }
  for (std::size_t i = 0; i < N / 2; ++i) {   // aggressive orders that match
    const bool buy = (i & 1) == 0;
    const auto a = clk::now();
    book.add_market(id++, buy ? mc::Side::Buy : mc::Side::Sell, 10, on_trade);
    match.add(ns(a, clk::now()));
  }
  const std::string cpu = cpu_model();
  std::printf("SIMULATION benchmark, machine: %s\n", cpu.c_str());
  if (argc > 1) {
    std::ofstream o(argv[1]);
    o << "{\n  \"label\":\"SIMULATION\",\n  \"machine\":\"" << cpu << "\",\n  \"unit\":\"ns\",\n"
      << "  \"note\":\"Measured on a shared machine with steady_clock; includes timer overhead. Not trading-node latency.\",\n  \"ops\":[\n";
    row(o, "add_resting_limit", add.stats(), false);
    row(o, "cancel", cancel.stats(), false);
    row(o, "market_order_match", match.stats(), true);
    o << "  ]\n}\n";
  }
  return traded > 0 ? 0 : 1;
}
