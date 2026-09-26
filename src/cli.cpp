#include "durable_kv/store.hpp"

#include <cstdio>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

void usage() {
  std::fputs(
      "usage: durable-kv <file> put <key> <value>\n"
      "       durable-kv <file> get <key>\n"
      "       durable-kv <file> erase <key>\n"
      "       durable-kv <file> compact\n"
      "\n"
      "Keys and values are one argument each. The library stores raw bytes;\n"
      "this tool passes argv strings through unchanged. get writes the value\n"
      "to stdout with no added newline.\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  const std::string_view command = argv[2];
  durable_kv::Store store;
  const auto opened = durable_kv::Store::open(argv[1], store);
  if (opened != durable_kv::Status::Ok) {
    std::fprintf(stderr, "open: %s (%s)\n", durable_kv::to_string(opened), store.last_error().c_str());
    return 2;
  }

  if (command == "put") {
    if (argc != 5) {
      usage();
      return 2;
    }
    const auto status = store.put(argv[3], argv[4]);
    if (status != durable_kv::Status::Ok) {
      std::fprintf(stderr, "put: %s (%s)\n", durable_kv::to_string(status), store.last_error().c_str());
      return 2;
    }
    return 0;
  }
  if (command == "get") {
    if (argc != 4) {
      usage();
      return 2;
    }
    std::string value;
    const auto status = store.get(argv[3], value);
    if (status == durable_kv::Status::NotFound) {
      std::fputs("not found\n", stderr);
      return 1;
    }
    if (status != durable_kv::Status::Ok) {
      std::fprintf(stderr, "get: %s (%s)\n", durable_kv::to_string(status), store.last_error().c_str());
      return 2;
    }
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::fwrite(value.data(), 1, value.size(), stdout);
    return 0;
  }
  if (command == "erase") {
    if (argc != 4) {
      usage();
      return 2;
    }
    const auto status = store.erase(argv[3]);
    if (status != durable_kv::Status::Ok) {
      std::fprintf(stderr, "erase: %s (%s)\n", durable_kv::to_string(status), store.last_error().c_str());
      return 2;
    }
    return 0;
  }
  if (command == "compact") {
    if (argc != 3) {
      usage();
      return 2;
    }
    const auto status = store.compact();
    if (status != durable_kv::Status::Ok) {
      std::fprintf(stderr, "compact: %s (%s)\n", durable_kv::to_string(status),
                   store.last_error().c_str());
      return 2;
    }
    return 0;
  }

  usage();
  return 2;
}
