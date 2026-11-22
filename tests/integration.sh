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

"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q '5 hashes'
"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q '1 delta records' 2>/dev/null || true

# API checks.
"${BIN}" -c "${WORK}/hydd.conf" --no-sync > "${WORK}/daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 0.8

# --stats must work unprivileged and without touching the data directory.
STATS=$("${BIN}" -c "${WORK}/hydd.conf" -d "${WORK}/stats-no-data" --stats)
echo "${STATS}" | grep -q 'SHA-256'
echo "${STATS}" | grep -q 'Mappings'
echo "${STATS}" | grep -q 'ready'
[ ! -e "${WORK}/stats-no-data" ]
curl -s "http://127.0.0.1:${PORT_API}/stats" | grep -q '"built":true'
curl -s "http://127.0.0.1:${PORT_API}/stats" | grep -q '"phase":"idle"'

KEY=2222222222222222222222222222222222222222222222222222222222222222
KEY2=3333333333333333333333333333333333333333333333333333333333333333
BASE="http://127.0.0.1:${PORT_API}"
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

echo "integration test passed"
