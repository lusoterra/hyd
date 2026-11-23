# AGENTS.md

Instructions for agents working in this repository.

## Hard limits

- **Test data budget: 10 GiB.** Any test against the real Hydrus PTR (or any
  other remote repository) must not download or process more than 10 GiB in
  total. Use `max_download_bytes = 10737418240` and a small
  `max_update_index` in the test config. Do not run a full sync "just to see if
  it works" on a development machine.
- Do not delete anything outside the repository without asking the user first.

## Build

```sh
cmake -S . -B /tmp/opencode/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/opencode/build -j $(( $(nproc) / 2 ))
ctest --test-dir /tmp/opencode/build --output-on-failure
```

Write build logs to `/tmp/opencode/build.log` when running long builds so the
user can `tail -f` them.

Dependencies: `cmake`, `ninja`, `gcc`, `libcurl`, `openssl`, `zlib`,
`nlohmann-json`, `simdjson`.

## Integration tests

`tests/fake_repository.py` is a self-contained fake Hydrus repository. It
serves the same zlib-compressed serialisable JSON as the real server and needs
no network access. See `tests/integration.sh`.

## Rules of thumb

- The store is immutable base files plus an append-only delta journal. Any new
  sync path must remain crash-safe and idempotent: reapplying a delta must not
  change query results.
- Do not store the full 32-byte sha256 twice. The hash index is the query
  index; mappings are keyed by hash id.
- The API must stay read-only. Never implement an endpoint that mutates the
  repository or the local store in response to a request.
- Keep the default config at `/etc/hydd.conf`, the default data directory at
  `/var/lib/hyd`, and the daemon running as an unprivileged user.
- Compaction is enabled by default. It must never discard live tags or
  mappings; users can disable it with `compaction_enabled = false`.
- The custom columnar store is the only supported backend. Do not reintroduce
  the removed SQLite mirror without a deliberate migration and format design.
- The initial build checkpoints after the definitions pass and after each
  partition. Resume only when the persisted update snapshot, partition count,
  and store format match; otherwise rebuild from scratch. Do not fetch newer
  metadata before finishing a valid checkpoint: apply new indices afterwards
  as an incremental delta. A PTR publication during a long build must not
  invalidate the checkpoint.
- Cache repair may only rewrite files under the update cache. It must never
  modify the query store, and it must stay bounded (no repair loop on every
  request).
- Logging must stay at `/var/log/hyd/hyd.log` by default, rotate at 10 MiB,
  keep 5 gzipped files, and remain configurable.
- Update downloads must reuse TLS connections and stay parallel by default.
  Opening a new connection per update file is a performance bug.
- The project is AGPL-3.0-or-later. Keep new files compatible with that.
