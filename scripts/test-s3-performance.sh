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
#   8. perf_test_scattered_random_reads_concurrent
#                                          — 5 clients × 100 random 4 KB reads
#                                            across a 100 MB image (the user's
#                                            "5 VMs booting on same lazy base"
#                                            scenario: scattered reads, low
#                                            cross-client overlap, dominated by
#                                            per-read coordination overhead)
#
# Usage:
#   ./test-s3-performance.sh                   # run all 8 tests
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
    perf_test_scattered_random_reads_concurrent
    perf_test_concurrent_fio_zero_parent
    perf_test_reader_persists_visited_zero
    perf_test_post_backfill_zero_no_s3
    perf_test_partial_backfill_zero_no_s3
    perf_test_no_trust_zero_baseline
    perf_test_snap_clone_zero_baseline
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
    rm -rf "$MINIO_DATA_DIR" "$MINIO_TRACE_LOG"
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
    upload_to_s3 "$fixture" "$S3_BUCKET" "$s3_name"

    # Create RBD parent referencing that S3 object
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$POOL/$PERF_PARENT" \
        --size "${size_mb}M" --object-size 4M
    set_s3_config "$POOL/$PERF_PARENT" "$s3_name"

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

    # Diagnostic only (recorded but NOT asserted): the ratio of warm RADOS
    # write_ops to cold RADOS write_ops is intrinsically noisy because
    # `rados df` polls OSD-reported stats with multi-second lag (see
    # `mon_osd_report_interval_max`).  The same ~60 cold-pass writes can
    # be attributed to the cold window, the warm window, or split between
    # them depending on poll timing.  Repeated runs of this exact test
    # have produced ratios 0%, 5%, 11%, 38%, 42%, 62% with NO code
    # change in between -- making any ratio-based assertion flaky.
    if [ "$cold_writes" -gt 0 ]; then
        local ratio_pct=$(( warm_writes * 100 / cold_writes ))
        perf_record "$name" warm_to_cold_pct "$ratio_pct"   ratio
        log_info "$name: cold writes=$cold_writes, warm writes=$warm_writes (${ratio_pct}%, diagnostic only)"
    else
        log_info "$name: cold writes=$cold_writes, warm writes=$warm_writes (rados df polled before cold writes landed)"
    fi

    # HARD assertion on the real warm-cache invariant: the warm pass must
    # not go to S3.  Once the cold pass populates RADOS (which it does
    # regardless of when rados df sees the writes), subsequent reads of
    # the same objects must hit RADOS, not refetch from S3.  warm_s3_gets
    # is read directly from the MinIO trace log -- not subject to the
    # stats-polling lag that confounds rados df -- so it is unambiguous.
    if [ "$warm_s3_gets" -gt 0 ]; then
        log_fail "$name: warm pass fired $warm_s3_gets S3 GETs (expected 0)"
        log_fail "  warm reads should hit RADOS after the cold pass populated it"
        log_fail "  this is the canonical bug-2 warm-cache regression"
        return 1
    fi
    log_success "$name: warm pass had 0 S3 GETs (cache invariant holds)"
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

    # Targets (after c4 readahead prefetch):
    #   N GETs = each process fetches object 0 once (within-process LRU
    #            dedups the 16 × 64 KB sub-reads); prefetch fires K async
    #            GETs for object 1..K behind each fetch.  Net per process
    #            is bounded by (1 + K).  Cross-process: no LRU sharing,
    #            so the per-process count multiplies by N clients.
    # Ceiling: N × (1 + K) where K = rbd_s3_readahead_objects (default 2).
    # Beyond that, within-process duplicate fetching has regressed.
    #
    # Cross-process write_full dedup (the original concern this test was
    # built around) is now verified by test-s3-dedup-{read,writeback}.sh,
    # which checks the cls_lock-EBUSY accounting in the throttler's
    # WritebackRequest.  This test stays focused on within-process
    # fetch-coalesce behaviour.
    local readahead_k
    readahead_k=$("$BUILD_DIR/bin/ceph-conf" --conf "$CEPH_CONF" \
        -- --name client.admin --show-config-value rbd_s3_readahead_objects \
        2>/dev/null || echo 2)
    local ceiling=$(( n * (1 + readahead_k) ))
    if [ "$s3_gets" -le "$ceiling" ]; then
        log_success "$name: $s3_gets GETs (≤ ${ceiling}: $n clients × (1 fetch + ${readahead_k} prefetch))"
        return 0
    else
        log_fail "$name: $s3_gets GETs > $ceiling — within-process fetch dedup regressed"
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

    # Tightened (post code-review): the original assertion checked only
    # AGREEMENT across the 3 paths.  The user's bug #1 was specifically
    # that daemon WROTE the zero object (1) while read/write paths
    # didn't (0) — divergence in the "buggy" direction.  But a regression
    # in the SAME direction across all 3 (everyone writes zero again)
    # would have silently passed the old assertion.  Now we require the
    # CORRECT value (all three = 0, meaning no zero object in RADOS;
    # sparse parent pool, S3 re-fetch on next read) and only accept
    # "consistent but wrong" as an explicit failure with a distinct
    # diagnostic.
    if [ "$daemon_zero_present" = "0" ] && \
       [ "$read_zero_present"   = "0" ] && \
       [ "$write_zero_present"  = "0" ]; then
        log_success "$name: all 3 paths skipped zero object (sparse — correct)"
        return 0
    fi
    if [ "$daemon_zero_present" = "$read_zero_present" ] && \
       [ "$read_zero_present" = "$write_zero_present" ]; then
        log_fail "$name: all 3 paths consistent but in BUGGY direction"
        log_fail "  (zero object persisted in RADOS; parent pool no longer sparse)"
        log_fail "  Expected: daemon=0 read=0 write=0"
        log_fail "  Got:      daemon=$daemon_zero_present read=$read_zero_present write=$write_zero_present"
        return 1
    fi
    log_fail "$name: paths disagree on zero-object handling — bug #1"
    log_fail "  Expected: all three = 0 (sparse)"
    log_fail "  Got:      daemon=$daemon_zero_present read=$read_zero_present write=$write_zero_present"
    return 1
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

        # User-reported regression threshold: "VM startup time doubled" when
        # 4 VMs cloned from the same S3-backed parent boot concurrently.
        # 200% n4/n1 is exactly the scenario the user reported as broken.
        # Within-process dedup brought baseline to 142% pre-cls_lock; with
        # cross-process cls_lock + skip-writeback + peer-writeback poll we
        # measure ~98%.  Anything beyond 200% is the bug returning.
        if [ "$scale_pct" -gt 200 ]; then
            log_fail "$name: regression — N=4 took ${scale_pct}% of N=1 wall time"
            log_fail "  expected < 200% (the user's 'VM startup time doubled' threshold)"
            log_fail "  cross-process dedup or wait-for-peer-writeback is broken"
            return 1
        fi
    fi

    log_success "$name: scaling test complete"
    return 0
}

