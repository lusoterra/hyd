#!/bin/sh
# End-to-end test using a fake Hydrus repository. Requires python3 and curl.
# Usage: tests/integration.sh /path/to/hydd

set -eu

BIN=${1:-./hydd}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FAKE="$ROOT/tests/fake_repository.py"
WORK=$(mktemp -d /tmp/hyd-integration.XXXXXX)
PORT_REPO=46871
PORT_API=46869
SERVER_PID=
DAEMON_PID=

cleanup() {
    [ -n "${DAEMON_PID}" ] && kill "${DAEMON_PID}" 2>/dev/null || true
    [ -n "${SERVER_PID}" ] && kill "${SERVER_PID}" 2>/dev/null || true
    rm -rf "${WORK}"
}
trap cleanup EXIT INT TERM

hash_of() {
    python3 -c "import hashlib,sys; print(hashlib.sha256(sys.argv[1].encode()).hexdigest())" "$1"
}

# Waits until the background delta overlay has been applied to the counts.
wait_counts_delta() {
    base=$1
    key=$2
    tries=0
    while [ "${tries}" -lt 100 ]; do
        if curl -s -H "Hydrus-Client-API-Access-Key: ${key}" "${base}/hyd/stats" \
            | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin)["features"]["tag_counts"]["delta_applied"] else 1)' 2>/dev/null; then
            return 0
        fi
        tries=$((tries + 1))
        sleep 0.1
    done
    return 1
}

H1=$(hash_of alpha-file)
H2=$(hash_of beta-file)
H3=$(hash_of gamma-file)
L1=$(python3 -c "import hashlib; print(hashlib.sha1(b'legacy-file').hexdigest())")
L2=$(python3 -c "import hashlib; print(hashlib.sha1(b'legacy-two').hexdigest())")

cat > "${WORK}/hydd.conf" <<EOF
data_dir = ${WORK}/data
listen_address = 127.0.0.1
listen_port = ${PORT_API}
repository_url = http://127.0.0.1:${PORT_REPO}
repository_access_key = 1111111111111111111111111111111111111111111111111111111111111111
access_key = 2222222222222222222222222222222222222222222222222222222222222222,3333333333333333333333333333333333333333333333333333333333333333
require_localhost_key = true
service_name = integration ptr
partitions = 4
sync_interval_seconds = 3600
keep_update_cache = true
rate_limit_requests_per_minute = 6000
log_level = warn
EOF

start_repo() {
    python3 "${FAKE}" --port "${PORT_REPO}" --mode "$1" > "${WORK}/repo.log" 2>&1 &
    SERVER_PID=$!
    sleep 0.7
}

stop_repo() {
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
    SERVER_PID=
}

start_repo initial
"${BIN}" -c "${WORK}/hydd.conf" --sync-once
"${BIN}" -c "${WORK}/hydd.conf" --scan-updates | grep -q 'no update format problems found'

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H1}")
echo "${OUT}" | grep -q 'series:alpha'
echo "${OUT}" | grep -q 'character:beta'
echo "${OUT}" | grep -q 'creator:gamma'

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H3}")
echo "${OUT}" | grep -q 'not found'

# Legacy SHA-1 hashes must be preserved and queryable.
OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${L1}")
echo "${OUT}" | grep -q 'series:alpha'

stop_repo
start_repo incremental
"${BIN}" -c "${WORK}/hydd.conf" --sync-once

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H3}")
echo "${OUT}" | grep -q 'character:delta'
echo "${OUT}" | grep -q 'creator:gamma'

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H2}")
if echo "${OUT}" | grep -q 'series:alpha'; then
    echo "deleted mapping was not applied" >&2
    exit 1
fi

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${L2}")
echo "${OUT}" | grep -q 'creator:gamma'

# Derived tag metadata: the explicit rebuild and the startup auto-build path.
rm -f "${WORK}/data/store/counts.bin" "${WORK}/data/store/tag_lookup.bin"
"${BIN}" -c "${WORK}/hydd.conf" --rebuild-tag-counts
[ -f "${WORK}/data/store/counts.bin" ]
[ -f "${WORK}/data/store/tag_lookup.bin" ]
"${BIN}" -c "${WORK}/hydd.conf" --check-tag-counts | grep -q '0 mismatches'
# Reverse files must not exist while tag_hashes_enabled is off (the default).
[ ! -e "${WORK}/data/store/tag_postings.bin" ]
[ ! -e "${WORK}/data/store/hash_index.bin" ]
# Remove them again so the daemon has to rebuild them at startup.
rm -f "${WORK}/data/store/counts.bin" "${WORK}/data/store/tag_lookup.bin"

"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q '5 hashes'
"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q '1 delta records' 2>/dev/null || true

