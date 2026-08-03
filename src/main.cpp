// SPDX-License-Identifier: MIT
// cpp-netdev-watch — Linux network interface monitor in C++23
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o netdev-watch
// Usage: ./netdev-watch [--watch SECONDS] [--json] [--no-color] [--selftest] [--help]

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace netdev {

// ── ANSI palette (Catppuccin-ish) ──────────────────────────────────────────
constexpr const char* kRed    = "\x1b[31m";
constexpr const char* kGreen  = "\x1b[32m";
constexpr const char* kYellow = "\x1b[33m";
constexpr const char* kCyan   = "\x1b[36m";
constexpr const char* kBold   = "\x1b[1m";
constexpr const char* kReset  = "\x1b[0m";

// ── Types ─────────────────────────────────────────────────────────────────
struct Counters {
  unsigned long long bytes = 0;
  unsigned long long packets = 0;
  unsigned long long errs = 0;
  unsigned long long drop = 0;
};

struct Interface {
  std::string name;             // "lo", "eth0", "wlan0"...
  Counters rx;
  Counters tx;
};

struct Sample {
  std::vector<Interface> interfaces;
  double uptime = 0.0;
  double idle = 0.0;
  std::optional<double> load1, load5, load15;
};

struct Rate {
  double rx_bytes_s = 0.0;
  double tx_bytes_s = 0.0;
  double rx_pkts_s = 0.0;
  double tx_pkts_s = 0.0;
};

// ── Small helpers ─────────────────────────────────────────────────────────
std::string format_human(unsigned long long v) {
  if (v >= 1'000'000'000ULL) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fG", static_cast<double>(v) / 1e9);
    return buf;
  }
  if (v >= 1'000'000ULL) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(v) / 1e6);
    return buf;
  }
  if (v >= 1'000ULL) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(v) / 1e3);
    return buf;
  }
  return std::to_string(v);
}

std::string format_thousands(unsigned long long v) {
  std::string s = std::to_string(v);
  for (size_t i = s.size(); i > 3; i -= 3)
    s.insert(i - 3, 1, ',');
  return s;
}

std::string format_duration(double seconds) {
  long long s = static_cast<long long>(seconds);
  int d = static_cast<int>(s / 86400);
  int h = static_cast<int>((s % 86400) / 3600);
  int m = static_cast<int>((s % 3600) / 60);
  int sec = static_cast<int>(s % 60);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%dd %02d:%02d:%02d", d, h, m, sec);
  return buf;
}