# ============================================================================
# Test 8 — scattered random reads across N concurrent clients
# ============================================================================
# Modelled after the user's "5 VMs booting on the same lazy base" scenario.
# Each VM does many small random reads scattered across the disk (boot
# reads partition table → FS metadata → file data at unrelated offsets).
# Cross-client overlap is LOW — different VMs hit different parts of the
# image — so cls_lock dedup buys very little.  This scenario is dominated
# by per-read coordination overhead (lock acquire → recheck → S3 GET →
# write_full → object_map update → unlock), not S3 throughput.
#
# Recorded so we can measure the effect of future optimisations
# targeting per-read overhead (drop recheck, async fire-and-forget
# write_back, skip lock entirely, etc.) WITHOUT having to read each
# change against the synthetic concurrent_read_dedup test, which
# exercises a workload (4 clients all on same object) that the user
# doesn't actually have.
#
# Asserts wall_time_per_read_ms — a soft regression budget so a future
# change that doubles per-read overhead trips the test, but absolute
# numbers stay informational (they depend on host RADOS + MinIO
# throughput, which varies).
perf_test_scattered_random_reads_concurrent() {
    local name="scattered_random_reads_concurrent"
    local n=5
    local reads_per_client=100
    local io_size=4096
    local total_per_client=$((reads_per_client * io_size))
    log_step "$name: $n clients × $reads_per_client × ${io_size}B random reads each"

    setup_fresh_image_n_children "$PERF_FIXTURE_PATTERN_100MB" "perf-pattern-100mb.raw" "$n"

    reset_trace
    local rados_before
    rados_before=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")

    local t0
    t0=$(perf_time_ms)
    perf_drive_concurrent_rand_reads "$CEPH_CONF" "$POOL" "$PERF_CHILD_BASE" \
        "$n" "$total_per_client" "$io_size" \
        || log_warn "$name: some readers failed (still recording metrics)"
    local elapsed
    elapsed=$(perf_time_elapsed_ms "$t0")

    # Let async fire-and-forget write-backs settle into RADOS counters.
    sleep 3

    local rados_after
    rados_after=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")
    local delta
    delta=$(perf_rados_delta "$rados_before" "$rados_after")
    local rados_writes
    rados_writes=$(perf_extract_field "$delta" write_ops)

    local s3_gets
    s3_gets=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")

    # Per-read latency = aggregate wall time / total reads.  With perfect
    # parallelism across N clients this would equal the wall-time of any
    # one client.  In practice it captures both per-client serialisation
    # and any cross-client contention.
    local total_reads=$((n * reads_per_client))
    local per_read_ms
    per_read_ms=$(awk "BEGIN{printf \"%.1f\", $elapsed / $total_reads}")

    perf_record "$name" num_clients         "$n"               count
    perf_record "$name" reads_per_client    "$reads_per_client" count
    perf_record "$name" io_size_bytes       "$io_size"         bytes
    perf_record "$name" wall_time_ms        "$elapsed"         ms
    perf_record "$name" s3_get_count        "$s3_gets"         count
    perf_record "$name" rados_writes        "$rados_writes"    count
    perf_record "$name" per_read_latency_ms "$per_read_ms"     ms

    log_info "$name: $elapsed ms wall, $s3_gets S3 GETs, $rados_writes RADOS writes"
    log_info "$name: avg per-read latency $per_read_ms ms across $total_reads reads"

    # Soft regression budget.  The original user-reported "VM boot too long"
    # symptom was ~500ms per read in their environment.  This test runs on
    # local MinIO with much lower latency; a per-read latency exceeding
    # 200ms here means coordination overhead has grown disproportionately
    # — likely a regression.  Tune upward if vstart's RADOS varies a lot.
    local per_read_budget_ms=200
    if [ "${per_read_ms%%.*}" -gt "$per_read_budget_ms" ]; then
        log_fail "$name: per-read latency $per_read_ms ms exceeds ${per_read_budget_ms} ms budget"
        log_fail "  coordination overhead (cls_lock + recheck + writeback + obj_map)"
        log_fail "  has likely regressed.  See perf_test_concurrent_read_dedup for"
        log_fail "  the within-object dedup behaviour; this test covers the OTHER"
        log_fail "  half of the user's workload (scattered, low-overlap reads)."
        return 1
    fi
    log_success "$name: per-read latency $per_read_ms ms within budget"
    return 0
}