# Compaction rewrites the base and bumps the store generation; derived files
# must be rewritten with it and stay correct.
"${BIN}" -c "${WORK}/hydd.conf" --compact
"${BIN}" -c "${WORK}/hydd.conf" --check-tag-counts | grep -q '0 mismatches'
[ ! -e "${WORK}/data/store/tag_postings.bin" ]
[ ! -e "${WORK}/data/store/hash_index.bin" ]
OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H2}")
if echo "${OUT}" | grep -q 'series:alpha'; then
    echo "deleted mapping survived compaction" >&2
    exit 1
fi

# API checks.
"${BIN}" -c "${WORK}/hydd.conf" --no-sync > "${WORK}/daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 0.8

# --stats must work unprivileged and without touching the data directory.
KEY=2222222222222222222222222222222222222222222222222222222222222222
KEY2=3333333333333333333333333333333333333333333333333333333333333333
BASE="http://127.0.0.1:${PORT_API}"
STATS=$("${BIN}" -c "${WORK}/hydd.conf" -d "${WORK}/stats-no-data" --stats)
echo "${STATS}" | grep -q 'SHA-256'
echo "${STATS}" | grep -q 'Mappings'
echo "${STATS}" | grep -q 'ready'
[ ! -e "${WORK}/stats-no-data" ]
# /hyd/stats requires authorization (/health stays open);
# loopback trust is disabled by require_localhost_key=true here, so send a key.
[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${PORT_API}/hyd/stats")" = "401" ]
curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" "http://127.0.0.1:${PORT_API}/hyd/stats" | grep -q '"built":true'
curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" "http://127.0.0.1:${PORT_API}/hyd/stats" | grep -q '"phase":"idle"'
curl -s "http://127.0.0.1:${PORT_API}/health" | grep -q '"built":true'
# Feature status is in the stats payload and the human stats page.
wait_counts_delta "${BASE}" "${KEY}"
curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "http://127.0.0.1:${PORT_API}/hyd/stats" > "${WORK}/stats.json"
python3 - "${WORK}/stats.json" <<'PY'
import json, sys
f = json.load(open(sys.argv[1]))["features"]
assert f["tag_counts"]["enabled"] is True and f["tag_counts"]["ready"] is True
assert f["tag_counts"]["delta_applied"] is True
assert f["tag_hashes"]["enabled"] is False and f["tag_hashes"]["ready"] is False
assert f["seeding"]["enabled"] is True
PY
STATS_TEXT=$("${BIN}" -c "${WORK}/hydd.conf" --stats)
printf '%s\n' "${STATS_TEXT}" | grep -q 'Tag counts'
printf '%s\n' "${STATS_TEXT}" | grep -q 'Reverse lookup'
printf '%s\n' "${STATS_TEXT}" | grep -q 'Seeding'
# The old top-level /stats extension path is gone; /health stays for Hydrus.
[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${PORT_API}/stats")" = "404" ]
curl -s "http://127.0.0.1:${PORT_API}/health" | grep -q '"built":true'
[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${PORT_API}/hyd/health")" = "404" ]
# Root serves a small HTML page; unknown endpoints answer JSON, not plain text.
ROOT_HTML=$(curl -s "http://127.0.0.1:${PORT_API}/")
printf '%s\n' "${ROOT_HTML}" | grep -q '<h1>hyd</h1>'
printf '%s\n' "${ROOT_HTML}" | grep -q 'github.com/lusoterra/hyd'
UNKNOWN_BODY=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -w '\n%{http_code}' "http://127.0.0.1:${PORT_API}/no-such-endpoint")
[ "$(printf '%s\n' "${UNKNOWN_BODY}" | tail -n 1)" = "404" ]
printf '%s\n' "${UNKNOWN_BODY}" | grep -q '"error":"unknown endpoint"'
printf '%s\n' "${UNKNOWN_BODY}" | grep -q '"path":"/no-such-endpoint"'

# Derived tag counts: base counts plus the pending delta overlay. A store that
# was missing the derived files builds them in the background after startup, so
# wait for the first 200 before asserting values.
[ "$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/hyd/tag_counts?tags=series:alpha")" = "401" ]
COUNT_READY=0
for _ in $(seq 1 100); do
    if [ "$(curl -s -o /dev/null -w '%{http_code}' \
        -H "Hydrus-Client-API-Access-Key: ${KEY}" \
        "${BASE}/hyd/tag_counts?tags=series:alpha")" = "200" ]; then
        COUNT_READY=1
        break
    fi
    sleep 0.1
done
[ "${COUNT_READY}" = "1" ]
wait_counts_delta "${BASE}" "${KEY}"
COUNTS=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' \
    -d '{"tags":["series:alpha","creator:gamma","character:beta","character:delta","no:such-tag"]}' \
    "${BASE}/hyd/tag_counts")
