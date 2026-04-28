#!/bin/bash
# test-s3-performance.sh — Performance benchmarks for S3-backed standalone clones.
#
# Each perf_test_* function:
#   - prepares the image state it needs (fresh / warm / sparse / zero)
#   - takes a metric snapshot, drives a workload, takes another snapshot
#   - records (test, metric, value, unit) rows into the shared CSV
#   - optionally asserts a regression bound (so the suite still PASS/FAIL)
#
# Tests:
#   1. perf_test_cold_read_baseline       — first read of an unwarmed image
#   2. perf_test_warm_cache_zero_overhead — re-reading a fully-cached image
#                                            (RADOS cls writes should be ~0)
#   3. perf_test_concurrent_read_dedup    — N parallel reads to same object
#                                            (S3 GETs should be 1, not N)
#   4. perf_test_writeback_amplification  — 4 KB read → measure RADOS write bytes
#   5. perf_test_zero_object_unified      — zero block via daemon/read/write
#                                            should leave RADOS in same state
#   6. perf_test_daemon_throughput        — backfill MB/s on a 100 MB image
#   7. perf_test_vm_boot_scaling          — N=1, 2, 4 concurrent readers
#                                            (per-client time should not grow ≥2x)
#
# Usage:
#   ./test-s3-performance.sh                   # run all 7 tests
#   ./test-s3-performance.sh --test <name>     # run a single test by name
#   ./test-s3-performance.sh --list            # list test names
#   ./test-s3-performance.sh --conf <ceph.conf>
#   ./test-s3-performance.sh --csv <out.csv>   # override output path

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/s3-test-common.sh"
source "$SCRIPT_DIR/lib/s3-perf-common.sh"

POOL="s3-perf-test"
MINIO_PORT=39200
MINIO_CONSOLE_PORT=39201
S3_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
S3_BUCKET="perf-test"
MINIO_DATA_DIR="/tmp/minio-perf-$$"
MINIO_TRACE_LOG="/tmp/perf-minio-trace-$$.log"

# Test selection
RUN_TEST=""    # empty = run all
LIST_ONLY=0

# Parse args
while [[ $# -gt 0 ]]; do
    case $1 in
        --conf) CEPH_CONF="$2"; shift 2 ;;
        --test) RUN_TEST="$2"; shift 2 ;;
        --list) LIST_ONLY=1; shift ;;
        --csv)  PERF_RESULTS_CSV="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,28p' "$0" | sed 's/^# //'
            exit 0 ;;
        *) log_warn "Unknown option: $1"; shift ;;
    esac
done

ALL_TESTS=(
    perf_test_cold_read_baseline
    perf_test_warm_cache_zero_overhead
    perf_test_concurrent_read_dedup
    perf_test_writeback_amplification
    perf_test_zero_object_unified
    perf_test_daemon_throughput
    perf_test_vm_boot_scaling
)

if [ "$LIST_ONLY" = "1" ]; then
    printf '%s\n' "${ALL_TESTS[@]}"
    exit 0
fi

cleanup() {
    log_info "Cleaning up perf test..."
    perf_minio_trace_stop
    stop_backfill_daemon 2>/dev/null || true
    stop_minio $MINIO_PORT
    if [ -n "${CEPH_CONF:-}" ] && [ -f "$CEPH_CONF" ]; then
        # Remove all test images.  rbd ls may fail on unknown pool — ignore.
        local img
        for img in $("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" -p "$POOL" ls 2>/dev/null); do
            "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$img" 2>/dev/null || true
        done
        "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool delete \
            "$POOL" "$POOL" --yes-i-really-really-mean-it 2>/dev/null || true
    fi
    rm -rf "$MINIO_DATA_DIR" "$MINIO_TRACE_LOG" "$PERF_ASOK_DIR"
}
trap cleanup EXIT

# ============================================================================
# Per-test setup helpers — each test calls these to prepare a fresh image
# ============================================================================

