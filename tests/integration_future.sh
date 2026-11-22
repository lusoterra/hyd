#!/bin/sh
# A repository update whose format this build does not understand must abort
# the sync and leave the existing store untouched, rather than silently
# dropping rows.
# Usage: tests/integration_future.sh /path/to/hydd

set -eu

BIN=${1:-./hydd}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FAKE="$ROOT/tests/fake_repository.py"
WORK=$(mktemp -d /tmp/hyd-future-test.XXXXXX)
PORT_REPO=46873
PORT_API=46874
SERVER_PID=

cleanup() {
    [ -n "${SERVER_PID}" ] && kill "${SERVER_PID}" 2>/dev/null || true
    rm -rf "${WORK}"
}
trap cleanup EXIT INT TERM

hash_of() {
    python3 -c "import hashlib,sys; print(hashlib.sha256(sys.argv[1].encode()).hexdigest())" "$1"
}

H1=$(hash_of alpha-file)

cat > "${WORK}/hydd.conf" <<EOF
data_dir = ${WORK}/data
listen_address = 127.0.0.1
listen_port = ${PORT_API}
repository_url = http://127.0.0.1:${PORT_REPO}
repository_access_key = 1111111111111111111111111111111111111111111111111111111111111111
access_key = 2222222222222222222222222222222222222222222222222222222222222222
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
"${BIN}" -c "${WORK}/hydd.conf" --query "${H1}" | grep -q 'series:alpha'
"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q '3 hashes'
stop_repo

start_repo future
if OUT=$("${BIN}" -c "${WORK}/hydd.conf" --sync-once 2>&1); then
    echo "sync with an unknown content type should have failed" >&2
    exit 1
fi
echo "${OUT}" | grep -q 'unknown content type 9'

# The failed update must not advance the index or change the store.
grep -q '"next_index":2' "${WORK}/data/state/sync.json"
"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q '3 hashes'
"${BIN}" -c "${WORK}/hydd.conf" --query "${H1}" | grep -q 'series:alpha'
[ "$(find "${WORK}/data/store/delta" -name 'd-*.bin' | wc -l)" -eq 0 ]
stop_repo

# Unknown invalid hash values fail, but the four known PTR values are skipped.
start_repo badhash
sed "s|^data_dir = .*|data_dir = ${WORK}/badhash-data|" "${WORK}/hydd.conf" > "${WORK}/hydd-badhash.conf"
if OUT=$("${BIN}" -c "${WORK}/hydd-badhash.conf" --sync-once 2>&1); then
    echo "sync with an unknown invalid hash definition should have failed" >&2
    exit 1
fi
echo "${OUT}" | grep -q 'unsupported 2-byte hash definition'
grep -q '"next_index":0' "${WORK}/badhash-data/state/sync.json"
stop_repo

start_repo knownbad
sed "s|^data_dir = .*|data_dir = ${WORK}/knownbad-data|" "${WORK}/hydd.conf" > "${WORK}/hydd-knownbad.conf"
printf 'keep_update_cache = true\n' >> "${WORK}/hydd-knownbad.conf"
"${BIN}" -c "${WORK}/hydd-knownbad.conf" --sync-once
"${BIN}" -c "${WORK}/hydd-knownbad.conf" --verify-store | grep -q '3 hashes'
grep -q '"next_index":3' "${WORK}/knownbad-data/state/sync.json"
"${BIN}" -c "${WORK}/hydd-knownbad.conf" --scan-updates | grep -q 'invalid hash'
"${BIN}" -c "${WORK}/hydd-knownbad.conf" --query "${H1}" | grep -q 'series:alpha'
stop_repo

echo "future-format test passed"
