// Test suite for lsmkv. Self-contained (no gtest) so it builds anywhere
// with a C++17 compiler: `make test`.

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../src/block.h"
#include "../src/bloom.h"
#include "../src/coding.h"
#include "../src/dbformat.h"
#include "../src/wal.h"
#include "lsmkv/db.h"

using namespace lsmkv;

// ---------------------------------------------------------------- harness
static int g_failures = 0;
static std::vector<std::pair<const char*, std::function<void()>>>& Registry() {
  static std::vector<std::pair<const char*, std::function<void()>>> r;
  return r;
}
struct Registrar {
  Registrar(const char* n, std::function<void()> f) { Registry().emplace_back(n, std::move(f)); }
};
#define TEST(name)                                   \
  static void name();                                \
  static Registrar reg_##name(#name, name);          \
  static void name()
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                                              \
      return;                                                                    \
    }                                                                            \
  } while (0)
#define CHECK_OK(expr)                                                          \
  do {                                                                          \
    Status _s = (expr);                                                         \
    if (!_s.ok()) {                                                             \
      std::fprintf(stderr, "  CHECK_OK failed %s:%d: %s -> %s\n", __FILE__, __LINE__, \
                   #expr, _s.ToString().c_str());                               \
      g_failures++;                                                             \
      return;                                                                   \
    }                                                                           \
  } while (0)

static std::string TempDir(const char* name) {
  std::string d = "/tmp/lsmkv_test_" + std::to_string(getpid()) + "_" + name;
  std::string cmd = "rm -rf '" + d + "'";
  (void)!std::system(cmd.c_str());
  return d;
}

static Options SmallOptions() {
  Options o;
  o.write_buffer_size = 32 * 1024;  // tiny buffers => many flushes & compactions
  o.target_file_size = 16 * 1024;
  o.level1_max_bytes = 64 * 1024;
  o.block_size = 512;
  o.block_cache_bytes = 64 * 1024;
  return o;
}

static std::string Get(DB* db, const std::string& k, const Snapshot* snap = nullptr) {
  ReadOptions ro;
  ro.snapshot = snap;
  std::string v;
  Status s = db->Get(ro, k, &v);
  if (s.IsNotFound()) return "NOT_FOUND";
  if (!s.ok()) return "ERROR:" + s.ToString();
  return v;
}

static std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return buf;
}

// ---------------------------------------------------------------- unit tests

TEST(CodingRoundTrip) {
  std::string s;
  for (uint32_t v : {0u, 1u, 127u, 128u, 300u, 1u << 20, 0xffffffffu}) PutVarint32(&s, v);
  std::string_view in(s);
  for (uint32_t v : {0u, 1u, 127u, 128u, 300u, 1u << 20, 0xffffffffu}) {
    uint32_t got;
    CHECK(GetVarint32(&in, &got));
    CHECK(got == v);
  }
  CHECK(in.empty());
  // Known CRC32C test vector: "123456789" -> 0xE3069283
  CHECK(Crc32c("123456789", 9) == 0xE3069283u);
}

TEST(InternalKeyOrdering) {
  auto a = MakeInternalKey("apple", 5, kTypeValue);
  auto b = MakeInternalKey("apple", 9, kTypeValue);
  auto c = MakeInternalKey("banana", 1, kTypeValue);
  CHECK(CompareInternalKeys(b, a) < 0);  // newer first
  CHECK(CompareInternalKeys(a, c) < 0);
  CHECK(CompareInternalKeys(MakeLookupKey("apple", 7), b) > 0);
  CHECK(CompareInternalKeys(MakeLookupKey("apple", 7), a) < 0);
}

TEST(BlockSeekAcrossRestarts) {
  BlockBuilder b;
  std::vector<std::string> keys;
  for (int i = 0; i < 200; i++) keys.push_back(MakeInternalKey(Key(i * 2), 1, kTypeValue));
  for (auto& k : keys) b.Add(k, "v" + k);
  auto contents = std::make_shared<const std::string>(std::string(b.Finish()));
  auto it = NewBlockIterator(contents, [](std::string_view a, std::string_view b) {
    return CompareInternalKeys(a, b);
  });
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    CHECK(it->key() == keys[n]);
    n++;
  }
  CHECK(n == 200);
  // Seek to a key that falls between entries.
  it->Seek(MakeLookupKey(Key(101), kMaxSequenceNumber));
  CHECK(it->Valid());
  CHECK(ExtractUserKey(it->key()) == Key(102));
  it->Seek(MakeLookupKey(Key(9999), kMaxSequenceNumber));
  CHECK(!it->Valid());
}