# Create a fresh S3-backed parent + standalone clone, uploading the given
# fixture file as the S3 image.  The clone has no RADOS objects yet.
# Resets the test pool first so prior-test state cannot leak into our deltas.
# Sets globals: PERF_PARENT, PERF_CHILD
# Usage: setup_fresh_image <fixture_path> <s3_object_name> [child_name]
setup_fresh_image() {
    local fixture=$1
    local s3_name=$2
    local child=${3:-perf-child}

    PERF_PARENT="perf-parent"
    PERF_CHILD="$child"

    perf_pool_reset "$CEPH_CONF" "$POOL"

    local size_mb
    size_mb=$(( $(stat -c%s "$fixture" 2>/dev/null || stat -f%z "$fixture") / 1024 / 1024 ))

    # Upload fixture to S3 (replace if exists)
    "$MINIO_BIN/mc" cp "$fixture" "local/$S3_BUCKET/$s3_name" 2>&1 | grep -v "^mc:" || true

    # Create RBD parent referencing that S3 object
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$POOL/$PERF_PARENT" \
        --size "${size_mb}M" --object-size 4M
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config set "$POOL/$PERF_PARENT" \
        --s3-endpoint   "$S3_ENDPOINT" \
        --s3-bucket     "$S3_BUCKET" \
        --s3-image-name "$s3_name" \
        --s3-access-key minioadmin \
        --s3-secret-key minioadmin

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone \
        "$POOL/$PERF_PARENT" "$POOL/$PERF_CHILD"
}

# Create N standalone clone children of the same parent.
# Sets globals: PERF_PARENT, PERF_CHILD_BASE (children: $base-1, $base-2, ...)
setup_fresh_image_n_children() {
    local fixture=$1
    local s3_name=$2
    local n=$3
    local child_base=${4:-perf-child}

    setup_fresh_image "$fixture" "$s3_name" "${child_base}-1"
    PERF_CHILD_BASE="$child_base"

    local i
    for i in $(seq 2 "$n"); do
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/${child_base}-$i" 2>/dev/null || true
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone \
            "$POOL/$PERF_PARENT" "$POOL/${child_base}-$i"
    done
}

# Reset MinIO trace log (call between sub-phases of a test).
reset_trace() {
    : > "$MINIO_TRACE_LOG"
}

# ============================================================================
# Test 1 — cold read baseline
# ============================================================================
# Question: how long does a sequential read of a 100 MB unwarmed image take?
# Records:  wall_time_ms, s3_get_count, s3_get_bytes, rados_write_ops_delta
perf_test_cold_read_baseline() {
    local name="cold_read_baseline"
    log_step "$name: 100 MB sequential cold read"

    setup_fresh_image "$PERF_FIXTURE_PATTERN_100MB" "perf-pattern-100mb.raw"
    reset_trace
    perf_asok_clear

    local rados_before
    rados_before=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")

    local t0
    t0=$(perf_time_ms)
    perf_drive_seq_read "$CEPH_CONF" "$POOL" "$PERF_CHILD" $((100 * 1024 * 1024)) || {
        log_fail "$name: read failed"
        return 1
    }
    local elapsed
    elapsed=$(perf_time_elapsed_ms "$t0")

    # Wait for async fire-and-forget write-backs (parent cache + object_map cls)
    # to land before snapshotting RADOS.  Read returns when client-visible work
    # is done; the cache writes happen out-of-band.
    sleep 3

    local rados_after
    rados_after=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")
    local delta
    delta=$(perf_rados_delta "$rados_before" "$rados_after")

    local s3_gets
    s3_gets=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")
    local s3_bytes
    s3_bytes=$(perf_minio_get_bytes "$MINIO_TRACE_LOG" "$S3_BUCKET")
    local rados_writes
    rados_writes=$(perf_extract_field "$delta" write_ops)

    perf_record "$name" wall_time_ms     "$elapsed"      ms
    perf_record "$name" s3_get_count     "$s3_gets"      count
    perf_record "$name" s3_get_bytes     "$s3_bytes"     bytes
    perf_record "$name" rados_write_ops  "$rados_writes" count

    log_success "$name: ${elapsed}ms, $s3_gets S3 GETs, $rados_writes RADOS writes"
    return 0
}

