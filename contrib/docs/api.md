# hydd API reference

`hydd` serves a read-only subset of the Hydrus Client API plus a few `hydd`
extension endpoints under `/hyd/`. Addresses and keys in the examples:

```sh
BASE=http://127.0.0.1:45869
KEY=$(hydd --config /etc/hydd.conf --print-access-key | head -n 1)
```

The API is plain HTTP on loopback by default. JSON is the only supported
response format; CBOR requests are rejected with `406`.

## Authentication

Every endpoint except `GET /`, `GET /api_version` and `GET /health`
requires a key. Send one of:

- `Hydrus-Client-API-Access-Key: <key>` header;
- `Hydrus-Client-API-Session-Key: <session>` header after `GET /session_key`;
- `Hydrus-Key: <key>` header (the repository protocol spelling, also accepted);
- the same name in the query string, e.g.
  `?Hydrus-Client-API-Access-Key=<key>`;
- the same name as a JSON body field.

Loopback peers skip the key check by default, and a bogus key is ignored too.
Set `require_localhost_key = true` to require a valid key locally.

`GET /session_key` returns a random 64-character session key that is valid for
24 hours and is renewed on use. At most 32 live sessions exist per configured
access key; a full session table returns `429`.

## Errors

| Status | Meaning |
| --- | --- |
| `400` | Malformed parameters, bad hash or tag, or too many values |
| `401` | No key provided |
| `403` | Invalid key or session |
| `404` | Unknown endpoint, disabled feature, or no such hash/tag |
| `405` | Method other than `GET` or `POST` |
| `406` | CBOR was requested; only JSON is supported |
| `419` | Expired session key |
| `429` | Rate limit or session table full; `Retry-After` is set |
| `500` | Internal error or corrupt cached relay file |
| `501` | Hydrus mutation endpoint not implemented |
| `503` | Initial build in progress, feature build in progress, or too many connections |

Text errors are `text/plain; charset=utf-8`. Unknown endpoints return JSON:

```json
{"version":97,"hydrus_version":696,"error":"unknown endpoint","path":"/no-such-endpoint"}
```

The old top-level `/stats` path returns a `404` hint; use `/hyd/stats`.
`/health` keeps the standard Hydrus spelling and is unauthenticated.

Parameters may be sent as a query string, a JSON body (`Content-Type:
application/json`), or both. Query values are parsed as JSON when possible, so
`?tags=["a","b"]` sends an array. Repeated query keys keep the last value; use a
JSON array or a POST body for lists.

## GET /

Unauthenticated HTML landing page: the project name and a link to
<https://github.com/lusoterra/hyd>.

## GET /api_version

Unauthenticated.

```json
{"version":97,"hydrus_version":696,"hyd_version":"0.8.0"}
```

## GET /health

Unauthenticated build and sync status, at the standard Hydrus path.
`GET /hyd/stats` returns the same body but requires a key.

```sh
curl -s "$BASE/health"
```

Fields:

- `version`, `hydrus_version`, `hyd_version` - API, Hydrus, and hydd versions.
- `service_name` - configure with `service_name`.
- `built`, `partial`, `next_index`, `next_update_due`, `last_sync_at`,
  `delta_generation` - sync state persisted by the daemon.
- `sync_enabled`, `sync_interval_seconds`, `next_check_at`,
  `data_age_seconds`, `update_overdue_seconds`,
  `estimated_update_applied_at` - scheduling information.
- `store_open`, `hash_count`, `mapping_count`, `tag_count` - counts reported by
  the last sync pass.
- `hashes`, `mappings`, `tags`, `siblings`, `parents`, `legacy_hashes`,
  `sha256_hashes`, `partitions`, `delta_records`, `delta_hashes`, `backend` -
  live store totals, present when the store is open.
- `build` - present while an initial build runs: `active`, `phase`
  (`definitions`, `partitions`, `idle`), `partition`, `partitions`,
  `files_done`, `files_total`, `bytes_done`, `bytes_total`, `hashes`,
  `mappings`, `percent`, `bytes_percent`, `started_at`, `updated_at`,
  `elapsed_seconds`, `phase_elapsed_seconds`, `eta_seconds`.
