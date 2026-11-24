# hydd examples

Runnable recipes against a local mirror. Start from:

```sh
BASE=http://127.0.0.1:45869
KEY=$(hydd --config /etc/hydd.conf --print-access-key | head -n 1)
```

Set `HYDD_API_KEY` instead of `KEY` if you prefer the helper's environment
variable. `curl -s` is assumed; add `-i` to see status codes and headers. Some
recipes use `jq` to read JSON fields.

## Check the daemon

```sh
curl -s "$BASE/api_version"
curl -s "$BASE/health"
```

The daemon is ready for file queries once `/health` reports `"built": true`.
While an initial build runs, `file_metadata` and the tag extensions answer
`503`.

## Authenticate

Loopback requests are trusted by default. For a remote or
`require_localhost_key = true` instance, send the key:

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" "$BASE/verify_access_key"
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" "$BASE/client_info"
```

Get a 24-hour session key and reuse it:

```sh
SESSION=$(curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/session_key" | jq -r .session_key)
curl -s -H "Hydrus-Client-API-Session-Key: $SESSION" "$BASE/get_services"
```

## Hashing a file

SHA-256 is the primary hash. The helper computes it for you:

```sh
hyd photo.jpg
```

Legacy hashes still work for very old PTR files:

```sh
hyd --sha1 photo.jpg
hyd --md5 photo.jpg
```

By hand:

```sh
sha256sum photo.jpg
```

## Look up tags for a file

Single hash via the query string:

```sh
HASH=4c77267f93415de0bc33b7725b8c331a809a924084bee03ab2f5fae1c6019eb2
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/get_files/file_metadata?hash=$HASH"
```

Several hashes via POST:

```sh
curl -s -H "Content-Type: application/json" \
  -H "Hydrus-Client-API-Access-Key: $KEY" \
  -d '{"hashes":["4c77267f93415de0bc33b7725b8c331a809a924084bee03ab2f5fae1c6019eb2"]}' \
  "$BASE/get_files/file_metadata"
```

Just identifiers:

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/get_files/file_metadata?hash=$HASH&only_return_identifiers=true"
```

Print only the display tags for the repository service:

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/get_files/file_metadata?hash=$HASH" \
  | jq -r '.metadata[0].tags[] | select(.type == 0) | .display_tags["0"][]'
```

An unknown hash is not an error:

```json
{"file_id": null, "hash": "<the hash you sent>"}
```

## Translate hashes

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/get_files/file_hashes?hashes=%5B%22$HASH%22%5D"
```

Only SHA-256 is stored, so leave both hash types at their `sha256` defaults.

## Services

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" "$BASE/get_services"
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/get_service?service_name=public%20tag%20repository"
```

`all known tags` is an aggregate tag domain; the repository service is the one
that stores the tags.

## Count mappings per tag

GET with one tag:

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/hyd/tag_counts?tags=series:alpha"
```

GET with a JSON array:

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/hyd/tag_counts?tags=%5B%22series:alpha%22%2C%22creator:gamma%22%5D"
```

POST with several tags:

```sh
curl -s -H "Content-Type: application/json" \
  -H "Hydrus-Client-API-Access-Key: $KEY" \
  -d '{"tags":["series:alpha","creator:gamma","no:such-tag"]}' \
  "$BASE/hyd/tag_counts"
```

```json
{"counts":{"series:alpha":2,"creator:gamma":3},"unknown":["no:such-tag"],"as_of":1764000000,"last_sync_at":1764000100}
```

Use the helper for a one-line-per-tag table:

```sh
hyd counts series:alpha creator:gamma
```

## Reverse lookup: files for a tag

Requires `tag_hashes_enabled = true`.

One page:

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/hyd/tag_hashes?tag=series:alpha&limit=100"
```

Tags are ANDed; `-tag` excludes:

```sh
curl -s -H "Content-Type: application/json" \
  -H "Hydrus-Client-API-Access-Key: $KEY" \
  -d '{"tags":["series:touhou","-character:reimu"],"limit":100}' \
  "$BASE/hyd/tag_hashes"
```

Walk every page with the cursor (cheap deep paging, no offset cap):

```sh
after=
while :; do
  if [ -n "$after" ]; then
    page=$(curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
      "$BASE/hyd/tag_hashes?tag=series:alpha&limit=1000&after=$after")
  else
    page=$(curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
      "$BASE/hyd/tag_hashes?tag=series:alpha&limit=1000")
  fi
  printf '%s\n' "$page" | jq -r '.hashes[]'
  after=$(printf '%s\n' "$page" | jq -r '.next_after // empty')
  [ -n "$after" ] || break
done
```

Walk with offsets instead:

```sh
offset=0
while :; do
  page=$(curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
    "$BASE/hyd/tag_hashes?tag=series:alpha&limit=1000&offset=$offset")
  printf '%s\n' "$page" | jq -r '.hashes[]'
  offset=$(printf '%s\n' "$page" | jq -r '.next_offset // empty')
  [ -n "$offset" ] || break
done
```

The helper wraps the same API:

```sh
hyd hashes series:alpha
hyd hashes --limit 1000 --offset 2000 series:alpha
hyd hashes --after 123456 series:alpha
hyd hashes-after series:alpha 123456 1000
```

## Statistics

Human-readable:

```sh
hyd stats
```

Raw JSON, including feature readiness:

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" "$BASE/hyd/stats" \
  | jq '{built, hashes, mappings, tags, features}'
```

`/health` is the unauthenticated build/sync status; `/hyd/stats` returns the
same body but requires a key.

## Bootstrap another mirror from this cache

Requires `keep_update_cache = true` and a complete cache.

Fetch the metadata stream, then any update it lists:

```sh
curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/metadata?since=0" --output metadata.bin

curl -s -H "Hydrus-Client-API-Access-Key: $KEY" \
  "$BASE/update?update_hash=<sha256-from-metadata>" --output update.bin
```

Both bodies are the exact zlib-compressed repository stream and can be fed to a
second `hydd` by pointing its `repository_url` at this instance. The official
Hydrus client needs repository login and account endpoints that Hyd does not
implement yet, so use another `hydd` (or wait for client support). They are
read-only: uploads and petitions are never accepted.

## Error handling

```sh
# 401 without a key when require_localhost_key = true
curl -s -o /dev/null -w '%{http_code}\n' "$BASE/hyd/stats"

# JSON 404 for an unknown endpoint
curl -s "$BASE/no-such-endpoint"

# Known but unsupported Hydrus endpoints return 501 JSON
curl -s "$BASE/get_files/file"
curl -s "$BASE/add_tags/add_tags"

# 406 for CBOR
curl -s -o /dev/null -w '%{http_code}\n' "$BASE/api_version?cbor=1"

# Rate-limited requests return 429 with Retry-After; inspect the status
curl -s -o /dev/null -w '%{http_code}\n' \
  -H "Hydrus-Client-API-Access-Key: $KEY" "$BASE/api_version"
```

## Helper client

`hyd` is installed next to `hydd` and uses the running API for query commands:

```sh
hyd photo.jpg                 # hash with SHA-256 and print its tags
hyd 4c77267f...               # query a hash directly
hyd --sha1 old.jpg            # use SHA-1
hyd stats                     # daemon and store statistics
hyd counts series:alpha       # mapping count
hyd hashes series:alpha       # reverse lookup, first page
hyd check                     # verify the store and derived counts
```

`hyd version` prints the daemon version. `HYD_BIN` overrides the `hydd` path and
`HYD_CONFIG` overrides `/etc/hydd.conf`. See `man hyd`.

## See also

- [api.md](api.md) - full endpoint reference
- [compatibility.md](compatibility.md) - Hyd vs the Hydrus PTR, client, and Client API
- `man hyd` - helper client
- `man hydd` - daemon and options
- `man hydd.conf` - configuration settings
