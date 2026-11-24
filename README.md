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

- Resumable initial and incremental PTR sync, with a checkpoint after the definitions pass and after each partition.
- Compact custom store with an append-only delta journal and compaction.
- Read-only repository relay (`/metadata`, `/update`) from a complete update cache.
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
| `repository_url` | `https://ptr.hydrus.network:45871` | Upstream Hydrus repository; must use `https` (plain `http` is accepted only on loopback) |
| `partitions` | `16` | Custom-store partitions, power of two |
| `compaction_enabled` | `true` | Merge the custom store's delta journal |
| `compact_after_delta_mb` | `512` | Automatic compaction threshold |
| `sync_interval_seconds` | `3600` | Metadata check interval |
| `download_workers` | `8` | Parallel update downloads (1-32) |
| `max_download_bytes` | `68719476736` (64 GiB) | Abort a sync after this many downloaded bytes; `0` means unlimited |
| `log_file` | `/var/log/hyd/hyd.log` | Set empty for stderr only |
| `log_rotate_mb` | `10` | Rotate when the plaintext log reaches this size |
| `log_rotate_keep` | `5` | Number of compressed old logs to keep |
| `log_rotate_compress` | `true` | Gzip rotated logs |
| `keep_update_cache` | `false` | Keep downloaded update files (also enables the relay and cache repair) |
| `repair_enabled` | `true` | Verify cached update files on sync and heal corruption automatically |
| `tag_counts_enabled` | `true` | Build and serve `/hydd/tag_counts` (adds ~4 B per defined tag plus ~8 B per tag for the name index) |
| `tag_counts_rebuild_after_sync` | `false` | Force a full count rebuild after every sync (usually unnecessary: the delta journal is applied in memory) |
| `tag_hashes_enabled` | `false` | Build and serve `/hydd/tag_hashes` (reverse tag search; adds ~18 GB: 4 B per mapping plus 8 B per hash) |
| `max_connections` | `128` | Concurrent API connections before answering 503 |
| `rate_limit_enabled` | `true` | Per-client token bucket on API requests |
| `rate_limit_requests_per_minute` | `600` | Sustained request rate per key or peer address |
| `rate_limit_burst` | `120` | Token bucket size |
| `rate_limit_local_bypass` | `false` | Skip the bucket for loopback peers (disable this behind a reverse proxy, which also looks like loopback) |

### Repository trust

`hydd` treats `repository_url` as its source of truth. Syncing requires `https`; a plain `http` upstream is refused unless the host is loopback, so a local test repository still works. TLS is the only transport authentication the Hydrus repository ecosystem provides, so do not disable it with a reverse proxy or a plain-HTTP upstream.

Within that channel:

- every update file is checked against the SHA-256 listed in the repository metadata before it is cached or used;
- cached files are re-hashed before being skipped, and a corrupt entry is deleted and downloaded again;
- malformed updates, unknown serialisable versions/types/actions, out-of-range ids, invalid metadata indices, and oversized responses abort the sync pass instead of being skipped silently.

That is integrity checking, not authenticity. The repository metadata is not signed, so a host you point at can choose which hashes and tags are "correct". If you sync from the official PTR you are trusting the PTR over TLS. If you point `repository_url` at another `hydd`, or at any other Hydrus server, you are trusting that operator with the complete contents of your mirror. The relay is a transport mirror, not a trust anchor: a client that syncs from your `hydd` applies the same checks against the metadata your `hydd` serves. The shared PTR access key is public and is an account credential, not a secret or a way to verify the server.

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
| `GET /hydd/health` | hydd extension: build and sync status |
| `GET /hydd/stats` | hydd extension: `/hydd/health` superset with derived sync estimates |
| `GET/POST /hydd/tag_counts` | hydd extension: current mapping count per storage tag |
| `GET /hydd/tag_hashes` | hydd extension: paged SHA-256 hashes for one storage tag (`tag_hashes_enabled = true`) |
| `GET /metadata` | Read-only repository relay metadata, with `keep_update_cache = true` |
| `GET /update` | Read-only repository relay update files, with `keep_update_cache = true` |

