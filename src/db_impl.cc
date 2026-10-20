// DBImpl: the write path, read path, background flush/compaction thread,
// manifest persistence and crash recovery.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "cache.h"
#include "coding.h"
#include "dbformat.h"
#include "env.h"
#include "iterators.h"
#include "lsmkv/db.h"
#include "memtable.h"
#include "table.h"
#include "version.h"
#include "wal.h"

namespace lsmkv {

class Snapshot {
 public:
  explicit Snapshot(SequenceNumber s) : seq(s) {}
  const SequenceNumber seq;
};

namespace {

constexpr uint64_t kManifestMagic = 0x4c534d4b564d4631ull;  // "LSMKVMF1"

std::string FileName(const std::string& db, uint64_t n, const char* ext) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s", static_cast<unsigned long long>(n), ext);
  return db + buf;
}

// Parse "000123.sst" -> (123, "sst").
bool ParseFileName(const std::string& name, uint64_t* number, std::string* ext) {
  auto dot = name.find('.');
  if (dot == std::string::npos || dot == 0) return false;
  for (size_t i = 0; i < dot; i++)
    if (name[i] < '0' || name[i] > '9') return false;
  *number = std::stoull(name.substr(0, dot));
  *ext = name.substr(dot + 1);
  return true;
}

// User-facing iterator: hides internal keys, older versions, tombstones and
// anything newer than the snapshot.
class DBIter : public Iterator {
 public:
  DBIter(std::unique_ptr<Iterator> internal, SequenceNumber snap,
         std::shared_ptr<const Version> version)
      : iter_(std::move(internal)), snap_(snap), version_(std::move(version)) {}

  bool Valid() const override { return valid_; }
  void SeekToFirst() override {
    iter_->SeekToFirst();
    FindNextUserEntry(false);
  }
  void Seek(std::string_view target) override {
    iter_->Seek(MakeLookupKey(target, snap_));
    FindNextUserEntry(false);
  }
  void Next() override {
    skip_ = key_;
    iter_->Next();
    FindNextUserEntry(true);
  }
  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }
  Status status() const override { return iter_->status(); }

 private:
  void FindNextUserEntry(bool skipping) {
    while (iter_->Valid()) {
      ParsedInternalKey p;
      if (ParseInternalKey(iter_->key(), &p) && p.sequence <= snap_) {
        if (skipping && p.user_key == skip_) {
          // Older version of a key we already emitted or saw deleted.
        } else if (p.type == kTypeDeletion) {
          skip_.assign(p.user_key);
          skipping = true;
        } else {
          key_.assign(p.user_key);
          value_.assign(iter_->value());
          valid_ = true;
          return;
        }
      }
      iter_->Next();
    }
    valid_ = false;
  }

  std::unique_ptr<Iterator> iter_;
  SequenceNumber snap_;
  std::shared_ptr<const Version> version_;  // pins table files
  std::string key_, value_, skip_;
  bool valid_ = false;
};

}  // namespace

class DBImpl : public DB {
 public:
  DBImpl(const Options& o, std::string name)
      : opts_(o), dbname_(std::move(name)), cache_(new BlockCache(o.block_cache_bytes)) {}

  ~DBImpl() override {
    {
      std::lock_guard<std::mutex> l(mu_);
      shutting_down_ = true;
    }
    work_cv_.notify_all();
    if (bg_thread_.joinable()) bg_thread_.join();
    if (log_) log_->Close();
  }

  Status Recover();

  Status Put(const WriteOptions& o, std::string_view k, std::string_view v) override {
    WriteBatch b;
    b.Put(k, v);
    return Write(o, &b);
  }

  Status Delete(const WriteOptions& o, std::string_view k) override {
    WriteBatch b;
    b.Delete(k);
    return Write(o, &b);
  }

  Status Write(const WriteOptions& o, WriteBatch* batch) override;
  Status Get(const ReadOptions& o, std::string_view key, std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& o) override;