# ============================================================================
# Test 8b — fio multi-child deep-queue random read on a COLD mostly-zero parent
# ============================================================================
# Faithfully reproduces the user's real workload (which the bash `rbd bench`
# drivers above do NOT): N child clones, each driven by fio's rbd ioengine at
# a DEEP queue depth, all random-reading a parent that is mostly zero and was
# NEVER backfilled (no on-disk bitmap, no backfill_status).
#
# This is the case where the OLD code re-fetched 4 MB of zeros from S3 on
# EVERY read of a zero object -- S3 GET count grew with the number of reads.
# With the P1 in-memory known-zero cache (ImageCtx::note_known_zero_object,
# consulted by read_object's pre-aio short-circuit and handle_read_object's
# ENOENT branch), each zero object is fetched at most once per child ImageCtx;
# subsequent reads short-circuit locally.  So S3 GETs become bounded by
# (unique objects x children), INDEPENDENT of how many reads are issued.
#
# Assertion: s3_get_count <= unique_objects * children * SLOP.  total reads is
# ~100x that bound, so a regression that reverts the short-circuit (every zero
# read -> S3 GET) trips the test decisively.  Skipped (not failed) when fio or
# its rbd engine is unavailable, so the suite stays portable.
perf_test_concurrent_fio_zero_parent() {
    local name="concurrent_fio_zero_parent"
    local io_size=4096
    # Defaults keep the committed test small + fast (works under the 32-entry
    # LRU, so it asserts P1 *fires* via the mechanism check rather than out-
    # scaling the LRU).  Override via env for a larger A/B stress run where the
    # working set exceeds rbd_s3_lru_max_entries and P1's GET-reduction shows:
    #   FIOZERO_FIXTURE=/tmp/sparse-512mb.raw FIOZERO_FIXTURE_MB=512 \
    #   FIOZERO_UNIQUE_OBJECTS=128 FIOZERO_IO_MB=64 FIOZERO_N=4
    local n="${FIOZERO_N:-4}"
    local io_mb="${FIOZERO_IO_MB:-8}"
    local fixture="${FIOZERO_FIXTURE:-$PERF_FIXTURE_SPARSE_40MB}"
    local fixture_mb="${FIOZERO_FIXTURE_MB:-40}"
    # SPARSE_40MB: 40 MB / 10 objects, only the first 2 filled -> 80% zero.
    local unique_objects="${FIOZERO_UNIQUE_OBJECTS:-10}"
    local per_child_io="${io_mb}M"

    if ! command -v fio >/dev/null 2>&1 || \
       ! fio --enghelp 2>/dev/null | grep -qw rbd; then
        log_warn "$name: fio (with rbd ioengine) not available -- skipping"
        return 0
    fi

    local reads_per_child=$(( io_mb * 1024 * 1024 / io_size ))
    log_step "$name: $n fio children x ~$reads_per_child random ${io_size}B reads, cold ${fixture_mb}M mostly-zero parent"

    setup_fresh_image_n_children "$fixture" "perf-fiozero.raw" "$n"

    # Reset trace AFTER setup (parent upload + clone touch S3); count only
    # workload GETs.  No backfill is run on purpose: backfill_visited is null,
    # so the persistent write-if-exists path is a no-op and we exercise the
    # pure in-memory P1 cache.
    reset_trace

    # fio's rbd ioengine uses librados, which honors $CEPH_CONF to find the
    # vstart cluster + admin keyring (the CLI tools take --conf; this is the
    # env equivalent).
    export CEPH_CONF
    # CRITICAL: the system /usr/bin/fio rbd engine links the SYSTEM librbd, not
    # our freshly-built tree.  Force it to load $BUILD_DIR/lib so the test
    # actually exercises the P1 code under review (same dev librbd/librados the
    # $BUILD_DIR/bin/rbd CLI uses via rpath).  The librbd C API fio consumes is
    # ABI-stable, so the soname override is safe.
    local ld_path="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    # Mechanism check (so a bounded GET count can't pass for the wrong reason --
    # e.g. coalescing/LRU masking re-fetches instead of P1 actually firing):
    # route child 1's librbd at debug 10 to a dedicated log via a debug copy of
    # the conf, then assert our short-circuit line appears.  Only child 1 is
    # instrumented to keep log volume + timing impact off the other children.
    local dbgconf dbglog
    dbgconf=$(mktemp /tmp/perf-fiozero-conf-XXXXXX)
    dbglog=$(mktemp /tmp/perf-fiozero-dbg-XXXXXX.log)
    cp "$CEPH_CONF" "$dbgconf"
    printf '\n[client]\n  debug rbd = 10\n  log file = %s\n  log to stderr = false\n' \
        "$dbglog" >> "$dbgconf"

    local t0
    t0=$(perf_time_ms)
    local pids=() logs=() i
    for i in $(seq 1 "$n"); do
        local flog
        flog=$(mktemp /tmp/perf-fiozero-XXXXXX.log)
        logs+=("$flog")
        local child_conf="$CEPH_CONF"
        [ "$i" -eq 1 ] && child_conf="$dbgconf"
        env LD_LIBRARY_PATH="$ld_path" CEPH_CONF="$child_conf" \
            fio --name="fiozero-$i" \
            --ioengine=rbd --pool="$POOL" --rbdname="${PERF_CHILD_BASE}-$i" \
            --rw=randread --bs="${io_size}" --iodepth=64 --numjobs=1 \
            --direct=1 --size="${fixture_mb}M" --io_size="$per_child_io" \
            --randrepeat=0 --group_reporting \
            >"$flog" 2>&1 &
        pids+=("$!")
    done
    local failures=0
    for i in "${!pids[@]}"; do
        wait "${pids[$i]}" || { failures=$((failures + 1)); \
            log_warn "$name: fio child $((i + 1)) failed; output:"; \
            tail -5 "${logs[$i]}" | sed 's/^/    /'; }
    done
    local elapsed
    elapsed=$(perf_time_elapsed_ms "$t0")
    rm -f "${logs[@]}"

    if [ "$failures" -ne 0 ]; then
        log_fail "$name: $failures/$n fio children failed"
        return 1
    fi

    # Let any in-flight cache-populate writebacks (data objects) settle.
    sleep 3

    local s3_gets
    s3_gets=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")
    local total_reads=$(( reads_per_child * n ))

    # Bound: each of the 10 objects fetched at most once per child, with slop
    # for concurrent cold misses that race ahead of the first recorded bit
    # (in-process S3ObjectFetcher coalescing collapses most of these).
    local slop=4
    local max_expected=$(( unique_objects * n * slop ))

    perf_record "$name" num_children    "$n"             count
    perf_record "$name" total_reads     "$total_reads"   count
    perf_record "$name" wall_time_ms    "$elapsed"       ms
    perf_record "$name" s3_get_count    "$s3_gets"       count
    perf_record "$name" max_expected    "$max_expected"  count

    log_info "$name: ${elapsed}ms, $s3_gets S3 GETs over $total_reads reads (bound $max_expected)"

    # Mechanism check: did the P1 short-circuit actually FIRE on child 1?  Both
    # the pre-aio and ENOENT branches log "in-memory known-zero".  This guards
    # against a bounded GET count passing for the wrong reason (in-process
    # coalescing + 2 s LRU masking re-fetches instead of P1 doing the work).
    local dbg_routed=0 short_circuits=0
    [ -s "$dbglog" ] && dbg_routed=1
    if [ "$dbg_routed" -eq 1 ]; then
        short_circuits=$(grep -c "in-memory known-zero" "$dbglog" 2>/dev/null || echo 0)
    fi
    perf_record "$name" short_circuits "$short_circuits" count
    rm -f "$dbgconf" "$dbglog"

    if [ "$s3_gets" -gt "$max_expected" ]; then
        log_fail "$name: $s3_gets S3 GETs exceeds bound of $max_expected"
        log_fail "  zero objects appear to be re-fetched per-read -- the P1"
        log_fail "  known-zero short-circuit (ImageCtx::note_known_zero_object +"
        log_fail "  read_object/handle_read_object checks) has likely regressed."
        return 1
    fi
    if [ "$dbg_routed" -eq 1 ] && [ "$short_circuits" -eq 0 ]; then
        log_fail "$name: GET count bounded but P1 short-circuit never logged on"
        log_fail "  child 1 -- low GETs may be coalescing/LRU masking, not P1."
        return 1
    fi
    if [ "$dbg_routed" -eq 0 ]; then
        log_warn "$name: debug-rbd log was empty; could not confirm P1 fired"
        log_warn "  (GET bound held, but mechanism unverified -- check conf routing)"
    fi
    log_success "$name: $s3_gets S3 GETs within bound $max_expected; $short_circuits P1 short-circuits fired"
    return 0
}

