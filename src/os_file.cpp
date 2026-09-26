#include "os_file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace durable_kv {
namespace {

std::string os_message() {
#ifdef _WIN32
  return std::system_category().message(static_cast<int>(GetLastError()));
#else
  return std::generic_category().message(errno);
#endif
}

#ifdef _WIN32
void* invalid_handle() { return reinterpret_cast<void*>(static_cast<intptr_t>(-1)); }

bool seek_to(void* handle, uint64_t offset, std::string& error) {
  LARGE_INTEGER pos;
  pos.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(static_cast<HANDLE>(handle), pos, nullptr, FILE_BEGIN)) {
    error = "seek failed: " + os_message();
    return false;
  }
  return true;
}
#endif

// DURABLE_KV_MAX_WRITE caps each OS write so tests can force short writes.
// Unset or empty means write the whole request. Zero is an error.
size_t write_cap(size_t remaining, std::string& error) {
  const char* env = std::getenv("DURABLE_KV_MAX_WRITE");
  if (env == nullptr || env[0] == '\0') {
    return remaining;
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(env, &end, 10);
  if (end == env || value == 0 || value > static_cast<unsigned long>(std::numeric_limits<size_t>::max())) {
    error = "DURABLE_KV_MAX_WRITE must be a positive integer";
    return 0;
  }
  const size_t cap = static_cast<size_t>(value);
  return cap < remaining ? cap : remaining;
}

}  // namespace

File::File() noexcept = default;

File::~File() { close(); }

File::File(File&& other) noexcept {
#ifdef _WIN32
  handle_ = other.handle_;
  other.handle_ = invalid_handle();
#else
  fd_ = other.fd_;
  other.fd_ = -1;
#endif
}

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    close();
#ifdef _WIN32
    handle_ = other.handle_;
    other.handle_ = invalid_handle();
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
  }
  return *this;
}

bool File::is_open() const noexcept {
#ifdef _WIN32
  return handle_ != invalid_handle() && handle_ != nullptr;
#else
  return fd_ >= 0;
#endif
}

void File::close() noexcept {
#ifdef _WIN32
  if (is_open()) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = invalid_handle();
  }
#else
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
#endif
}

bool File::open(const std::filesystem::path& path, std::string& error) {
  return open_native(path, true, false, error);
}

bool File::create(const std::filesystem::path& path, std::string& error) {
  return open_native(path, true, true, error);
}

bool File::open_existing(const std::filesystem::path& path, std::string& error) {
  return open_native(path, false, false, error);
}

bool File::open_native(const std::filesystem::path& path, bool create, bool truncate, std::string& error) {
  close();
#ifdef _WIN32
  DWORD disposition = OPEN_EXISTING;
  if (create && truncate) {
    disposition = CREATE_ALWAYS;
  } else if (create) {
    disposition = OPEN_ALWAYS;
  }
  // FILE_FLAG_WRITE_THROUGH, then FlushFileBuffers after the write.
  // Some drives acknowledge the flush from a volatile cache.
  // FILE_FLAG_NO_BUFFERING is off, so writes need no sector alignment.
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    error = "open failed: " + os_message();
    return false;
  }
  handle_ = handle;
  return true;
#else
  int flags = O_RDWR | O_CLOEXEC;
  if (create) {
    flags |= O_CREAT;
  }
  if (truncate) {
    flags |= O_TRUNC;
  }
  const int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    error = "open failed: " + os_message();
    return false;
  }
  fd_ = fd;
  return true;
#endif
}

bool File::read_exact(uint64_t offset, void* data, size_t size, std::string& error) {
  auto* bytes = static_cast<uint8_t*>(data);
  size_t done = 0;
  while (done < size) {
#ifdef _WIN32
    if (!seek_to(handle_, offset + done, error)) {
      return false;
    }
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 20));
    if (!ReadFile(static_cast<HANDLE>(handle_), bytes + done, want, &got, nullptr)) {
      error = "read failed: " + os_message();
      return false;
    }
    if (got == 0) {
      error = "short read at end of file";
      return false;
    }
    done += got;
#else
    const ssize_t got = ::pread(fd_, bytes + done, size - done, static_cast<off_t>(offset + done));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = "read failed: " + os_message();
      return false;
    }
    if (got == 0) {
      error = "short read at end of file";
      return false;
    }
    done += static_cast<size_t>(got);
#endif
  }
  return true;
}