  const Snapshot* GetSnapshot() override {
    std::lock_guard<std::mutex> l(mu_);
    snapshots_.insert(last_sequence_);
    return new Snapshot(last_sequence_);
  }

  void ReleaseSnapshot(const Snapshot* s) override {
    {
      std::lock_guard<std::mutex> l(mu_);
      snapshots_.erase(snapshots_.find(s->seq));
    }
    delete s;
  }

  Status Flush() override;
  Status CompactAll() override;
  void WaitForIdle() override;
  DBStats Stats() override;
  std::string DebugString() override;

 private:
  struct CompactionJob {
    std::vector<FilePtr> inputs;
    int output_level = 1;
    bool trivial_move = false;
  };

  Status MakeRoomForWrite(std::unique_lock<std::mutex>& l, bool force);
  void BackgroundThread();
  bool NeedsCompaction() const;
  Status FlushImmutable(std::unique_lock<std::mutex>& l);
  bool PickCompaction(CompactionJob* job);
  Status RunCompaction(std::unique_lock<std::mutex>& l, CompactionJob job);
  Status BuildTable(Iterator* it, uint64_t number, FilePtr* out);
  FilePtr MakeFileMeta(uint64_t number);
  Status InstallVersion(std::shared_ptr<Version> v);
  std::string EncodeManifest(const Version& v) const;
  Status DecodeManifest(std::string_view data, Version* v);
  void DeleteObsoleteFiles();
  bool IsBaseLevelForKey(const Version& v, int output_level, std::string_view user_key) const;
  uint64_t MaxBytesForLevel(int level) const {
    uint64_t b = opts_.level1_max_bytes;
    for (int i = 1; i < level; i++) b *= 10;
    return b;
  }
  uint64_t NewFileNumber() {
    std::lock_guard<std::mutex> l(mu_);
    return next_file_number_++;
  }

  const Options opts_;
  const std::string dbname_;
  std::unique_ptr<FileLock> lock_;
  std::unique_ptr<BlockCache> cache_;

  std::mutex mu_;
  std::condition_variable work_cv_;  // wakes the background thread
  std::condition_variable bg_cv_;    // signals background progress
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;
  std::shared_ptr<const Version> current_;
  std::unique_ptr<LogWriter> log_;
  uint64_t log_number_ = 0;
  uint64_t next_file_number_ = 1;
  SequenceNumber last_sequence_ = 0;
  std::multiset<SequenceNumber> snapshots_;
  std::string compact_pointer_[kNumLevels];
  bool shutting_down_ = false;
  bool bg_busy_ = false;
  bool manual_compaction_ = false;
  Status bg_error_;
  std::thread bg_thread_;

  std::atomic<uint64_t> user_bytes_{0}, wal_bytes_{0}, sst_bytes_{0}, flushes_{0},
      compactions_{0}, trivial_moves_{0}, bloom_checks_{0}, bloom_negatives_{0}, stalls_{0};
};

// ============================ Write path ============================

Status DBImpl::Write(const WriteOptions& o, WriteBatch* batch) {
  std::unique_lock<std::mutex> l(mu_);
  Status s = MakeRoomForWrite(l, false);
  if (!s.ok()) return s;
  SequenceNumber seq = last_sequence_ + 1;
  WriteBatchInternal::SetSequence(batch, seq);
  const std::string& rep = WriteBatchInternal::Contents(batch);
  // 1. Durability first: the batch is in the WAL before it is visible.
  s = log_->AddRecord(rep, o.sync);
  if (!s.ok()) return s;
  wal_bytes_ += rep.size() + 8;
  user_bytes_ += rep.size() - 12;
  // 2. Apply to the memtable. Readers are lock-free on the skiplist but
  //    take mu_ to read last_sequence_, so they never see a half-applied batch.
  s = WriteBatchInternal::InsertInto(batch, mem_.get());
  last_sequence_ = seq + batch->Count() - 1;
  return s;
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& l, bool force) {
  while (true) {
    if (!bg_error_.ok()) return bg_error_;
    if (!force && mem_->ApproximateMemoryUsage() < opts_.write_buffer_size) return Status::OK();
    if (force && mem_->NumEntries() == 0) return Status::OK();
    if (imm_) {
      // Previous memtable still flushing: backpressure.
      stalls_++;
      bg_cv_.wait(l);
      continue;
    }
    if (static_cast<int>(current_->files[0].size()) >= opts_.l0_stop_writes_trigger) {
      // Too many overlapping L0 files make reads slow: let compaction catch up.
      stalls_++;
      work_cv_.notify_one();
      bg_cv_.wait(l);
      continue;
    }
    // Rotate: freeze the memtable and start a fresh WAL for the new one.
    uint64_t new_log = next_file_number_++;
    std::unique_ptr<WritableFile> f;
    Status s = WritableFile::Open(FileName(dbname_, new_log, "log"), true, &f);
    if (!s.ok()) return s;
    log_->Close();
    log_.reset(new LogWriter(std::move(f)));
    log_number_ = new_log;
    imm_ = std::move(mem_);
    mem_ = std::make_shared<MemTable>();
    force = false;
    work_cv_.notify_one();
  }
}

