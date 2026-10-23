// db_bench-style microbenchmark.
//   ./build/lsmkv_bench [num_keys] [value_size] [dir]

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "lsmkv/db.h"

using namespace lsmkv;
using Clock = std::chrono::steady_clock;

static std::string Key(uint64_t i) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%016llu", static_cast<unsigned long long>(i));
  return buf;
}

struct Result {
  const char* name;
  double secs;
  uint64_t ops;
  std::vector<double> lat_us;
};

static void Report(Result& r, size_t bytes_per_op) {
  double ops_s = r.ops / r.secs;
  std::printf("%-14s %10.0f ops/s  %7.1f MB/s", r.name, ops_s, ops_s * bytes_per_op / 1e6);
  if (!r.lat_us.empty()) {
    std::sort(r.lat_us.begin(), r.lat_us.end());
    auto pct = [&](double p) { return r.lat_us[size_t(p * (r.lat_us.size() - 1))]; };
    std::printf("   p50 %6.2fus  p99 %7.2fus  p99.9 %8.2fus", pct(0.5), pct(0.99), pct(0.999));
  }
  std::printf("\n");
}

template <typename F>
static Result Run(const char* name, uint64_t n, F&& op, bool latency) {
  Result r{name, 0, n, {}};
  if (latency) r.lat_us.reserve(n);
  auto start = Clock::now();
  for (uint64_t i = 0; i < n; i++) {
    if (latency) {
      auto t0 = Clock::now();
      op(i);
      r.lat_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    } else {
      op(i);
    }
  }
  r.secs = std::chrono::duration<double>(Clock::now() - start).count();
  return r;
}

int main(int argc, char** argv) {
  uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
  size_t value_size = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 100;
  std::string dir = argc > 3 ? argv[3] : "/tmp/lsmkv_bench_" + std::to_string(getpid());
  (void)!std::system(("rm -rf '" + dir + "'").c_str());

  Options opts;
  std::unique_ptr<DB> db;
  Status s = DB::Open(opts, dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "open: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("lsmkv bench: %llu keys, 16B keys, %zuB values, dir=%s\n\n",
              static_cast<unsigned long long>(n), value_size, dir.c_str());

  std::mt19937_64 rng(301);
  std::string value(value_size, 'x');
  for (auto& c : value) c = 'a' + rng() % 26;
  std::vector<uint64_t> perm(n);
  std::iota(perm.begin(), perm.end(), 0);
  std::shuffle(perm.begin(), perm.end(), rng);
  size_t bpo = 16 + value_size;

  auto fillrandom = Run("fillrandom", n, [&](uint64_t i) { db->Put({}, Key(perm[i] * 2), value); }, false);
  db->WaitForIdle();
  Report(fillrandom, bpo);

  auto overwrite = Run("overwrite", n, [&](uint64_t i) { db->Put({}, Key(rng() % n * 2), value); }, false);
  db->WaitForIdle();
  Report(overwrite, bpo);

  WriteOptions sync;
  sync.sync = true;
  uint64_t nsync = std::min<uint64_t>(n / 100, 2000);
  auto fillsync = Run("fillsync", nsync, [&](uint64_t i) { db->Put(sync, Key(i * 2 + 1), value); }, true);
  Report(fillsync, bpo);

  std::string v;
  uint64_t found = 0;
  auto readrandom = Run("readrandom", n, [&](uint64_t i) {
    found += db->Get({}, Key(rng() % n * 2), &v).ok();
  }, true);
  Report(readrandom, bpo);

  DBStats before = db->Stats();
  uint64_t missing_found = 0;
  auto readmissing = Run("readmissing", n, [&](uint64_t i) {
    // Odd keys > 2*nsync were never written: exercises the bloom filters.
    missing_found += db->Get({}, Key((rng() % n) * 2 + 1 + 2 * nsync), &v).ok();
  }, true);
  Report(readmissing, 16);
  DBStats after = db->Stats();

  uint64_t scanned = 0;
  auto t0 = Clock::now();
  auto it = db->NewIterator({});
  for (it->SeekToFirst(); it->Valid(); it->Next()) scanned++;
  Result scan{"readseq", std::chrono::duration<double>(Clock::now() - t0).count(), scanned, {}};
  it.reset();
  Report(scan, bpo);

  std::printf("\nfound %llu/%llu random reads, %llu false hits on missing keys\n",
              (unsigned long long)found, (unsigned long long)n, (unsigned long long)missing_found);
  uint64_t checks = after.bloom_checks - before.bloom_checks;
  uint64_t negs = after.bloom_negatives - before.bloom_negatives;
  std::printf("readmissing: bloom filters skipped %llu of %llu table probes (%.1f%%)\n\n",
              (unsigned long long)negs, (unsigned long long)checks,
              checks ? 100.0 * negs / checks : 0.0);
  std::printf("%s", db->DebugString().c_str());
  db.reset();
  (void)!std::system(("rm -rf '" + dir + "'").c_str());
  return 0;
}
