#include "format.hpp"

#include <array>

namespace durable_kv::format {
namespace {

uint32_t read_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void append_u32(std::string& out, uint32_t value) {
  char bytes[4] = {
      static_cast<char>(value & 0xffu),
      static_cast<char>((value >> 8) & 0xffu),
      static_cast<char>((value >> 16) & 0xffu),
      static_cast<char>((value >> 24) & 0xffu),
  };
  out.append(bytes, 4);
}

const std::array<uint32_t, 256>& crc_table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> values{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        if ((crc & 1u) != 0) {
          crc = 0xedb88320u ^ (crc >> 1);
        } else {
          crc >>= 1;
        }
      }
      values[i] = crc;
    }
    return values;
  }();
  return table;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t len) {
  const auto& table = crc_table();
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < len; ++i) {
    crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffu;
}

std::string encode(uint8_t op, std::string_view key, std::string_view value) {
  std::string payload;
  payload.reserve(kMinPayload + key.size() + value.size());
  payload.push_back(static_cast<char>(op));
  append_u32(payload, static_cast<uint32_t>(key.size()));
  append_u32(payload, static_cast<uint32_t>(value.size()));
  payload.append(key.data(), key.size());
  payload.append(value.data(), value.size());

  std::string out;
  out.reserve(kHeaderBytes + payload.size());
  append_u32(out, static_cast<uint32_t>(payload.size()));
  append_u32(out, crc32(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));
  out.append(payload);
  return out;
}

Frame read_record(const uint8_t* data, size_t size, uint64_t offset, Record& out,
                  std::string& error) {
  if (offset > size || size - offset < kHeaderBytes) {
    error = "truncated record header";
    return Frame::Truncated;
  }
  const uint8_t* header = data + offset;
  const uint32_t payload_len = read_u32(header);
  const uint32_t expect_crc = read_u32(header + 4);
  const uint64_t remain = size - offset - kHeaderBytes;
  if (remain < payload_len) {
    error = "truncated record body";
    return Frame::Truncated;
  }

  const auto* payload = data + offset + kHeaderBytes;
  out.next = offset + kHeaderBytes + payload_len;
  const uint32_t actual_crc = crc32(payload, payload_len);
  if (actual_crc != expect_crc) {
    error = "checksum mismatch";
    return Frame::BadChecksum;
  }
  if (payload_len < kMinPayload || payload_len > kMaxPayload) {
    error = "payload length is outside the supported range";
    return Frame::Malformed;
  }

  const uint8_t op = payload[0];
  const uint32_t key_len = read_u32(payload + 1);
  const uint32_t value_len = read_u32(payload + 5);
  const uint64_t body = static_cast<uint64_t>(kMinPayload) + key_len + value_len;
  if (body != payload_len) {
    error = "payload length does not match key and value lengths";
    return Frame::Malformed;
  }
  if (op != kOpPut && op != kOpErase && op != kOpSeal) {
    error = "unknown record type";
    return Frame::Malformed;
  }
  if (op == kOpErase && value_len != 0) {
    error = "erase record contains a value";
    return Frame::Malformed;
  }
  if (op == kOpSeal && (key_len != 0 || value_len != 0)) {
    error = "seal record is not empty";
    return Frame::Malformed;
  }

  out.op = op;
  out.key.assign(reinterpret_cast<const char*>(payload + kMinPayload), key_len);
  out.value.assign(reinterpret_cast<const char*>(payload + kMinPayload + key_len), value_len);
  error.clear();
  return Frame::Ok;
}

}  // namespace durable_kv::format