// ============================ Read path ============================

Status DBImpl::Get(const ReadOptions& o, std::string_view key, std::string* value) {
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<const Version> v;
  SequenceNumber snap;
  {
    std::lock_guard<std::mutex> l(mu_);
    mem = mem_;
    imm = imm_;
    v = current_;
    snap = o.snapshot ? o.snapshot->seq : last_sequence_;
  }
  bool deleted = false;
  if (mem->Get(key, snap, value, &deleted) || (imm && imm->Get(key, snap, value, &deleted)))
    return deleted ? Status::NotFound() : Status::OK();

  std::string lookup = MakeLookupKey(key, snap);
  auto probe = [&](const FilePtr& f, bool* done) -> Status {
    bool found = false, bloom_neg = false;
    std::string ikey;
    bloom_checks_++;
    Status s = f->table->Get(lookup, &found, &ikey, value, &bloom_neg);
    if (bloom_neg) bloom_negatives_++;
    if (!s.ok()) {
      *done = true;
      return s;
    }
    if (found) {
      *done = true;
      return ExtractTag(ikey) & 0xff ? Status::OK() : Status::NotFound();
    }
    return Status::OK();
  };

  // L0: files overlap, search newest to oldest.
  for (const auto& f : v->files[0]) {
    if (key < f->smallest_user() || key > f->largest_user()) continue;
    bool done = false;
    Status s = probe(f, &done);
    if (done) return s;
  }
  // L1+: at most one candidate file per level (binary search).
  for (int level = 1; level < kNumLevels; level++) {
    const auto& files = v->files[level];
    auto it = std::lower_bound(files.begin(), files.end(), key,
                               [](const FilePtr& f, std::string_view k) {
                                 return f->largest_user() < k;
                               });
    if (it == files.end() || key < (*it)->smallest_user()) continue;
    bool done = false;
    Status s = probe(*it, &done);
    if (done) return s;
  }
  return Status::NotFound();
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& o) {
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<const Version> v;
  SequenceNumber snap;
  {
    std::lock_guard<std::mutex> l(mu_);
    mem = mem_;
    imm = imm_;
    v = current_;
    snap = o.snapshot ? o.snapshot->seq : last_sequence_;
  }
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(NewMemTableIterator(mem));
  if (imm) children.push_back(NewMemTableIterator(imm));
  for (const auto& f : v->files[0]) children.push_back(f->table->NewIterator(f->table));
  for (int level = 1; level < kNumLevels; level++)
    if (!v->files[level].empty()) children.push_back(NewLevelIterator(v->files[level]));
  return std::make_unique<DBIter>(NewMergingIterator(std::move(children)), snap, v);
}

// ======================= Background work =======================

bool DBImpl::NeedsCompaction() const {
  if (static_cast<int>(current_->files[0].size()) >= opts_.l0_compaction_trigger) return true;
  for (int level = 1; level < kNumLevels - 1; level++)
    if (current_->LevelBytes(level) > MaxBytesForLevel(level)) return true;
  return false;
}

