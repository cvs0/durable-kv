#include "durable_kv/store.hpp"

#include "format.hpp"
#include "os_file.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace durable_kv {
namespace {

// Test hooks. They do nothing unless the environment variable is set.
// DURABLE_KV_CRASH_AT=name or name:N calls _Exit on the Nth hit (default 1).
// DURABLE_KV_INJECT=write|write_partial|sync|compact_write fails that step once.
// DURABLE_KV_MAX_WRITE is applied in File::write_exact.

struct CrashCtl {
  bool loaded = false;
  std::string point;
  int target = 0;
  int seen = 0;

  void ensure() {
    if (loaded) {
      return;
    }
    loaded = true;
    const char* env = std::getenv("DURABLE_KV_CRASH_AT");
    if (env == nullptr || env[0] == '\0') {
      return;
    }
    const std::string spec(env);
    const auto colon = spec.rfind(':');
    if (colon != std::string::npos && colon + 1 < spec.size()) {
      bool digits = true;
      for (size_t i = colon + 1; i < spec.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(spec[i])) == 0) {
          digits = false;
          break;
        }
      }
      if (digits) {
        point = spec.substr(0, colon);
        target = std::atoi(spec.c_str() + colon + 1);
        return;
      }
    }
    point = spec;
    target = 1;
  }

  bool armed(const char* name) {
    ensure();
    if (point != name || target <= 0) {
      return false;
    }
    ++seen;
    return seen == target;
  }
};

CrashCtl& crash_ctl() {
  static CrashCtl ctl;
  return ctl;
}

[[noreturn]] void crash_exit() { std::_Exit(97); }

bool take_inject(const char* name) {
  const char* env = std::getenv("DURABLE_KV_INJECT");
  if (env == nullptr || std::strcmp(env, name) != 0) {
    return false;
  }
#ifdef _WIN32
  _putenv_s("DURABLE_KV_INJECT", "");
#else
  unsetenv("DURABLE_KV_INJECT");
#endif
  return true;
}

using Index = std::map<std::string, std::string, std::less<>>;

struct Scan {
  Status status = Status::Ok;
  uint64_t valid_end = 0;
  bool discarded_tail = false;
  bool ends_with_seal = false;
  bool contains_seal = false;
  std::string error;
};

Scan scan_bytes(const uint8_t* data, size_t size, Index* index) {
  Scan scan;
  uint64_t offset = 0;
  while (offset < size) {
    format::Record rec;
    std::string rec_error;
    const format::Frame frame = format::read_record(data, size, offset, rec, rec_error);
    if (frame == format::Frame::Truncated) {
      scan.discarded_tail = true;
      scan.valid_end = offset;
      return scan;
    }
    if (frame == format::Frame::BadChecksum) {
      // A bad checksum on the last record is dropped.
      // A bad checksum with more bytes after it means the log is corrupt.
      if (rec.next == size) {
        scan.discarded_tail = true;
        scan.valid_end = offset;
        return scan;
      }
      scan.status = Status::Corruption;
      scan.error = "checksum mismatch at offset " + std::to_string(offset) +
                   " with more bytes after the record";
      return scan;
    }
    if (frame != format::Frame::Ok) {
      scan.status = Status::Corruption;
      scan.error = rec_error + " at offset " + std::to_string(offset);
      return scan;
    }

    if (rec.op == format::kOpPut) {
      scan.ends_with_seal = false;
      if (index != nullptr) {
        index->insert_or_assign(std::move(rec.key), std::move(rec.value));
      }
    } else if (rec.op == format::kOpErase) {
      scan.ends_with_seal = false;
      if (index != nullptr) {
        index->erase(rec.key);
      }
    } else {
      scan.contains_seal = true;
      scan.ends_with_seal = true;
    }
    offset = rec.next;
  }
  scan.valid_end = offset;
  return scan;
}

bool load_file(File& file, std::vector<uint8_t>& bytes, std::string& error) {
  uint64_t size = 0;
  if (!file.file_size(size, error)) {
    return false;
  }
  if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    error = "log is too large to recover";
    return false;
  }
  bytes.resize(static_cast<size_t>(size));
  if (size == 0) {
    return true;
  }
  return file.read_exact(0, bytes.data(), bytes.size(), error);
}

std::filesystem::path with_suffix(const std::filesystem::path& path, const char* suffix) {
  auto out = path;
  out.concat(suffix);
  return out;
}

enum class SnapshotKind { Complete, Incomplete, Unreadable };

