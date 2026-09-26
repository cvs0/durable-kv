#include "durable_kv/store.hpp"
#include "format.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using durable_kv::Status;
using durable_kv::Store;

namespace {

int g_failed = 0;
fs::path g_exe;

void fail(const char* file, int line, const std::string& message) {
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, message.c_str());
  ++g_failed;
}

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fail(__FILE__, __LINE__, #cond);                                   \
    }                                                                    \
  } while (0)

#define CHECK_MSG(cond, msg)                                             \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fail(__FILE__, __LINE__, msg);                                     \
    }                                                                    \
  } while (0)

class EnvVar {
 public:
  EnvVar(const char* name, const char* value) : name_(name) { set(value); }
  ~EnvVar() { set(""); }

 private:
  void set(const char* value) {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value);
#else
    if (value[0] == '\0') {
      unsetenv(name_.c_str());
    } else {
      setenv(name_.c_str(), value, 1);
    }
#endif
  }
  std::string name_;
};

class TempDir {
 public:
  TempDir() {
    static int next = 0;
    path = fs::temp_directory_path() / "durable-kv-tests" / std::to_string(++next);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  fs::path path;
  fs::path db() const { return path / "store.kv"; }
};

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void spit(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void append_bytes(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void move_file(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::remove(to, ec);
  fs::rename(from, to, ec);
  CHECK_MSG(!ec, "rename failed: " + ec.message());
}

struct Saved {
  uint8_t op = 0;
  std::string key;
  std::string value;
};

std::vector<Saved> decode_log(const fs::path& path) {
  const std::string bytes = slurp(path);
  std::vector<Saved> records;
  uint64_t offset = 0;
  while (offset < bytes.size()) {
    durable_kv::format::Record rec;
    std::string error;
    const auto frame = durable_kv::format::read_record(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), offset, rec, error);
    if (frame != durable_kv::format::Frame::Ok) {
      break;
    }
    records.push_back(Saved{rec.op, rec.key, rec.value});
    offset = rec.next;
  }
  return records;
}

void expect_value(Store& store, const std::string& key, const std::string& value) {
  std::string got;
  const Status status = store.get(key, got);
  if (status != Status::Ok || got != value) {
    fail(__FILE__, __LINE__, "get " + key + " status " + durable_kv::to_string(status) + " value [" + got +
                                 "] want [" + value + "]");
  }
}

void expect_missing(Store& store, const std::string& key) {
  std::string got = "sentinel";
  const Status status = store.get(key, got);
  if (status != Status::NotFound) {
    fail(__FILE__, __LINE__, "get " + key + " status " + durable_kv::to_string(status));
  }
}

Status open_ok(const fs::path& path, Store& store) {
  const Status status = Store::open(path, store);
  if (status != Status::Ok) {
    fail(__FILE__, __LINE__, std::string("open: ") + durable_kv::to_string(status) + " " + store.last_error());
  }
  return status;
}

void put_ok(Store& store, const std::string& key, const std::string& value) {
  const Status status = store.put(key, value);
  if (status != Status::Ok) {
    fail(__FILE__, __LINE__, std::string("put: ") + store.last_error());
  }
}

struct Child {
  int code = -1;
  std::string output;
  std::string error;
};

Child run_child(const fs::path& db, const char* crash_at, const std::vector<std::string>& args) {
  Child child;
  std::vector<std::string> argv = {g_exe.string(), "--worker", db.string()};
  argv.insert(argv.end(), args.begin(), args.end());

#ifdef _WIN32
  std::wstring cmd;
  for (const std::string& arg : argv) {
    if (!cmd.empty()) {
      cmd.push_back(L' ');
    }
    cmd.push_back(L'"');
    for (unsigned char ch : arg) {
      if (ch == '"') {
        cmd.push_back(L'\\');
      }
      cmd.push_back(static_cast<wchar_t>(ch));
    }
    cmd.push_back(L'"');
  }
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
    child.error = "CreatePipe failed";
    return child;
  }
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;
  PROCESS_INFORMATION pi{};
  EnvVar crash_env("DURABLE_KV_CRASH_AT", crash_at);
  const BOOL started =
      CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
  CloseHandle(write_pipe);
  if (nul != INVALID_HANDLE_VALUE) {
    CloseHandle(nul);
  }
  if (!started) {
    CloseHandle(read_pipe);
    std::string narrow_cmd;
    narrow_cmd.reserve(cmd.size());
    for (wchar_t ch : cmd) {
      narrow_cmd.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    child.error = "CreateProcess failed: " + std::to_string(GetLastError()) + " cmd " + narrow_cmd;
    return child;
  }
  std::string output;
  char buffer[512];
  DWORD got = 0;
  while (ReadFile(read_pipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
    output.append(buffer, buffer + got);
  }
  CloseHandle(read_pipe);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  child.code = static_cast<int>(code);
  child.output = std::move(output);
  return child;
#else
  int pipes[2];
  if (pipe(pipes) != 0) {
    child.error = "pipe failed";
    return child;
  }
  EnvVar crash_env("DURABLE_KV_CRASH_AT", crash_at);
  const pid_t pid = fork();
  if (pid < 0) {
    child.error = "fork failed";
    close(pipes[0]);
    close(pipes[1]);
    return child;
  }
  if (pid == 0) {
    dup2(pipes[1], STDOUT_FILENO);
    dup2(pipes[1], STDERR_FILENO);
    close(pipes[0]);
    close(pipes[1]);
    std::vector<std::string> owned = argv;
    std::vector<char*> c_args;
    for (std::string& arg : owned) {
      c_args.push_back(arg.data());
    }
    c_args.push_back(nullptr);
    execv(c_args[0], c_args.data());
    std::_Exit(127);
  }
  close(pipes[1]);
  std::string output;
  char buffer[512];
  ssize_t got = 0;
  while ((got = ::read(pipes[0], buffer, sizeof(buffer))) > 0) {
    output.append(buffer, buffer + got);
  }
  close(pipes[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  child.output = std::move(output);
  if (WIFEXITED(status)) {
    child.code = WEXITSTATUS(status);
  } else {
    child.code = -1;
    child.error = "child did not exit normally";
  }
  return child;
#endif
}

void expect_child_crashed(const Child& child) {
  if (child.code != 97) {
    fail(__FILE__, __LINE__, "child exit " + std::to_string(child.code) + " error [" + child.error +
                                 "] output [" + child.output + "]");
  }
}

void reopen_matches(const fs::path& db, const std::vector<std::pair<std::string, std::string>>& live,
                    const std::vector<std::string>& missing) {
  Store store;
  if (open_ok(db, store) != Status::Ok) {
    return;
  }
  for (const auto& [key, value] : live) {
    expect_value(store, key, value);
  }
  for (const std::string& key : missing) {
    expect_missing(store, key);
  }
  put_ok(store, "after-recovery", "yes");
  Store again;
  if (open_ok(db, again) != Status::Ok) {
    return;
  }
  expect_value(again, "after-recovery", "yes");
  for (const auto& [key, value] : live) {
    expect_value(again, key, value);
  }
}

void run(const char* name, void (*fn)()) {
  const int before = g_failed;
  fn();
  if (g_failed == before) {
    std::printf("PASS %s\n", name);
  } else {
    std::printf("FAIL %s\n", name);
  }
  std::fflush(stdout);
}

void test_crc32() {
  const auto* text = reinterpret_cast<const uint8_t*>("123456789");
  CHECK(durable_kv::format::crc32(text, 9) == 0xCBF43926u);
  const std::string binary("a\0b", 3);
  const std::string encoded = durable_kv::format::encode(durable_kv::format::kOpPut, "k", binary);
  durable_kv::format::Record rec;
  std::string error;
  const auto frame = durable_kv::format::read_record(reinterpret_cast<const uint8_t*>(encoded.data()),
                                                      encoded.size(), 0, rec, error);
  CHECK(frame == durable_kv::format::Frame::Ok);
  CHECK(rec.key == "k");
  CHECK(rec.value == binary);
}

void test_reopen_overwrite_erase() {
  TempDir dir;
  const fs::path db = dir.db();
  {
    Store store;
    CHECK(open_ok(db, store) == Status::Ok);
    put_ok(store, "k", "v1");
    put_ok(store, "k", "v2");
    put_ok(store, "other", "stay");
    CHECK(store.erase("k") == Status::Ok);
    CHECK(store.erase("missing") == Status::Ok);
    expect_missing(store, "k");
    expect_value(store, "other", "stay");
  }
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  expect_missing(store, "k");
  expect_value(store, "other", "stay");
  put_ok(store, "k", "again");
  expect_value(store, "k", "again");
}

void test_empty_and_binary() {
  TempDir dir;
  const fs::path db = dir.db();
  const std::string binary("\0\xff\x01", 3);
  const std::string key("a\0b", 3);
  {
    Store store;
    CHECK(open_ok(db, store) == Status::Ok);
    put_ok(store, "empty", "");
    put_ok(store, "", "blank-key");
    put_ok(store, key, binary);
    expect_value(store, "empty", "");
    expect_value(store, "", "blank-key");
    expect_value(store, key, binary);
    expect_missing(store, "nope");
  }
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  expect_value(store, "empty", "");
  expect_value(store, "", "blank-key");
  expect_value(store, key, binary);
}

void test_truncated_tail() {
  TempDir dir;
  const fs::path db = dir.db();
  {
    Store store;
    CHECK(open_ok(db, store) == Status::Ok);
    put_ok(store, "k", "v");
    put_ok(store, "b", "2");
  }
  const auto size_before = fs::file_size(db);
  append_bytes(db, "\x01\x02\x03");
  append_bytes(db, std::string("\xff\xff\xff\x7f\x00\x00\x00\x00", 8));
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  expect_value(store, "k", "v");
  expect_value(store, "b", "2");
  CHECK(fs::file_size(db) == size_before);
}

void test_bad_final_checksum_keeps_previous_value() {
  TempDir dir;
  const fs::path db = dir.db();
  {
    Store store;
    CHECK(open_ok(db, store) == Status::Ok);
    put_ok(store, "other", "stay");
    put_ok(store, "k", "v1");
    put_ok(store, "k", "v2");
  }
  std::string bytes = slurp(db);
  uint64_t offset = 0;
  uint64_t last = 0;
  while (offset < bytes.size()) {
    durable_kv::format::Record rec;
    std::string error;
    const auto frame = durable_kv::format::read_record(reinterpret_cast<const uint8_t*>(bytes.data()),
                                                        bytes.size(), offset, rec, error);
    CHECK(frame == durable_kv::format::Frame::Ok);
    last = offset;
    offset = rec.next;
  }
  const auto valid_size = static_cast<std::uintmax_t>(last);
  bytes[static_cast<size_t>(last + 4)] ^= 0x5a;
  spit(db, bytes);
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  expect_value(store, "k", "v1");
  expect_value(store, "other", "stay");
  CHECK(fs::file_size(db) == valid_size);
}

void test_bad_checksum_in_the_middle_is_corruption() {
  TempDir dir;
  const fs::path db = dir.db();
  {
    Store store;
    CHECK(open_ok(db, store) == Status::Ok);
    put_ok(store, "a", "1");
    put_ok(store, "b", "2");
  }
  std::string bytes = slurp(db);
  bytes[4] ^= 0xff;
  spit(db, bytes);
  {
    Store store;
    const Status status = Store::open(db, store);
    CHECK(status == Status::Corruption);
    CHECK(store.last_error().find("checksum") != std::string::npos);
    std::string value;
    CHECK(store.get("b", value) == Status::IoError);
  }
  const auto size_before = fs::file_size(db);
  Store store;
  CHECK(Store::open(db, store) == Status::Corruption);
  CHECK(fs::file_size(db) == size_before);
}

void test_short_writes() {
  TempDir dir;
  const fs::path db = dir.db();
  std::string value(50, '\x7f');
  value[0] = '\0';
  value[49] = '\xff';
  {
    Store store;
    CHECK(open_ok(db, store) == Status::Ok);
    EnvVar cap("DURABLE_KV_MAX_WRITE", "3");
    put_ok(store, "k", value);
  }
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  expect_value(store, "k", value);
}

void test_io_errors_do_not_publish(const char* inject) {
  TempDir dir;
  const fs::path db = dir.db();
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  put_ok(store, "k", "v1");
  const auto size_before = fs::file_size(db);
  {
    EnvVar fail_io("DURABLE_KV_INJECT", inject);
    const Status status = store.put("k", "v2");
    CHECK(status == Status::IoError);
    CHECK(!store.last_error().empty());
  }
  expect_value(store, "k", "v1");
  CHECK(fs::file_size(db) == size_before);
  put_ok(store, "k", "v3");
  Store again;
  CHECK(open_ok(db, again) == Status::Ok);
  expect_value(again, "k", "v3");
}

void test_injected_write() { test_io_errors_do_not_publish("write"); }
void test_injected_sync() { test_io_errors_do_not_publish("sync"); }
void test_injected_partial() { test_io_errors_do_not_publish("write_partial"); }

void test_compact_rewrites_live_keys() {
  TempDir dir;
  const fs::path db = dir.db();
  {
    Store store;
    CHECK(open_ok(db, store) == Status::Ok);
    put_ok(store, "k", "v1");
    put_ok(store, "k", "v2");
    put_ok(store, "drop", "x");
    CHECK(store.erase("drop") == Status::Ok);
    put_ok(store, "keep", "y");
    CHECK(store.compact() == Status::Ok);
    expect_value(store, "k", "v2");
    expect_value(store, "keep", "y");
    expect_missing(store, "drop");
    if (fs::is_regular_file(db.string() + ".old")) {
      std::printf("note: compaction left .old in place (directory flush did not succeed)\n");
    }
  }
  const auto records = decode_log(db);
  int puts = 0;
  int seals = 0;
  int erases = 0;
  for (const Saved& rec : records) {
    if (rec.op == durable_kv::format::kOpPut) {
      ++puts;
    } else if (rec.op == durable_kv::format::kOpErase) {
      ++erases;
    } else if (rec.op == durable_kv::format::kOpSeal) {
      ++seals;
    }
  }
  CHECK(puts == 2);
  CHECK(erases == 0);
  CHECK(seals == 1);
  CHECK(records.back().op == durable_kv::format::kOpSeal);
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  expect_value(store, "k", "v2");
  expect_missing(store, "drop");
}

void test_compact_write_failure_keeps_store() {
  TempDir dir;
  const fs::path db = dir.db();
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  put_ok(store, "k1", "v1");
  put_ok(store, "k2", "v2");
  {
    EnvVar fail_io("DURABLE_KV_INJECT", "compact_write");
    CHECK(store.compact() == Status::IoError);
  }
  expect_value(store, "k1", "v1");
  expect_value(store, "k2", "v2");
  CHECK(!fs::exists(db.string() + ".compact"));
  CHECK(store.compact() == Status::Ok);
  expect_value(store, "k1", "v1");
  expect_value(store, "k2", "v2");
}

void write_log(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& puts,
               const std::vector<std::string>& erases, bool compact_log) {
  Store store;
  CHECK(open_ok(path, store) == Status::Ok);
  for (const auto& [key, value] : puts) {
    put_ok(store, key, value);
  }
  for (const std::string& key : erases) {
    CHECK(store.erase(key) == Status::Ok);
  }
  if (compact_log) {
    CHECK(store.compact() == Status::Ok);
  }
}

void test_incomplete_compact_restores_old() {
  TempDir dir;
  const fs::path db = dir.db();
  write_log(dir.path / "oldlog", {{"k1", "v1"}, {"k2", "v2"}}, {}, false);
  write_log(dir.path / "partial", {{"k1", "only"}}, {}, false);
  move_file(dir.path / "oldlog", db.string() + ".old");
  move_file(dir.path / "partial", db.string() + ".compact");
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  expect_value(store, "k1", "v1");
  expect_value(store, "k2", "v2");
  CHECK(!fs::exists(db.string() + ".compact"));
}

void test_sealed_compact_is_installed_when_current_is_missing() {
  TempDir dir;
  const fs::path db = dir.db();
  write_log(dir.path / "snapshot", {{"k1", "new"}, {"k2", "v2"}}, {}, true);
  write_log(dir.path / "previous", {{"k1", "old"}, {"k2", "v2"}, {"k3", "junk"}}, {}, false);
  move_file(dir.path / "snapshot", db.string() + ".compact");
  move_file(dir.path / "previous", db.string() + ".old");
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  expect_value(store, "k1", "new");
  expect_value(store, "k2", "v2");
  expect_missing(store, "k3");
  const auto records = decode_log(db);
  CHECK(!records.empty());
  CHECK(records.back().op == durable_kv::format::kOpSeal);
  bool saw_junk = false;
  for (const Saved& rec : records) {
    if (rec.key == "k3") {
      saw_junk = true;
    }
  }
  CHECK(!saw_junk);
}

void test_torn_current_is_not_replaced_by_older_backup() {
  TempDir dir;
  const fs::path db = dir.db();
  write_log(db, {{"k", "current"}}, {}, false);
  append_bytes(db, "\x10\x11\x12\x13");
  write_log(dir.path / "backup", {{"k", "old"}}, {}, false);
  move_file(dir.path / "backup", db.string() + ".old");
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  expect_value(store, "k", "current");
}

void test_recordless_current_restores_backup() {
  TempDir dir;
  const fs::path db = dir.db();
  write_log(dir.path / "backup", {{"k", "from-old"}}, {}, false);
  move_file(dir.path / "backup", db.string() + ".old");
  spit(db, "");
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  expect_value(store, "k", "from-old");
}

void test_sealed_empty_snapshot_is_not_replaced_by_old() {
  TempDir dir;
  const fs::path db = dir.db();
  write_log(dir.path / "snapshot", {{"k", "gone"}}, {"k"}, true);
  write_log(dir.path / "backup", {{"k", "from-old"}}, {}, false);
  const auto snapshot = decode_log(dir.path / "snapshot");
  CHECK(snapshot.size() == 1);
  CHECK(snapshot[0].op == durable_kv::format::kOpSeal);
  move_file(dir.path / "snapshot", db);
  move_file(dir.path / "backup", db.string() + ".old");
  Store store;
  CHECK(open_ok(db, store) == Status::Ok);
  expect_missing(store, "k");
}

void test_corrupt_current_is_not_swapped_for_old() {
  TempDir dir;
  const fs::path db = dir.db();
  write_log(db, {{"a", "1"}, {"b", "2"}}, {}, false);
  std::string bytes = slurp(db);
  bytes[4] ^= 0xff;
  spit(db, bytes);
  write_log(dir.path / "backup", {{"a", "old"}}, {}, false);
  move_file(dir.path / "backup", db.string() + ".old");
  Store store;
  CHECK(Store::open(db, store) == Status::Corruption);
  CHECK(fs::is_regular_file(db));
  CHECK(fs::is_regular_file(db.string() + ".old"));
  Store again;
  CHECK(Store::open(db, again) == Status::Corruption);
}

void test_only_incomplete_compact_does_not_open_empty() {
  TempDir dir;
  const fs::path db = dir.db();
  write_log(dir.path / "partial", {{"k", "v"}}, {}, false);
  move_file(dir.path / "partial", db.string() + ".compact");
  Store store;
  CHECK(Store::open(db, store) == Status::Corruption);
  CHECK(fs::is_regular_file(db.string() + ".compact"));
  CHECK(!fs::exists(db));
  Store again;
  CHECK(Store::open(db, again) == Status::Corruption);
  CHECK(!fs::exists(db));
}

void test_fault_put(const char* point, bool value_should_exist) {
  TempDir dir;
  const fs::path db = dir.db();
  {
    Store store;
    CHECK(open_ok(db, store) == Status::Ok);
    put_ok(store, "k1", "v1");
    put_ok(store, "k1", "v2");
    put_ok(store, "k2", "v2");
  }
  const Child child = run_child(db, point, {"put", "knew", "vnew"});
  expect_child_crashed(child);
  {
    Store store;
    if (open_ok(db, store) != Status::Ok) {
      return;
    }
    expect_value(store, "k1", "v2");
    expect_value(store, "k2", "v2");
    if (value_should_exist) {
      expect_value(store, "knew", "vnew");
    } else {
      expect_missing(store, "knew");
    }
  }
  reopen_matches(db,
                 value_should_exist ? std::vector<std::pair<std::string, std::string>>{
                                          {"k1", "v2"}, {"k2", "v2"}, {"knew", "vnew"}}
                                    : std::vector<std::pair<std::string, std::string>>{{"k1", "v2"}, {"k2", "v2"}},
                 value_should_exist ? std::vector<std::string>{} : std::vector<std::string>{"knew"});
}

void test_fault_put_before_write() { test_fault_put("record.before_write", false); }
void test_fault_put_partial() { test_fault_put("record.write_partial", false); }
void test_fault_put_before_sync() { test_fault_put("record.before_sync", true); }
void test_fault_put_after_sync() { test_fault_put("record.after_sync", true); }

void test_fault_erase(const char* point, bool key_should_remain) {
  TempDir dir;
  const fs::path db = dir.db();
  {
    Store store;
    CHECK(open_ok(db, store) == Status::Ok);
    put_ok(store, "k", "v");
    put_ok(store, "other", "stay");
  }
  const Child child = run_child(db, point, {"erase", "k"});
  expect_child_crashed(child);
  Store store;
  if (open_ok(db, store) != Status::Ok) {
    return;
  }
  expect_value(store, "other", "stay");
  if (key_should_remain) {
    expect_value(store, "k", "v");
  } else {
    expect_missing(store, "k");
  }
}

void test_fault_erase_partial() { test_fault_erase("record.write_partial", true); }
void test_fault_erase_after_sync() { test_fault_erase("record.after_sync", false); }

void test_fault_compact(const char* point) {
  TempDir dir;
  const fs::path db = dir.db();
  {
    Store store;
    CHECK(open_ok(db, store) == Status::Ok);
    put_ok(store, "k1", "v1");
    put_ok(store, "k1", "v2");
    put_ok(store, "k2", "v2");
    put_ok(store, "k3", "gone");
    CHECK(store.erase("k3") == Status::Ok);
  }
  const Child child = run_child(db, point, {"compact"});
  expect_child_crashed(child);
  {
    Store store;
    if (open_ok(db, store) != Status::Ok) {
      return;
    }
    expect_value(store, "k1", "v2");
    expect_value(store, "k2", "v2");
    expect_missing(store, "k3");
  }
  reopen_matches(db, {{"k1", "v2"}, {"k2", "v2"}}, {"k3"});
}

void test_fault_compact_partial() { test_fault_compact("compact.write_partial"); }
void test_fault_compact_before_install() { test_fault_compact("compact.before_install"); }
void test_fault_compact_after_aside() { test_fault_compact("compact.after_aside"); }
void test_fault_compact_after_install() { test_fault_compact("compact.after_install"); }
void test_fault_compact_before_delete_old() { test_fault_compact("compact.before_delete_old"); }

int worker_main(int argc, char** argv) {
  if (argc < 4) {
    std::fputs("usage: durable_kv_tests --worker <db> put <key> <value> | erase <key> | compact\n", stderr);
    return 2;
  }
  Store store;
  if (Store::open(argv[2], store) != Status::Ok) {
    std::fprintf(stderr, "worker open: %s\n", store.last_error().c_str());
    return 2;
  }
  for (int i = 3; i < argc; ++i) {
    const std::string cmd = argv[i];
    if (cmd == "put") {
      if (i + 2 >= argc) {
        return 2;
      }
      const std::string key = argv[++i];
      const std::string value = argv[++i];
      if (store.put(key, value) != Status::Ok) {
        std::fprintf(stderr, "worker put: %s\n", store.last_error().c_str());
        return 2;
      }
      std::printf("OK put %s %s\n", key.c_str(), value.c_str());
      std::fflush(stdout);
    } else if (cmd == "erase") {
      if (i + 1 >= argc) {
        return 2;
      }
      const std::string key = argv[++i];
      if (store.erase(key) != Status::Ok) {
        std::fprintf(stderr, "worker erase: %s\n", store.last_error().c_str());
        return 2;
      }
      std::printf("OK erase %s\n", key.c_str());
      std::fflush(stdout);
    } else if (cmd == "compact") {
      if (store.compact() != Status::Ok) {
        std::fprintf(stderr, "worker compact: %s\n", store.last_error().c_str());
        return 2;
      }
      std::printf("OK compact\n");
      std::fflush(stdout);
    } else {
      std::fprintf(stderr, "unknown worker command %s\n", cmd.c_str());
      return 2;
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--worker") {
    return worker_main(argc, argv);
  }
  g_exe = fs::absolute(argv[0]);

  run("crc32", test_crc32);
  run("reopen_overwrite_erase", test_reopen_overwrite_erase);
  run("empty_and_binary", test_empty_and_binary);
  run("truncated_tail", test_truncated_tail);
  run("bad_final_checksum", test_bad_final_checksum_keeps_previous_value);
  run("bad_middle_checksum", test_bad_checksum_in_the_middle_is_corruption);
  run("short_writes", test_short_writes);
  run("injected_write", test_injected_write);
  run("injected_sync", test_injected_sync);
  run("injected_partial_write", test_injected_partial);
  run("compact_live_keys", test_compact_rewrites_live_keys);
  run("compact_write_failure", test_compact_write_failure_keeps_store);
  run("incomplete_compact_restores_old", test_incomplete_compact_restores_old);
  run("sealed_compact_installed", test_sealed_compact_is_installed_when_current_is_missing);
  run("torn_current_kept", test_torn_current_is_not_replaced_by_older_backup);
  run("empty_current_restores_backup", test_recordless_current_restores_backup);
  run("sealed_empty_not_replaced", test_sealed_empty_snapshot_is_not_replaced_by_old);
  run("corrupt_current_not_swapped", test_corrupt_current_is_not_swapped_for_old);
  run("incomplete_compact_only", test_only_incomplete_compact_does_not_open_empty);
  run("fault_put_before_write", test_fault_put_before_write);
  run("fault_put_partial", test_fault_put_partial);
  run("fault_put_before_sync", test_fault_put_before_sync);
  run("fault_put_after_sync", test_fault_put_after_sync);
  run("fault_erase_partial", test_fault_erase_partial);
  run("fault_erase_after_sync", test_fault_erase_after_sync);
  run("fault_compact_partial", test_fault_compact_partial);
  run("fault_compact_before_install", test_fault_compact_before_install);
  run("fault_compact_after_aside", test_fault_compact_after_aside);
  run("fault_compact_after_install", test_fault_compact_after_install);
  run("fault_compact_before_delete_old", test_fault_compact_before_delete_old);

  if (g_failed != 0) {
    std::printf("%d failure(s)\n", g_failed);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