void DBImpl::BackgroundThread() {
  std::unique_lock<std::mutex> l(mu_);
  while (true) {
    work_cv_.wait(l, [&] {
      return shutting_down_ ||
             (bg_error_.ok() && (imm_ || manual_compaction_ || NeedsCompaction()));
    });
    if (shutting_down_) break;
    bg_busy_ = true;
    Status s;
    if (imm_) {
      s = FlushImmutable(l);  // flushes always take priority: writers may be stalled
    } else if (manual_compaction_) {
      CompactionJob job;
      int deepest = 1;
      for (int level = 0; level < kNumLevels; level++) {
        for (auto& f : current_->files[level]) job.inputs.push_back(f);
        if (!current_->files[level].empty()) deepest = std::max(deepest, level);
      }
      job.output_level = deepest;
      if (!job.inputs.empty()) s = RunCompaction(l, std::move(job));
      manual_compaction_ = false;
    } else {
      CompactionJob job;
      if (PickCompaction(&job)) s = RunCompaction(l, std::move(job));
    }
    if (!s.ok() && bg_error_.ok()) bg_error_ = s;
    bg_busy_ = false;
    bg_cv_.notify_all();
  }
}

Status DBImpl::FlushImmutable(std::unique_lock<std::mutex>& l) {
  std::shared_ptr<MemTable> imm = imm_;
  uint64_t number = next_file_number_++;
  l.unlock();
  FilePtr meta;
  auto it = NewMemTableIterator(imm);
  Status s = BuildTable(it.get(), number, &meta);
  l.lock();
  if (!s.ok()) return s;
  auto v = std::make_shared<Version>(*current_);
  if (meta) v->files[0].insert(v->files[0].begin(), meta);  // newest first
  // The new manifest records log_number_ (the WAL of the *active* memtable),
  // so the WAL that backed `imm` is no longer needed for recovery.
  s = InstallVersion(v);
  if (!s.ok()) return s;
  imm_.reset();
  flushes_++;
  DeleteObsoleteFiles();
  return Status::OK();
}

bool DBImpl::PickCompaction(CompactionJob* job) {
  const Version& v = *current_;
  int best_level = -1;
  double best_score = 1.0;
  double s0 = double(v.files[0].size()) / opts_.l0_compaction_trigger;
  if (s0 >= best_score) {
    best_score = s0;
    best_level = 0;
  }
  for (int level = 1; level < kNumLevels - 1; level++) {
    double s = double(v.LevelBytes(level)) / double(MaxBytesForLevel(level));
    if (s > best_score) {
      best_score = s;
      best_level = level;
    }
  }
  if (best_level < 0) return false;

  std::vector<FilePtr> level_inputs;
  if (best_level == 0) {
    level_inputs = v.files[0];
  } else {
    // Round-robin through the key space so every range eventually compacts.
    const auto& files = v.files[best_level];
    const std::string& ptr = compact_pointer_[best_level];
    auto it = std::find_if(files.begin(), files.end(), [&](const FilePtr& f) {
      return ptr.empty() || f->largest_user() > ptr;
    });
    if (it == files.end()) it = files.begin();
    level_inputs.push_back(*it);
  }

  std::string lo(level_inputs[0]->smallest_user()), hi(level_inputs[0]->largest_user());
  for (auto& f : level_inputs) {
    if (f->smallest_user() < lo) lo = std::string(f->smallest_user());
    if (f->largest_user() > hi) hi = std::string(f->largest_user());
  }
  compact_pointer_[best_level] = hi;

  std::vector<FilePtr> next_inputs;
  for (auto& f : v.files[best_level + 1])
    if (!(f->largest_user() < lo || f->smallest_user() > hi)) next_inputs.push_back(f);

  job->output_level = best_level + 1;
  job->trivial_move = best_level > 0 && level_inputs.size() == 1 && next_inputs.empty();
  job->inputs = std::move(level_inputs);
  job->inputs.insert(job->inputs.end(), next_inputs.begin(), next_inputs.end());
  return true;
}

