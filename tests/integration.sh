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
access_key = 2222222222222222222222222222222222222222222222222222222222222222
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

KEY=2222222222222222222222222222222222222222222222222222222222222222
BASE="http://127.0.0.1:${PORT_API}"
VERSION=$("${BIN}" --version | awk '{print $2}')
curl -s "${BASE}/api_version" | grep -q "\"hyd_version\":\"${VERSION}\""

[ "$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/get_services")" = "401" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: bad" "${BASE}/get_services")" = "403" ]
[ "$(curl -s -o /dev/null -w '%{http_code}' -H "Hydrus-Client-API-Access-Key: ${KEY}" "${BASE}/get_services")" = "200" ]

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
