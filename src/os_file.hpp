#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace durable_kv {

// Owns one OS file handle. Writes go through WriteFile or pwrite.
class File {
 public:
  File() noexcept;
  ~File();
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  // Open an existing file, or create it. Leaves existing bytes in place.
  bool open(const std::filesystem::path& path, std::string& error);
  // Create or replace with an empty file.
  bool create(const std::filesystem::path& path, std::string& error);
  // Open only if the file already exists.
  bool open_existing(const std::filesystem::path& path, std::string& error);

  void close() noexcept;
  bool is_open() const noexcept;

  bool read_exact(uint64_t offset, void* data, size_t size, std::string& error);
  bool write_exact(uint64_t offset, const void* data, size_t size, std::string& error);
  // Flush file data and the file size. The OS call depends on the platform;
  // see File::sync.
  bool sync(std::string& error);
  bool truncate(uint64_t size, std::string& error);
  bool file_size(uint64_t& size, std::string& error) const;

 private:
  bool open_native(const std::filesystem::path& path, bool create, bool truncate, std::string& error);

#ifdef _WIN32
  void* handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(-1));
#else
  int fd_ = -1;
#endif
};

// Flush the directory that contains `file`, so the directory entry for a
// create or rename can survive power loss when the OS and the drive honor
// the flush. false means that directory flush failed.
bool sync_parent_directory(const std::filesystem::path& file, std::string& error);

// Replaces `to` if it exists. POSIX rename is atomic. Windows uses
// MoveFileEx with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH.
bool rename_file(const std::filesystem::path& from, const std::filesystem::path& to,
                 std::string& error);

// true if the path is already gone.
bool remove_file(const std::filesystem::path& path, std::string& error);

bool regular_file_exists(const std::filesystem::path& path);

// On success, `size` is the length in bytes. Empty files return true and 0.
bool read_file_size(const std::filesystem::path& path, uint64_t& size, std::string& error);

}  // namespace durable_kv