std::string format_rate(double bytes_per_s) {
  if (bytes_per_s >= 1'000'000.0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f MB/s", bytes_per_s / 1e6);
    return buf;
  }
  if (bytes_per_s >= 1'000.0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f kB/s", bytes_per_s / 1e3);
    return buf;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f B/s", bytes_per_s);
  return buf;
}

const char* err_color(unsigned long long errs, unsigned long long drop, bool color) {
  if (!color) return "";
  if (errs > 0 || drop > 0) return kYellow;
  return kGreen;
}

// ── /proc/net/dev parsing ─────────────────────────────────────────────────
// Header lines:
//   Inter-|   Receive  ...
//    face |bytes ...
// Data line:
//   "  eth0: 375414459 1467100    0    0    0     0          0         0 647965971  732802    0    0    0     0       0          0"
std::optional<Interface> parse_dev_line(const std::string& line) {
  std::istringstream ss(line);
  std::string name_tok;
  ss >> name_tok;
  if (name_tok.empty() || name_tok.back() != ':') return std::nullopt;

  std::vector<unsigned long long> vals(16, 0);
  int n = 0;
  unsigned long long v = 0;
  while (n < 16 && (ss >> v)) vals[n++] = v;
  if (n < 16) return std::nullopt;  // need all 16 counters

  Interface iface;
  iface.name = name_tok.substr(0, name_tok.size() - 1);
  iface.rx = Counters{vals[0], vals[1], vals[2], vals[3]};
  iface.tx = Counters{vals[8], vals[9], vals[10], vals[11]};
  return iface;
}

Sample read_sample(const std::string& path = "/proc/net/dev") {
  Sample s;
  std::ifstream f(path);
  if (!f.is_open()) return s;

  std::string line;
  while (std::getline(f, line)) {
    if (line.find('|') != std::string::npos) continue;   // header block
    if (auto iface = parse_dev_line(line); iface)
      s.interfaces.push_back(std::move(*iface));
  }

  std::ifstream up("/proc/uptime");
  if (up.is_open()) {
    std::string a, b;
    up >> a >> b;
    if (!a.empty() && !b.empty()) {
      s.uptime = std::atof(a.c_str());
      s.idle = std::atof(b.c_str());
    }
  }

  std::ifstream la("/proc/loadavg");
  if (la.is_open()) {
    std::string a, b, c, d, e;
    la >> a >> b >> c >> d >> e;
    if (!a.empty()) s.load1 = std::atof(a.c_str());
    if (!b.empty()) s.load5 = std::atof(b.c_str());
    if (!c.empty()) s.load15 = std::atof(c.c_str());
  }
  return s;
}

// Delta rates between two samples over `interval` seconds.
Rate compute_rate(const Interface& prev, const Interface& cur, double interval) {
  Rate r;
  if (interval <= 0.0) return r;
  r.rx_bytes_s = (cur.rx.bytes > prev.rx.bytes)
      ? static_cast<double>(cur.rx.bytes - prev.rx.bytes) / interval : 0.0;
  r.tx_bytes_s = (cur.tx.bytes > prev.tx.bytes)
      ? static_cast<double>(cur.tx.bytes - prev.tx.bytes) / interval : 0.0;
  r.rx_pkts_s = (cur.rx.packets > prev.rx.packets)
      ? static_cast<double>(cur.rx.packets - prev.rx.packets) / interval : 0.0;
  r.tx_pkts_s = (cur.tx.packets > prev.tx.packets)
      ? static_cast<double>(cur.tx.packets - prev.tx.packets) / interval : 0.0;
  return r;
}

// ── Rendering ─────────────────────────────────────────────────────────────
void print_banner(bool color) {
  const char* c = color ? kCyan : "";
  const char* b = color ? kBold : "";
  const char* r = color ? kReset : "";
  std::println(std::cout, "{}{}netdev-watch{} — Linux network interface monitor (C++23)", c, b, r);
  std::println(std::cout);
}

void render_table(const Sample& s, bool color) {
  std::cout << "  " << std::left << std::setw(10) << "INTERFACE"
            << std::right << std::setw(9) << "RX" << std::setw(11) << "RX pkts"
            << std::setw(6) << "errs" << std::setw(6) << "drop"
            << std::setw(10) << "TX" << std::setw(11) << "TX pkts"
            << std::setw(6) << "errs" << std::setw(6) << "drop" << '\n';

  for (const auto& iface : s.interfaces) {
    const char* c = err_color(iface.rx.errs + iface.tx.errs, iface.rx.drop + iface.tx.drop, color);
    const char* rr = color ? kReset : "";
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "  %-10s %9s %11s %s%6llu%s %s%6llu%s  %9s %11s %s%6llu%s %s%6llu%s",
                  iface.name.c_str(),
                  format_human(iface.rx.bytes).c_str(), format_thousands(iface.rx.packets).c_str(),
                  c, iface.rx.errs, rr, c, iface.rx.drop, rr,
                  format_human(iface.tx.bytes).c_str(), format_thousands(iface.tx.packets).c_str(),
                  c, iface.tx.errs, rr, c, iface.tx.drop, rr);
    std::cout << buf << '\n';
  }
  std::println(std::cout);
}

void render_rates(const Sample& prev, const Sample& cur, double interval, bool color) {
  std::println(std::cout, "  {:<10} {:>10} {:>10} {:>9} {:>9}", "INTERFACE", "RX rate", "TX rate", "RX pkt/s", "TX pkt/s");
  std::unordered_map<std::string, const Interface*> pmap;
  for (const auto& i : prev.interfaces) pmap[i.name] = &i;

  for (const auto& iface : cur.interfaces) {
    std::string rxr = "-", txr = "-", rxp = "-", txp = "-";
    if (auto it = pmap.find(iface.name); it != pmap.end()) {
      Rate r = compute_rate(*it->second, iface, interval);
      rxr = format_rate(r.rx_bytes_s);
      txr = format_rate(r.tx_bytes_s);
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.1f", r.rx_pkts_s);
      rxp = buf;
      std::snprintf(buf, sizeof(buf), "%.1f", r.tx_pkts_s);
      txp = buf;
    }
    const char* c = color ? kCyan : "";
    const char* rr = color ? kReset : "";
    std::println(std::cout, "  {:<10} {}{:>10}{} {:>10} {:>9} {:>9}",
                 iface.name, c, rxr, rr, txr, rxp, txp);
  }
  std::println(std::cout);
}

void render_context(const Sample& s, bool color) {
  (void)color;
  if (s.load1 && s.load5 && s.load15) {
    std::println(std::cout, "  load   {:<5.2f} {:<5.2f} {:<5.2f}    source /proc/net/dev", *s.load1, *s.load5, *s.load15);
  }
  std::println(std::cout, "  uptime {}", format_duration(s.uptime));
  std::println(std::cout);
}