- `features` - `seeding.enabled`, `tag_counts.{enabled,ready,delta_applied,
  defined_tags,bytes,as_of}`, `tag_hashes.{enabled,ready,bytes,as_of}`. No keys,
  hashes, or tags are exposed.

## GET /hyd/stats

Authenticated. Identical body to `/health`.

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" "$BASE/hyd/stats"
```

## GET /session_key

Authenticated. Returns a 24-hour session key.

```json
{"version":97,"hydrus_version":696,"session_key":"<64 hex characters>"}
```

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" "$BASE/session_key"
```

## GET /verify_access_key

Authenticated. Reports the key's permissions.

```json
{
  "version": 97,
  "hydrus_version": 696,
  "name": "hydd",
  "permits_everything": false,
  "basic_permissions": [3],
  "human_description": "hydd read-only PTR mirror: search for and fetch files"
}
```

## GET /client_info

Authenticated. Boot identifier and timestamp; the mirror is always idle.

```json
{"version":97,"hydrus_version":696,"boot_id":"<64 hex>","boot_time":1764000000.0,"currently_idle":true}
```

## GET /get_services

Authenticated. Lists the tag repository and the "all known tags" domain.

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" "$BASE/get_services"
```

Local file service lists are empty arrays because the mirror stores no files.
`tag_repositories` and `all_known_tags` contain the two services, and
`services`/`services_v2` repeat them in the same shapes as
`/get_files/file_metadata`.

## GET /get_service

Authenticated. Look up one service by `service_name` or `service_key`.

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/get_service?service_name=all%20known%20tags"
```

```json
{"version":97,"hydrus_version":696,"service":{"name":"all known tags","type":10,"type_pretty":"combined tag domain","service_key":"616c6c206b6e6f776e2074616773"}}
```

An unknown name or key returns `404`.

## GET/POST /get_files/file_metadata

Authenticated. Looks up tags for up to 1000 hashes per request. Accepts SHA-256
(64 hex), legacy SHA-1 (40 hex), and legacy MD5 (32 hex).

Parameters:

- `hash` or `hashes` - a string or an array of strings. Duplicates are removed.
- `only_return_identifiers` (default `false`) - return only `file_id` and
  `hash`.
- `only_return_basic_information` (default `false`) - return identifiers plus
  the (empty) file facts, without tags or viewing statistics.
- `include_notes` (default `false`) - add an empty `notes` object.
- `detailed_url_information` (default `false`) - add an empty
  `detailed_known_urls` array.
- `include_services_object` (default `true`) - add top-level `services` and
  `services_v2`.

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/get_files/file_metadata?hashes=%5B%224c77267f93415de0bc33b7725b8c331a809a924084bee03ab2f5fae1c6019eb2%22%5D"
```

```sh
curl -s -H "Content-Type: application/json" \
  -H "Hydrus-Client-API-Access-Key: $KEY" \
  -d '{"hashes":["4c77267f93415de0bc33b7725b8c331a809a924084bee03ab2f5fae1c6019eb2"]}' \
  "$BASE/get_files/file_metadata"