# ============================================================================
# Test 8c — reader persists VISITED_ZERO to an existing bitmap (write-if-exists)
# ============================================================================
# Covers the P1 persistent path: when the rbd_backfill_visited.<id> bitmap
# already exists, ImageCtx::note_known_zero_object fires a best-effort
# object_map_update marking the just-discovered zero object VISITED_ZERO, so
# the knowledge survives into a fresh ImageCtx (and across clients).  This is
# the half the pure-in-memory fio test cannot reach (there backfill_visited is
# null, so the persistent branch is skipped).
#
# Constructing the precondition deterministically without a bitmap decoder:
#   - The bitmap must EXIST but with the target zero object's bit still
#     VISITED_NO, so the reader's write is the ONLY thing that can set it.
#   - The daemon creates the bitmap (all VISITED_NO) at init, THEN marks
#     objects in order.  We start it, stop it the instant the bitmap object
#     appears, and target the LAST object -- with a data-heavy prefix the
#     daemon is still writing early data objects (slow 4 MB RADOS writes) and
#     has not reached the trailing zeros.
#
# Assertions (no decoder needed -- inferred end-to-end via S3 GET counts):
#   - Child A read of the target zero object: >= 1 S3 GET  (proves the bit was
#     VISITED_NO so the reader had to fetch -- and per P1 then persists ZERO).
#   - Fresh Child B read of the same object:  0 S3 GETs    (proves the write
#     landed and a new ImageCtx loads it, short-circuiting via the bitmap).
# Without the persistent write, Child B would re-fetch (GETs > 0).
perf_test_reader_persists_visited_zero() {
    local name="reader_persists_visited_zero"
    local obj_size=$((4 * 1024 * 1024))
    local data_objs=8           # objects 0..7 = data (32 MB of slow RADOS writes)
    local total_objs=16         # objects 8..15 = zero
    local target_obj=15         # LAST object: maximal margin before daemon reaches it
    local target_off=$(( target_obj * obj_size ))
    local img_mb=$(( total_objs * 4 ))

    if ! command -v fio >/dev/null 2>&1 || \
       ! fio --enghelp 2>/dev/null | grep -qw rbd; then
        log_warn "$name: fio (with rbd ioengine) not available -- skipping"
        return 0
    fi

    log_step "$name: persist VISITED_ZERO for object $target_obj via a child read, verify a fresh child short-circuits"

    # Data-heavy sparse fixture: first $data_objs objects filled, trailing
    # objects zero.  The filled prefix gives the daemon enough slow write work
    # that we can stop it before it marks the trailing zeros.
    local fix=/tmp/perf-persist-${img_mb}mb.raw
    dd if=/dev/zero of="$fix" bs=1M count="$img_mb" status=none
    local i
    for i in $(seq 0 $((data_objs - 1))); do
        printf "PARENT-BLOCK-%04d" "$i" | dd of="$fix" bs=4M seek="$i" conv=notrunc status=none
    done

    setup_fresh_image_n_children "$fix" "perf-persist.raw" 2

    # Resolve the parent's bitmap oid (rbd_backfill_visited.<image_id>).
    local parent_id
    parent_id=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$POOL/$PERF_PARENT" \
        | sed -n 's/^[[:space:]]*block_name_prefix: rbd_data\.//p')
    if [ -z "$parent_id" ]; then
        log_fail "$name: could not resolve parent image id"
        rm -f "$fix"
        return 1
    fi
    local bitmap_oid="rbd_backfill_visited.${parent_id}"

    # Schedule backfill, start the daemon directly (NOT run_backfill_daemon,
    # which sleeps 2 s -- long enough for a small image to finish), and stop it
    # the instant the bitmap object is created.
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$POOL/$PERF_PARENT"
    local blog
    blog=$(mktemp /tmp/perf-persist-bf-XXXXXX.log)
    "$BUILD_DIR/bin/rbd-backfill" --conf "$CEPH_CONF" --foreground >"$blog" 2>&1 &
    local bf_pid=$!
    local created=0
    for i in $(seq 1 200); do
        if "$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" \
                stat "$bitmap_oid" >/dev/null 2>&1; then
            created=1
            break
        fi
        kill -0 "$bf_pid" 2>/dev/null || break   # daemon exited early
        sleep 0.05
    done
    kill "$bf_pid" 2>/dev/null || true
    wait "$bf_pid" 2>/dev/null || true
    rm -f "$blog"

    if [ "$created" -ne 1 ]; then
        log_fail "$name: backfill-visited bitmap $bitmap_oid was never created"
        rm -f "$fix"
        return 1
    fi

    export CEPH_CONF
    local ld_path="$BUILD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    # Child A: read the target zero object.  Bit is VISITED_NO -> fetch from S3
    # (>= 1 GET) and persist VISITED_ZERO.
    reset_trace
    env LD_LIBRARY_PATH="$ld_path" CEPH_CONF="$CEPH_CONF" \
        fio --name=persistA --ioengine=rbd --pool="$POOL" \
            --rbdname="${PERF_CHILD_BASE}-1" --rw=read --bs=4096 \
            --offset="$target_off" --io_size=4096 --numjobs=1 --direct=1 \
            >/dev/null 2>&1 || log_warn "$name: child A read reported failure"
    local gets_a
    gets_a=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")

    # Let the fire-and-forget object_map_update land on disk before a fresh
    # ImageCtx loads the bitmap.
    sleep 2

    # Child B: fresh ImageCtx (different child image), same object.  If the
    # persistent write landed, its bitmap load sees VISITED_ZERO -> 0 GETs.
    reset_trace
    env LD_LIBRARY_PATH="$ld_path" CEPH_CONF="$CEPH_CONF" \
        fio --name=persistB --ioengine=rbd --pool="$POOL" \
            --rbdname="${PERF_CHILD_BASE}-2" --rw=read --bs=4096 \
            --offset="$target_off" --io_size=4096 --numjobs=1 --direct=1 \
            >/dev/null 2>&1 || log_warn "$name: child B read reported failure"
    local gets_b
    gets_b=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")

    rm -f "$fix"

    perf_record "$name" target_object   "$target_obj" count
    perf_record "$name" child_a_s3_gets "$gets_a"     count
    perf_record "$name" child_b_s3_gets "$gets_b"     count

    log_info "$name: child A $gets_a S3 GETs (expect >=1), child B $gets_b S3 GETs (expect 0)"

    if [ "$gets_a" -lt 1 ]; then
        log_fail "$name: child A made $gets_a S3 GETs -- precondition not met"
        log_fail "  the daemon marked object $target_obj before we stopped it"
        log_fail "  (too fast); increase data_objs so the write prefix is longer."
        return 1
    fi
    if [ "$gets_b" -ne 0 ]; then
        log_fail "$name: child B made $gets_b S3 GETs (expected 0)"
        log_fail "  the reader's persistent VISITED_ZERO write did not take"
        log_fail "  effect: ImageCtx::note_known_zero_object's object_map_update"
        log_fail "  on $bitmap_oid did not land, or a fresh ImageCtx is not"
        log_fail "  consulting it.  write-if-exists path is broken."
        return 1
    fi
    log_success "$name: reader persisted VISITED_ZERO (A=$gets_a GET, fresh B=0 GETs)"
    return 0
}