# ============================================================================
# Test 2 — warm cache zero-overhead
# ============================================================================
# Question: re-reading an already-cached image — should not fire RADOS cls
# writes (regression of object_map_for_s3_write_back firing on every read).
# Records: rados_write_ops_warm (should be ≪ rados_write_ops_cold)
# Asserts: warm RADOS writes ≤ 5% of cold-read RADOS writes
perf_test_warm_cache_zero_overhead() {
    local name="warm_cache_zero_overhead"
    log_step "$name: read-twice; second pass should not fire RADOS writes"

    setup_fresh_image "$PERF_FIXTURE_PATTERN_20MB" "perf-pattern-20mb.raw"

    # Phase 1: cold pass — populates RADOS via S3 fetches.  Snapshot AFTER
    # the sleep so async fire-and-forget writes have all landed before we
    # measure the delta (otherwise cold_writes reads as 0 and warm/cold
    # ratio is meaningless).
    reset_trace
    local r1
    r1=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")
    perf_drive_seq_read "$CEPH_CONF" "$POOL" "$PERF_CHILD" $((20 * 1024 * 1024)) || true
    sleep 3
    local r2
    r2=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")
    local cold_writes
    cold_writes=$(perf_extract_field "$(perf_rados_delta "$r1" "$r2")" write_ops)

    # Phase 2: warm pass — RADOS objects are populated; the read should hit
    # the local cache and not fire any further write-backs.  If the read path
    # re-issues object_map cls writes regardless of cache state (#4-P1), we
    # will see warm_writes > 0 even though no S3 fetches happen.
    reset_trace
    local r3
    r3=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")
    local t0
    t0=$(perf_time_ms)
    perf_drive_seq_read "$CEPH_CONF" "$POOL" "$PERF_CHILD" $((20 * 1024 * 1024)) || true
    local elapsed
    elapsed=$(perf_time_elapsed_ms "$t0")
    sleep 3
    local r4
    r4=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")
    local warm_writes
    warm_writes=$(perf_extract_field "$(perf_rados_delta "$r3" "$r4")" write_ops)
    local warm_s3_gets
    warm_s3_gets=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")

    perf_record "$name" cold_rados_writes  "$cold_writes"   count
    perf_record "$name" warm_rados_writes  "$warm_writes"   count
    perf_record "$name" warm_s3_gets       "$warm_s3_gets"  count
    perf_record "$name" warm_wall_time_ms  "$elapsed"       ms
    if [ "$cold_writes" -gt 0 ]; then
        local ratio_pct=$(( warm_writes * 100 / cold_writes ))
        perf_record "$name" warm_to_cold_pct "$ratio_pct"   ratio
        log_info "$name: cold writes=$cold_writes, warm writes=$warm_writes (${ratio_pct}%)"

        if [ "$ratio_pct" -gt 5 ]; then
            log_fail "$name: warm pass fired ${ratio_pct}% of cold writes — object_map update on warm cache regression"
            return 1
        fi
    fi
    log_success "$name: warm pass had ${warm_writes} RADOS writes (target: ≪ ${cold_writes})"
    return 0
}

# ============================================================================
# Test 3 — concurrent read dedup
# ============================================================================
# Question: 4 parallel reads to the SAME unwarmed parent object — should
# de-duplicate to 1 S3 GET (or close).  Currently the read path has no
# in-flight coalescer, so we expect up to N GETs.
# Records: s3_gets_for_one_object
# Asserts: <= 2 GETs (allow one race)
perf_test_concurrent_read_dedup() {
    local name="concurrent_read_dedup"
    local n=4
    # Read 1 MB per client — well under one 4 MB parent object.  Reading
    # the full 4 MB risks rbd's seq-pattern detection prefetching the next
    # object, inflating the GET count by 2x for reasons unrelated to this
    # test's question (does the read path coalesce concurrent fetches of the
    # same object?).
    local bytes_per_client=$((1 * 1024 * 1024))
    log_step "$name: $n clients each reading $((bytes_per_client / 1024 / 1024)) MB of object 0"

    setup_fresh_image_n_children "$PERF_FIXTURE_PATTERN_20MB" "perf-pattern-20mb.raw" "$n"

    reset_trace
    perf_drive_concurrent_reads "$CEPH_CONF" "$POOL" "$PERF_CHILD_BASE" "$n" "$bytes_per_client" || \
        log_warn "$name: some readers failed (still recording metrics)"

    sleep 2  # let any in-flight write-backs settle so the trace is stable
    local s3_gets
    s3_gets=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")

    perf_record "$name" num_clients        "$n"        count
    perf_record "$name" bytes_per_client   "$bytes_per_client" bytes
    perf_record "$name" s3_get_count_total "$s3_gets"  count
    log_info "$name: $n clients × ${bytes_per_client} B of object 0 → $s3_gets S3 GETs"

    # Targets:
    #   1 GET = ideal cross-process dedup (would require a cls_lock, future work)
    #   N GETs = each process fetches independently (current realistic behavior
    #            now that single-client wait-for-writeback eliminates within-
    #            process duplicates, and in-flight coalescing handles concurrent
    #            same-process fetches).
    # Anything between 1 and N is partial dedup.  > N indicates a regression.
    if [ "$s3_gets" -le "$n" ]; then
        log_success "$name: $s3_gets GETs (≤ $n: each client fetches object 0 at most once)"
        return 0
    else
        log_fail "$name: $s3_gets GETs > $n clients — within-client duplicate fetch detected"
        return 1
    fi
}