bool DBImpl::IsBaseLevelForKey(const Version& v, int output_level,
                               std::string_view user_key) const {
  for (int level = output_level + 1; level < kNumLevels; level++)
    for (auto& f : v.files[level])
      if (user_key >= f->smallest_user() && user_key <= f->largest_user()) return false;
  return true;
}

Status DBImpl::RunCompaction(std::unique_lock<std::mutex>& l, CompactionJob job) {
  std::set<uint64_t> input_numbers;
  for (auto& f : job.inputs) input_numbers.insert(f->number);

  auto install = [&](const std::vector<FilePtr>& outputs) {
    auto v = std::make_shared<Version>();
    for (int level = 0; level < kNumLevels; level++)
      for (auto& f : current_->files[level])
        if (!input_numbers.count(f->number)) v->files[level].push_back(f);
    auto& out = v->files[job.output_level];
    out.insert(out.end(), outputs.begin(), outputs.end());
    std::sort(out.begin(), out.end(), [](const FilePtr& a, const FilePtr& b) {
      return CompareInternalKeys(a->smallest, b->smallest) < 0;
    });
    return InstallVersion(v);
  };

  if (job.trivial_move) {
    // Nothing to merge with below: re-link the file one level down, no I/O.
    trivial_moves_++;
    compactions_++;
    return install({job.inputs[0]});
  }

  SequenceNumber smallest_snapshot = snapshots_.empty() ? last_sequence_ : *snapshots_.begin();
  std::shared_ptr<const Version> base = current_;
  l.unlock();

  std::vector<std::unique_ptr<Iterator>> children;
  for (auto& f : job.inputs) children.push_back(f->table->NewIterator(f->table));
  auto it = NewMergingIterator(std::move(children));

  std::vector<FilePtr> outputs;
  std::unique_ptr<TableBuilder> builder;
  uint64_t builder_number = 0;
  Status s;

  auto finish_output = [&]() -> Status {
    Status fs = builder->Finish();
    if (!fs.ok()) return fs;
    sst_bytes_ += builder->FileSize();
    FilePtr meta = MakeFileMeta(builder_number);
    meta->file_size = builder->FileSize();
    meta->smallest = builder->smallest();
    meta->largest = builder->largest();
    fs = TableReader::Open(meta->path, meta->number, cache_.get(), &meta->table);
    if (!fs.ok()) return fs;
    outputs.push_back(meta);
    builder.reset();
    return Status::OK();
  };

  std::string current_user_key;
  bool has_current = false;
  SequenceNumber last_seq_for_key = kMaxSequenceNumber;
  std::string last_output_user_key;

  for (it->SeekToFirst(); it->Valid() && s.ok(); it->Next()) {
    ParsedInternalKey p;
    if (!ParseInternalKey(it->key(), &p)) {
      s = Status::Corruption("bad internal key during compaction");
      break;
    }
    if (!has_current || p.user_key != current_user_key) {
      current_user_key.assign(p.user_key);
      has_current = true;
      last_seq_for_key = kMaxSequenceNumber;
    }
    bool drop = false;
    if (last_seq_for_key <= smallest_snapshot) {
      // A newer version of this key is already visible to every snapshot.
      drop = true;
    } else if (p.type == kTypeDeletion && p.sequence <= smallest_snapshot &&
               IsBaseLevelForKey(*base, job.output_level, p.user_key)) {
      // Tombstone with nothing underneath it left to shadow.
      drop = true;
    }
    last_seq_for_key = p.sequence;
    if (drop) continue;

    // Cut output files only between user keys, so levels >= 1 never have
    // one user key spanning two files.
    if (builder && builder->EstimatedSize() >= opts_.target_file_size &&
        p.user_key != last_output_user_key) {
      s = finish_output();
      if (!s.ok()) break;
    }
    if (!builder) {
      builder_number = NewFileNumber();
      std::unique_ptr<WritableFile> f;
      s = WritableFile::Open(FileName(dbname_, builder_number, "sst"), true, &f);
      if (!s.ok()) break;
      builder.reset(new TableBuilder(std::move(f), opts_.block_size, opts_.bloom_bits_per_key));
    }
    s = builder->Add(it->key(), it->value());
    last_output_user_key.assign(p.user_key);
  }
  if (s.ok()) s = it->status();
  if (s.ok() && builder) s = finish_output();
  it.reset();

  l.lock();
  if (!s.ok()) return s;  // orphan outputs are removed on next open
  compactions_++;
  s = install(outputs);
  if (s.ok()) DeleteObsoleteFiles();
  return s;
}