```

A known file returns a row with the real `file_id`, `hash` (SHA-256), the
storage and display tags, and PTR-unavailable facts set to `null`/`false`/empty:

```json
{
  "file_id": 12345,
  "hash": "4c77267f93415de0bc33b7725b8c331a809a924084bee03ab2f5fae1c6019eb2",
  "size": null,
  "mime": null,
  "filetype_human": null,
  "filetype_enum": null,
  "ext": null,
  "width": null,
  "height": null,
  "duration": null,
  "num_frames": null,
  "num_words": null,
  "has_audio": false,
  "filetype_forced": false,
  "blurhash": null,
  "pixel_hash": null,
  "file_services": {"current": {}, "deleted": {}},
  "time_modified": null,
  "time_modified_details": {},
  "is_inbox": false,
  "is_local": false,
  "is_trashed": false,
  "is_deleted": false,
  "has_transparency": false,
  "has_exif": false,
  "has_xmp": false,
  "has_iptc": false,
  "has_human_readable_embedded_metadata": false,
  "has_software_source": false,
  "has_icc_profile": false,
  "known_urls": [],
  "ipfs_multihashes": {},
  "ratings": {},
  "tags": {
    "<service key>": {
      "name": "public tag repository",
      "type": 0,
      "type_pretty": "hydrus tag repository",
      "storage_tags": {"0": ["creator:someone", "series:something"]},
      "display_tags": {"0": ["creator:someone", "series:something"]}
    },
    "616c6c206b6e6f776e2074616773": {
      "name": "all known tags",
      "type": 10,
      "type_pretty": "combined tag domain",
      "storage_tags": {"0": ["creator:someone", "series:something"]},
      "display_tags": {"0": ["creator:someone", "series:something"]}
    }
  },
  "file_viewing_statistics": [
    {"canvas_type": 0, "canvas_type_pretty": "media viewer", "views": 0, "viewtime": 0.0, "last_viewed_timestamp": null},
    {"canvas_type": 1, "canvas_type_pretty": "preview viewer", "views": 0, "viewtime": 0.0, "last_viewed_timestamp": null},
    {"canvas_type": 4, "canvas_type_pretty": "client api viewer", "views": 0, "viewtime": 0.0, "last_viewed_timestamp": null}
  ]
}
```

An unknown or deleted hash returns `{"file_id": null, "hash": "<as sent>"}`.
Storage tags are the exact PTR tags; display tags have siblings and parents
applied. The PTR publishes no file facts, so `size`, `mime`, dimensions, URLs,
ratings, and notes are always empty or `null`.

Invalid hashes, more than 1000 hashes, or malformed `hashes` values return
`400`.

## GET/POST /get_files/file_hashes

Authenticated. Translates hashes. Because the mirror stores only SHA-256, both
`source_hash_type` and `desired_hash_type` must be `sha256` (the defaults).

Parameters:

- `hashes` or `hash` - a string or array of SHA-256 hashes, up to 1000.
- `source_hash_type`, `desired_hash_type` - must be `sha256`.

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/get_files/file_hashes?hashes=%5B%224c77267f93415de0bc33b7725b8c331a809a924084bee03ab2f5fae1c6019eb2%22%5D"
```

```json
{"version":97,"hydrus_version":696,"hashes":{"4c77267f93415de0bc33b7725b8c331a809a924084bee03ab2f5fae1c6019eb2":"4c77267f93415de0bc33b7725b8c331a809a924084bee03ab2f5fae1c6019eb2"}}
```

Only known, non-deleted hashes are mapped. Unsupported hash types return `400`.

## GET/POST /hyd/tag_counts

Authenticated extension. Exact current mapping count for each storage tag.
Requires `tag_counts_enabled = true` (the default).

Parameters:

- `tag` or `tags` - a string or array of strings, up to 1000 tags, each at
  most 4096 bytes.

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/hyd/tag_counts?tags=series:alpha"
```

```sh
curl -s -H "Content-Type: application/json" \
  -H "Hydrus-Client-API-Access-Key: $KEY" \
  -d '{"tags":["series:alpha","creator:gamma","no:such-tag"]}' \
  "$BASE/hyd/tag_counts"
