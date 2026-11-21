#!/bin/sh
# End-to-end test for SQLite -> custom migration.
# Usage: tests/integration_migration.sh /path/to/hydd

set -eu

BIN=${1:-./hydd}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FAKE="$ROOT/tests/fake_repository.py"
WORK=$(mktemp -d /tmp/hyd-migration-test.XXXXXX)
PORT_REPO=46873
PORT_API=46871
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
H2=$(hash_of beta-file)
H3=$(hash_of gamma-file)

cat > "${WORK}/hydd.conf" <<EOF
data_dir = ${WORK}/data
listen_address = 127.0.0.1
listen_port = ${PORT_API}
repository_url = http://127.0.0.1:${PORT_REPO}
repository_access_key = 1111111111111111111111111111111111111111111111111111111111111111
access_key = 2222222222222222222222222222222222222222222222222222222222222222
service_name = migration integration ptr
compression = false
log_level = warn
EOF

python3 "${FAKE}" --port "${PORT_REPO}" --mode initial > "${WORK}/repo.log" 2>&1 &
SERVER_PID=$!
sleep 0.7

"${BIN}" -c "${WORK}/hydd.conf" --sync-once
"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q 'store ok (sqlite)'
[ "$(sqlite3 "${WORK}/data/sqlite/hyd.sqlite3" 'PRAGMA auto_vacuum;')" = "2" ]

# Turn compression on and let the daemon migrate the SQLite store.
sed -i 's/compression = false/compression = true/' "${WORK}/hydd.conf"
"${BIN}" -c "${WORK}/hydd.conf" --compact

"${BIN}" -c "${WORK}/hydd.conf" --verify-store | grep -q 'store ok (custom)'
[ ! -f "${WORK}/data/sqlite/hyd.sqlite3" ]

OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H1}")
echo "${OUT}" | grep -q 'series:alpha'
echo "${OUT}" | grep -q 'character:beta'

# The reverse switch must fail loudly instead of silently picking a backend.
sed -i 's/compression = true/compression = false/' "${WORK}/hydd.conf"
if "${BIN}" -c "${WORK}/hydd.conf" --compact > "${WORK}/reverse.log" 2>&1; then
    echo "compression=false unexpectedly succeeded after migration" >&2
    exit 1
fi
grep -q 'compression = false refused' "${WORK}/reverse.log"

# Switch back, bring in new updates, and verify post-migration syncing.
sed -i 's/compression = false/compression = true/' "${WORK}/hydd.conf"
kill "${SERVER_PID}" 2>/dev/null || true
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=
python3 "${FAKE}" --port "${PORT_REPO}" --mode incremental > "${WORK}/repo2.log" 2>&1 &
SERVER_PID=$!
sleep 0.7

"${BIN}" -c "${WORK}/hydd.conf" --sync-once
OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H3}")
echo "${OUT}" | grep -q 'character:delta'
OUT=$("${BIN}" -c "${WORK}/hydd.conf" --query "${H2}")
if echo "${OUT}" | grep -q 'series:alpha'; then
    echo "post-migration deleted mapping was not applied" >&2
    exit 1
fi

echo "migration integration test passed"