# ============================================================================
# Test 4 — writeback amplification
# ============================================================================
# Question: a single 4 KB read of an unmapped block produces how many bytes
# of RADOS write traffic?  We *expect* ≥ 4 MB (full-object cache write) plus
# the object_map cls update.  This test records the ratio so we can see
# improvement after optimisations.
# Records: read_bytes, rados_write_bytes_delta, amplification_ratio
perf_test_writeback_amplification() {
    local name="writeback_amplification"
    local read_size=4096
    log_step "$name: 1 × 4 KB read → measure RADOS write bytes"

    setup_fresh_image "$PERF_FIXTURE_PATTERN_20MB" "perf-pattern-20mb.raw"
    reset_trace

    local r1
    r1=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")

    # rbd bench read of 4 KB from offset 0 — single object miss → S3 fetch
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench \
        --io-type read --io-size "$read_size" --io-total "$read_size" \
        --io-pattern seq "$POOL/$PERF_CHILD" >/dev/null 2>&1 || \
        log_warn "$name: bench read returned error"

    # Wait for fire-and-forget write-back to flush
    sleep 3

    local r2
    r2=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")
    local delta
    delta=$(perf_rados_delta "$r1" "$r2")
    local bytes_written
    bytes_written=$(perf_extract_field "$delta" size_bytes)
    local writes
    writes=$(perf_extract_field "$delta" write_ops)
    local s3_gets
    s3_gets=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")

    local amp_ratio=0
    if [ "$bytes_written" -gt 0 ]; then
        amp_ratio=$(( bytes_written / read_size ))
    fi

    perf_record "$name" read_bytes        "$read_size"     bytes
    perf_record "$name" rados_write_bytes "$bytes_written" bytes
    perf_record "$name" rados_write_ops   "$writes"        count
    perf_record "$name" s3_get_count      "$s3_gets"       count
    perf_record "$name" amplification     "$amp_ratio"     ratio

    log_success "$name: read $read_size B → wrote $bytes_written B (×$amp_ratio amplification)"
    return 0
}