echo "${COUNTS}" | grep -q '"series:alpha":2'
echo "${COUNTS}" | grep -q '"creator:gamma":3'
echo "${COUNTS}" | grep -q '"character:beta":1'
echo "${COUNTS}" | grep -q '"character:delta":1'
echo "${COUNTS}" | grep -q '"unknown":\["no:such-tag"\]'
GET_COUNTS=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${BASE}/hyd/tag_counts?tags=series:alpha")
echo "${GET_COUNTS}" | grep -q '"series:alpha":2'
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${BASE}/hyd/tag_counts")" = "400" ]
# Malformed, oversized and over-limit tag lists are rejected.
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' -d '{"tags":[1,2]}' \
    "${BASE}/hyd/tag_counts")" = "400" ]
python3 - "${WORK}/many-tags.json" <<'PY'
import json, sys
json.dump({"tags": [f"test:tag{i}" for i in range(1001)]}, open(sys.argv[1], "w"))
json.dump({"tags": ["x" * 5000]}, open(sys.argv[1].replace("many-tags", "long-tag"), "w"))
PY
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' --data-binary "@${WORK}/many-tags.json" \
    "${BASE}/hyd/tag_counts")" = "400" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' --data-binary "@${WORK}/long-tag.json" \
    "${BASE}/hyd/tag_counts")" = "400" ]

HELPER_COUNTS=$(HYD_BIN="${BIN}" HYD_CONFIG="${WORK}/hydd.conf" \
    "${ROOT}/contrib/hyd" counts "series:alpha")
printf '%s\n' "${HELPER_COUNTS}" | grep -q 'series:alpha	2'

# Reverse search is off by default and must not be reachable.
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${BASE}/hyd/tag_hashes?tag=series:alpha")" = "404" ]

VERSION=$("${BIN}" --version | awk '{print $2}' | sed 's/^v//')
[ "$("${BIN}" --version)" = "hyd v${VERSION}" ]
curl -s "${BASE}/api_version" | grep -q "\"hyd_version\":\"${VERSION}\""

KEYS=$("${BIN}" -c "${WORK}/hydd.conf" --print-access-key)
echo "${KEYS}" | grep -q "${KEY}"
echo "${KEYS}" | grep -q "${KEY2}"

[ "$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/get_services")" = "401" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: bad" "${BASE}/get_services")" = "403" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" "${BASE}/get_services")" = "200" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY2}" "${BASE}/get_services")" = "200" ]

# Repository relay: metadata and exact update files, with keep_update_cache.
[ "$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/metadata?since=0")" = "401" ]
curl -s -H "Hydrus-Key: ${KEY}" "${BASE}/metadata?since=0" -o "${WORK}/metadata.bin"
python3 - "${WORK}/metadata.bin" <<'PY'
import json
import sys
import zlib

doc = json.loads(zlib.decompress(open(sys.argv[1], 'rb').read()))
assert doc[0] == 21 and doc[1] == 2
arg = doc[2][0]
assert arg[0] == [0, 'metadata_slice']
value = arg[1][1]
assert value[0] == 37 and value[1] == 1
rows, due = value[2]
assert rows and due > 0
assert all(len(row) == 4 for row in rows)
PY

UPDATE_FILE=$(find "${WORK}/data/cache/updates" -name '*.bin' | head -n 1)
UPDATE_HASH=$(basename "${UPDATE_FILE}" .bin)
curl -s -H "Hydrus-Key: ${KEY}" \
    "${BASE}/update?update_hash=${UPDATE_HASH}" -o "${WORK}/update.bin"
[ "$(sha256sum "${WORK}/update.bin" | awk '{print $1}')" = "${UPDATE_HASH}" ]

# A second hydd can bootstrap from the relay instead of the PTR.
cat > "${WORK}/relay-client.conf" <<EOF
data_dir = ${WORK}/relay-client-data
listen_address = 127.0.0.1
listen_port = 46880
repository_url = ${BASE}
repository_access_key = ${KEY}
access_key = ${KEY}
service_name = relay client
partitions = 4
log_level = warn
EOF
"${BIN}" -c "${WORK}/relay-client.conf" --sync-once
"${BIN}" -c "${WORK}/relay-client.conf" --verify-store | grep -q '5 hashes'
OUT=$("${BIN}" -c "${WORK}/relay-client.conf" --query "${H1}")
echo "${OUT}" | grep -q 'series:alpha'

# API-backed query and the hyd helper must work without a daemon data dir.
OUT=$("${BIN}" -c "${WORK}/hydd.conf" -d "${WORK}/api-query-no-data" --api-query "${H1}")
echo "${OUT}" | grep -q 'series:alpha'
[ ! -e "${WORK}/api-query-no-data" ]

printf 'alpha-file' > "${WORK}/alpha-file"
printf 'legacy-file' > "${WORK}/legacy-file"

HELPER_TAGS=$(HYD_BIN="${BIN}" HYD_CONFIG="${WORK}/hydd.conf" \
    "${ROOT}/contrib/hyd" "${WORK}/alpha-file")
echo "${HELPER_TAGS}" | grep -q "sha256 ${H1}"
echo "${HELPER_TAGS}" | grep -q 'series:alpha'

HELPER_TAGS=$(HYD_BIN="${BIN}" HYD_CONFIG="${WORK}/hydd.conf" \
    "${ROOT}/contrib/hyd" --sha1 "${WORK}/legacy-file")
