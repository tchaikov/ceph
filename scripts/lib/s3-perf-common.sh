#!/bin/bash
# s3-perf-common.sh — Performance measurement framework for S3-backed RBD tests.
#
# Layered on top of lib/s3-test-common.sh.  Provides:
#   - Metric snapshots (RADOS pool ops + MinIO GET count + librbd perfcounters)
#   - Delta computation between snapshots
#   - Structured result recording (CSV with header)
#   - Shared fixture management (one-time generation, reused across tests)
#
# Source order: callers must source s3-test-common.sh first, then this file.

# ---------------------------------------------------------------------------
# Result recording — CSV format
# ---------------------------------------------------------------------------

PERF_RESULTS_CSV="${PERF_RESULTS_CSV:-/tmp/perf-results.csv}"
PERF_RUN_ID="${PERF_RUN_ID:-run-$(date +%Y%m%d-%H%M%S)-$$}"

# Initialise the CSV with a header explaining columns.  Idempotent: only writes
# the header if the file is missing or empty.
# Usage: perf_init_results [csv_path]
perf_init_results() {
    local csv=${1:-$PERF_RESULTS_CSV}

    if [ ! -s "$csv" ]; then
        cat > "$csv" <<'EOF'
# Performance test results — one row per (test, metric) pair.
# Columns:
#   test_name      — name of the perf_test_* function in test-s3-performance.sh
#   metric         — what was measured (e.g. wall_time_ms, s3_get_count, rados_write_ops)
#   value          — numeric value; empty means metric collection failed
#   unit           — ms | bytes | count | mb_per_sec | ratio
#   timestamp_iso  — UTC time when this row was recorded (RFC3339)
#   run_id         — opaque per-execution identifier; group rows from one run together
test_name,metric,value,unit,timestamp_iso,run_id
EOF
    fi
}

# Append a single (metric, value, unit) row for a test.
# Usage: perf_record <test_name> <metric> <value> <unit>
perf_record() {
    local test_name=$1
    local metric=$2
    local value=$3
    local unit=$4
    local ts
    ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)

    perf_init_results "$PERF_RESULTS_CSV"
    printf '%s,%s,%s,%s,%s,%s\n' \
        "$test_name" "$metric" "$value" "$unit" "$ts" "$PERF_RUN_ID" \
        >> "$PERF_RESULTS_CSV"
}

# Print a human-readable summary of all rows for the current run.
# Usage: perf_print_summary
perf_print_summary() {
    if [ ! -s "$PERF_RESULTS_CSV" ]; then
        log_warn "No perf results recorded"
        return 0
    fi

    echo
    echo "=========================================="
    echo "    PERFORMANCE RESULTS — $PERF_RUN_ID"
    echo "=========================================="
    awk -F, -v run="$PERF_RUN_ID" '
        $0 !~ /^#/ && NR > 1 && $6 == run {
            printf "  %-38s %-22s %12s %s\n", $1, $2, $3, $4
        }
    ' "$PERF_RESULTS_CSV"
    echo "=========================================="
    echo "Full CSV: $PERF_RESULTS_CSV"
    echo
}

# ---------------------------------------------------------------------------
# RADOS pool op counters (option A: rados df)
# ---------------------------------------------------------------------------

# Snapshot RADOS write/read op counters for a pool.  Output is one line of
# "k=v" pairs that perf_rados_delta can parse.
# Usage: perf_rados_snapshot <conf> <pool>
perf_rados_snapshot() {
    local conf=$1
    local pool=$2

    "$BUILD_DIR/bin/rados" --conf "$conf" df --format json 2>/dev/null \
        | python3 -c "
import json, sys
d = json.load(sys.stdin)
for p in d.get('pools', []):
    if p.get('name') == '$pool':
        print('write_ops={} read_ops={} num_objects={} size_bytes={}'.format(
            p.get('write_ops', 0),
            p.get('read_ops', 0),
            p.get('num_objects', 0),
            p.get('size_bytes', 0)))
        break
" 2>/dev/null || echo "write_ops=0 read_ops=0 num_objects=0 size_bytes=0"
}

# Compute delta between two snapshots produced by perf_rados_snapshot.
# Echoes the same key=value format with deltas.
# Usage: perf_rados_delta "<before>" "<after>"
perf_rados_delta() {
    local before="$1"
    local after="$2"

    python3 - <<EOF
def parse(s):
    return {kv.split('=')[0]: int(kv.split('=')[1]) for kv in s.split() if '=' in kv}
b = parse("""$before""")
a = parse("""$after""")
out = ' '.join(f'{k}={a.get(k,0) - b.get(k,0)}' for k in sorted(set(a) | set(b)))
print(out)
EOF
}