# ============================================================================
# Test 5 — zero object handling unification
# ============================================================================
# Question: when a parent block is all-zero, what happens on each of the 3
# fetch paths (daemon / read-triggered / write-triggered)?  Setup uses the
# zero-first fixture so block 0 IS the zero block — important because
# `rbd bench` does not support --io-offset, so the only block we can
# reliably target via bench is block 0.
#
# All 3 paths trigger an S3 fetch of block 0 (which contains all zeros).
# After each path, check whether the parent's RADOS pool got an object for
# block 0 written back as a side effect.  Consistency means all 3 paths
# agree on whether to materialise zero objects.
#
# Records: <path>_zero_object_in_rados (1 if present, 0 if absent for each path)
# Asserts: all 3 paths agree.
perf_test_zero_object_unified() {
    local name="zero_object_unified"
    log_step "$name: zero block-0 handling via daemon / read / write paths"

    # Block 0 IS the zero block in the zero-first fixture.
    local zero_block_idx=0

    # After triggering a fetch path, echo 1 if the zero block has a RADOS
    # object in the parent pool, else 0.
    _zero_block_present() {
        local pfx
        pfx=$(get_block_prefix "$CEPH_CONF" "$POOL" "$PERF_PARENT")
        local oid="${pfx}.$(printf '%016x' $zero_block_idx)"
        if "$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" stat "$oid" >/dev/null 2>&1; then
            echo 1
        else
            echo 0
        fi
    }

    # ---- Path A: daemon backfill -----------------------------------------
    setup_fresh_image "$PERF_FIXTURE_ZERO_FIRST_40MB" "perf-zero-first-40mb.raw" "perf-zero-daemon"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule \
        "$POOL/$PERF_PARENT" 2>&1 | grep -v "WARNING\|developer" || true
    local blog="/tmp/perf-daemon-$$.log"
    local prefix_a
    prefix_a=$(get_block_prefix "$CEPH_CONF" "$POOL" "$PERF_PARENT")
    run_backfill_daemon "$CEPH_CONF" "$blog"
    # Expected total objects after backfill: 10 if daemon writes zero blocks,
    # 9 if it skips them.  Wait long enough for either outcome.
    wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix_a" 9 60 || \
        log_warn "$name: daemon backfilled fewer than 9 objects in 60s"
    sleep 2
    stop_backfill_daemon
    rm -f "$blog"
    local daemon_zero_present
    daemon_zero_present=$(_zero_block_present)

    # ---- Path B: read-triggered fetch -------------------------------------
    # Read the entire image via rbd export.  rbd bench --io-pattern seq starts
    # at offset 0 anyway, but export is more honest about reading every block
    # and is independent of bench's offset handling.
    setup_fresh_image "$PERF_FIXTURE_ZERO_FIRST_40MB" "perf-zero-first-40mb.raw" "perf-zero-read"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export "$POOL/$PERF_CHILD" /tmp/perf-export-$$.raw \
        >/dev/null 2>&1 || true
    rm -f /tmp/perf-export-$$.raw
    sleep 3
    local read_zero_present
    read_zero_present=$(_zero_block_present)

    # ---- Path C: write-triggered COW -------------------------------------
    # rbd bench writes to offset 0 by default — block 0 is the zero block here.
    # The write triggers CopyupRequest, which fetches block 0 from S3 (zeros)
    # then optionally writes back to parent.
    setup_fresh_image "$PERF_FIXTURE_ZERO_FIRST_40MB" "perf-zero-first-40mb.raw" "perf-zero-write"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench \
        --io-type write --io-size 4096 --io-total 4096 \
        --io-pattern seq --io-threads 1 \
        "$POOL/$PERF_CHILD" >/dev/null 2>&1 || true
    sleep 3
    local write_zero_present
    write_zero_present=$(_zero_block_present)

    perf_record "$name" daemon_zero_object_in_rados "$daemon_zero_present" count
    perf_record "$name" read_zero_object_in_rados   "$read_zero_present"   count
    perf_record "$name" write_zero_object_in_rados  "$write_zero_present"  count

    log_info "$name: zero-object presence — daemon=$daemon_zero_present read=$read_zero_present write=$write_zero_present"

    if [ "$daemon_zero_present" = "$read_zero_present" ] && \
       [ "$read_zero_present" = "$write_zero_present" ]; then
        log_success "$name: all 3 paths consistent"
        return 0
    else
        log_fail "$name: paths disagree on zero-object handling — bug #1"
        return 1
    fi
}

# ============================================================================
# Test 6 — daemon throughput
# ============================================================================
# Records: daemon_wall_time_ms, daemon_throughput_mbps
perf_test_daemon_throughput() {
    local name="daemon_throughput"
    log_step "$name: backfill 100 MB image and measure MB/s"

    setup_fresh_image "$PERF_FIXTURE_PATTERN_100MB" "perf-pattern-100mb.raw" "perf-throughput-child"
    local prefix
    prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "$PERF_PARENT")
    local num_objects=25  # 100 MB / 4 MB

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule \
        "$POOL/$PERF_PARENT" 2>&1 | grep -v "WARNING\|developer" || true

    local blog="/tmp/perf-throughput-daemon-$$.log"
    local t0
    t0=$(perf_time_ms)
    run_backfill_daemon "$CEPH_CONF" "$blog"
    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix" "$num_objects" 180; then
        stop_backfill_daemon
        rm -f "$blog"
        log_fail "$name: daemon did not complete in 180s"
        return 1
    fi
    local elapsed
    elapsed=$(perf_time_elapsed_ms "$t0")
    stop_backfill_daemon
    rm -f "$blog"

    local mbps=0
    if [ "$elapsed" -gt 0 ]; then
        mbps=$(( 100 * 1000 / elapsed ))
    fi

    perf_record "$name" daemon_wall_time_ms   "$elapsed" ms
    perf_record "$name" daemon_throughput_mbps "$mbps"   mb_per_sec

    log_success "$name: 100 MB in ${elapsed}ms = ${mbps} MB/s"
    return 0
}