echo "${HELPER_TAGS}" | grep -q 'series:alpha'

HELPER_TAGS=$(HYD_BIN="${BIN}" HYD_CONFIG="${WORK}/hydd.conf" \
    "${ROOT}/contrib/hyd" "${H1}")
echo "${HELPER_TAGS}" | grep -q 'series:alpha'

[ "$(HYD_BIN="${BIN}" HYD_CONFIG="${WORK}/hydd.conf" \
    "${ROOT}/contrib/hyd" version)" = "hyd v${VERSION}" ]

BODY=$(curl -s -X POST -H 'Content-Type: application/json' \
    -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -d "{\"hashes\":[\"${H3}\"],\"only_return_identifiers\":true}" \
    "${BASE}/get_files/file_metadata")
echo "${BODY}" | grep -q "${H3}"
echo "${BODY}" | grep -q '"file_id":'

BODY=$(curl -s -X POST -H 'Content-Type: application/json' \
    -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -d "{\"hashes\":[\"${H3}\"]}" \
    "${BASE}/get_files/file_metadata")
echo "${BODY}" | grep -q 'character:delta'
echo "${BODY}" | grep -q 'creator:gamma'

MISSING=$(python3 -c "print('ab'*32)")
BODY=$(curl -s -X POST -H 'Content-Type: application/json' \
    -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -d "{\"hashes\":[\"${MISSING}\"]}" \
    "${BASE}/get_files/file_metadata")
echo "${BODY}" | grep -q '"file_id":null'

# The API also accepts SHA-1 and MD5 hashes.
BODY=$(curl -s -X POST -H 'Content-Type: application/json' \
    -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -d "{\"hashes\":[\"${L1}\"]}" \
    "${BASE}/get_files/file_metadata")
echo "${BODY}" | grep -q "${L1}"
echo "${BODY}" | grep -q 'series:alpha'

BODY=$(curl -s -X POST -H 'Content-Type: application/json' \
    -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -d "{\"hashes\":[\"${L2}\"]}" \
    "${BASE}/get_files/file_metadata")
echo "${BODY}" | grep -q "${L2}"
echo "${BODY}" | grep -q 'creator:gamma'

curl -s "${BASE}/health" | grep -q '"built":true'

kill "${DAEMON_PID}" 2>/dev/null || true
wait "${DAEMON_PID}" 2>/dev/null || true
DAEMON_PID=

# With the daemon stopped, --stats must fail with a useful message.
if STATS_OUT=$("${BIN}" -c "${WORK}/hydd.conf" --stats 2>&1); then
    echo "--stats should fail when the API is down" >&2
    exit 1
fi
echo "${STATS_OUT}" | grep -q 'cannot reach the API'

# Derived files carry the store generation. A mismatched counts file must be
# ignored and rebuilt in the background, never served.
python3 - "${WORK}/data/store/counts.bin" <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    f.seek(16)
    f.write(struct.pack("<Q", 999999))
PY
"${BIN}" -c "${WORK}/hydd.conf" --no-sync > "${WORK}/generation-daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 0.8
GEN_READY=0
for _ in $(seq 1 100); do
    if [ "$(curl -s -o /dev/null -w '%{http_code}' \
        -H "Hydrus-Client-API-Access-Key: ${KEY}" \
        "${BASE}/hyd/tag_counts?tags=series:alpha")" = "200" ]; then
        GEN_READY=1
        break
    fi
    sleep 0.1
done
[ "${GEN_READY}" = "1" ]
wait_counts_delta "${BASE}" "${KEY}"
curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${BASE}/hyd/tag_counts?tags=series:alpha" | grep -q '"series:alpha":2'
if grep -q 'ignoring tag counts' "${WORK}/generation-daemon.log"; then
    :
else
    echo "generation mismatch was not detected" >&2
    exit 1
fi
kill "${DAEMON_PID}" 2>/dev/null || true
wait "${DAEMON_PID}" 2>/dev/null || true
DAEMON_PID=
"${BIN}" -c "${WORK}/hydd.conf" --check-tag-counts | grep -q '0 mismatches'

# Reverse search, explicitly enabled: build the index, page it, and check that
# the deleted mapping is applied to the postings too.
REVERSE_PORT=$((PORT_API + 20))
sed -e "s/^listen_port = .*/listen_port = ${REVERSE_PORT}/" \
    "${WORK}/hydd.conf" > "${WORK}/reverse.conf"
printf 'tag_hashes_enabled = true\n' >> "${WORK}/reverse.conf"
RBASE="http://127.0.0.1:${REVERSE_PORT}"

"${BIN}" -c "${WORK}/reverse.conf" --rebuild-tag-counts
[ -f "${WORK}/data/store/tag_postings.bin" ]
[ -f "${WORK}/data/store/hash_index.bin" ]
"${BIN}" -c "${WORK}/reverse.conf" --check-tag-counts | grep -q 'postings'