void render_json(const Sample& s) {
  std::string out = "{\n  \"interfaces\": [\n";
  for (size_t i = 0; i < s.interfaces.size(); ++i) {
    const auto& iface = s.interfaces[i];
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "    {\"name\":\"%s\",\"rx\":{\"bytes\":%llu,\"packets\":%llu,\"errs\":%llu,\"drop\":%llu},"
                  "\"tx\":{\"bytes\":%llu,\"packets\":%llu,\"errs\":%llu,\"drop\":%llu}}",
                  iface.name.c_str(), iface.rx.bytes, iface.rx.packets, iface.rx.errs, iface.rx.drop,
                  iface.tx.bytes, iface.tx.packets, iface.tx.errs, iface.tx.drop);
    out += buf;
    if (i + 1 < s.interfaces.size()) out += ",";
    out += "\n";
  }
  out += "  ],\n  \"uptime\": ";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "{\"seconds\":%.2f,\"idle_seconds\":%.2f}", s.uptime, s.idle);
  out += buf;
  out += ",\n  \"loadavg\": {";
  if (s.load1 && s.load5 && s.load15) {
    std::snprintf(buf, sizeof(buf), "\"l1\":%.2f,\"l5\":%.2f,\"l15\":%.2f", *s.load1, *s.load5, *s.load15);
    out += buf;
  }
  out += "}\n}\n";
  std::cout << out;
}

void print_help() {
  std::cout <<
    "netdev-watch — Linux network interface monitor in C++23\n"
    "\n"
    "Reads /proc/net/dev and reports per-interface RX/TX byte and packet\n"
    "counters, errors and drops, with human-formatted totals (34.5M, 647.9M)\n"
    "and color thresholds: green = clean, yellow = errors/drops present.\n"
    "\n"
    "Usage:\n"
    "  ./netdev-watch             one-shot table of interface counters\n"
    "  ./netdev-watch --watch 2   refresh every 2 seconds with live rates (Ctrl+C to stop)\n"
    "  ./netdev-watch --json      machine-readable JSON output\n"
    "  ./netdev-watch --selftest  run parser/format self-tests\n"
    "  ./netdev-watch --no-color  plain output (scripts/pipes)\n"
    "  ./netdev-watch --help      show this help\n"
    "\n"
    "Exit codes: 0 = ok, 2 = no interfaces found or bad arguments\n";
}

