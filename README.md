# hyd

`hydd` is a small, read-only C++ daemon that mirrors the Hydrus Public Tag Repository (PTR) and answers Hydrus Client API queries. Give it a file's SHA-256 and it returns the tags the community has attached to that exact file. It stores no media and never uploads tags, petitions, or account data.

- Binary: `hydd`
- Config: `/etc/hydd.conf`
- Data: `/var/lib/hyd`
- Logs: `/var/log/hyd/hyd.log`
- API: `127.0.0.1:45869`
- systemd user: `hyd`
- License: AGPL-3.0-or-later

## Features

- Resumable initial and incremental PTR sync.
- Compact custom store with an append-only delta journal and compaction.
- Tag siblings and parents applied to `display_tags`.
- Hydrus-compatible access keys, session keys, error codes, and JSON shapes.
- Rotating log file with gzip retention.

## Build

Arch Linux dependencies:

```sh
sudo pacman -S --needed cmake ninja gcc curl openssl zlib nlohmann-json simdjson
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure
sudo cmake --install build
sudo chown root:hyd /etc/hydd.conf && sudo chmod 640 /etc/hydd.conf
```

## Configuration

The commented default file is [`contrib/hydd.conf`](contrib/hydd.conf). The main settings:

| Setting | Default | Meaning |
| --- | --- | --- |
| `data_dir` | `/var/lib/hyd` | Store, state, cache, and generated keys |
| `listen_address` / `listen_port` | `127.0.0.1` / `45869` | API listener |
| `access_key` / `access_key_file` | generated | One or more API keys; comma-separated in config, one per line in the file |
| `require_localhost_key` | `false` | Require a valid key even on loopback (disables localhost trust) |
| `repository_url` | `https://ptr.hydrus.network:45871` | Upstream PTR |
| `partitions` | `16` | Custom-store partitions, power of two |
| `compaction_enabled` | `true` | Merge the custom store's delta journal |
| `compact_after_delta_mb` | `512` | Automatic compaction threshold |
| `sync_interval_seconds` | `3600` | Metadata check interval |
| `download_workers` | `8` | Parallel update downloads (1-32) |
| `log_file` | `/var/log/hyd/hyd.log` | Set empty for stderr only |
| `log_rotate_mb` | `10` | Rotate when the plaintext log reaches this size |
| `log_rotate_keep` | `5` | Number of compressed old logs to keep |
| `log_rotate_compress` | `true` | Gzip rotated logs |
| `keep_update_cache` | `false` | Keep downloaded update files |

### Storage

hydd uses a packed columnar store (roughly 25-30 GB for a full mirror) with an append-only delta journal. It is not a byte-compatible Hydrus client or server database; it stores the read-only tag facts hydd's API serves. Use a fresh `data_dir` to rebuild from scratch.

### Logging

Logs go to stderr and, by default, `/var/log/hyd/hyd.log`. When the plaintext log reaches `log_rotate_mb`, it is gzipped to `hyd.log.1.gz`, older files are shifted, and `log_rotate_keep` compressed files are retained:

```sh
tail -f /var/log/hyd/hyd.log
```

systemd also keeps stdout/stderr in the journal.

## API

Implemented read-only endpoints:

| Endpoint | Notes |
| --- | --- |
| `GET /api_version` | No auth |
| `GET /session_key` | 24-hour session keys |
| `GET /verify_access_key` | Reports search permission |
| `GET /client_info` | Boot information |
| `GET /get_services` | PTR service plus "all known tags" |
| `GET /get_service` | By name or key |
| `GET/POST /get_files/file_metadata` | Hashes (SHA-256, legacy SHA-1/MD5), services, storage/display tags |
| `GET/POST /get_files/file_hashes` | SHA-256 to SHA-256 only |
| `GET /health` | hydd extension: build and sync status |
| `GET /stats` | hydd extension: `/health` superset with derived sync estimates |
| `GET /metadata` | Read-only repository relay metadata, with `keep_update_cache = true` |
| `GET /update` | Read-only repository relay update files, with `keep_update_cache = true` |

Every endpoint except `/api_version`, `/health`, `/stats`, and `/` requires an access or session key. JSON only; CBOR returns `406`. Media, thumbnail, tag-search, and mutation endpoints are not implemented. File facts the PTR does not publish (`size`, `mime`, dimensions, URLs, ratings, notes) are returned as `null` or empty. Unknown file hashes are returned as `{"file_id": null, "hash": "..."}`.

Example:

