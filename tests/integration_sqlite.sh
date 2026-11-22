#!/bin/sh
# End-to-end test of the plain SQLite backend.
# Usage: tests/integration_sqlite.sh /path/to/hydd

set -eu

BIN=${1:-./hydd}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FAKE="$ROOT/tests/fake_repository.py"
WORK=$(mktemp -d /tmp/hyd-sqlite-test.XXXXXX)
PORT_REPO=46872
PORT_API=46870
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
service_name = sqlite integration ptr
compression = false
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

[ "$("${BIN}" -c "${WORK}/hydd.conf" --query "${H3}")" = "not found" ]

# Legacy SHA-1 hashes must be preserved and queryable.
"${BIN}" -c "${WORK}/hydd.conf" --query "${L1}" | grep -q 'series:alpha'

stop_repo
start_repo incremental
"${BIN}" -c "${WORK}/hydd.conf" --sync-once

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H3}")
echo "${OUT}" | grep -q 'character:delta'

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H2}")
if echo "${OUT}" | grep -q 'series:alpha'; then
    echo "deleted mapping was not applied in SQLite mode" >&2
    exit 1
fi

"${BIN}" -c "${WORK}/hydd.conf" --query "${L2}" | grep -q 'creator:gamma'

"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q '5 hashes'
"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q 'store ok (sqlite)'

# API against the SQLite backend.
"${BIN}" -c "${WORK}/hydd.conf" --no-sync > "${WORK}/daemon.log" 2>&1 &
DAEMON_PID=$!
sleep 0.8

KEY=2222222222222222222222222222222222222222222222222222222222222222
BASE="http://127.0.0.1:${PORT_API}"

BODY=$(curl -s -X POST -H 'Content-Type: application/json' \
    -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -d "{\"hashes\":[\"${H3}\"]}" \
    "${BASE}/get_files/file_metadata")
echo "${BODY}" | grep -q 'character:delta'
curl -s "${BASE}/health" | grep -q '"backend":"sqlite"'
curl -s "${BASE}/stats" | grep -q '"legacy_hashes":2'

BODY=$(curl -s -X POST -H 'Content-Type: application/json' \
    -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -d "{\"hashes\":[\"${L1}\"]}" \
    "${BASE}/get_files/file_metadata")
echo "${BODY}" | grep -q 'series:alpha'

BODY=$(curl -s -X POST -H 'Content-Type: application/json' \
    -H "Hydrus-Client-API-Access-Key: ${KEY}" \
    -d "{\"hashes\":[\"${L2}\"]}" \
    "${BASE}/get_files/file_metadata")
echo "${BODY}" | grep -q 'creator:gamma'

kill "${DAEMON_PID}" 2>/dev/null || true
wait "${DAEMON_PID}" 2>/dev/null || true
DAEMON_PID=

# VACUUM is the SQLite equivalent of compaction.
"${BIN}" -c "${WORK}/hydd.conf" --compact
OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H1}")
echo "${OUT}" | grep -q 'creator:gamma'
"${BIN}" -c "${WORK}/hydd.conf" --query "${L2}" | grep -q 'creator:gamma'

echo "sqlite integration test passed"