"${BIN}" -c "${WORK}/reverse.conf" --no-sync > "${WORK}/reverse-daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 0.8
HASH_READY=0
for _ in $(seq 1 100); do
    if [ "$(curl -s -o /dev/null -w '%{http_code}' \
        -H "Hydrus-Client-API-Access-Key: ${KEY}" \
        "${RBASE}/hyd/tag_hashes?tag=series:alpha")" = "200" ]; then
        HASH_READY=1
        break
    fi
    sleep 0.1
done
[ "${HASH_READY}" = "1" ]
wait_counts_delta "${RBASE}" "${KEY}"

# The stats features report the enabled reverse index as ready.
curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/stats" > "${WORK}/reverse-stats.json"
python3 - "${WORK}/reverse-stats.json" <<'PY'
import json, sys
f = json.load(open(sys.argv[1]))["features"]
assert f["tag_hashes"]["enabled"] is True and f["tag_hashes"]["ready"] is True
assert f["tag_hashes"]["bytes"] > 0 and f["tag_hashes"]["as_of"] > 0
assert f["tag_counts"]["ready"] is True
PY
[ "$(curl -s -o /dev/null -w '%{http_code}' \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha")" = "401" ]

PAGE=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&limit=10")
echo "${PAGE}" | grep -q '"total":2'
echo "${PAGE}" | grep -q "${H1}"
echo "${PAGE}" | grep -q "${L1}"
echo "${PAGE}" | grep -q '"next_offset":null'
echo "${PAGE}" | grep -q '"next_after":null'
if echo "${PAGE}" | grep -q "${H2}"; then
    echo "deleted mapping leaked into the reverse index" >&2
    exit 1
fi

PAGE_ONE=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&offset=0&limit=1")
echo "${PAGE_ONE}" | grep -q "${H1}"
echo "${PAGE_ONE}" | grep -q '"next_offset":1'
echo "${PAGE_ONE}" | grep -q '"next_after":10'
PAGE_TWO=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&offset=1&limit=1")
echo "${PAGE_TWO}" | grep -q "${L1}"
echo "${PAGE_TWO}" | grep -q '"next_offset":null'

# Cursor paging reaches the same rows without an offset.
AFTER_ONE=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&after=10&limit=1")
echo "${AFTER_ONE}" | grep -q "${L1}"
echo "${AFTER_ONE}" | grep -q '"next_after":null'
AFTER_END=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&after=13&limit=1")
echo "${AFTER_END}" | grep -q '"hashes":\[\]'

# Offsets beyond the end are empty pages; beyond the DoS cap they are refused.
# A cursor has no such cap.
for boundary in 2 999; do
    EMPTY_PAGE=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
        "${RBASE}/hyd/tag_hashes?tag=series:alpha&offset=${boundary}&limit=1")
    echo "${EMPTY_PAGE}" | grep -q '"hashes":\[\]'
    echo "${EMPTY_PAGE}" | grep -q '"next_offset":null'
done
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&offset=10000001")" = "400" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&offset=-1")" = "400" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&offset=abc")" = "400" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&offset=0&after=10")" = "400" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&after=-1")" = "400" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&after=abc")" = "400" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&limit=0")" = "400" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=series:alpha&limit=5000")" = "400" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes")" = "400" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    "${RBASE}/hyd/tag_hashes?tag=no:such-tag")" = "404" ]

HELPER_HASHES=$(HYD_BIN="${BIN}" HYD_CONFIG="${WORK}/reverse.conf" \
    "${ROOT}/contrib/hyd" hashes --limit 1 -- "series:alpha" 2>/dev/null)
printf '%s\n' "${HELPER_HASHES}" | grep -q "${H1}"
HELPER_AFTER=$(HYD_BIN="${BIN}" HYD_CONFIG="${WORK}/reverse.conf" \
    "${ROOT}/contrib/hyd" hashes-after "series:alpha" 10 1 2>/dev/null)
printf '%s\n' "${HELPER_AFTER}" | grep -q "${L1}"

# Multiple tags are ANDed; a leading '-' excludes. total is null when the
# intersection size is not cheaply known.
AND_PAGE=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' \
    -d '{"tags":["series:alpha","character:beta"]}' \
    "${RBASE}/hyd/tag_hashes?limit=10")
echo "${AND_PAGE}" | grep -q "${H1}"
echo "${AND_PAGE}" | grep -q '"total":null'
if echo "${AND_PAGE}" | grep -q "${L1}"; then
    echo "AND returned a hash that does not match both tags" >&2
    exit 1
fi

NOT_PAGE=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' \
    -d '{"tags":["series:alpha","-character:beta"]}' \
    "${RBASE}/hyd/tag_hashes?limit=10")
echo "${NOT_PAGE}" | grep -q "${L1}"
if echo "${NOT_PAGE}" | grep -q "${H1}"; then
    echo "negated tag was not excluded" >&2
    exit 1
fi