FilePtr DBImpl::MakeFileMeta(uint64_t number) {
  auto meta = std::make_shared<FileMetaData>();
  meta->number = number;
  meta->path = FileName(dbname_, number, "sst");
  meta->cache = cache_.get();
  return meta;
}

Status DBImpl::BuildTable(Iterator* it, uint64_t number, FilePtr* out) {
  out->reset();
  std::string path = FileName(dbname_, number, "sst");
  std::unique_ptr<WritableFile> f;
  Status s = WritableFile::Open(path, true, &f);
  if (!s.ok()) return s;
  TableBuilder b(std::move(f), opts_.block_size, opts_.bloom_bits_per_key);
  for (it->SeekToFirst(); it->Valid() && s.ok(); it->Next()) s = b.Add(it->key(), it->value());
  if (!s.ok()) return s;
  if (b.NumEntries() == 0) {
    b.Finish();
    RemoveFile(path);
    return Status::OK();
  }
  s = b.Finish();
  if (!s.ok()) return s;
  sst_bytes_ += b.FileSize();
  FilePtr meta = MakeFileMeta(number);
  meta->file_size = b.FileSize();
  meta->smallest = b.smallest();
  meta->largest = b.largest();
  s = TableReader::Open(path, number, cache_.get(), &meta->table);
  if (!s.ok()) return s;
  *out = meta;
  return Status::OK();
}

// ======================= Manifest =======================
//
// The manifest is a full description of the current Version plus counters,
// rewritten atomically (temp file + fsync + rename) on every change. Simpler
// than LevelDB's append-only edit log at the cost of O(#files) bytes per
// install, which is negligible at this scale.

std::string DBImpl::EncodeManifest(const Version& v) const {
  std::string body;
  PutVarint64(&body, next_file_number_);
  PutFixed64(&body, last_sequence_);
  PutVarint64(&body, log_number_);
  for (int level = 0; level < kNumLevels; level++) {
    PutVarint32(&body, static_cast<uint32_t>(v.files[level].size()));
    for (auto& f : v.files[level]) {
      PutVarint64(&body, f->number);
      PutVarint64(&body, f->file_size);
      PutLengthPrefixed(&body, f->smallest);
      PutLengthPrefixed(&body, f->largest);
    }
  }
  std::string out;
  PutFixed64(&out, kManifestMagic);
  PutFixed32(&out, Crc32c(body));
  out += body;
  return out;
}

Status DBImpl::DecodeManifest(std::string_view in, Version* v) {
  if (in.size() < 12 || DecodeFixed64(in.data()) != kManifestMagic)
    return Status::Corruption("bad manifest header");
  uint32_t crc = DecodeFixed32(in.data() + 8);
  in.remove_prefix(12);
  if (Crc32c(in) != crc) return Status::Corruption("manifest checksum mismatch");
  uint64_t log_number;
  if (!GetVarint64(&in, &next_file_number_) || !GetFixed64(&in, &last_sequence_) ||
      !GetVarint64(&in, &log_number))
    return Status::Corruption("bad manifest counters");
  log_number_ = log_number;
  for (int level = 0; level < kNumLevels; level++) {
    uint32_t n;
    if (!GetVarint32(&in, &n)) return Status::Corruption("bad manifest level");
    for (uint32_t i = 0; i < n; i++) {
      uint64_t number, size;
      std::string_view smallest, largest;
      if (!GetVarint64(&in, &number) || !GetVarint64(&in, &size) ||
          !GetLengthPrefixed(&in, &smallest) || !GetLengthPrefixed(&in, &largest))
        return Status::Corruption("bad manifest file entry");
      FilePtr meta = MakeFileMeta(number);
      meta->file_size = size;
      meta->smallest.assign(smallest);
      meta->largest.assign(largest);
      Status s = TableReader::Open(meta->path, number, cache_.get(), &meta->table);
      if (!s.ok()) return s;
      v->files[level].push_back(meta);
    }
  }
  return Status::OK();
}