Every endpoint except `/api_version`, `/hydd/health`, and `/` requires an access or session key. `/hydd/health` stays unauthenticated for Hydrus parity; `/hydd/stats` (the richer hydd superset) requires authorization, but loopback trust means local `hyd --stats` still works with no flags. The old top-level `/health` and `/stats` paths were moved under `/hydd/` in 0.8.0 and now return 404. JSON only; CBOR returns `406`. Media, thumbnail, tag-search, and mutation endpoints are not implemented. File facts the PTR does not publish (`size`, `mime`, dimensions, URLs, ratings, notes) are returned as `null` or empty. Unknown file hashes are returned as `{"file_id": null, "hash": "..."}`. At most 1000 hashes per metadata/hashes request and at most 1000 tags per tag-count request; at most `max_connections` concurrent API connections; at most 32 live sessions per configured access key (429 beyond that).

Example:

```sh
KEY=$(hydd --config /etc/hydd.conf --print-access-key)
curl -s -X POST \
  -H "Content-Type: application/json" \
  -H "Hydrus-Client-API-Access-Key: $KEY" \
  -d '{"hashes":["4c77267f93415de0bc33b7725b8c331a809a924084bee03ab2f5fae1c6019eb2"]}' \
  http://127.0.0.1:45869/get_files/file_metadata

# Tag amounts (exact storage tags, no sibling expansion)
curl -s -X POST \
  -H "Content-Type: application/json" \
  -H "Hydrus-Client-API-Access-Key: $KEY" \
  -d '{"tags":["character:hatsune miku","series:touhou"]}' \
  http://127.0.0.1:45869/hydd/tag_counts

# Reverse lookup, one page (requires tag_hashes_enabled = true).
# Multiple tags are ANDed; a leading '-' excludes.
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  -H 'Content-Type: application/json' \
  -d '{"tags":["series:touhou","-character:reimu"]}' \
  'http://127.0.0.1:45869/hydd/tag_hashes?limit=100'
```

`/hydd/tag_counts` answers with `{"counts": {...}, "unknown": [...], "as_of": <unix>, "last_sync_at": <unix|null>}`. Values are the number of current mappings for the exact storage tag: the base counts plus the pending delta journal, so they are exact as of the running store. `as_of` records when the base counts file was written. `unknown` lists tags that are not defined in the repository at all.

`/hydd/tag_hashes` answers with `{"tags": [...], "excluded": [...], "offset": N, "limit": M, "total": T|null, "hashes": [...], "next_offset": N2|null, "next_after": ID|null}`. Pass `tags` as a string or an array: multiple positive tags are **ANDed**, and an entry starting with `-` is **excluded** (AND NOT). At most 16 tags per request and at least one positive tag; exclusion-only queries are refused. There is no OR syntax. Pages are 1-1000 hashes (default 100). Offset paging is capped at 10,000,000 entries per request; for anything beyond that (or for cheap deep paging) pass `after=<next_after>`, which jumps directly to the next hash id and has no cap. `offset` and `after` are mutually exclusive. `total` is exact for a single positive tag with no exclusions, and `null` for multi-tag or negated queries because the intersection size is not stored. An offset or cursor past the end returns an empty page rather than an error.

## What hydd is and is not

`hydd` is a read-only PTR mirror. It is not a Hydrus client or a replacement PTR server.

It does:

- download the PTR metadata and update stream;
- build a compact local tag index for SHA-256 and legacy SHA-1/MD5 hashes;
- answer the read-only Client API subset documented above;
- optionally relay the exact cached update stream through `/metadata` and `/update`.

It does not:

- store media, thumbnails, or files;
- implement file search, tag editing, uploads, petitions, or any mutation;
- implement Hydrus accounts, registration, account types, permissions, bandwidth/rate limits, janitor review, or voting/petitions;
- keep the full Hydrus client or server schema, so its data directory cannot be dropped into Hydrus;
- terminate TLS.

The PTR is a Hydrus server with no real website of its own. The official Python client is the UI. Reading is available through a shared read-only access key; uploading tags, deletions, and sibling/parent changes require an account and janitor approval. There is no upvote system - "voting" in Hydrus means mapping/parent/sibling petitions that janitors approve or deny. That account-and-client requirement, plus permissions, rate limits, and janitor review, is the main spam control. `hydd` uses only the read-only public key and never writes.