// ── Self-tests ────────────────────────────────────────────────────────────
int run_selftest() {
  int failures = 0;
  auto check = [&failures](bool cond, std::string_view name) {
    if (cond) {
      std::println(std::cout, "  ✓ {}", name);
    } else {
      std::println(std::cout, "  ✗ {}", name);
      ++failures;
    }
  };

  std::println(std::cout, "netdev-watch selftest");

  // 1. Parse a loopback line
  auto lo = parse_dev_line("    lo: 34571588   52988    0    0    0     0          0         0 34571588   52988    0    0    0     0       0          0");
  check(lo.has_value(), "parse lo line");
  check(lo && lo->name == "lo", "interface name stripped of ':' (lo)");
  check(lo && lo->rx.bytes == 34571588ULL, "rx bytes parsed (34571588)");
  check(lo && lo->rx.packets == 52988ULL, "rx packets parsed (52988)");
  check(lo && lo->tx.bytes == lo->rx.bytes, "tx bytes mirror rx on lo");

  // 2. Parse an ethernet line with errors
  auto eth = parse_dev_line("  eth0: 375414459 1467100   12    3    0     0          0         0 647965971  732802    0    0    0     0       0          0");
  check(eth.has_value(), "parse eth0 line");
  check(eth && eth->rx.errs == 12ULL, "rx errs parsed (12)");
  check(eth && eth->rx.drop == 3ULL, "rx drop parsed (3)");
  check(eth && eth->tx.bytes == 647965971ULL, "tx bytes parsed (647965971)");

  // 3. Header lines are rejected
  check(!parse_dev_line("Inter-|   Receive                                                |  Transmit").has_value(), "reject header line");
  check(!parse_dev_line(" face |bytes    packets errs drop fifo frame compressed multicast|bytes").has_value(), "reject column header");

  // 4. Malformed / truncated lines are rejected
  check(!parse_dev_line("").has_value(), "reject empty line");
  check(!parse_dev_line("  eth0: 1 2 3").has_value(), "reject truncated counters");
  check(!parse_dev_line("garbage without colon 1 2 3").has_value(), "reject line without colon");

  // 5. Human formatting
  check(format_human(34571588ULL) == "34.6M", "format 34.6M");
  check(format_human(647965971ULL) == "648.0M", "format 648.0M");
  check(format_human(88213ULL) == "88.2K", "format 88.2K");
  check(format_human(999ULL) == "999", "format 999");
  check(format_human(0ULL) == "0", "format 0");

  // 6. Thousands separator
  check(format_thousands(1467100ULL) == "1,467,100", "thousands separator");
  check(format_thousands(52988ULL) == "52,988", "small thousands");

  // 7. Rate computation
  Interface p{{"eth0"}, {1000ULL, 10ULL, 0, 0}, {2000ULL, 20ULL, 0, 0}};
  Interface c{{"eth0"}, {1'001'000ULL, 10010ULL, 0, 0}, {2'002'000ULL, 20020ULL, 0, 0}};
  Rate r = compute_rate(p, c, 2.0);
  check(r.rx_bytes_s > 499'999.0 && r.rx_bytes_s < 500'001.0, "rx rate 500,000 B/s over 2s");
  check(r.tx_bytes_s > 999'999.0 && r.tx_bytes_s < 1'000'001.0, "tx rate 1,000,000 B/s over 2s");
  check(r.rx_pkts_s > 4999.0 && r.rx_pkts_s < 5001.0, "rx packet rate 5,000 pkt/s");
  check(r.tx_pkts_s > 9999.0 && r.tx_pkts_s < 10001.0, "tx packet rate 10,000 pkt/s");

  // 8. Rate formatting
  check(format_rate(500'000.0) == "500.0 kB/s", "format 500.0 kB/s");
  check(format_rate(2'000'000.0) == "2.00 MB/s", "format 2.00 MB/s");
  check(format_rate(42.0) == "42 B/s", "format 42 B/s");

  // 9. Error colors
  check(std::string(err_color(0, 0, true)) == kGreen, "clean iface -> green");
  check(std::string(err_color(1, 0, true)) == kYellow, "errors -> yellow");
  check(std::string(err_color(0, 5, true)) == kYellow, "drops -> yellow");
  check(std::string(err_color(5, 5, false)).empty(), "no-color disables ansi");

  // 10. Duration formatting
  check(format_duration(1845045.0) == "21d 08:30:45", "format 21d 08:30:45");
  check(format_duration(3661.0) == "0d 01:01:01", "format 3661s");

  // 11. Live read of /proc/net/dev (lo always exists)
  Sample live = read_sample();
  check(!live.interfaces.empty(), "live read /proc/net/dev has interfaces");
  bool has_lo = false;
  for (const auto& i : live.interfaces)
    if (i.name == "lo") has_lo = true;
  check(has_lo, "loopback interface present");

  std::println(std::cout);
  if (failures == 0) {
    std::println(std::cout, "  {} 35/35 selftests passed{}", kGreen, kReset);
    return 0;
  }
  std::println(std::cout, "  {} {} selftest(s) failed{}", kRed, failures, kReset);
  return 1;
}

}  // namespace netdev

// ── Signal handling (watch mode) ──────────────────────────────────────────
static volatile std::sig_atomic_t g_stop = 0;
extern "C" void netdev_on_sigint(int) { g_stop = 1; }

int main(int argc, char** argv) {
  using namespace netdev;

  bool color = true, json = false, selftest = false;
  int watch = 0;

  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    if (a == "--no-color") {
      color = false;
    } else if (a == "--json") {
      json = true;
    } else if (a == "--selftest") {
      selftest = true;
    } else if (a == "--watch" || a == "-w") {
      if (i + 1 < argc) {
        watch = std::atoi(argv[++i]);
        if (watch <= 0) watch = 1;
      }
    } else if (a == "--help" || a == "-h") {
      print_help();
      return 0;
    } else {
      std::cerr << "netdev-watch: unknown option '" << a << "'\n\n";
      print_help();
      return 2;
    }
  }

  if (selftest) return run_selftest();

  Sample first = read_sample();
  if (first.interfaces.empty()) {
    std::cerr << "netdev-watch: no interfaces found in /proc/net/dev\n";
    return 2;
  }

  if (json) {
    render_json(first);
    return 0;
  }

  if (watch > 0) {
    std::signal(SIGINT, netdev_on_sigint);
    Sample prev = first;
    while (!g_stop) {
      Sample cur = read_sample();
      if (color) std::print(std::cout, "\x1b[2J\x1b[H");
      print_banner(color);
      render_table(cur, color);
      render_rates(prev, cur, static_cast<double>(watch), color);
      render_context(cur, color);
      std::cout << std::flush;  // render immediately, even piped
      if (g_stop) break;
      prev = std::move(cur);
      std::this_thread::sleep_for(std::chrono::seconds(watch));
    }
    std::println(std::cout);
    return 0;
  }

  print_banner(color);
  render_table(first, color);
  render_context(first, color);
  return 0;
}