Status DBImpl::InstallVersion(std::shared_ptr<Version> v) {
  Status s = WriteFileAtomic(dbname_ + "/MANIFEST", EncodeManifest(*v));
  if (!s.ok()) return s;
  std::set<uint64_t> live;
  for (int level = 0; level < kNumLevels; level++)
    for (auto& f : v->files[level]) live.insert(f->number);
  if (current_) {
    for (int level = 0; level < kNumLevels; level++)
      for (auto& f : current_->files[level])
        if (!live.count(f->number)) f->obsolete = true;  // unlinked when last ref drops
  }
  current_ = std::move(v);
  return Status::OK();
}

void DBImpl::DeleteObsoleteFiles() {
  // Table files are reclaimed by FileMetaData's destructor; here we only
  // remove WAL files older than the one backing the active memtable.
  std::vector<std::string> names;
  if (!ListDir(dbname_, &names).ok()) return;
  for (auto& n : names) {
    uint64_t number;
    std::string ext;
    if (ParseFileName(n, &number, &ext) && ext == "log" && number < log_number_)
      RemoveFile(dbname_ + "/" + n);
  }
}

// ======================= Recovery =======================

Status DBImpl::Recover() {
  Status s = CreateDirIfMissing(dbname_);
  if (!s.ok()) return s;
  s = FileLock::Acquire(dbname_ + "/LOCK", &lock_);
  if (!s.ok()) return s;

  auto v = std::make_shared<Version>();
  std::string manifest;
  if (FileExists(dbname_ + "/MANIFEST")) {
    s = ReadFileToString(dbname_ + "/MANIFEST", &manifest);
    if (s.ok()) s = DecodeManifest(manifest, v.get());
    if (!s.ok()) return s;
  } else if (!opts_.create_if_missing) {
    return Status::InvalidArgument(dbname_ + " does not exist");
  }

  std::set<uint64_t> live;
  for (int level = 0; level < kNumLevels; level++)
    for (auto& f : v->files[level]) live.insert(f->number);

  // Replay every WAL not yet covered by an SSTable, oldest first.
  std::vector<std::string> names;
  s = ListDir(dbname_, &names);
  if (!s.ok()) return s;
  std::vector<uint64_t> logs;
  for (auto& n : names) {
    uint64_t number;
    std::string ext;
    if (!ParseFileName(n, &number, &ext)) continue;
    next_file_number_ = std::max(next_file_number_, number + 1);
    if (ext == "log" && number >= log_number_) logs.push_back(number);
    if (ext == "sst" && !live.count(number)) RemoveFile(dbname_ + "/" + n);  // crashed compaction
  }
  std::sort(logs.begin(), logs.end());

  auto mem = std::make_shared<MemTable>();
  for (uint64_t number : logs) {
    std::string contents;
    s = ReadFileToString(FileName(dbname_, number, "log"), &contents);
    if (!s.ok()) return s;
    LogReader reader(std::move(contents));
    std::string_view record;
    WriteBatch batch;
    while (reader.ReadRecord(&record)) {
      s = WriteBatchInternal::SetContents(&batch, record);
      if (s.ok()) s = WriteBatchInternal::InsertInto(&batch, mem.get());
      if (!s.ok()) return s;
      last_sequence_ = std::max(last_sequence_,
                                WriteBatchInternal::Sequence(&batch) + batch.Count() - 1);
    }
    // A torn tail is expected after a crash: the un-acked write is dropped.
  }

  current_ = v;
  if (mem->NumEntries() > 0) {
    FilePtr meta;
    auto it = NewMemTableIterator(mem);
    s = BuildTable(it.get(), next_file_number_++, &meta);
    if (!s.ok()) return s;
    auto nv = std::make_shared<Version>(*v);
    if (meta) nv->files[0].insert(nv->files[0].begin(), meta);
    current_ = nv;
  }

  log_number_ = next_file_number_++;
  std::unique_ptr<WritableFile> f;
  s = WritableFile::Open(FileName(dbname_, log_number_, "log"), true, &f);
  if (!s.ok()) return s;
  log_.reset(new LogWriter(std::move(f)));
  mem_ = std::make_shared<MemTable>();

  auto snapshot = current_;
  current_ = nullptr;
  s = InstallVersion(std::make_shared<Version>(*snapshot));
  if (!s.ok()) return s;
  DeleteObsoleteFiles();

  bg_thread_ = std::thread([this] { BackgroundThread(); });
  return Status::OK();
}