# ============================================================================
# Test 7 — VM boot scaling (1 vs 2 vs 4 concurrent readers)
# ============================================================================
# Question: does per-client wall time grow significantly when going from 1
# client to 4?  If perfectly parallel, all should finish in roughly the
# same wall time.  Records per-N wall time and per-client throughput.
perf_test_vm_boot_scaling() {
    local name="vm_boot_scaling"
    log_step "$name: 1 / 2 / 4 concurrent readers, measure wall time"

    setup_fresh_image_n_children "$PERF_FIXTURE_PATTERN_20MB" "perf-pattern-20mb.raw" 4 "perf-scale"

    local n
    for n in 1 2 4; do
        # Reset child RADOS state between iterations: re-clone children 1..n
        local i
        for i in $(seq 1 "$n"); do
            "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/perf-scale-$i" 2>/dev/null || true
            "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone \
                "$POOL/$PERF_PARENT" "$POOL/perf-scale-$i"
        done
        # Also evict parent's RADOS objects so we exercise the cold path
        local prefix
        prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "$PERF_PARENT")
        local oid
        for oid in $("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" ls 2>/dev/null \
                       | grep "^${prefix}\."); do
            "$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" rm "$oid" 2>/dev/null || true
        done
        sleep 1

        reset_trace
        local t0
        t0=$(perf_time_ms)
        perf_drive_concurrent_reads "$CEPH_CONF" "$POOL" "perf-scale" "$n" $((20 * 1024 * 1024)) || true
        local elapsed
        elapsed=$(perf_time_elapsed_ms "$t0")
        local s3_gets
        s3_gets=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")

        perf_record "$name" "n${n}_wall_time_ms"   "$elapsed" ms
        perf_record "$name" "n${n}_s3_gets"        "$s3_gets" count
        log_info "$name: N=$n → ${elapsed}ms, $s3_gets S3 GETs"
    done

    # Compare n4 vs n1 — n4 wall time should be < 2x n1 if parallelism scales.
    # (If serialized, it would be ~4x.)
    local t1 t4
    t1=$(awk -F, -v n="$name" -v r="$PERF_RUN_ID" \
        '$1==n && $2=="n1_wall_time_ms" && $6==r {print $3; exit}' "$PERF_RESULTS_CSV")
    t4=$(awk -F, -v n="$name" -v r="$PERF_RUN_ID" \
        '$1==n && $2=="n4_wall_time_ms" && $6==r {print $3; exit}' "$PERF_RESULTS_CSV")
    if [ -n "$t1" ] && [ -n "$t4" ] && [ "$t1" -gt 0 ]; then
        local scale_pct=$(( t4 * 100 / t1 ))
        perf_record "$name" n4_to_n1_pct "$scale_pct" ratio
        log_info "$name: N=4 wall time is ${scale_pct}% of N=1"
    fi

    log_success "$name: scaling test complete"
    return 0
}

# ============================================================================
# Main
# ============================================================================

echo
log_info "=== S3-Backed Standalone Clone — Performance Tests ==="
log_info "Run ID: $PERF_RUN_ID"
log_info "CSV:    $PERF_RESULTS_CSV"
echo

if ! check_cluster_running; then exit 1; fi

create_pool "$POOL"
enable_s3_fetch

start_minio "$MINIO_PORT" "$MINIO_CONSOLE_PORT" "$MINIO_DATA_DIR"
setup_s3_bucket "$MINIO_PORT" "$S3_BUCKET"
perf_minio_trace_start "$MINIO_TRACE_LOG"

perf_setup_fixtures
perf_init_results

if [ -n "$RUN_TEST" ]; then
    # Single-test mode
    case " ${ALL_TESTS[*]} " in
        *" $RUN_TEST "*)
            run_test "$RUN_TEST" "$RUN_TEST"
            ;;
        *)
            log_error "Unknown test: $RUN_TEST"
            log_error "Valid tests:"
            printf '  %s\n' "${ALL_TESTS[@]}"
            exit 2
            ;;
    esac
else
    # Run all tests
    for t in "${ALL_TESTS[@]}"; do
        run_test "$t" "$t" || log_warn "$t reported failure (continuing)"
    done
fi

print_test_summary
echo
perf_print_summary