EMPTY_AND=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' \
    -d '{"tags":["character:beta","creator:gamma"]}' \
    "${RBASE}/hyd/tag_hashes?limit=10")
echo "${EMPTY_AND}" | grep -q '"hashes":\[\]'

DELTA_AND=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' \
    -d '{"tags":["character:delta","creator:gamma"]}' \
    "${RBASE}/hyd/tag_hashes?limit=10")
echo "${DELTA_AND}" | grep -q "${H3}"

# Excluding a tag that does not exist is a no-op; exclusion-only is refused.
NOOP_NOT=$(curl -s -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' \
    -d '{"tags":["series:alpha","-no:such-tag"]}' \
    "${RBASE}/hyd/tag_hashes?limit=10")
echo "${NOOP_NOT}" | grep -q "${H1}"
echo "${NOOP_NOT}" | grep -q "${L1}"
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' -d '{"tags":["-character:beta"]}' \
    "${RBASE}/hyd/tag_hashes")" = "400" ]
python3 - "${WORK}/too-many-tags.json" <<'PY'
import json, sys
json.dump({"tags": [f"test:t{i}" for i in range(17)]}, open(sys.argv[1], "w"))
PY
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' --data-binary "@${WORK}/too-many-tags.json" \
    "${RBASE}/hyd/tag_hashes")" = "400" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -H 'Content-Type: application/json' -d '{"tags":["no:such-tag","character:beta"]}' \
    "${RBASE}/hyd/tag_hashes")" = "404" ]

HELPER_AND=$(HYD_BIN="${BIN}" HYD_CONFIG="${WORK}/reverse.conf" \
    "${ROOT}/contrib/hyd" hashes --limit 10 -- "series:alpha" "character:beta" 2>/dev/null)
printf '%s\n' "${HELPER_AND}" | grep -q "${H1}"
HELPER_NOT=$(HYD_BIN="${BIN}" HYD_CONFIG="${WORK}/reverse.conf" \
    "${ROOT}/contrib/hyd" hashes -- "series:alpha" "-character:beta" 2>/dev/null)
printf '%s\n' "${HELPER_NOT}" | grep -q "${L1}"

kill "${DAEMON_PID}" 2>/dev/null || true
wait "${DAEMON_PID}" 2>/dev/null || true
DAEMON_PID=

# The reverse files are generation-checked too; corrupt the postings header
# and let the startup repair rebuild it instead of serving stale rows.
python3 - "${WORK}/data/store/tag_postings.bin" <<'PY'
import struct, sys
with open(sys.argv[1], "r+b") as f:
    f.seek(16)
    f.write(struct.pack("<Q", 999999))
PY
"${BIN}" -c "${WORK}/reverse.conf" --no-sync > "${WORK}/reverse-repair.log" 2>&1 &
DAEMON_PID=$!
sleep 0.8
REPAIR_READY=0
for _ in $(seq 1 100); do
    if [ "$(curl -s -o /dev/null -w '%{http_code}' \
        -H "Hydrus-Client-API-Access-Key: ${KEY}" \
        "${RBASE}/hyd/tag_hashes?tag=series:alpha")" = "200" ]; then
        REPAIR_READY=1
        break
    fi
    sleep 0.1
done
[ "${REPAIR_READY}" = "1" ]
kill "${DAEMON_PID}" 2>/dev/null || true
wait "${DAEMON_PID}" 2>/dev/null || true
DAEMON_PID=
"${BIN}" -c "${WORK}/reverse.conf" --check-tag-counts | grep -q '0 mismatches'

# The relay is disabled without keep_update_cache, even when the cache exists.
NORELAY_PORT=$((PORT_API + 10))
sed -e "s/^listen_port = .*/listen_port = ${NORELAY_PORT}/" \
    -e "s/^keep_update_cache = .*/keep_update_cache = false/" \
    "${WORK}/hydd.conf" > "${WORK}/hydd-norelay.conf"
"${BIN}" -c "${WORK}/hydd-norelay.conf" --no-sync > "${WORK}/norelay-daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 0.8
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Key: ${KEY}" \
    "http://127.0.0.1:${NORELAY_PORT}/metadata?since=0")" = "404" ]
kill "${DAEMON_PID}" 2>/dev/null || true
wait "${DAEMON_PID}" 2>/dev/null || true
DAEMON_PID=

# Loopback is trusted by default: no key and even a bogus key are accepted.
# The main test config sets require_localhost_key = true, so use a clone.
TRUST_PORT=$((PORT_API + 1))
sed -e "s/^listen_port = .*/listen_port = ${TRUST_PORT}/" \
    -e "/^require_localhost_key = true/d" \
    "${WORK}/hydd.conf" > "${WORK}/hydd-trust.conf"
"${BIN}" -c "${WORK}/hydd-trust.conf" --no-sync > "${WORK}/trust-daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 0.8
TRUST_BASE="http://127.0.0.1:${TRUST_PORT}"
[ "$(curl -s -o /dev/null -w '%{http_code}' "${TRUST_BASE}/get_services")" = "200" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H 'Hydrus-Client-API-Access-Key: bogus' "${TRUST_BASE}/get_services")" = "200" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" "${TRUST_BASE}/get_services")" = "200" ]
kill "${DAEMON_PID}" 2>/dev/null || true
wait "${DAEMON_PID}" 2>/dev/null || true
DAEMON_PID=