# Extract a single field from a snapshot/delta line.
# Usage: perf_extract_field "key=val key2=val2" key2  → "val2"
perf_extract_field() {
    local kv_line="$1"
    local key="$2"
    echo "$kv_line" | tr ' ' '\n' | awk -F= -v k="$key" '$1==k {print $2; exit}'
}

# ---------------------------------------------------------------------------
# MinIO GET counter via mc admin trace (option A)
# ---------------------------------------------------------------------------
# Background: MinIO server does not log per-request by default.  We run
# `mc admin trace --json` against a registered alias and pipe to a log file;
# each S3 request emits one JSON line with method/URL/status.

PERF_MINIO_TRACE_PID=""
PERF_MINIO_TRACE_LOG=""

# Start mc trace against the "local" alias (set up by start_minio + setup_s3_bucket).
# Usage: perf_minio_trace_start <log_file>
perf_minio_trace_start() {
    local log_file=$1
    : > "$log_file"

    "$MINIO_BIN/mc" admin trace --json local > "$log_file" 2>/dev/null &
    PERF_MINIO_TRACE_PID=$!
    PERF_MINIO_TRACE_LOG="$log_file"

    # Give mc a moment to attach; without this, early requests can be missed
    sleep 1

    if ! kill -0 "$PERF_MINIO_TRACE_PID" 2>/dev/null; then
        log_warn "mc admin trace failed to start (continuing without S3 metrics)"
        PERF_MINIO_TRACE_PID=""
        return 1
    fi
    return 0
}

perf_minio_trace_stop() {
    if [ -n "$PERF_MINIO_TRACE_PID" ] && kill -0 "$PERF_MINIO_TRACE_PID" 2>/dev/null; then
        kill "$PERF_MINIO_TRACE_PID" 2>/dev/null || true
        wait "$PERF_MINIO_TRACE_PID" 2>/dev/null || true
    fi
    PERF_MINIO_TRACE_PID=""
}

# Count GetObject API calls recorded in the trace log.  The mc trace JSON shape
# uses {"api":"s3.GetObject", "path":"/bucket/key", ...}; we filter on api for
# real data fetches (excludes HeadObject, ListObjectsV2, GetBucketLocation, etc.).
# Optional <bucket> filter restricts to a specific bucket prefix.
# Always emits exactly one integer line so it is safe inside CSV value fields.
# Usage: perf_minio_get_count <trace_log> [bucket]
perf_minio_get_count() {
    local trace_log=$1
    local bucket=${2:-}

    [ -f "$trace_log" ] || { echo 0; return; }

    awk -v b="$bucket" '
        /"api":"s3.GetObject"/ {
            if (b == "") { c++; next }
            if (index($0, "\"path\":\"/" b "/") > 0) c++
        }
        END { print c+0 }
    ' "$trace_log"
}

# Sum of bytes returned by all GetObject responses.  Each trace line includes a
# "size" field for the response body; we sum those.  Returns 0 if no events.
# Usage: perf_minio_get_bytes <trace_log> [bucket]
perf_minio_get_bytes() {
    local trace_log=$1
    local bucket=${2:-}

    [ -f "$trace_log" ] || { echo 0; return; }

    python3 - "$trace_log" "$bucket" <<'PYEOF'
import json, sys
log_path, bucket = sys.argv[1], sys.argv[2]
total = 0
try:
    with open(log_path) as f:
        for line in f:
            try:
                d = json.loads(line)
            except Exception:
                continue
            if d.get('api') != 's3.GetObject':
                continue
            if bucket:
                path = d.get('path', '')
                if not path.startswith('/' + bucket + '/'):
                    continue
            sz = d.get('size') or 0
            try:
                total += int(sz)
            except Exception:
                pass
except FileNotFoundError:
    pass
print(total)
PYEOF
}

# ---------------------------------------------------------------------------
# librbd perf counters via admin socket (option C)
# ---------------------------------------------------------------------------
# librbd registers an admin socket per ImageCtx at $PERF_ASOK_DIR.  We point
# rbd CLI invocations there via --conf "admin_socket = ..." override.

PERF_ASOK_DIR="${PERF_ASOK_DIR:-/tmp/perf-asok-$$}"

# Initialise the directory; safe to call multiple times.
perf_asok_init() {
    mkdir -p "$PERF_ASOK_DIR"
    chmod 0777 "$PERF_ASOK_DIR" 2>/dev/null || true
}