TEST(BloomFilterFalsePositiveRate) {
  BloomFilterBuilder b(10);
  for (int i = 0; i < 10000; i++) b.AddKey(Key(i));
  std::string f = b.Finish();
  for (int i = 0; i < 10000; i++) CHECK(BloomMayContain(f, Key(i)));  // no false negatives
  int fp = 0;
  for (int i = 10000; i < 110000; i++) fp += BloomMayContain(f, Key(i));
  double rate = fp / 100000.0;
  std::printf("    bloom FPR at 10 bits/key: %.3f%%\n", rate * 100);
  CHECK(rate < 0.02);
}

TEST(WalTornTailIsIgnored) {
  std::string dir = TempDir("wal");
  CreateDirIfMissing(dir);
  std::string path = dir + "/x.log";
  {
    std::unique_ptr<WritableFile> f;
    CHECK_OK(WritableFile::Open(path, true, &f));
    LogWriter w(std::move(f));
    CHECK_OK(w.AddRecord("one", false));
    CHECK_OK(w.AddRecord("two", false));
    CHECK_OK(w.AddRecord("three-is-long", false));
    CHECK_OK(w.Close());
  }
  std::string data;
  CHECK_OK(ReadFileToString(path, &data));
  data.resize(data.size() - 3);  // simulate a crash mid-write
  LogReader r(data);
  std::string_view rec;
  CHECK(r.ReadRecord(&rec) && rec == "one");
  CHECK(r.ReadRecord(&rec) && rec == "two");
  CHECK(!r.ReadRecord(&rec));
  CHECK(r.hit_corruption());
}

// ---------------------------------------------------------------- DB tests

TEST(PutGetDelete) {
  std::string dir = TempDir("basic");
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(Options(), dir, &db));
  WriteOptions wo;
  CHECK_OK(db->Put(wo, "a", "1"));
  CHECK_OK(db->Put(wo, "b", "2"));
  CHECK(Get(db.get(), "a") == "1");
  CHECK_OK(db->Put(wo, "a", "3"));
  CHECK(Get(db.get(), "a") == "3");
  CHECK_OK(db->Delete(wo, "a"));
  CHECK(Get(db.get(), "a") == "NOT_FOUND");
  CHECK(Get(db.get(), "b") == "2");
  CHECK(Get(db.get(), "zzz") == "NOT_FOUND");
}

TEST(EmptyValuesAndBinaryKeys) {
  std::string dir = TempDir("binary");
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(Options(), dir, &db));
  std::string k1("a\0b", 3), k2("a\0c", 3);
  CHECK_OK(db->Put({}, k1, ""));
  CHECK_OK(db->Put({}, k2, std::string(10000, 'x')));
  CHECK_OK(db->Flush());
  CHECK(Get(db.get(), k1).empty());
  CHECK(Get(db.get(), k2).size() == 10000);
}

TEST(RecoverFromWalWithoutFlush) {
  std::string dir = TempDir("walrecover");
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(Options(), dir, &db));
    for (int i = 0; i < 1000; i++) CHECK_OK(db->Put({}, Key(i), "v" + std::to_string(i)));
    CHECK_OK(db->Delete({}, Key(7)));
  }  // closed without flushing: data lives only in the WAL
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(Options(), dir, &db));
  CHECK(Get(db.get(), Key(0)) == "v0");
  CHECK(Get(db.get(), Key(999)) == "v999");
  CHECK(Get(db.get(), Key(7)) == "NOT_FOUND");
  // Sequence numbers must continue past the recovered ones.
  CHECK_OK(db->Put({}, Key(0), "new"));
  CHECK(Get(db.get(), Key(0)) == "new");
}

TEST(WriteBatchIsAtomic) {
  std::string dir = TempDir("batch");
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(Options(), dir, &db));
  CHECK_OK(db->Put({}, "from", "100"));
  WriteBatch b;
  b.Put("from", "70");
  b.Put("to", "30");
  b.Delete("pending");
  CHECK_OK(db->Write({}, &b));
  CHECK(Get(db.get(), "from") == "70");
  CHECK(Get(db.get(), "to") == "30");
}