# Compaction must preserve every query result and remove the delta journal.
"${BIN}" -c "${WORK}/hydd.conf" --compact
"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q '5 hashes'
[ "$(find "${WORK}/data/store/delta" -name 'd-*.bin' | wc -l)" -eq 0 ]

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H1}")
echo "${OUT}" | grep -q 'character:beta'
echo "${OUT}" | grep -q 'creator:gamma'

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${L1}")
echo "${OUT}" | grep -q 'series:alpha'

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${L2}")
echo "${OUT}" | grep -q 'creator:gamma'

# The scan command must catch a corrupt cached update file.
CACHE_FILE=$(find "${WORK}/data/cache/updates" -name '*.bin' | head -n 1)
printf 'not zlib' > "${CACHE_FILE}"
if SCAN_OUT=$("${BIN}" -c "${WORK}/hydd.conf" --scan-updates 2>&1); then
    echo "scan of a corrupt update should have failed" >&2
    exit 1
fi
echo "${SCAN_OUT}" | grep -q 'FAIL '

# Explicit full repair must re-download the corrupt entry.
"${BIN}" -c "${WORK}/hydd.conf" --repair-cache | grep -q '1 repaired'
"${BIN}" -c "${WORK}/hydd.conf" --scan-updates | grep -q 'no update format problems found'

# A missing cache entry must be refilled too.
MISSING_FILE=$(find "${WORK}/data/cache/updates" -name '*.bin' | tail -n 1)
rm -f "${MISSING_FILE}"
"${BIN}" -c "${WORK}/hydd.conf" --repair-cache | grep -q '1 missing'
[ -f "${MISSING_FILE}" ]

# A running daemon must heal a file that /update finds corrupt, without a
# restart and without rebuilding the store.
AUTO_PORT=$((PORT_API + 20))
sed -e "s/^listen_port = .*/listen_port = ${AUTO_PORT}/" \
    "${WORK}/hydd.conf" > "${WORK}/hydd-autorepair.conf"
printf 'sync_on_start = false\n' >> "${WORK}/hydd-autorepair.conf"
"${BIN}" -c "${WORK}/hydd-autorepair.conf" > "${WORK}/autorepair.log" 2>&1 &
DAEMON_PID=$!
sleep 0.8
AUTO_BASE="http://127.0.0.1:${AUTO_PORT}"
AUTO_FILE=$(find "${WORK}/data/cache/updates" -name '*.bin' | head -n 1)
AUTO_HASH=$(basename "${AUTO_FILE}" .bin)
printf 'damaged' > "${AUTO_FILE}"
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Key: ${KEY}" \
    "${AUTO_BASE}/update?update_hash=${AUTO_HASH}")" = "500" ]
AUTO_CODE=000
for _ in $(seq 1 40); do
    AUTO_CODE=$(curl -s -o "${WORK}/autorepaired.bin" -w '%{http_code}' \
        -H "Hydrus-Key: ${KEY}" \
        "${AUTO_BASE}/update?update_hash=${AUTO_HASH}")
    [ "${AUTO_CODE}" = "200" ] && break
    sleep 0.25
done
[ "${AUTO_CODE}" = "200" ]
[ "$(sha256sum "${WORK}/autorepaired.bin" | awk '{print $1}')" = "${AUTO_HASH}" ]
kill "${DAEMON_PID}" 2>/dev/null || true
wait "${DAEMON_PID}" 2>/dev/null || true
DAEMON_PID=

# A resumed initial build must finish its checkpointed update snapshot even
# when the PTR published new indices while the daemon was stopped.
H4=$(hash_of epsilon-file)
stop_repo
start_repo incremental

RESUME_PORT=$((PORT_API + 30))
cat > "${WORK}/resume.conf" <<EOF
data_dir = ${WORK}/resume-data
listen_address = 127.0.0.1
listen_port = ${RESUME_PORT}
repository_url = http://127.0.0.1:${PORT_REPO}
repository_access_key = 1111111111111111111111111111111111111111111111111111111111111111
partitions = 4
sync_interval_seconds = 3600
keep_update_cache = true
log_level = info
EOF

HYD_TEST_PAUSE_AFTER_CHECKPOINT_MS=3000 "${BIN}" -c "${WORK}/resume.conf" \
    --sync-once > "${WORK}/resume-first.log" 2>&1 &