# ============================================================================
# Test 9 — post-backfill perf on mostly-zero parent (user-reported scenario)
# ============================================================================
# Mimics the user's "VM image with allocated head + zero tail" workload.  A
# typical cloud VM image has a small populated head (kernel, initrd, root
# FS) and a large zero tail (allocated but unused disk).  After running
# rbd-backfill to completion:
#   - non-zero blocks were copied to the parent's RADOS pool
#   - zero blocks were skipped by ObjectBackfillRequest's is_zero()
#     optimization (no write_full, no object_map update)
#   - backfill_status = "complete" -> ImageCtx::s3_backfill_complete = true
#
# Pre-fix (before commits 9a6d00e + 923f03b), random reads from a child
# clone would still go to S3 for every zero-block read, because the
# parent's read_object short-circuit was suppressed for S3-backed images
# and handle_read_object's ENOENT branch unconditionally fell through to
# should_read_from_s3 -> S3 GET.  Under N-VM concurrency this dominated
# wall time as the throttler kept dropping over-cap writes, locking
# clients into a refetch loop.
#
# Post-fix: zero-block reads short-circuit at handle_read_object's
# ENOENT branch when s3_backfill_complete is set -- finish(-ENOENT) and
# the upper librbd layer zero-fills the child read.  Zero S3 traffic.
#
# Asserts: s3_get_count == 0.  Wall time is recorded as a trend metric
# (varies with host RADOS latency; not asserted).
perf_test_post_backfill_zero_no_s3() {
    local name="post_backfill_zero_no_s3"
    local reads=200
    local io_size=4096
    local total_bytes=$((reads * io_size))

    # SPARSE_40MB: 40 MB / 10 blocks; only the first 2 blocks are filled.
    # 8 of 10 blocks (80%) are entirely zero -- close to the
    # allocated/total ratio of a typical thin-provisioned VM image.
    log_step "$name: $reads random ${io_size}B reads on backfilled sparse parent (80% zero)"

    setup_fresh_image "$PERF_FIXTURE_SPARSE_40MB" "perf-sparse-40mb.raw"

    # Run rbd-backfill to completion.  With 8 zero blocks skipped, the
    # daemon only writes the 2 data blocks; should finish in seconds.
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$POOL/$PERF_PARENT"
    local blog
    blog=$(mktemp /tmp/perf-bf-zero-XXXXXX.log)
    run_backfill_daemon "$CEPH_CONF" "$blog"

    # Wait for the trust signal (backfill_status=complete).  This is what
    # ImageCtx::apply_metadata reads to set s3_backfill_complete=true; the
    # fix's short-circuit depends on it.
    local complete=0
    local i
    for i in $(seq 1 30); do
        local s
        s=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
            image-meta get "$POOL/$PERF_PARENT" backfill_status 2>/dev/null \
            || echo "")
        if [ "$s" = "complete" ]; then
            complete=1
            break
        fi
        sleep 1
    done
    stop_backfill_daemon
    rm -f "$blog"

    if [ "$complete" -ne 1 ]; then
        log_fail "$name: backfill never reached complete within 30s"
        return 1
    fi

    # Reset the minio trace AFTER backfill so we only count workload
    # GETs, not setup GETs from the daemon.  rados_writes snapshot also
    # taken here to isolate workload-triggered throttler writes (should
    # be 0 since no S3 GETs -> no handle_read_from_s3 -> no throttler).
    reset_trace
    local rados_before
    rados_before=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")

    local t0
    t0=$(perf_time_ms)
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench \
        --io-type read \
        --io-size "$io_size" \
        --io-total "$total_bytes" \
        --io-pattern rand \
        --io-threads 1 \
        "$POOL/$PERF_CHILD" >/dev/null 2>&1 \
        || log_warn "$name: bench reported failure (still recording metrics)"
    local elapsed
    elapsed=$(perf_time_elapsed_ms "$t0")

    # Brief settle for any async accounting; the workload itself should
    # not have triggered writebacks (zero S3 GETs -> zero handle_read_
    # from_s3 -> zero try_submit calls).
    sleep 1

    local rados_after
    rados_after=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")
    local rados_writes
    rados_writes=$(perf_extract_field "$(perf_rados_delta "$rados_before" "$rados_after")" write_ops)

    local s3_gets
    s3_gets=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")

    local per_read_ms
    per_read_ms=$(awk "BEGIN{printf \"%.2f\", $elapsed / $reads}")

    perf_record "$name" reads             "$reads"        count
    perf_record "$name" io_size_bytes     "$io_size"      bytes
    perf_record "$name" wall_time_ms      "$elapsed"      ms
    perf_record "$name" s3_get_count      "$s3_gets"      count
    perf_record "$name" rados_writes      "$rados_writes" count
    perf_record "$name" per_read_ms       "$per_read_ms"  ms

    log_info "$name: $elapsed ms wall, $s3_gets S3 GETs, $rados_writes RADOS writes, $per_read_ms ms/read"

    # HARD assertion on the canonical post-backfill invariant.  The fix's
    # whole point is that backfill+complete should END S3 traffic for the
    # child -- whether the reads hit data blocks (RADOS HIT via aio_operate)
    # or zero blocks (short-circuit via handle_read_object ENOENT branch +
    # s3_backfill_complete).  Any non-zero count means the fix regressed.
    if [ "$s3_gets" -ne 0 ]; then
        log_fail "$name: $s3_gets S3 GETs after backfill complete (expected 0)"
        log_fail "  post-backfill zero-block short-circuit (s3_backfill_complete)"
        log_fail "  is not eliminating S3 traffic.  Check ImageCtx::s3_backfill_"
        log_fail "  complete propagation and handle_read_object's ENOENT branch."
        return 1
    fi
    log_success "$name: 0 S3 GETs after backfill complete"
    return 0
}