# Generate the rbd CLI flag set that points admin sockets into the perf dir.
# Use as: rbd $(perf_asok_args) ...
# Usage: perf_asok_args
perf_asok_args() {
    perf_asok_init
    # Format: name.cct.PID.<NNN>.asok — placed under PERF_ASOK_DIR for discovery
    echo "--admin-socket=$PERF_ASOK_DIR/\$cluster-\$name.\$pid.\$cctid.asok"
}

# Read all librbd s3_* counters from a single asok and emit "k=v" lines.
# Usage: perf_librbd_read <asok_path>
perf_librbd_read() {
    local asok=$1

    [ -S "$asok" ] || return 1

    "$BUILD_DIR/bin/ceph" --admin-daemon "$asok" perf dump 2>/dev/null \
        | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(0)
out = []
for section, body in d.items():
    if not isinstance(body, dict): continue
    for k, v in body.items():
        if not k.startswith('s3_'): continue
        if isinstance(v, dict):
            v = v.get('avgcount') or v.get('sum') or 0
        out.append(f'{section}.{k}={v}')
print(' '.join(out))
" 2>/dev/null || true
}

# Aggregate librbd counters across all asoks created during a test run.
# Useful when N parallel rbd processes each created their own socket.
# Usage: perf_librbd_aggregate
perf_librbd_aggregate() {
    perf_asok_init
    local asok
    local agg=""
    for asok in "$PERF_ASOK_DIR"/*.asok; do
        [ -S "$asok" ] || continue
        local one
        one=$(perf_librbd_read "$asok") || continue
        [ -n "$one" ] && agg="$agg $one"
    done

    # Sum same-keyed values from multiple sockets
    echo "$agg" | python3 -c "
import sys
from collections import defaultdict
totals = defaultdict(int)
for tok in sys.stdin.read().split():
    if '=' not in tok: continue
    k, v = tok.split('=', 1)
    try:
        totals[k] += int(v)
    except ValueError:
        try:
            totals[k] += int(float(v))
        except ValueError:
            pass
print(' '.join(f'{k}={v}' for k, v in sorted(totals.items())))
"
}

# Clear all asoks under the perf dir.  Call between tests to reset counters.
perf_asok_clear() {
    rm -f "$PERF_ASOK_DIR"/*.asok 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Time measurement
# ---------------------------------------------------------------------------

# Echo current time in milliseconds since epoch.
perf_time_ms() {
    date +%s%3N
}

# Echo elapsed milliseconds since <start_ms>.
# Usage: perf_time_elapsed_ms <start_ms>
perf_time_elapsed_ms() {
    local start=$1
    echo $(( $(perf_time_ms) - start ))
}

# ---------------------------------------------------------------------------
# Shared fixtures — generated once, reused across tests
# ---------------------------------------------------------------------------

PERF_FIXTURE_DIR="${PERF_FIXTURE_DIR:-/tmp/perf-fixtures}"

# Generate the standard set of fixture files.  Each fixture is generated only
# if missing or stale (size mismatch).  Returns paths via global vars:
#   PERF_FIXTURE_PATTERN_20MB    — 20 MB, all blocks have PARENT-BLOCK-NNNN
#   PERF_FIXTURE_PATTERN_100MB   — 100 MB, all blocks have pattern
#   PERF_FIXTURE_ZERO_ALT_40MB   — 40 MB, alternating pattern/zero blocks
#   PERF_FIXTURE_SPARSE_40MB     — 40 MB, only first 2 blocks filled
perf_setup_fixtures() {
    mkdir -p "$PERF_FIXTURE_DIR"

    PERF_FIXTURE_PATTERN_20MB="$PERF_FIXTURE_DIR/pattern-20mb.raw"
    PERF_FIXTURE_PATTERN_100MB="$PERF_FIXTURE_DIR/pattern-100mb.raw"
    PERF_FIXTURE_ZERO_ALT_40MB="$PERF_FIXTURE_DIR/zero-alt-40mb.raw"
    PERF_FIXTURE_ZERO_FIRST_40MB="$PERF_FIXTURE_DIR/zero-first-40mb.raw"
    PERF_FIXTURE_SPARSE_40MB="$PERF_FIXTURE_DIR/sparse-40mb.raw"

    _ensure_fixture_size() {
        local file=$1
        local expected_mb=$2
        local generator=$3
        shift 3
        local actual=0
        if [ -f "$file" ]; then
            actual=$(( $(stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null || echo 0) / 1024 / 1024 ))
        fi
        if [ "$actual" -ne "$expected_mb" ]; then
            log_info "Generating fixture: $file (${expected_mb}MB)"
            "$generator" "$@" "$file"
        fi
    }

    _ensure_fixture_size "$PERF_FIXTURE_PATTERN_20MB"   20  create_test_image_with_pattern    20
    _ensure_fixture_size "$PERF_FIXTURE_PATTERN_100MB"  100 create_test_image_with_pattern    100
    _ensure_fixture_size "$PERF_FIXTURE_ZERO_ALT_40MB"  40  create_test_image_zero_alternating 40
    _ensure_fixture_size "$PERF_FIXTURE_ZERO_FIRST_40MB" 40 create_test_image_zero_first      40
    _ensure_fixture_size "$PERF_FIXTURE_SPARSE_40MB"    40  create_test_image_sparse          40 2

    log_success "Fixtures ready under $PERF_FIXTURE_DIR"
}

# ---------------------------------------------------------------------------
# Per-test isolation — recreate pool to wipe all RADOS state
# ---------------------------------------------------------------------------

# Delete and recreate the test pool, plus re-enable rbd application.  Call this
# at the START of each perf test to guarantee a clean slate (no objects from
# prior tests, no in-flight async writes leaking deltas into our measurement).
# Usage: perf_pool_reset <conf> <pool>
perf_pool_reset() {
    local conf=$1
    local pool=$2

    # Best-effort: remove all images first so OSDs don't keep replaying ops
    local img
    for img in $("$BUILD_DIR/bin/rbd" --conf "$conf" -p "$pool" ls 2>/dev/null); do
        "$BUILD_DIR/bin/rbd" --conf "$conf" rm "$pool/$img" 2>/dev/null || true
    done

    "$BUILD_DIR/bin/ceph" --conf "$conf" osd pool delete \
        "$pool" "$pool" --yes-i-really-really-mean-it >/dev/null 2>&1 || true
    "$BUILD_DIR/bin/ceph" --conf "$conf" osd pool create "$pool" 8 >/dev/null 2>&1
    "$BUILD_DIR/bin/ceph" --conf "$conf" osd pool application enable \
        "$pool" rbd >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------------------
# I/O drivers — decoupled from test logic
# ---------------------------------------------------------------------------
# All drivers force --io-threads 1 so total bytes covered equals --io-total
# (with default 16 threads + seq pattern, threads stack at offset 0 and only
# the first ~io-total/16 of the image gets covered, defeating cold-read tests).

# Sequentially read <total_bytes> from <pool>/<image> using rbd bench.
# Returns 0 on success, non-zero on bench failure.
# Usage: perf_drive_seq_read <conf> <pool> <image> <total_bytes> [io_size]
perf_drive_seq_read() {
    local conf=$1
    local pool=$2
    local image=$3
    local total_bytes=$4
    local io_size=${5:-65536}

    "$BUILD_DIR/bin/rbd" --conf "$conf" bench \
        --io-type read \
        --io-size "$io_size" \
        --io-total "$total_bytes" \
        --io-pattern seq \
        --io-threads 1 \
        "$pool/$image" >/dev/null 2>&1
}

# Issue <num_clients> parallel rbd bench reads against <image_basename>-1..N,
# each of <total_bytes>.  Waits for all to complete and returns 0 if all
# succeeded, otherwise the count of failures.
# Usage: perf_drive_concurrent_reads <conf> <pool> <image_basename> <num_clients> <total_bytes> [io_size]
perf_drive_concurrent_reads() {
    local conf=$1
    local pool=$2
    local image_base=$3
    local num=$4
    local total_bytes=$5
    local io_size=${6:-65536}

    local pids=()
    local i
    for i in $(seq 1 "$num"); do
        "$BUILD_DIR/bin/rbd" --conf "$conf" bench \
            --io-type read \
            --io-size "$io_size" \
            --io-total "$total_bytes" \
            --io-pattern seq \
            --io-threads 1 \
            "$pool/${image_base}-$i" >/dev/null 2>&1 &
        pids+=("$!")
    done

    local failures=0
    for pid in "${pids[@]}"; do
        wait "$pid" || failures=$((failures + 1))
    done
    return $failures
}

# Write <num_4kb_writes> 4 KB blocks at byte offset <offset> on <image> via
# rbd bench.  This is the canonical way to trigger CopyupRequest in tests.
# Usage: perf_drive_cow_write <conf> <pool> <image> <offset_bytes> <num_4kb>
perf_drive_cow_write() {
    local conf=$1
    local pool=$2
    local image=$3
    local offset=$4
    local num=$5

    "$BUILD_DIR/bin/rbd" --conf "$conf" bench \
        --io-type write \
        --io-size 4096 \
        --io-total $((num * 4096)) \
        --io-offset "$offset" \
        --io-pattern seq \
        --io-threads 1 \
        "$pool/$image" >/dev/null 2>&1
}
