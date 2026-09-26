#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace durable_kv {

// put and erase return Ok after the record is fully written and the OS flush
// has returned. The in-memory index is updated after that flush.
enum class Status {
  Ok = 0,
  NotFound,
  InvalidArgument,
  IoError,
  Corruption,
};

const char* to_string(Status status) noexcept;

// One process, one writer. Do not share a Store across threads or processes.
//
// Keys and values are raw bytes, including NULs and empty strings.
// An empty value is stored data. A missing key returns NotFound.
//
// The README describes what Ok means after a crash or a power loss.
// Log I/O uses WriteFile, pwrite, FlushFileBuffers, fdatasync, or F_FULLFSYNC.
class Store {
 public:
  Store();
  ~Store();
  Store(Store&&) noexcept;
  Store& operator=(Store&&) noexcept;
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Opens or creates the log and rebuilds the index by scanning it.
  // A truncated or bad-checksum record at the end of the file is dropped.
  // If compaction stopped halfway, open finishes it or switches back to the
  // previous log. On failure the Store stays closed; see last_error().
  [[nodiscard]] static Status open(const std::filesystem::path& path, Store& store);

  // Appends a put. Ok means the bytes and the OS flush both succeeded, and
  // the index now returns `value`. After a process crash, the next open
  // still returns that value.
  [[nodiscard]] Status put(std::string_view key, std::string_view value);

  // Copies the current value into `value`. Does not write.
  // Ok with an empty string is a stored empty value. NotFound means the
  // key is absent.
  [[nodiscard]] Status get(std::string_view key, std::string& value) const;

  // If the key is present, appends a tombstone and drops it from the index.
  // Ok means the key is gone. When a tombstone was required, that record
  // went through the same flush as put. Erasing a missing key returns Ok
  // and writes nothing.
  [[nodiscard]] Status erase(std::string_view key);

  // Rewrites live keys to a new file and makes that file the log.
  // Ok means `path` now holds the new log. A crash before Ok still leaves
  // some complete log for the next open(). The README lists the rename order.
  [[nodiscard]] Status compact();

  // Text from the last failed open, put, erase, or compact.
  // Cleared when one of those calls succeeds. get leaves it alone.
  [[nodiscard]] const std::string& last_error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace durable_kv
