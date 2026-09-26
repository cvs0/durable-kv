# durable-kv

A single-process, single-writer key-value store for small applications. The log is the database. `put`, `get`, and `erase` are the data API; `compact` rewrites the log so it holds only live keys.

Keys and values are raw bytes. An empty value is stored data. A missing key is not.

## Build and test

```
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The CLI is `build/Debug/durable-kv` on a Visual Studio generator (or `build/durable-kv` with Ninja):

```
durable-kv data.kv put hello world
durable-kv data.kv get hello
durable-kv data.kv erase hello
durable-kv data.kv compact
```

`get` writes the value to stdout with no extra newline and no text-mode translation. `put` / `erase` / `compact` exit 0 on success. `get` exits 1 when the key is absent. Anything else exits 2.

## When `put` and `erase` succeed

`put` returns Ok only after all three of these have happened, in order:

1. The full record was written. Short writes are retried. A write error stops here.
2. The OS file-data barrier returned success (`FlushFileBuffers` on Windows, `fdatasync` on Linux, `F_FULLFSYNC` on macOS).
3. The in-memory index was updated to the new value.

`erase` of a key that is already absent returns Ok and writes nothing: the key is already gone, including after a crash. `erase` of a present key appends a tombstone under the same three steps, then removes the key from the index.

A process crash after Ok still shows that operation on the next open. A crash before Ok may or may not show it. In particular, if the process is killed after the barrier and before Ok is returned to the caller, the record is already durable and the next open will apply it.

On any failed write or barrier, the index is left unchanged. The implementation then truncates the log back to the last offset that completed the barrier and issues that barrier again. If the truncate or its barrier fails, the store closes and must be reopened; it does not keep serving an index that disagrees with a log it could not roll back.

No path uses `std::fstream`, `fflush`, or `fsync` on a `FILE*`. Those calls are not this store's durability barrier.

## Power loss

Process crash (the OS keeps running) is the guarantee above. Sudden power loss is weaker, and it is not the same call on every OS.

| OS | File-data barrier used | What it does not promise |
| --- | --- | --- |
| Windows | `FlushFileBuffers` on a handle opened with `FILE_FLAG_WRITE_THROUGH` | A drive that ignores flush commands can still lose the record. `FILE_FLAG_NO_BUFFERING` is not used. |
| Linux | `fdatasync` on the log file | Same hardware limit. `fdatasync` covers file data and the size needed to read it back, not unrelated metadata such as timestamps. |
| macOS | `F_FULLFSYNC` on the log file | `fsync` alone is not treated as success. If `F_FULLFSYNC` fails, the operation returns an I/O error. |

Creating the file and publishing a compacted log also try to flush the parent directory (`FlushFileBuffers` on a directory handle opened with `FILE_FLAG_BACKUP_SEMANTICS`, or `fsync` / `F_FULLFSYNC` of the directory). That is what makes the file name itself durable across power loss. If the directory flush fails, `put` and `erase` still succeed after the file-data barrier: a process crash cannot drop them, but power loss might lose a newly created file's directory entry. Compaction will not delete the previous log unless that directory flush succeeds, so a crash still has a complete file under one of the names below.

A kernel panic is closer to power loss than to killing the process. Records that have passed the barrier are as durable as the OS and the drive's flush behavior. Records that have not passed it are not acknowledged.

## File format

The log is a sequence of records. Integers are little-endian. The checksum is CRC-32/ISO-HDLC (the `123456789` → `0xCBF43926` polynomial), over the payload only.

```
u32 payload_len
u32 crc32
u8  op            1 put, 2 erase, 3 seal
u32 key_len
u32 value_len
u8  key[key_len]
u8  value[value_len]
```

`payload_len` is the number of bytes after the checksum and must equal `9 + key_len + value_len`. An erase has an empty value. A seal has an empty key and value. Ordinary logs contain puts and erases. A compaction file ends with a seal so a snapshot that stopped on a record boundary is not mistaken for a finished rewrite.

Limits are 32 MiB per key and per value.

## Recovery

On open the whole log is read and scanned from the start. Each valid record updates an in-memory map: put replaces the value, erase removes the key, seal changes nothing. The map is the index; the log is not consulted again until the next open.

The scan stops at the first record it cannot use:

- The header or body runs past the end of the file: that tail is incomplete. It is dropped.
- The checksum does not match and the record ends at the end of the file: that final record is dropped.
- The checksum does not match and more bytes follow, or a full record has a matching checksum but illegal contents: open returns corruption and does not modify the file. Earlier keys are not served from a log with a hole in it.

A dropped tail is truncated away and the same file-data barrier is issued, so the next append starts at the last valid byte.

Compaction is several renames, so open also looks at sibling files:

| `path` | `path.old` | `path.compact` | Action |
| --- | --- | --- | --- |
| missing | present | sealed, scan succeeds | Install the compact file as `path`. |
| missing | present | missing or not a finished seal | Delete an unfinished compact file and rename `.old` back to `path`. |
| missing | missing | sealed | Install it. |
| missing | missing | unfinished or unreadable | Return an error and leave the file. An empty store is not created in its place. |
| present | anything | anything | Open `path` if it scans. A leftover `.compact` is removed only after that scan succeeds. |

A zero-length `path` next to `.old` or `.compact` is not treated as a real log: a finished snapshot always contains a seal. A non-empty `path` whose scan yields no records, while `.old` exists, is parked and `.old` is restored. A `path` that scans, including one whose only damage is a torn tail, is kept even if `.old` holds different bytes. If `path` is corrupt in the middle and `.old` exists, open fails and both files stay where they are.

`.old` is deleted only after `path` has scanned successfully and the directory flush has succeeded.

## Compaction

`compact` writes the live map into a new file in the same directory. Tombstones and overwritten values are omitted.

1. Create `path.compact` and write one put per live key, then a seal.
2. Run the file-data barrier on `path.compact` and close it. `path` is still the live log.
3. Rename `path` to `path.old`.
4. Rename `path.compact` to `path`.
5. Flush the parent directory.
6. Delete `path.old` only if that flush succeeded.
7. Reopen `path` for later appends.

If the process stops in the middle, the next open uses the table above. The previous log is not deleted before the new file is in place and the directory flush has returned. A failed compaction in-process deletes `path.compact` and leaves the open store on the original log.

## Limitations

- One process, one writer. A second process appending to the same files will corrupt the log. The store is not internally synchronized.
- Every acknowledged `put` or `erase` is its own barrier. There is no group commit.
- Open reads the entire log into memory. Compact to keep that small. This is not a database for large data sets.
- Recovery will not skip a damaged record in the middle of the file.
- CRC-32 detects torn writes and bit flips; it is not an authenticity check.
- Power loss durability ends at the drive. A disk that reports a flush before data is stable can still lose an acknowledged operation.
- If the directory flush is unavailable, `.old` is left behind after compaction. The live file is still `path`.
- There is no networking, SQL, transactions, range scan, or encryption.

## Test hooks

These exist so the tests can fail I/O and kill the process. Leave them unset in normal use.

- `DURABLE_KV_CRASH_AT=name` or `name:N` calls `_Exit(97)` on the Nth hit (default 1). Names: `record.before_write`, `record.write_partial`, `record.before_sync`, `record.after_sync`, `compact.write_partial`, `compact.before_install`, `compact.after_aside`, `compact.after_install`, `compact.before_delete_old`.
- `DURABLE_KV_INJECT=write`, `write_partial`, `sync`, or `compact_write` fails that step once.
- `DURABLE_KV_MAX_WRITE=N` caps each OS write at N bytes so short writes are exercised.
