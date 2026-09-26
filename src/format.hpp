#pragma once

// On-disk record (little-endian):
//
//   u32 payload_len
//   u32 crc32          CRC-32/ISO-HDLC of the payload bytes only
//   u8  op             1 put, 2 erase (tombstone), 3 seal
//   u32 key_len
//   u32 value_len
//   u8  key[key_len]
//   u8  value[value_len]
//
// payload_len is the number of bytes after the crc word.
// It must equal 9 + key_len + value_len.
//
// A live log is put and erase records. Compaction writes the live puts and
// then a seal. open() will install a compact file only when that seal is the
// last valid record, so a rewrite that stopped between records is left alone.
// Puts and erases after a seal are normal updates.

#include <cstdint>
#include <string>
#include <string_view>

namespace durable_kv::format {

constexpr uint8_t kOpPut = 1;
constexpr uint8_t kOpErase = 2;
constexpr uint8_t kOpSeal = 3;

constexpr uint32_t kHeaderBytes = 8;
constexpr uint32_t kMinPayload = 9;
constexpr uint32_t kMaxKeyOrValue = 32u * 1024u * 1024u;
constexpr uint32_t kMaxPayload = kMinPayload + kMaxKeyOrValue + kMaxKeyOrValue;

uint32_t crc32(const uint8_t* data, size_t len);

std::string encode(uint8_t op, std::string_view key, std::string_view value);

enum class Frame {
  Ok,
  Truncated,    // header or body runs past the end of the buffer
  BadChecksum,  // payload was all there, crc did not match
  Malformed,    // payload was all there, crc matched, fields are illegal
};

struct Record {
  uint8_t op = 0;
  std::string key;
  std::string value;
  // First byte after this record, once the payload was fully present.
  uint64_t next = 0;
};

// `data` is the whole log. `offset` is the start of one record.
// Truncated leaves `out.next` alone. BadChecksum and Malformed set it to
// the first byte after the payload.
Frame read_record(const uint8_t* data, size_t size, uint64_t offset, Record& out,
                  std::string& error);

}  // namespace durable_kv::format