TEST(FlushCompactionAndReopen) {
  std::string dir = TempDir("compact");
  const int N = 20000;
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(SmallOptions(), dir, &db));
    for (int i = 0; i < N; i++) CHECK_OK(db->Put({}, Key(i), std::string(50, 'a' + i % 26)));
    for (int i = 0; i < N; i += 2) CHECK_OK(db->Delete({}, Key(i)));
    db->WaitForIdle();
    DBStats st = db->Stats();
    CHECK(st.flushes > 10);
    CHECK(st.compactions > 0);
    int deeper = 0;
    for (int l = 1; l < 7; l++) deeper += st.files_per_level[l];
    CHECK(deeper > 0);
  }
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(SmallOptions(), dir, &db));
  for (int i = 0; i < N; i++) {
    std::string expect = (i % 2 == 0) ? "NOT_FOUND" : std::string(50, 'a' + i % 26);
    CHECK(Get(db.get(), Key(i)) == expect);
  }
}

TEST(CompactAllDropsTombstonesAndOldVersions) {
  std::string dir = TempDir("compactall");
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(SmallOptions(), dir, &db));
  for (int round = 0; round < 5; round++)
    for (int i = 0; i < 2000; i++) CHECK_OK(db->Put({}, Key(i), std::string(40, 'a' + round)));
  for (int i = 0; i < 1000; i++) CHECK_OK(db->Delete({}, Key(i)));
  CHECK_OK(db->CompactAll());
  db->WaitForIdle();
  DBStats st = db->Stats();
  uint64_t total = 0;
  for (auto b : st.bytes_per_level) total += b;
  // 1000 live keys x ~60 bytes; 5 versions + tombstones would be > 5x that.
  CHECK(total < 1000 * 60 * 2);
  CHECK(Get(db.get(), Key(5)) == "NOT_FOUND");
  CHECK(Get(db.get(), Key(1500)) == std::string(40, 'e'));
}

TEST(SnapshotIsolationSurvivesCompaction) {
  std::string dir = TempDir("snapshot");
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(SmallOptions(), dir, &db));
  for (int i = 0; i < 3000; i++) CHECK_OK(db->Put({}, Key(i), "old"));
  const Snapshot* snap = db->GetSnapshot();
  for (int i = 0; i < 3000; i++) CHECK_OK(db->Put({}, Key(i), "new"));
  for (int i = 0; i < 3000; i += 3) CHECK_OK(db->Delete({}, Key(i)));
  CHECK_OK(db->CompactAll());
  for (int i = 0; i < 3000; i++) {
    CHECK(Get(db.get(), Key(i), snap) == "old");
    CHECK(Get(db.get(), Key(i)) == (i % 3 == 0 ? "NOT_FOUND" : "new"));
  }
  // Iterators honour the snapshot too.
  ReadOptions ro;
  ro.snapshot = snap;
  auto it = db->NewIterator(ro);
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    CHECK(it->value() == "old");
    n++;
  }
  CHECK(n == 3000);
  it.reset();
  db->ReleaseSnapshot(snap);
  CHECK_OK(db->CompactAll());
  CHECK(Get(db.get(), Key(1)) == "new");
}

TEST(IteratorRangeScan) {
  std::string dir = TempDir("iter");
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(SmallOptions(), dir, &db));
  for (int i = 0; i < 5000; i++) CHECK_OK(db->Put({}, Key(i), std::to_string(i)));
  for (int i = 100; i < 200; i++) CHECK_OK(db->Delete({}, Key(i)));
  db->WaitForIdle();
  auto it = db->NewIterator({});
  it->Seek(Key(95));
  std::vector<std::string> got;
  for (; it->Valid() && got.size() < 10; it->Next()) got.emplace_back(it->key());
  CHECK(got.size() == 10);
  CHECK(got[0] == Key(95));
  CHECK(got[4] == Key(99));
  CHECK(got[5] == Key(200));  // deleted range skipped
  CHECK_OK(it->status());
}

