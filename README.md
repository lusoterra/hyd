# hyd

`hyd` is a small, read-only C++ daemon that mirrors the [Hydrus Network](https://hydrusnetwork.github.io/hydrus/index.html) Public Tag Repository and answers Hydrus Client API queries.

You give it the SHA-256 of a file. It tells you which tags the community has already attached to that exact file. It does not store media, does not upload tags, does not petition anything, and never participates in the community database beyond downloading it.

- Daemon binary: **`hydd`**
- Default config: **`/etc/hydd.conf`**
- Default data directory: **`/var/lib/hyd`**
- Default API: **`127.0.0.1:45869`**
- Runs as an unprivileged **`hyd`** user under systemd
- Read-only Hydrus Client API subset (JSON)

## How the Hydrus database is actually distributed

This was the first question, so here is the verified answer:

**The Public Tag Repository (PTR) is centralized, not peer-to-peer.** It is a normal Hydrus server:

- Address: `ptr.hydrus.network:45871`
- Transport: HTTPS
- Access: shared read-only key
  `4a285629721ca442541ef2c15ea17d1f7f7578b0c3f4f5f2a05f8f0ab297786f`
- Sync protocol:
  - `GET /metadata?since=N` returns a zlib-compressed JSON "metadata slice": update indices, update file hashes, and timestamps.
  - `GET /update?update_hash=...` returns each zlib-compressed JSON update file.
  - Authentication is a plain `Hydrus-Key: <hex>` header.
- The PTR stores only file SHA-256 hashes and tag mappings. It does not store files, thumbnails, URLs, notes, ratings, or non-tag metadata.

On 2026-11-21 the live server reported:

| Metric | Value |
| --- | ---: |
| Live mappings | 3,898,076,225 |
| File hashes ever seen | 206,154,404 |
| Tags | 54,975,491 |
| Update indices | 4,721 |
| Update files | 27,566 |
| Tag siblings | ~577,000 |
| Tag parents | ~36,900 |

There is no official "download the database as one file" endpoint. A mirror has to replay the update stream from index 0 and then keep applying new indices.

### Size reality check

- **QuickSync archive (2026-10-29):** 50.6 GB compressed, **160 GB extracted**. This is a full Python Hydrus client database, so the "160 GB uncompressed / ~50 GB compacted" figures you had are accurate.
- **Update stream:** sampling 42 update files across the history suggests roughly **7-8 GB** of compressed downloads for a fresh full sync. This is an estimate; the exact number depends on the repository's current history and is printed by `hydd` as it downloads.
- **hydd custom store:** much smaller than a Python client database because it keeps no update history, no SQLite indices, no file metadata, and no media. A full mirror is expected to land in the **25-30 GB** range. A real partial build of update indices 0-20 produced 9,307 hashes and 16,205 mappings in 588 KiB, which scales to roughly 27 GB at the current PTR counts.
- **hydd SQLite store (`compression = false`):** a plain schema with the same data is expected to be **45-60 GB**, because SQLite B-trees and the hash index cost more per row than the packed custom format.

`hydd` also needs transient disk space for the downloaded update stream during the first build. Plan for roughly **40-45 GB free** for a custom-store first sync (55-70 GB for SQLite), including temporary definition files, then the update cache is deleted (unless `keep_update_cache = true`).

## Storage modes

`hydd` has two backends, selected by `compression`:

| Mode | Setting | Approximate full size | Notes |
| --- | --- | ---: | --- |
| Custom columnar | `compression = true` (default) | 25-30 GB | Sorted hash index, tag-id lists, append-only delta journal |
| Plain SQLite | `compression = false` | 45-60 GB | Ordinary `hashes`/`tags`/`mappings`/`siblings`/`parents` tables; inspect it with `sqlite3` |

Both modes run the same PTR sync engine and answer the same API queries. The SQLite mode is for people who want the "full", straightforward database and don't mind the size; the custom mode is for people who want the compact result. `--compact` merges the custom store's delta journal, or runs `VACUUM` on the SQLite database.

A note on wording: Hydrus does use SQLite, but "the database" is not one file. A Hydrus **client** uses several SQLite files with a client-specific schema (`client.master.db`, `client.mappings.db`, `client.caches.db`, ...), and the QuickSync archive of a fully synced client is the 50.6 GB compressed / 160 GB extracted image mentioned above. The SQLite backend here is a plain mirror schema, not a byte-compatible clone of a Hydrus client or server database. Reading a real Hydrus client DB directly is not implemented. PostgreSQL is not part of Hydrus today and is not implemented here; it is deferred.

Changing `compression` on an existing data directory is refused with a clear error; use a new data directory or switch back.

### Why not just clone the Hydrus SQLite database?

You can, but it is not a single-file database and it is enormous. The QuickSync client image is 160 GB extracted. `hydd`'s SQLite mode gives you a standard, inspectable SQLite database with a small, stable schema and the same sync engine, without forcing you to run a Hydrus client. Byte-compatible Hydrus-schema support (reading an existing client DB or writing the client/server schema) is deliberately not in this version.

## Features

- Resume-safe, incremental PTR synchronisation.
- Read-only immutable columnar store with an append-only delta journal.
- Tag siblings and parents applied to `display_tags`, matching the Python client's behaviour for the data the PTR provides.
- Hydrus-compatible access keys, session keys, error codes, and JSON shapes.
- `systemd` unit, `sysusers.d`/`tmpfiles.d` integration, man pages, bash/zsh/fish completions.
- Two storage modes: a compact custom columnar store (`compression = true`, default) or a plain SQLite mirror (`compression = false`).
- Optional compaction of the custom store's delta journal, enabled by default and safe to disable.
- No upload endpoints, no telemetry, no phoning home except the configured repository.

## Build

Dependencies (Arch names):

```sh
sudo pacman -S --needed cmake ninja gcc curl openssl zlib sqlite nlohmann-json simdjson
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure
sudo cmake --install build
sudo chown root:hyd /etc/hydd.conf && sudo chmod 640 /etc/hydd.conf
```

The install step puts:

- `/usr/bin/hydd`
- `/etc/hydd.conf`
- `/usr/lib/systemd/system/hydd.service`
- `/usr/lib/sysusers.d/hyd.conf`
- `/usr/lib/tmpfiles.d/hyd.conf`
- man pages `hydd(8)` and `hydd.conf(5)`
- shell completions

## Quick start

### User-level test (no root, small on-disk footprint)

```sh
mkdir -p /tmp/hyd-test
cat > /tmp/hyd-test/hydd.conf <<'EOF'
data_dir = /tmp/hyd-test/data
listen_address = 127.0.0.1
listen_port = 45869
access_key = 2222222222222222222222222222222222222222222222222222222222222222
service_name = public tag repository
partitions = 8
# Test only: stop after 100 update indices. Remove this for a real mirror.
max_update_index = 100
log_level = info
EOF

hydd -c /tmp/hyd-test/hydd.conf --sync-once
hydd -c /tmp/hyd-test/hydd.conf --query <sha256>
```

### System daemon

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now hydd
systemctl status hydd
journalctl -u hydd -f

# The key is generated on first start:
sudo -u hyd hydd --config /etc/hydd.conf --print-access-key
```

The first start performs the initial sync in the background. Until it finishes, the API returns `503` with a clear message. Progress is in the journal.

## Configuration

The full commented default file is [`contrib/hydd.conf`](contrib/hydd.conf). Key settings:

| Setting | Default | Meaning |
| --- | --- | --- |
| `data_dir` | `/var/lib/hyd` | Store, state, cache, and generated keys |
| `listen_address` | `127.0.0.1` | API bind address |
| `listen_port` | `45869` | API port |
| `access_key` / `access_key_file` | generated in data dir | API key |
| `service_name` | `public tag repository` | Name reported by the API |
| `repository_url` | `https://ptr.hydrus.network:45871` | Upstream repository |
| `repository_access_key` | shared PTR key | Upstream read-only key |
| `sync_enabled` | `true` | Enable background syncing |
| `sync_on_start` | `true` | Sync immediately at startup |
| `sync_interval_seconds` | `3600` | Metadata check interval |
| `compression` | `true` | `true` = custom columnar store, `false` = plain SQLite |
| `partitions` | `16` | Custom-store partitions (power of two; ignored by SQLite) |
| `compaction_enabled` | `true` | Merge delta journal into base |
| `compact_after_delta_mb` | `512` | Compaction threshold |
| `keep_update_cache` | `false` | Keep downloaded update files |
| `max_download_bytes` | unlimited | Test-only hard download cap |
| `max_update_index` | unlimited | Test-only partial store limit |

### About compaction

The store has two layers:

1. **Base**: immutable, sorted partition files.
2. **Delta**: an append-only journal of newer syncs.

Compaction folds delta records into the base and deletes the journal. It **never discards live tags or mappings**; query results are identical before and after. It keeps startup memory flat and reclaims journal space.

Compaction is **on by default**. Users who want to keep the raw incremental journal can set:

```ini
compaction_enabled = false
```

You can also force a compaction at any time:

```sh
sudo systemctl stop hydd
sudo -u hyd hydd --config /etc/hydd.conf --compact
sudo systemctl start hydd
```

## API compatibility

The daemon implements a read-only subset of the Hydrus Client API. Every JSON response includes `version` (Client API version, currently reported as `97`) and `hydrus_version` (reported as `696`).

### Implemented

| Endpoint | Notes |
| --- | --- |
| `GET /api_version` | No auth |
| `GET /session_key` | 24-hour session keys |
| `GET /verify_access_key` | Reports search permission |
| `GET /client_info` | `boot_id`, `boot_time`, `currently_idle` |
| `GET /get_services` | PTR tag service plus "all known tags" |
| `GET /get_service` | By name or key |
| `GET/POST /get_files/file_metadata` | Hashes, services, storage/display tags |
| `GET/POST /get_files/file_hashes` | SHA-256 to SHA-256 only |
| `GET /health` | hydd extension: build/sync status |

`get_files/file_metadata` supports `hash`/`hashes`, `only_return_identifiers`, `only_return_basic_information`, `include_services_object`, `include_notes`, and `detailed_url_information`. Unknown hashes are returned as `{"file_id": null, "hash": "..."}`, exactly like the Python client.

### Deliberately not implemented

| Endpoint or feature | Behaviour |
| --- | --- |
| `/get_files/search_files` | `501` - no inverted tag search index |
| `/get_files/file`, `/thumbnail`, `/render` | `404` - no media is stored |
| `/add_*`, `/edit_*`, `/manage_*` | `501` - read-only mirror |
| `/request_new_permissions` | `501` |
| CBOR requests | `406` - JSON only |

### Response fields that are unknown

The PTR contains hashes and tags only, so fields such as `size`, `mime`, `width`, `height`, `duration`, `known_urls`, `ratings`, and `notes` are returned as `null` or empty. `file_id` is the PTR's internal hash id and is stable for this store. Tags are returned under the PTR service key and under the synthetic `all known tags` key (`616c6c206b6e6f776e2074616773`), with both `storage_tags` and `display_tags`.

### Example

```sh
KEY=$(hydd --config /etc/hydd.conf --print-access-key)
HASH=4c77267f93415de0bc33b7725b8c331a809a924084bee03ab2f5fae1c6019eb2

curl -s -X POST \
  -H "Content-Type: application/json" \
  -H "Hydrus-Client-API-Access-Key: $KEY" \
  -d "{\"hashes\":[\"$HASH\"]}" \
  http://127.0.0.1:45869/get_files/file_metadata
```


### Parity with the Python client

What matches:

- Endpoint names, JSON field names, SHA-256 parameters, and 401/403/419 error codes.
- `Hydrus-Client-API-Access-Key`, session keys, query/body parameter forms, and CORS headers including `OPTIONS` preflight.
- Unknown hashes returned with `"file_id": null` instead of being omitted.
- Per-service `storage_tags` and `display_tags` objects, with PTR siblings and parents expanded for the display view.
- `version`/`hydrus_version` in JSON responses, legacy arrays in `/get_services`, and the `all known tags` pseudo-service.

What intentionally differs:

- File facts the PTR does not publish (`size`, `mime`, dimensions, URLs, notes, ratings) are `null`/empty.
- No CBOR, no media/thumbnail endpoints, no tag search, and no mutation endpoints.
- The service key is generated locally (as the Python client also does for a repository service), so it is stable per installation rather than globally fixed.
- The SQLite backend's schema is a read-only mirror schema, not the multi-file Hydrus client schema.

## Store layout

```
/var/lib/hyd/
  api_access_key           # generated API key, mode 0600
  service_key              # generated service key
  cache/updates/           # downloaded update files (deleted after build)
  state/
    repository.json        # merged repository metadata
    sync.json              # progress and counters
    update_kinds.json      # definitions/content classification
  store/
    manifest.json
    tags/seg-*.str|.idx|.ids
    p00/ hashes.bin maps.bin
    p01/ ...
    siblings.bin parents.bin
    delta/d-*.bin          # append-only journal
```

With `compression = false` the store is instead:

```
/var/lib/hyd/
  sqlite/hyd.sqlite3       # plain SQLite mirror
  sqlite/hyd.sqlite3-wal
  sqlite/hyd.sqlite3-shm
  state/ cache/ api_access_key service_key
```

The SQLite backend uses ordinary tables (`hashes`, `tags`, `mappings`, `siblings`, `parents`, `meta`) with `mappings` as a `WITHOUT ROWID` primary key on `(hash_id, tag_id)`. `--compact` runs `VACUUM`, which needs roughly as much free space as the database itself.

- `hashes.bin`: sorted `(sha256, hash_id)` entries. This is the query index.
- `maps.bin`: sorted `hash_id` list, offsets, and tag ids.
- Tag segments: sorted tag ids, offsets/lengths, and a string blob.
- The store never keeps a second full copy of every SHA-256.

## Testing

```sh
ctest --test-dir build --output-on-failure
```

The integration test runs a fake Hydrus repository in Python, exercises initial sync, incremental sync, tag deletions, siblings/parents, the API, and compaction. It needs no network.

Real-PTR tests must stay under **10 GiB**. Use `max_download_bytes = 10737418240` and a small `max_update_index`.

## Limitations

- No HTTPS listener and no CBOR. Put a TLS reverse proxy in front for remote access and use JSON clients.
- No tag search. The inverted index for 3.9 billion mappings would be larger than the whole store.
- No file bytes, thumbnails, local files, notes, ratings, or URLs.
- Tag orphan cleanup depends on the PTR's own `delete` records; the mirror reflects the repository as published.
- The initial build is CPU/IO heavy: it replays every update file once for definitions and once per partition for mappings. This is a one-time cost; catch-up syncs are small.

## License

The daemon code is licensed under the **GNU Affero General Public License, version 3 or later** (AGPL-3.0-or-later). See [`LICENSE`](LICENSE).

The tag data is published by the Hydrus community; see the Hydrus project for its terms.