// ======================= Admin =======================

Status DBImpl::Flush() {
  std::unique_lock<std::mutex> l(mu_);
  Status s = MakeRoomForWrite(l, true);
  if (!s.ok()) return s;
  bg_cv_.wait(l, [&] { return !imm_ || !bg_error_.ok(); });
  return bg_error_;
}

Status DBImpl::CompactAll() {
  Status s = Flush();
  if (!s.ok()) return s;
  std::unique_lock<std::mutex> l(mu_);
  manual_compaction_ = true;
  work_cv_.notify_one();
  bg_cv_.wait(l, [&] { return !manual_compaction_ || !bg_error_.ok(); });
  return bg_error_;
}

void DBImpl::WaitForIdle() {
  std::unique_lock<std::mutex> l(mu_);
  work_cv_.notify_one();
  bg_cv_.wait(l, [&] {
    return !bg_error_.ok() || (!imm_ && !manual_compaction_ && !bg_busy_ && !NeedsCompaction());
  });
}

DBStats DBImpl::Stats() {
  DBStats st;
  std::shared_ptr<const Version> v;
  {
    std::lock_guard<std::mutex> l(mu_);
    v = current_;
  }
  st.user_bytes_written = user_bytes_;
  st.wal_bytes_written = wal_bytes_;
  st.sst_bytes_written = sst_bytes_;
  st.flushes = flushes_;
  st.compactions = compactions_;
  st.trivial_moves = trivial_moves_;
  st.bloom_checks = bloom_checks_;
  st.bloom_negatives = bloom_negatives_;
  st.block_cache_hits = cache_->hits();
  st.block_cache_misses = cache_->misses();
  st.write_stalls = stalls_;
  for (int level = 0; level < kNumLevels; level++) {
    st.files_per_level.push_back(static_cast<int>(v->files[level].size()));
    st.bytes_per_level.push_back(v->LevelBytes(level));
  }
  return st;
}

std::string DBImpl::DebugString() {
  DBStats st = Stats();
  std::ostringstream os;
  os << "level  files  size(KB)\n";
  for (int level = 0; level < kNumLevels; level++)
    if (st.files_per_level[level] > 0)
      os << "  L" << level << "  " << st.files_per_level[level] << "  "
         << st.bytes_per_level[level] / 1024 << "\n";
  os << "flushes=" << st.flushes << " compactions=" << st.compactions
     << " trivial_moves=" << st.trivial_moves << " stalls=" << st.write_stalls << "\n";
  os << "bloom: " << st.bloom_negatives << "/" << st.bloom_checks << " table probes skipped\n";
  os << "block cache: " << st.block_cache_hits << " hits, " << st.block_cache_misses
     << " misses\n";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "write amplification: %.2fx\n", st.WriteAmplification());
  os << buf;
  return os.str();
}

Status DB::Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db) {
  auto impl = std::make_unique<DBImpl>(options, path);
  Status s = impl->Recover();
  if (!s.ok()) return s;
  *db = std::move(impl);
  return Status::OK();
}

}  // namespace lsmkv