SnapshotKind inspect_snapshot(const std::filesystem::path& path, std::string& error) {
  File file;
  if (!file.open_existing(path, error)) {
    return SnapshotKind::Unreadable;
  }
  std::vector<uint8_t> bytes;
  if (!load_file(file, bytes, error)) {
    return SnapshotKind::Unreadable;
  }
  const Scan scan = scan_bytes(bytes.data(), bytes.size(), nullptr);
  if (scan.status != Status::Ok || !scan.ends_with_seal) {
    return SnapshotKind::Incomplete;
  }
  return SnapshotKind::Complete;
}

// Compaction takes more than one rename. open() picks one complete log from
// whatever files are present:
//
//   current  old  compact     what open does
//   no       yes  sealed      rename compact onto current
//   no       yes  other       delete the unfinished compact file, rename old onto current
//   no       no   sealed      rename compact onto current
//   no       no   unfinished  return an error and leave the compact file
//   no       no   unreadable  return an error and leave the compact file
//   yes      *    *           keep current; drop a leftover compact file after a good scan
//
// A zero-length current next to old or compact is removed first.
// A finished snapshot ends with a seal. An empty file has no seal.
Status recover_names(const std::filesystem::path& current, std::string& error) {
  const auto compact_path = with_suffix(current, ".compact");
  const auto old_path = with_suffix(current, ".old");
  bool has_current = regular_file_exists(current);
  const bool has_compact = regular_file_exists(compact_path);
  const bool has_old = regular_file_exists(old_path);

  if (has_current && (has_old || has_compact)) {
    uint64_t current_size = 0;
    if (!read_file_size(current, current_size, error)) {
      return Status::IoError;
    }
    if (current_size == 0) {
      if (!remove_file(current, error)) {
        return Status::IoError;
      }
      has_current = false;
    }
  }

  if (has_current || (!has_old && !has_compact)) {
    return Status::Ok;
  }

  if (has_compact) {
    const SnapshotKind snapshot = inspect_snapshot(compact_path, error);
    if (snapshot == SnapshotKind::Unreadable) {
      error = "could not read compaction file: " + error;
      return Status::IoError;
    }
    if (snapshot == SnapshotKind::Complete) {
      if (!rename_file(compact_path, current, error)) {
        if (!has_old) {
          return Status::IoError;
        }
        std::string restore_error;
        if (!rename_file(old_path, current, restore_error)) {
          error = "could not install compacted log (" + error + ") or restore the previous log (" +
                  restore_error + ")";
          return Status::IoError;
        }
      }
      return Status::Ok;
    }
    if (!has_old) {
      error = "compaction file is incomplete and no previous log exists";
      return Status::Corruption;
    }
    std::string remove_error;
    remove_file(compact_path, remove_error);
  }

  if (!rename_file(old_path, current, error)) {
    return Status::IoError;
  }
  return Status::Ok;
}

size_t partial_write_size(size_t record_size) {
  size_t n = record_size / 2;
  if (n == 0 || n >= record_size) {
    n = record_size > 1 ? record_size - 1 : 1;
  }
  return n;
}

}  // namespace

struct Store::Impl {
  std::filesystem::path path;
  File file;
  Index index;
  uint64_t durable_size = 0;
  bool is_open = false;
  bool directory_durable = false;
  std::string error;

  Status fail(Status status) {
    file.close();
    is_open = false;
    index.clear();
    return status;
  }

  void note_directory_sync() {
    if (directory_durable) {
      return;
    }
    std::string dir_error;
    if (sync_parent_directory(path, dir_error)) {
      directory_durable = true;
    }
  }

  // After a good scan, the live log has the bytes worth keeping.
  // Delete .old only after the directory flush. If that flush fails, leave .old.
  void discard_old_backup() {
    const auto old = with_suffix(path, ".old");
    if (!regular_file_exists(old)) {
      return;
    }
    note_directory_sync();
    if (!directory_durable) {
      return;
    }
    std::string remove_error;
    remove_file(old, remove_error);
  }

  Status rollback_to(uint64_t start) {
    std::string rollback_error;
    if (!file.truncate(start, rollback_error) || !file.sync(rollback_error)) {
      file.close();
      is_open = false;
      error += "; rollback failed (" + rollback_error + "); reopen the store";
      return Status::IoError;
    }
    return Status::IoError;
  }

  Status open_at(const std::filesystem::path& user_path) {
    path = user_path;
    if (const Status recovered = recover_names(path, error); recovered != Status::Ok) {
      return fail(recovered);
    }

    const auto old_path = with_suffix(path, ".old");
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!file.open(path, error)) {
        return fail(Status::IoError);
      }
      std::vector<uint8_t> bytes;
      if (!load_file(file, bytes, error)) {
        return fail(Status::IoError);
      }
      index.clear();
      const Scan scan = scan_bytes(bytes.data(), bytes.size(), &index);
      if (scan.status != Status::Ok) {
        if (regular_file_exists(old_path)) {
          error = scan.error + "; left the log unchanged and kept " + old_path.string();
        } else {
          error = scan.error;
        }
        return fail(scan.status);
      }