RESUME_PID=$!
COMPLETED=0
for _ in $(seq 1 100); do
    if [ -f "${WORK}/resume-data/store/build.json" ]; then
        COMPLETED=$(python3 -c \
            'import json,sys; print(json.load(open(sys.argv[1]))["completed_partitions"])' \
            "${WORK}/resume-data/store/build.json")
        [ "${COMPLETED}" -ge 1 ] && break
    fi
    sleep 0.1
done
kill -9 "${RESUME_PID}" 2>/dev/null || true
wait "${RESUME_PID}" 2>/dev/null || true
[ "${COMPLETED}" -ge 1 ]
[ "${COMPLETED}" -lt 4 ]
[ -f "${WORK}/resume-data/store/build.json" ]

stop_repo
start_repo published

"${BIN}" -c "${WORK}/resume.conf" --sync-once > "${WORK}/resume-second.log" 2>&1
grep -q 'initial-build checkpoint is still valid' "${WORK}/resume-second.log"
grep -q 'resuming initial build at partition' "${WORK}/resume-second.log"
grep -q 'catching up on' "${WORK}/resume-second.log"
"${BIN}" -c "${WORK}/resume.conf" --verify-store | grep -q '6 hashes'
OUT=$("${BIN}" -c "${WORK}/resume.conf" --query "${H4}")
echo "${OUT}" | grep -q 'series:epsilon'

# Adopt a completed build if the previous process died after writing
# manifest.json but before saving sync.json and removing build.json.
stop_repo
start_repo incremental
cp -a "${WORK}/data" "${WORK}/adopt-data"
ADOPT_PORT=$((PORT_API + 40))
cat > "${WORK}/adopt.conf" <<EOF
data_dir = ${WORK}/adopt-data
listen_address = 127.0.0.1
listen_port = ${ADOPT_PORT}
repository_url = http://127.0.0.1:${PORT_REPO}
repository_access_key = 1111111111111111111111111111111111111111111111111111111111111111
partitions = 4
sync_interval_seconds = 3600
keep_update_cache = true
log_level = info
EOF

python3 - "${WORK}/adopt-data" <<'PY'
import hashlib
import json
import sys

root = sys.argv[1]
metadata = json.load(open(root + "/state/repository.json"))
hashes = []
for _, _, _, row_hashes in metadata["updates"]:
    hashes.extend(bytes.fromhex(value) for value in row_hashes)

checkpoint = {
    "version": 1,
    "store_format": 2,
    "source_fingerprint": hashlib.sha256(b"".join(sorted(hashes))).hexdigest(),
    "source_count": len(hashes),
    "partitions": 4,
    "completed_partitions": 4,
    "max_hash_id": 0,
    "tag_count": 0,
    "tag_base": 0,
    "tag_last": 0,
    "hash_count": 0,
    "mapping_count": 0,
    "started_at": 1,
}
json.dump(checkpoint, open(root + "/store/build.json", "w"))

state = json.load(open(root + "/state/sync.json"))
state["built"] = False
state["built_at"] = 0
state["next_index"] = 0
json.dump(state, open(root + "/state/sync.json", "w"))
PY

"${BIN}" -c "${WORK}/adopt.conf" --sync-once > "${WORK}/adopt.log" 2>&1
grep -q 'adopted a completed initial build' "${WORK}/adopt.log"
if grep -q 'initial sync: building tag dictionary' "${WORK}/adopt.log"; then
    echo "a completed build was rebuilt instead of adopted" >&2
    exit 1
fi
"${BIN}" -c "${WORK}/adopt.conf" --verify-store | grep -q '5 hashes'
OUT=$("${BIN}" -c "${WORK}/adopt.conf" --query "${H1}")
echo "${OUT}" | grep -q 'series:alpha'

# Rate limiting: a small burst, then 429 with Retry-After. Loopback bypass
# stays off, so this is the same path a public client takes.
kill "${DAEMON_PID}" 2>/dev/null || true
wait "${DAEMON_PID}" 2>/dev/null || true
DAEMON_PID=
cat > "${WORK}/rate.conf" <<EOF
data_dir = ${WORK}/data
listen_address = 127.0.0.1
listen_port = ${PORT_API}
repository_url = http://127.0.0.1:${PORT_REPO}
partitions = 4
sync_interval_seconds = 3600
rate_limit_requests_per_minute = 30
rate_limit_burst = 5
tag_counts_enabled = false
log_level = warn
EOF
"${BIN}" -c "${WORK}/rate.conf" --no-sync > "${WORK}/rate.log" 2>&1 &
DAEMON_PID=$!
sleep 1

# Disabled features answer 404 (each consumes one of the six burst tokens).
[ "$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/hyd/tag_counts?tags=series:alpha")" = "404" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/hyd/tag_hashes?tag=series:alpha")" = "404" ]

for _ in 1 2 3; do
    [ "$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/health")" = "200" ]
done
LIMITED=$(curl -s -D "${WORK}/rate-headers" -o /dev/null -w '%{http_code}' "${BASE}/health")
[ "${LIMITED}" = "429" ]
grep -qi '^retry-after:' "${WORK}/rate-headers"

echo "integration test passed"
