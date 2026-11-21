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

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H1}")
echo "${OUT}" | grep -q 'series:alpha'
echo "${OUT}" | grep -q 'character:beta'
echo "${OUT}" | grep -q 'creator:gamma'

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H3}")
echo "${OUT}" | grep -q 'not found'

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

"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q '3 hashes'
"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q '1 delta records' 2>/dev/null || true

# API checks.
"${BIN}" -c "${WORK}/hydd.conf" --no-sync > "${WORK}/daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 0.8

KEY=2222222222222222222222222222222222222222222222222222222222222222
BASE="http://127.0.0.1:${PORT_API}"

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

curl -s "${BASE}/health" | grep -q '"built":true'

kill "${DAEMON_PID}" 2>/dev/null || true
wait "${DAEMON_PID}" 2>/dev/null || true
DAEMON_PID=

# Compaction must preserve every query result and remove the delta journal.
"${BIN}" -c "${WORK}/hydd.conf" --compact
"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q '3 hashes'
[ "$(find "${WORK}/data/store/delta" -name 'd-*.bin' | wc -l)" -eq 0 ]

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H1}")
echo "${OUT}" | grep -q 'character:beta'
echo "${OUT}" | grep -q 'creator:gamma'

echo "integration test passed"
