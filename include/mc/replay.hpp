// Historical bar loader with data-quality checks (duplicates, out-of-order rows, gaps, bad rows).
// CSV format: date,open,high,low,close,volume   (date = YYYYMMDD, prices in rupees). Lines starting with # are comments.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "mc/types.hpp"

namespace mc {

struct LoadResult {
  std::vector<Bar> bars;
  int duplicates = 0, out_of_order = 0, gaps = 0, bad_rows = 0;
  bool synthetic = false, ok = false;
};

inline long days_from_civil(int ymd) {
  int y = ymd / 10000; const int m = (ymd / 100) % 100, d = ymd % 100;
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(d) - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097L + static_cast<long>(doe) - 719468L;
}

inline LoadResult parse_bars(std::istream& in, int gap_days = 5) {
  LoadResult r;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line[0] == '#') { if (line.find("SYNTHETIC") != std::string::npos) r.synthetic = true; continue; }
    if (line[0] < '0' || line[0] > '9') continue;   // header
    std::stringstream ss(line);
    std::string f[6];
    int n = 0;
    while (n < 6 && std::getline(ss, f[n], ',')) ++n;
    if (n < 5) { ++r.bad_rows; continue; }
    Bar b;
    b.date = std::atoi(f[0].c_str());
    auto px = [](const std::string& s) { return static_cast<Price>(std::llround(std::atof(s.c_str()) * 100.0)); };
    b.o = px(f[1]); b.h = px(f[2]); b.l = px(f[3]); b.c = px(f[4]);
    b.vol = n >= 6 ? std::atoll(f[5].c_str()) : 0;
    const bool valid = b.date > 19000101 && b.l > 0 && b.h >= b.l && b.o >= b.l && b.o <= b.h &&
                       b.c >= b.l && b.c <= b.h && b.vol >= 0;
    if (!valid) { ++r.bad_rows; continue; }
    if (!r.bars.empty() && b.date < r.bars.back().date) ++r.out_of_order;
    r.bars.push_back(b);
  }
  std::stable_sort(r.bars.begin(), r.bars.end(), [](const Bar& a, const Bar& b) { return a.date < b.date; });
  std::vector<Bar> out;
  out.reserve(r.bars.size());
  for (const Bar& b : r.bars) {
    if (!out.empty() && out.back().date == b.date) { ++r.duplicates; continue; }   // keep the first copy
    if (!out.empty() && days_from_civil(b.date) - days_from_civil(out.back().date) > gap_days) ++r.gaps;
    out.push_back(b);
  }
  r.bars.swap(out);
  r.ok = !r.bars.empty();
  return r;
}

inline LoadResult load_bars(const std::string& path, int gap_days = 5) {
  std::ifstream in(path);
  if (!in) return LoadResult{};
  return parse_bars(in, gap_days);
}

}  // namespace mc