perf_test_partial_backfill_zero_no_s3() {
    local name="partial_backfill_zero_no_s3"
    local reads=200
    local io_size=4096
    local total_bytes=$((reads * io_size))

    # Same sparse 40 MB fixture as perf_test_post_backfill_zero_no_s3, but
    # this variant isolates the PER-OBJECT bitmap path from the whole-image
    # s3_backfill_complete flag.  Run backfill to completion (populates the
    # per-image rbd_backfill_visited.<id> bitmap), then delete the
    # backfill_status metadata key so the next ImageCtx open sees
    # s3_backfill_complete=false.  Reads must still hit zero S3 GETs --
    # the short-circuit fires via the bitmap's VISITED_ZERO bits, not
    # via the whole-image trust flag.
    log_step "$name: $reads random ${io_size}B reads with bitmap-only trust (partial-backfill simulation)"

    setup_fresh_image "$PERF_FIXTURE_SPARSE_40MB" "perf-sparse-40mb.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$POOL/$PERF_PARENT"
    local blog
    blog=$(mktemp /tmp/perf-bf-partial-XXXXXX.log)
    run_backfill_daemon "$CEPH_CONF" "$blog"

    local complete=0
    local i
    for i in $(seq 1 30); do
        local s
        s=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
            image-meta get "$POOL/$PERF_PARENT" backfill_status 2>/dev/null \
            || echo "")
        if [ "$s" = "complete" ]; then
            complete=1
            break
        fi
        sleep 1
    done
    stop_backfill_daemon
    rm -f "$blog"

    if [ "$complete" -ne 1 ]; then
        log_fail "$name: backfill never reached complete within 30s"
        return 1
    fi

    # Clear backfill_status so the next parent ImageCtx open sets
    # s3_backfill_complete=false.  The per-image bitmap stays intact (it
    # lives in its own RADOS object).  This isolates the bitmap path.
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
        image-meta remove "$POOL/$PERF_PARENT" backfill_status

    reset_trace
    local rados_before
    rados_before=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")

    local t0
    t0=$(perf_time_ms)
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench \
        --io-type read \
        --io-size "$io_size" \
        --io-total "$total_bytes" \
        --io-pattern rand \
        --io-threads 1 \
        "$POOL/$PERF_CHILD" >/dev/null 2>&1 \
        || log_warn "$name: bench reported failure (still recording metrics)"
    local elapsed
    elapsed=$(perf_time_elapsed_ms "$t0")

    sleep 1

    local rados_after
    rados_after=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")
    local rados_writes
    rados_writes=$(perf_extract_field "$(perf_rados_delta "$rados_before" "$rados_after")" write_ops)

    local s3_gets
    s3_gets=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")

    local per_read_ms
    per_read_ms=$(awk "BEGIN{printf \"%.2f\", $elapsed / $reads}")

    perf_record "$name" reads             "$reads"        count
    perf_record "$name" io_size_bytes     "$io_size"      bytes
    perf_record "$name" wall_time_ms      "$elapsed"      ms
    perf_record "$name" s3_get_count      "$s3_gets"      count
    perf_record "$name" rados_writes      "$rados_writes" count
    perf_record "$name" per_read_ms       "$per_read_ms"  ms

    log_info "$name: $elapsed ms wall, $s3_gets S3 GETs, $rados_writes RADOS writes, $per_read_ms ms/read"

    if [ "$s3_gets" -ne 0 ]; then
        log_fail "$name: $s3_gets S3 GETs with bitmap-only trust (expected 0)"
        log_fail "  Per-object backfill_visited bitmap short-circuit is not"
        log_fail "  firing.  Check ImageCtx::backfill_visited load in"
        log_fail "  apply_metadata and the bounds-checked consult in"
        log_fail "  ObjectReadRequest::handle_read_object."
        return 1
    fi
    log_success "$name: 0 S3 GETs with bitmap-only trust"
    return 0
}