```

```json
{
  "counts": {"series:alpha": 2, "creator:gamma": 3},
  "unknown": ["no:such-tag"],
  "as_of": 1764000000,
  "last_sync_at": 1764000100
}
```

Counts are base counts plus the pending delta journal, so they are exact for the
running store. No sibling or parent expansion is applied. `as_of` is when the
base counts file was written; `last_sync_at` is `null` before the first sync.
Undefined tags are listed in `unknown`. A disabled feature returns `404`, a
store still building counts returns `503`.

## GET/POST /hyd/tag_hashes

Authenticated extension. Pages SHA-256 hashes for a tag query. Requires
`tag_hashes_enabled = true` (default `false`) and `tag_counts_enabled = true`.

Parameters:

- `tag` or `tags` - a string or array of strings. Multiple positive tags are
  **ANDed**. A leading `-` excludes that tag (AND NOT). At most 16 entries and
  at least one positive tag. There is no OR syntax. Tags are matched exactly;
  no sibling or parent expansion.
- `offset` - page offset, default `0`, maximum `10000000`.
- `after` - cursor: return hashes after this internal hash id. Mutually
  exclusive with `offset` and has no depth cap.
- `limit` - page size, `1` to `1000`, default `100`.

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/hyd/tag_hashes?tag=series:alpha&limit=100"

curl -s -H "Content-Type: application/json" \
  -H "Hydrus-Client-API-Access-Key: $KEY" \
  -d '{"tags":["series:touhou","-character:reimu"],"limit":100}' \
  "$BASE/hyd/tag_hashes"
```

```json
{
  "tags": ["series:touhou"],
  "excluded": ["character:reimu"],
  "offset": 0,
  "limit": 100,
  "total": null,
  "hashes": ["<sha256>", "<sha256>"],
  "next_offset": 100,
  "next_after": 123456
}
```

`total` is exact for a single positive tag with no exclusions; it is `null` for
multi-tag or negated queries because the intersection size is not stored.
`next_offset` is `null` on the last page, and `next_after` is the cursor for the
next page. An offset or cursor past the end returns an empty page. An undefined
tag returns `404`; a disabled feature returns `404`; a still-building index
returns `503`.

## GET /metadata

Authenticated repository relay. Requires `keep_update_cache = true` and a
complete cache. Serves the exact Hydrus repository metadata stream so another
`hydd` can sync from this mirror. The official Hydrus client also downloads the
whole repository, so the tag data model fits; it additionally needs
`/service_info`, repository session cookies, and account endpoints that this
read-only mirror does not implement yet. See [compatibility.md](compatibility.md).

Parameter: `since` - non-negative update index, default `0`.

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/metadata?since=0" --output metadata.bin
```

The body is zlib-compressed serialisable JSON
(`application/octet-stream`). A missing cache or disabled relay returns `404`;
an incomplete cache returns `503` and starts a repair pass.

## GET /update

Authenticated repository relay. Requires `keep_update_cache = true`. Serves one
cached, already zlib-compressed update file.

Parameter: `update_hash` - 64-character SHA-256 of the update.

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/update?update_hash=<sha256>" --output update.bin
```

The file is re-hashed before serving. A missing `update_hash` or invalid hash
returns `400`, an uncached file `404`, and a corrupt file `500` (it is removed
and queued for re-download). The body is `application/octet-stream`.

## Not implemented

Known Hydrus Client API endpoints that this mirror deliberately does not
serve return `501` with a JSON body so a client can tell a missing feature from
a mistyped URL:

- `GET /get_files/file`, `/get_files/thumbnail`, `/get_files/render`,
  `/get_files/file_seed` - the mirror stores no media.
- `/add_*`, `/edit_*`, `/manage_*`, `/get_files/search_files`,
  `/request_new_permissions`, `/commit_pending` - the mirror is read-only.

```json
{
  "version": 97,
  "hydrus_version": 696,
  "error": "not implemented",
  "path": "/get_files/file",
  "reason": "the mirror stores tags only; files and thumbnails are not served"
}
```

Anything else returns `404` JSON with `error` and `path`:

## Limits

- At most 1000 hashes per metadata/hashes request.
- At most 1000 tags per tag-count request; at most 16 entries per reverse
  lookup.
- At most `max_connections` concurrent sockets (`503` beyond that).
- Rate limiting per access key or peer address: `429` with `Retry-After`.
- At most 32 live sessions per configured access key.

## See also

- [compatibility.md](compatibility.md) - Hyd vs the Hydrus PTR, client, and Client API
- [examples.md](examples.md) - runnable curl and helper recipes
- `man hydd` - daemon options and deployment
- `man hyd` - helper client
- `man hydd.conf` - configuration settings