```sh
KEY=$(hydd --config /etc/hydd.conf --print-access-key)
curl -s -X POST \
  -H "Content-Type: application/json" \
  -H "Hydrus-Client-API-Access-Key: $KEY" \
  -d '{"hashes":["4c77267f93415de0bc33b7725b8c331a809a924084bee03ab2f5fae1c6019eb2"]}' \
  http://127.0.0.1:45869/get_files/file_metadata
```

## Notes

- The PTR is a centralized Hydrus server, not peer-to-peer. It publishes SHA-256 hashes, tags, and tag relationships. A small number of very old files only have a legacy SHA-1 (or MD5) hash; hydd indexes and preserves those too, and `file_metadata` accepts 40- and 32-character hexadecimal hashes in addition to 64-character SHA-256.
- If an update contains a serialisable version, definition/content type, action, or metadata version this build does not understand, hydd aborts the sync pass with an error and leaves the existing store untouched instead of silently dropping rows. File-add records are the only known no-op, since they carry no tag data.
- The PTR contains exactly four hash-table values that are not valid SHA-256/SHA-1/MD5 hashes and can never be queried. Those four are skipped and reported as expected, matching what the official Hydrus client does (it stores them as opaque blobs and never resolves them). Any other invalid value still aborts the sync.
- `/health` and `/stats` are hydd extensions and are unauthenticated, like `/api_version`. They expose aggregate counts and sync timing only - no keys, hashes, or tags. `hydd --stats` renders the same data as a human-readable page, including live progress for the one-time initial build (phase, partition, files, bytes, elapsed, ETA), and does not need root or data-directory access. Keep `listen_address = 127.0.0.1` unless you accept publishing those counts.
- With `keep_update_cache = true` and a complete cache, hydd also serves the read-only repository protocol endpoints `/metadata?since=N` and `/update?update_hash=...`. They require a configured API key (or loopback trust), serve the exact cached update files, and can be used to bootstrap another hydd or a Hydrus client. They never accept uploads or petitions. If the cache is incomplete the metadata endpoint returns 503; `/update` verifies the file's sha256 before serving it. The custom store is not needed for this relay, but keeping it gives fast local query answers.
- The raw update cache is the archival copy of the PTR stream. It is already zlib-compressed by the server, so it is about 21 GiB for the current corpus, smaller than a compressed Hydrus client database (the client schema has much more overhead and file metadata). A custom store plus the raw cache is roughly 26 GiB + 21 GiB; `keep_update_cache = false` deletes the cache after a successful sync.
- Loopback TCP peers skip the API key check by default, and a bogus key is ignored too, so local tools work without `sudo -u hyd`. Set `require_localhost_key = true` to require a valid key from localhost as well. The check uses the socket peer, not `X-Forwarded-For`. If a public reverse proxy connects to hydd from localhost, loopback trust makes the read-only API public through that proxy.
- `access_key` accepts one key or a comma-separated list; `access_key_file` accepts one key per line, with blank lines and `#` comments ignored. Removing a key and restarting revokes it (sessions live in memory, so they are cleared by the restart). `hydd --print-access-key` prints every configured key.
- `contrib/hyd` (installed as `/usr/bin/hyd`) is a small helper: `hyd FILE` hashes with SHA-256 by default and queries its tags, `hyd --sha1 FILE` and `hyd --md5 FILE` select a legacy hash, `hyd HASH` queries a hash directly, and `hyd stats`, `hyd scan`, `hyd verify`, `hyd version` cover the rest. Set `HYD_BIN` or `HYD_CONFIG` to override the defaults. `hydd --api-query HASH` is the underlying API-backed lookup.
- `hydd --scan-updates` re-parses every cached update file, verifies its sha256 against the cache filename, and reports all format failures at once. Run it after a large download and before a long build, or after a Hydrus update.
- Hydrus uses SQLite, but a Hydrus client database is split across several files with a client-specific schema and carries file metadata and history; hydd's store is a smaller read-only mirror schema, not a byte-compatible client or server database. PostgreSQL is not implemented.
- At the time of writing the PTR has about 4.18 billion mappings, 206 million SHA-256 hashes, and 55 million tags. A full initial sync downloads roughly 21 GiB of already-compressed update files; keep extra free space for the store being built.
- The first store build is CPU and I/O heavy because it replays every update file once for definitions and once per partition for mappings. Incremental syncs are small.

## Testing

`ctest` runs unit tests plus fake-repository end-to-end tests for the custom store, tag deletes, siblings/parents, the API, the repository relay, and compaction. No network is required. Real-PTR tests must stay under 10 GiB; see [AGENTS.md](AGENTS.md).

## License

AGPL-3.0-or-later. See [`LICENSE`](LICENSE).