perf_test_no_trust_zero_baseline() {
    local name="no_trust_zero_baseline"
    local reads=200
    local io_size=4096
    local total_bytes=$((reads * io_size))

    # Baseline counterpart to perf_test_partial_backfill_zero_no_s3.  Same
    # fixture and workload, but the parent has NEVER been backfilled:
    #   - no rbd_backfill_visited.<id> bitmap object (apply_metadata's
    #     synchronous load returns -ENOENT, ImageCtx::backfill_visited
    #     stays nullptr)
    #   - no backfill_status metadata key (s3_backfill_complete=false)
    #
    # Every read of a zero block must therefore fall through to S3.  The
    # LRU dedup means each unique zero object hits S3 at most once for
    # the duration of the workload, so the expected baseline is
    # roughly (unique zero objects touched in 200 random 4KB reads).
    # For PERF_FIXTURE_SPARSE_40MB (10 blocks, 8 zero), the upper bound
    # is 8; the partial-backfill variant must beat this by short-circuiting
    # all of them.
    log_step "$name: $reads random ${io_size}B reads with NO trust (baseline for bitmap delta)"

    setup_fresh_image "$PERF_FIXTURE_SPARSE_40MB" "perf-sparse-40mb.raw"

    # Deliberately skip backfill -- no bitmap, no s3_backfill_complete.
    reset_trace
    local rados_before
    rados_before=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")

    local t0
    t0=$(perf_time_ms)
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench \
        --io-type read \
        --io-size "$io_size" \
        --io-total "$total_bytes" \
        --io-pattern rand \
        --io-threads 1 \
        "$POOL/$PERF_CHILD" >/dev/null 2>&1 \
        || log_warn "$name: bench reported failure (still recording metrics)"
    local elapsed
    elapsed=$(perf_time_elapsed_ms "$t0")

    sleep 1

    local rados_after
    rados_after=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")
    local rados_writes
    rados_writes=$(perf_extract_field "$(perf_rados_delta "$rados_before" "$rados_after")" write_ops)

    local s3_gets
    s3_gets=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")

    local per_read_ms
    per_read_ms=$(awk "BEGIN{printf \"%.2f\", $elapsed / $reads}")

    perf_record "$name" reads             "$reads"        count
    perf_record "$name" io_size_bytes     "$io_size"      bytes
    perf_record "$name" wall_time_ms      "$elapsed"      ms
    perf_record "$name" s3_get_count      "$s3_gets"      count
    perf_record "$name" rados_writes      "$rados_writes" count
    perf_record "$name" per_read_ms       "$per_read_ms"  ms

    log_info "$name: $elapsed ms wall, $s3_gets S3 GETs, $rados_writes RADOS writes, $per_read_ms ms/read"

    # Diagnostic-only assertion: we EXPECT non-zero S3 GETs here (no trust
    # signal, so zero-block reads must consult S3).  If we see 0, something
    # is wrong with the test setup -- e.g., a previous run left a bitmap
    # behind in the pool, or the parent's S3 endpoint isn't being hit at
    # all.  A "zero baseline" would make the partial-backfill comparison
    # meaningless: the bitmap can't save what wasn't being spent.
    if [ "$s3_gets" -eq 0 ]; then
        log_fail "$name: 0 S3 GETs without any trust signal -- unexpected"
        log_fail "  Possible causes: stale rbd_backfill_visited.<id> from a"
        log_fail "  prior test run, or rados ls/pool cleanup didn't reset"
        log_fail "  the test pool.  Without S3 GETs as the baseline, the"
        log_fail "  partial-backfill bitmap savings are unmeasurable."
        return 1
    fi
    log_success "$name: baseline = $s3_gets S3 GETs (partial-backfill variant saves all of them)"
    return 0
}