bool File::write_exact(uint64_t offset, const void* data, size_t size, std::string& error) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  size_t done = 0;
  while (done < size) {
    const size_t cap = write_cap(size - done, error);
    if (cap == 0) {
      return false;
    }
#ifdef _WIN32
    if (!seek_to(handle_, offset + done, error)) {
      return false;
    }
    DWORD wrote = 0;
    const DWORD want = static_cast<DWORD>(std::min<size_t>(cap, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
    if (!WriteFile(static_cast<HANDLE>(handle_), bytes + done, want, &wrote, nullptr)) {
      error = "write failed: " + os_message();
      return false;
    }
    if (wrote == 0) {
      error = "write returned zero bytes";
      return false;
    }
    done += wrote;
#else
    const ssize_t wrote =
        ::pwrite(fd_, bytes + done, cap, static_cast<off_t>(offset + done));
    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = "write failed: " + os_message();
      return false;
    }
    if (wrote == 0) {
      error = "write returned zero bytes";
      return false;
    }
    done += static_cast<size_t>(wrote);
#endif
  }
  return true;
}

bool File::sync(std::string& error) {
  // Flush file data and size via the OS. Success means the OS accepted the
  // call. A drive that keeps a volatile write cache can still lose the bytes.
  // See the README.
#ifdef _WIN32
  if (!FlushFileBuffers(static_cast<HANDLE>(handle_))) {
    error = "FlushFileBuffers failed: " + os_message();
    return false;
  }
  return true;
#elif defined(__APPLE__)
  // On macOS, fsync can return before the drive cache is flushed.
  // Require F_FULLFSYNC. If it fails, sync() fails.
  if (::fcntl(fd_, F_FULLFSYNC) != 0) {
    error = "F_FULLFSYNC failed: " + os_message();
    return false;
  }
  return true;
#else
  if (::fdatasync(fd_) != 0) {
    error = "fdatasync failed: " + os_message();
    return false;
  }
  return true;
#endif
}

bool File::truncate(uint64_t size, std::string& error) {
#ifdef _WIN32
  if (!seek_to(handle_, size, error)) {
    return false;
  }
  if (!SetEndOfFile(static_cast<HANDLE>(handle_))) {
    error = "SetEndOfFile failed: " + os_message();
    return false;
  }
  return true;
#else
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
    error = "ftruncate failed: " + os_message();
    return false;
  }
  return true;
#endif
}

bool File::file_size(uint64_t& size, std::string& error) const {
#ifdef _WIN32
  LARGE_INTEGER value;
  if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &value)) {
    error = "GetFileSizeEx failed: " + os_message();
    return false;
  }
  size = static_cast<uint64_t>(value.QuadPart);
  return true;
#else
  const off_t end = ::lseek(fd_, 0, SEEK_END);
  if (end < 0) {
    error = "lseek failed: " + os_message();
    return false;
  }
  size = static_cast<uint64_t>(end);
  return true;
#endif
}

bool sync_parent_directory(const std::filesystem::path& file, std::string& error) {
  std::filesystem::path dir = file.parent_path();
  if (dir.empty()) {
    dir = ".";
  }
#ifdef _WIN32
  HANDLE handle = CreateFileW(dir.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    error = "open directory failed: " + os_message();
    return false;
  }
  const BOOL flushed = FlushFileBuffers(handle);
  const DWORD flush_error = GetLastError();
  CloseHandle(handle);
  if (!flushed) {
    SetLastError(flush_error);
    error = "FlushFileBuffers on directory failed: " + os_message();
    return false;
  }
  return true;
#else
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    error = "open directory failed: " + os_message();
    return false;
  }
#if defined(__APPLE__)
  const int rc = ::fcntl(fd, F_FULLFSYNC);
#else
  const int rc = ::fsync(fd);
#endif
  const int saved = errno;
  ::close(fd);
  if (rc != 0) {
    errno = saved;
    error = "directory sync failed: " + os_message();
    return false;
  }
  return true;
#endif
}

bool rename_file(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error) {
#ifdef _WIN32
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = "MoveFileEx failed: " + os_message();
    return false;
  }
  return true;
#else
  if (::rename(from.c_str(), to.c_str()) != 0) {
    error = "rename failed: " + os_message();
    return false;
  }
  return true;
#endif
}

bool remove_file(const std::filesystem::path& path, std::string& error) {
  std::error_code ec;
  const bool present = std::filesystem::exists(path, ec);
  if (ec) {
    error = ec.message();
    return false;
  }
  if (!present) {
    return true;
  }
  if (!std::filesystem::remove(path, ec) || ec) {
    error = ec ? ec.message() : "remove failed";
    return false;
  }
  return true;
}

bool regular_file_exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec);
}

bool read_file_size(const std::filesystem::path& path, uint64_t& size, std::string& error) {
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec) {
    error = ec.message();
    return false;
  }
  size = static_cast<uint64_t>(bytes);
  return true;
}

}  // namespace durable_kv