A read-only relay is still good for decentralisation: it serves the same update files as the source of truth, so it mirrors rather than forks. The PTR remains authoritative.

## Notes

- The PTR is a centralized Hydrus server, not peer-to-peer. It publishes SHA-256 hashes, tags, and tag relationships. A small number of very old files only have a legacy SHA-1 (or MD5) hash; hydd indexes and preserves those too, and `file_metadata` accepts 40- and 32-character hexadecimal hashes in addition to 64-character SHA-256.
- If an update contains a serialisable version, definition/content type, action, or metadata version this build does not understand, hydd aborts the sync pass with an error and leaves the existing store untouched instead of silently dropping rows. File-add records are the only known no-op, since they carry no tag data.
- The PTR contains exactly four hash-table values that are not valid SHA-256/SHA-1/MD5 hashes and can never be queried. Those four are skipped and reported as expected, matching what the official Hydrus client does (it stores them as opaque blobs and never resolves them). Any other invalid value still aborts the sync. The raw cache and `/metadata`/`/update` relay preserve the four exactly as the PTR served them; only hydd's derived query index omits them.
- `/hydd/health` is unauthenticated (Hydrus parity). `/hydd/stats` requires authorization and exposes the same aggregate counts plus sync timing, build progress, store totals, and a `features` block: `seeding` (enabled), `tag_counts` (enabled, ready, defined tags, bytes, as_of) and `tag_hashes` (enabled, ready, bytes, as_of) - no keys, hashes, or tags. `hyd stats` renders the same feature states. `hydd --stats` sends your configured keys automatically (or `HYDD_API_KEY`), so it works unprivileged locally and against `require_localhost_key = true` hosts. Keep `listen_address = 127.0.0.1` unless you accept publishing `/hydd/health`. Both paths moved under `/hydd/` in 0.8.0; the old top-level spellings return 404.
- `tag_counts_enabled = true` (the default) makes the build and every compaction write `store/counts.bin` and `store/tag_lookup.bin`. Counts are 4 bytes per defined tag id, the name index 8 bytes per defined tag, so together about 660 MB at 55 million tags on top of the 25-30 GB store. A store built before 0.8.0 starts building them in the background on the first start (the API answers 503 from `/hydd/tag_counts` until it finishes), or explicitly with `hydd --rebuild-tag-counts`. Counts cover exact storage tags only: no sibling or parent expansion, and they include the pending delta journal. `hydd --check-tag-counts` recounts and verifies the files without writing (it reads the live store, so run it while a compaction is not in flight for a guaranteed-consistent result); `hydd --tag-counts TAG` queries the running API.
- Incremental syncs do not need a recount: the delta journal is applied to the base counts in memory when the store is reopened, so new mappings and deletions show up immediately. Compaction rewrites the base counts inline while it already walks every partition. `tag_counts_rebuild_after_sync = true` forces a full recount after each sync anyway; it is off by default because it re-reads the whole store.
- `tag_hashes_enabled = true` adds the reverse index: `store/tag_postings.bin` (4 bytes per mapping, ~17 GB at 4.18B mappings) and `store/hash_index.bin` (8 bytes per hash, ~1.6 GB at 206M hashes). It is built only when enabled, during the initial build, compaction, or the background upgrade build, and requires `tag_counts_enabled = true`. Every request returns one bounded page (1-1000 hashes), and results include the pending delta.
- Requests are rate limited per access key (or peer address when unauthenticated): `rate_limit_requests_per_minute` sustained with `rate_limit_burst` headroom, answering 429 with `Retry-After`. Loopback is not exempt by default (`rate_limit_local_bypass = false`), because a reverse proxy makes public traffic look local. `max_connections` bounds concurrent sockets separately.
- With `keep_update_cache = true` and a complete cache, hydd also serves the read-only repository protocol endpoints `/metadata?since=N` and `/update?update_hash=...`. They require a configured API key (or loopback trust), serve the exact cached update files, and can be used to bootstrap another hydd or a Hydrus client. They never accept uploads or petitions. If the cache is incomplete the metadata endpoint returns 503; `/update` verifies the file's sha256 before serving it. The custom store is not needed for this relay, but keeping it gives fast local query answers.
- Cached update files are self-healing while syncing is enabled. `/update` reports a corrupt file back to the sync thread, which re-downloads just that file; `/metadata` reporting an incomplete cache triggers a full verification pass (at most once per ten minutes). The same check runs before every initial build or resume. `repair_enabled = false` disables the automatic pass. `hydd --repair-cache` runs the full verification and refill explicitly, and `hydd --scan-updates` only reports format and hash problems without downloading. Repair never modifies the query store.
- The raw update cache is the archival copy of the PTR stream. It is already zlib-compressed by the server, so it is about 21 GiB for the current corpus, smaller than a compressed Hydrus client database (the client schema has much more overhead and file metadata). A custom store plus the raw cache is roughly 26 GiB + 21 GiB; `keep_update_cache = false` deletes the cache after a successful sync.
- Cached update files are hash-addressed and verified with SHA-256 before being skipped on a sync; a corrupt entry is deleted and downloaded again. `/update` verifies the file before serving it and removes it on mismatch. A build that fails on malformed content still reports the file; run `hydd --scan-updates` to find every bad entry at once.
- Loopback TCP peers skip the API key check by default, and a bogus key is ignored too, so local tools work without `sudo -u hyd`. Set `require_localhost_key = true` to require a valid key from localhost as well. The check uses the socket peer, not `X-Forwarded-For`. If a public reverse proxy connects to hydd from localhost, loopback trust makes the read-only API public through that proxy.
- `access_key` accepts one key or a comma-separated list; `access_key_file` accepts one key per line, with blank lines and `#` comments ignored. Removing a key and restarting revokes it (sessions live in memory, so they are cleared by the restart). `hydd --print-access-key` prints every configured key.
- `contrib/hyd` (installed as `/usr/bin/hyd`) is a small helper: `hyd FILE` hashes with SHA-256 by default and queries its tags, `hyd --sha1 FILE` and `hyd --md5 FILE` select a legacy hash, `hyd HASH` queries a hash directly, `hyd counts TAG...` asks the running API for mapping counts, `hyd hashes [--offset N] [--limit N] [--after ID] TAG...` pages the reverse index (tags are ANDed, `-tag` excludes), `hyd hashes-after TAG ID [LIMIT]` continues from a `next_after`, `hyd rebuild-counts` rebuilds the derived tag metadata (escalating with doas/sudo if the data dir is root-owned), `hyd check` verifies the store and the derived counts, and `hyd stats`, `hyd scan`, `hyd repair`, `hyd verify`, `hyd version` cover the rest. Set `HYD_BIN` or `HYD_CONFIG` to override the defaults. `hydd --api-query HASH`, `hydd --tag-counts TAG` and `hydd --tag-hashes TAG` are the underlying API-backed lookups.
- `hydd --scan-updates` re-parses every cached update file, verifies its sha256 against the cache filename, and reports all format failures at once. Run it after a large download and before a long build, or after a Hydrus update.
- Hydrus uses SQLite, but a Hydrus client database is split across several files with a client-specific schema and carries file metadata and history; hydd's store is a smaller read-only mirror schema, not a byte-compatible client or server database.
- At the time of writing the PTR has about 4.18 billion mappings, 206 million SHA-256 hashes, and 55 million tags. A full initial sync downloads roughly 21 GiB of already-compressed update files; keep extra free space for the store being built.
- The first store build is CPU and I/O heavy because it replays every update file once for definitions and once per partition for mappings. Incremental syncs are small. The build writes a checkpoint after the definitions pass and after each partition; if it is interrupted, the next start finishes that same update snapshot and only then applies newer indices as an incremental delta. A publication that happens during a long build or while the daemon is stopped therefore does not invalidate the checkpoint. A changed update snapshot, partition count, or store format still forces a rebuild.

## Testing

`ctest` runs unit tests plus fake-repository end-to-end tests for the custom store, tag deletes, siblings/parents, the API, the repository relay, and compaction. No network is required. Real-PTR tests must stay under 10 GiB; see [AGENTS.md](AGENTS.md).

## License

AGPL-3.0-or-later. See [`LICENSE`](LICENSE).