perf_test_snap_clone_zero_baseline() {
    local name="snap_clone_zero_baseline"
    local reads=200
    local io_size=4096
    local total_bytes=$((reads * io_size))

    # "Speed of light" reference for the partial-backfill bitmap path: a
    # traditional snapshot-based RBD clone of the SAME sparse content
    # (10 x 4 MB blocks, 80% zero), running the SAME 200-random-4KB-read
    # workload.  Compares to perf_test_partial_backfill_zero_no_s3 and
    # perf_test_post_backfill_zero_no_s3.
    #
    # Expected gap: regular clone is faster than the S3-backed
    # post-backfill case because read_parent opens the snapshot with
    # snap_id != CEPH_NOSNAP, which loads the parent's object_map (per
    # RefreshRequest::send_v2_open_object_map at line ~1062).  Reads of
    # zero blocks short-circuit IN MEMORY at the read_object entry
    # point (object_map[N] == NONEXISTENT) and never aio_operate to
    # the OSD.  The S3-backed case has no object_map loaded (parent is
    # read-only opened without exclusive_lock) so every zero-block read
    # pays one RADOS RTT for aio_operate -> ENOENT before the
    # s3_backfill_complete / bitmap short-circuit fires in
    # handle_read_object.
    log_step "$name: $reads random ${io_size}B reads on traditional snap-clone (reference)"

    perf_pool_reset "$CEPH_CONF" "$POOL"

    local parent="snap-perf-parent"
    local snap="perf-snap"
    local child="snap-perf-child"
    local size_mb
    size_mb=$(( $(stat -c%s "$PERF_FIXTURE_SPARSE_40MB") / 1024 / 1024 ))

    # `rbd import` recognizes zero regions and skips writing them (sparse
    # object_map entries) -- same RADOS shape as a post-backfill S3-backed
    # parent: data blocks present, zero blocks NONEXISTENT.
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" import \
        --image-feature layering,exclusive-lock,object-map,fast-diff \
        --object-size 4M \
        "$PERF_FIXTURE_SPARSE_40MB" "$POOL/$parent" >/dev/null 2>&1

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" snap create "$POOL/$parent@$snap"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" snap protect "$POOL/$parent@$snap"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone "$POOL/$parent@$snap" \
        "$POOL/$child"

    reset_trace
    local rados_before
    rados_before=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")

    local t0
    t0=$(perf_time_ms)
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench \
        --io-type read \
        --io-size "$io_size" \
        --io-total "$total_bytes" \
        --io-pattern rand \
        --io-threads 1 \
        "$POOL/$child" >/dev/null 2>&1 \
        || log_warn "$name: bench reported failure (still recording metrics)"
    local elapsed
    elapsed=$(perf_time_elapsed_ms "$t0")

    sleep 1

    local rados_after
    rados_after=$(perf_rados_snapshot "$CEPH_CONF" "$POOL")
    local rados_reads
    rados_reads=$(perf_extract_field "$(perf_rados_delta "$rados_before" "$rados_after")" read_ops)

    local s3_gets
    s3_gets=$(perf_minio_get_count "$MINIO_TRACE_LOG" "$S3_BUCKET")

    local per_read_ms
    per_read_ms=$(awk "BEGIN{printf \"%.2f\", $elapsed / $reads}")

    perf_record "$name" reads             "$reads"        count
    perf_record "$name" io_size_bytes     "$io_size"      bytes
    perf_record "$name" wall_time_ms      "$elapsed"      ms
    perf_record "$name" s3_get_count      "$s3_gets"      count
    perf_record "$name" rados_reads       "$rados_reads"  count
    perf_record "$name" per_read_ms       "$per_read_ms"  ms

    log_info "$name: $elapsed ms wall, $s3_gets S3 GETs, $rados_reads RADOS reads, $per_read_ms ms/read"

    # No hard assertion: this test is a reference number, not a regression
    # gate.  S3 GETs MUST be 0 (no S3 config at all on the parent); if not,
    # something is very wrong in the test harness (e.g., a previous test
    # left the parent with S3 metadata that leaked through pool reset).
    if [ "$s3_gets" -ne 0 ]; then
        log_fail "$name: $s3_gets S3 GETs on a traditional clone -- pool reset leaked"
        return 1
    fi

    # Cleanup snapshot so pool reset on next test works cleanly.
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$child" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" snap unprotect "$POOL/$parent@$snap" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" snap rm "$POOL/$parent@$snap" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$parent" 2>/dev/null || true

    log_success "$name: reference per_read_ms=$per_read_ms (compare to partial_backfill_zero_no_s3)"
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