      // No complete record in this file. If .old exists, switch to it
      // instead of opening an empty store.
      if (attempt == 0 && scan.valid_end == 0 && !scan.contains_seal &&
          regular_file_exists(old_path)) {
        file.close();
        index.clear();
        const auto parked = with_suffix(path, ".discarded");
        std::string park_error;
        remove_file(parked, park_error);
        if (!rename_file(path, parked, error)) {
          return fail(Status::IoError);
        }
        if (!rename_file(old_path, path, error)) {
          return fail(Status::IoError);
        }
        remove_file(parked, park_error);
        continue;
      }

      if (scan.discarded_tail) {
        if (!file.truncate(scan.valid_end, error) || !file.sync(error)) {
          return fail(Status::IoError);
        }
      }
      durable_size = scan.valid_end;
      break;
    }

    // Current scanned cleanly, so a sibling .compact was never installed.
    // The acknowledged writes are already in the live file.
    std::string remove_error;
    remove_file(with_suffix(path, ".compact"), remove_error);
    note_directory_sync();
    discard_old_backup();
    is_open = true;
    error.clear();
    return Status::Ok;
  }

  Status append(uint8_t op, std::string key, std::string value, bool erase) {
    // Flush the parent directory so a new filename can survive power loss.
    // If that fails, the put or erase still continues. Surviving a process
    // crash only requires the file flush below. Power-loss limits are in
    // the README.
    note_directory_sync();

    const std::string record = format::encode(op, key, value);
    const uint64_t start = durable_size;

    if (crash_ctl().armed("record.before_write")) {
      crash_exit();
    }
    if (take_inject("write")) {
      error = "injected write failure";
      return rollback_to(start);
    }
    if (take_inject("write_partial")) {
      std::string ignored;
      file.write_exact(start, record.data(), std::min<size_t>(4, record.size()), ignored);
      error = "injected partial write failure";
      return rollback_to(start);
    }
    if (crash_ctl().armed("record.write_partial")) {
      std::string ignored;
      file.write_exact(start, record.data(), partial_write_size(record.size()), ignored);
      crash_exit();
    }
    if (!file.write_exact(start, record.data(), record.size(), error)) {
      return rollback_to(start);
    }
    if (crash_ctl().armed("record.before_sync")) {
      crash_exit();
    }
    if (take_inject("sync")) {
      error = "injected sync failure";
      return rollback_to(start);
    }
    if (!file.sync(error)) {
      return rollback_to(start);
    }
    if (crash_ctl().armed("record.after_sync")) {
      crash_exit();
    }

    // Update the index only after the OS flush returns.
    // On failure the previous index stays, and the file is truncated back
    // to the last flushed offset when that rollback works.
    durable_size = start + record.size();
    if (erase) {
      index.erase(key);
    } else {
      index.insert_or_assign(std::move(key), std::move(value));
    }
    error.clear();
    return Status::Ok;
  }

  Status put(std::string_view key, std::string_view value) {
    if (!is_open) {
      error = "store is not open";
      return Status::IoError;
    }
    if (key.size() > format::kMaxKeyOrValue || value.size() > format::kMaxKeyOrValue) {
      error = "key or value exceeds 32 MiB";
      return Status::InvalidArgument;
    }
    return append(format::kOpPut, std::string(key), std::string(value), false);
  }

  Status get(std::string_view key, std::string& value) const {
    if (!is_open) {
      return Status::IoError;
    }
    const auto it = index.find(key);
    if (it == index.end()) {
      return Status::NotFound;
    }
    value = it->second;
    return Status::Ok;
  }

  Status erase(std::string_view key) {
    if (!is_open) {
      error = "store is not open";
      return Status::IoError;
    }
    if (key.size() > format::kMaxKeyOrValue) {
      error = "key or value exceeds 32 MiB";
      return Status::InvalidArgument;
    }
    if (index.find(key) == index.end()) {
      error.clear();
      return Status::Ok;
    }
    return append(format::kOpErase, std::string(key), std::string(), true);
  }

  Status write_compact_record(File& out, uint64_t& offset, uint8_t op, std::string_view key,
                              std::string_view value) {
    const std::string record = format::encode(op, key, value);
    if (crash_ctl().armed("compact.write_partial")) {
      std::string ignored;
      out.write_exact(offset, record.data(), partial_write_size(record.size()), ignored);
      crash_exit();
    }
    if (take_inject("compact_write")) {
      error = "injected compaction write failure";
      return Status::IoError;
    }
    if (!out.write_exact(offset, record.data(), record.size(), error)) {
      return Status::IoError;
    }
    offset += record.size();
    return Status::Ok;
  }

  Status compact() {
    if (!is_open) {
      error = "store is not open";
      return Status::IoError;
    }

    const auto compact_path = with_suffix(path, ".compact");
    const auto old_path = with_suffix(path, ".old");

    auto cleanup_compact = [&]() {
      std::string remove_error;
      remove_file(compact_path, remove_error);
    };

    File out;
    if (!out.create(compact_path, error)) {
      return Status::IoError;
    }
    uint64_t offset = 0;
    for (const auto& [key, value] : index) {
      if (const Status wrote = write_compact_record(out, offset, format::kOpPut, key, value);
          wrote != Status::Ok) {
        out.close();
        cleanup_compact();
        return wrote;
      }
    }
    if (const Status wrote = write_compact_record(out, offset, format::kOpSeal, {}, {});
        wrote != Status::Ok) {
      out.close();
      cleanup_compact();
      return wrote;
    }
    // Flush the snapshot before renaming the live log away.
    if (!out.sync(error)) {
      out.close();
      cleanup_compact();
      return Status::IoError;
    }
    out.close();

    if (crash_ctl().armed("compact.before_install")) {
      crash_exit();
    }

    // Windows will not rename a file we still have open.
    file.close();
    if (regular_file_exists(path)) {
      if (!rename_file(path, old_path, error)) {
        cleanup_compact();
        if (!file.open(path, error)) {
          is_open = false;
          error += "; and the log could not be reopened";
          return Status::IoError;
        }
        return Status::IoError;
      }
    }

    if (crash_ctl().armed("compact.after_aside")) {
      crash_exit();
    }

    if (!rename_file(compact_path, path, error)) {
      std::string restore_error;
      if (!regular_file_exists(path) && regular_file_exists(old_path) &&
          !rename_file(old_path, path, restore_error)) {
        is_open = false;
        error = "could not install compacted log (" + error + ") or restore the previous log (" +
                restore_error + ")";
        return Status::IoError;
      }
      cleanup_compact();
      if (!file.open(path, error)) {
        is_open = false;
        return Status::IoError;
      }
      return Status::IoError;
    }

    if (crash_ctl().armed("compact.after_install")) {
      crash_exit();
    }

    // `path` is the new log. `path.old` still has the previous bytes.
    // Delete the backup only after the directory flush. A crash before that
    // delete still has a complete file under one of the two names.
    if (sync_parent_directory(path, error)) {
      directory_durable = true;
      if (crash_ctl().armed("compact.before_delete_old")) {
        crash_exit();
      }
      std::string remove_error;
      remove_file(old_path, remove_error);
    } else if (crash_ctl().armed("compact.before_delete_old")) {
      crash_exit();
    }

    if (!file.open(path, error)) {
      is_open = false;
      error = "compacted log was installed but could not be reopened: " + error;
      return Status::IoError;
    }
    uint64_t size = 0;
    if (!file.file_size(size, error)) {
      file.close();
      is_open = false;
      return Status::IoError;
    }
    if (size < offset) {
      file.close();
      is_open = false;
      error = "compacted log is shorter than the bytes just written";
      return Status::IoError;
    }
    if (size > offset && (!file.truncate(offset, error) || !file.sync(error))) {
      file.close();
      is_open = false;
      return Status::IoError;
    }
    durable_size = offset;
    is_open = true;
    error.clear();
    return Status::Ok;
  }
};

Store::Store() = default;
Store::~Store() = default;
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;

const char* to_string(Status status) noexcept {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::NotFound:
      return "not found";
    case Status::InvalidArgument:
      return "invalid argument";
    case Status::IoError:
      return "io error";
    case Status::Corruption:
      return "corruption";
  }
  return "unknown";
}

Status Store::open(const std::filesystem::path& path, Store& store) {
  auto next = std::make_unique<Impl>();
  const Status status = next->open_at(path);
  store.impl_ = std::move(next);
  return status;
}

Status Store::put(std::string_view key, std::string_view value) {
  if (!impl_) {
    return Status::IoError;
  }
  return impl_->put(key, value);
}

Status Store::get(std::string_view key, std::string& value) const {
  if (!impl_) {
    return Status::IoError;
  }
  return impl_->get(key, value);
}

Status Store::erase(std::string_view key) {
  if (!impl_) {
    return Status::IoError;
  }
  return impl_->erase(key);
}

Status Store::compact() {
  if (!impl_) {
    return Status::IoError;
  }
  return impl_->compact();
}

const std::string& Store::last_error() const {
  static const std::string empty;
  if (!impl_) {
    return empty;
  }
  return impl_->error;
}

}  // namespace durable_kv