// The core correctness test: thousands of random puts/deletes/batches,
// interleaved with reopens, flushes and compactions, checked against an
// in-memory std::map model after every phase.
TEST(RandomizedModelCheck) {
  std::string dir = TempDir("model");
  std::map<std::string, std::string> model;
  std::mt19937 rng(42);
  auto rand_key = [&] { return Key(rng() % 3000); };
  auto verify = [&](DB* db) -> bool {
    auto it = db->NewIterator({});
    auto m = model.begin();
    for (it->SeekToFirst(); it->Valid(); it->Next(), ++m) {
      if (m == model.end() || it->key() != m->first || it->value() != m->second) return false;
    }
    if (m != model.end()) return false;
    for (int i = 0; i < 300; i++) {
      std::string k = rand_key();
      auto f = model.find(k);
      if (Get(db, k) != (f == model.end() ? "NOT_FOUND" : f->second)) return false;
    }
    return true;
  };

  for (int phase = 0; phase < 6; phase++) {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(SmallOptions(), dir, &db));
    CHECK(verify(db.get()));  // state survived the reopen
    for (int op = 0; op < 8000; op++) {
      int r = rng() % 100;
      if (r < 60) {
        std::string k = rand_key(), v = std::string(rng() % 80, 'a' + rng() % 26);
        CHECK_OK(db->Put({}, k, v));
        model[k] = v;
      } else if (r < 85) {
        std::string k = rand_key();
        CHECK_OK(db->Delete({}, k));
        model.erase(k);
      } else {
        WriteBatch b;
        for (int j = 0; j < 5; j++) {
          std::string k = rand_key();
          if (rng() % 2) {
            b.Put(k, "batch" + std::to_string(op));
            model[k] = "batch" + std::to_string(op);
          } else {
            b.Delete(k);
            model.erase(k);
          }
        }
        CHECK_OK(db->Write({}, &b));
      }
    }
    if (phase % 3 == 1) CHECK_OK(db->Flush());
    if (phase % 3 == 2) CHECK_OK(db->CompactAll());
    CHECK(verify(db.get()));
  }
}

TEST(ConcurrentReadersAndWriter) {
  std::string dir = TempDir("concurrent");
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(SmallOptions(), dir, &db));
  // Invariant: value of key i is always "v<i>"-prefixed; readers check it
  // while the writer churns memtables, flushes and compactions underneath.
  for (int i = 0; i < 2000; i++) CHECK_OK(db->Put({}, Key(i), "v" + std::to_string(i)));
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0}, reads{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; t++) {
    readers.emplace_back([&, t] {
      std::mt19937 rng(t);
      while (!stop) {
        int i = rng() % 2000;
        std::string v = Get(db.get(), Key(i));
        if (v.rfind("v" + std::to_string(i), 0) != 0) bad++;
        reads++;
      }
    });
  }
  for (int round = 0; round < 10; round++)
    for (int i = 0; i < 2000; i++)
      CHECK_OK(db->Put({}, Key(i), "v" + std::to_string(i) + "_r" + std::to_string(round)));
  stop = true;
  for (auto& t : readers) t.join();
  std::printf("    %d concurrent reads, %d inconsistent\n", reads.load(), bad.load());
  CHECK(bad == 0);
}

TEST(DetectsSSTableCorruption) {
  std::string dir = TempDir("corrupt");
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(Options(), dir, &db));
    for (int i = 0; i < 500; i++) CHECK_OK(db->Put({}, Key(i), std::string(100, 'z')));
    CHECK_OK(db->Flush());
  }
  // Flip a byte inside the first data block of the table.
  std::vector<std::string> names;
  CHECK_OK(ListDir(dir, &names));
  std::string sst;
  for (auto& n : names)
    if (n.size() > 4 && n.substr(n.size() - 4) == ".sst") sst = dir + "/" + n;
  CHECK(!sst.empty());
  FILE* f = std::fopen(sst.c_str(), "r+b");
  std::fseek(f, 100, SEEK_SET);
  int c = std::fgetc(f);
  std::fseek(f, 100, SEEK_SET);
  std::fputc(c ^ 0xff, f);
  std::fclose(f);
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(Options(), dir, &db));
  std::string v;
  Status s = db->Get({}, Key(0), &v);
  CHECK(s.IsCorruption());
}

TEST(SecondOpenIsLocked) {
  std::string dir = TempDir("lock");
  std::unique_ptr<DB> a, b;
  CHECK_OK(DB::Open(Options(), dir, &a));
  Status s = DB::Open(Options(), dir, &b);
  CHECK(!s.ok());
}

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (auto& [name, fn] : Registry()) {
    if (filter && std::string(name).find(filter) == std::string::npos) continue;
    int before = g_failures;
    std::printf("[ RUN  ] %s\n", name);
    std::fflush(stdout);
    fn();
    std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", name);
    run++;
  }
  std::printf("\n%d tests, %d failures\n", run, g_failures);
  return g_failures == 0 ? 0 : 1;
}
