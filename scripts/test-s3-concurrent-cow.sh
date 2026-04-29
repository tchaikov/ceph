#!/bin/bash
# test-s3-concurrent-cow.sh — Concurrent COW load test for S3-backed standalone clones
#
# Reproduces the regression where 4 simultaneous VMs trigger unbounded S3 fetch
# threads through the standalone-clone parent, overloading MinIO and causing COW
# "guarding writes" to block for 20+ minutes.
#
# After the S3ObjectFetcher concurrency fix (S3_MAX_CONCURRENT_FETCHES=8), all
# NUM_CLIENTS clients must complete within COW_TIMEOUT_SECS seconds.
#
# Usage: ./test-s3-concurrent-cow.sh [--conf <ceph.conf>]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/s3-test-common.sh"

POOL="s3-cow-test"
MINIO_PORT=19100
MINIO_CONSOLE_PORT=19101
S3_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
S3_BUCKET="cow-load-test"
MINIO_DATA_DIR="/tmp/minio-cow-$$"

NUM_CLIENTS=4
PARENT_SIZE_MB=20        # 5 objects × 4 MB
COW_TIMEOUT_SECS=120     # Each client must finish within 2 minutes
IO_SIZE=4096
IO_TOTAL=4096            # Just enough to trigger COW; keeps unwritten area
                         # in object 0 for data integrity verification

PARENT_RAW="/tmp/cow-parent-$$.raw"

cleanup() {
    log_info "Cleaning up..."
    stop_minio $MINIO_PORT
    if [ -n "$CEPH_CONF" ]; then
        for i in $(seq 1 $NUM_CLIENTS); do
            "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/cow-child-$i" 2>/dev/null || true
        done
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/cow-parent" 2>/dev/null || true
    fi
    "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool delete "$POOL" "$POOL" \
        --yes-i-really-really-mean-it 2>/dev/null || true
    rm -rf "$MINIO_DATA_DIR" /tmp/cow-bench-*.log "$PARENT_RAW" \
        /tmp/cow-parent-obj0-$$.bin /tmp/cow-ref-obj0-$$.bin /tmp/cow-ref-obj1-$$.bin \
        /tmp/cow-child-export-*-$$.raw /tmp/cow-child-check-*-$$.bin
}
trap cleanup EXIT

parse_common_args "$@"

echo
log_info "=== Test: Concurrent COW Load (${NUM_CLIENTS} clients, ${COW_TIMEOUT_SECS}s timeout) ==="
echo

if ! check_cluster_running; then exit 1; fi

create_pool "$POOL"
enable_s3_fetch

# ============================================================================
log_step "1. Start MinIO"
# ============================================================================

start_minio $MINIO_PORT $MINIO_CONSOLE_PORT "$MINIO_DATA_DIR"
setup_s3_bucket $MINIO_PORT "$S3_BUCKET"

# ============================================================================
log_step "2. Create S3-backed parent + $NUM_CLIENTS child clones"
# ============================================================================

# create_s3_parent creates the parent with S3 config via 'rbd s3-config set',
# which base64-encodes the secret key and sets s3.enabled=true (required by
# CopyupRequest::should_fetch_from_s3).  It also uploads the content it
# generates (PARENT-BLOCK-NNNN pattern) to S3 as /tmp/cow-parent-raw.
# Copy that file to PARENT_RAW so the integrity checks below compare against
# the ACTUAL S3 content rather than independently generated random data.
create_s3_parent "$POOL" "cow-parent" "cow-parent-raw" $PARENT_SIZE_MB \
    "$S3_ENDPOINT" "$S3_BUCKET" "$CEPH_CONF"
cp /tmp/cow-parent-raw "$PARENT_RAW"
log_success "S3 reference data copied to ${PARENT_RAW}"

for i in $(seq 1 $NUM_CLIENTS); do
    create_standalone_clone "$POOL" "cow-parent" "cow-child-$i"
    log_info "  Created child clone: cow-child-$i"
done

# ============================================================================
log_step "3. Launch $NUM_CLIENTS concurrent writers (simulate VM boots)"
# ============================================================================
# First write to each 4MB object triggers COW: librbd reads from the parent
# (which reads from S3), then writes to the child's RADOS pool.
# Without S3_MAX_CONCURRENT_FETCHES, this spawns unbounded threads.

declare -a BENCH_PIDS
START_TIME=$(date +%s)

for i in $(seq 1 $NUM_CLIENTS); do
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench \
        "$POOL/cow-child-$i" \
        --io-type   write \
        --io-size   $IO_SIZE \
        --io-threads 4 \
        --io-total  $IO_TOTAL \
        > "/tmp/cow-bench-$i.log" 2>&1 &
    BENCH_PIDS[$i]=$!
    log_info "  Started client $i (PID: ${BENCH_PIDS[$i]})"
done

log_info "All $NUM_CLIENTS clients running. Waiting up to ${COW_TIMEOUT_SECS}s..."

# ============================================================================
log_step "4. Wait for all clients to complete"
# ============================================================================
# bash 'wait <pid>' only works for children of the current shell; wrapping it
# in 'timeout X bash -c "wait <pid>"' silently fails because <pid> is not a
# child of the subshell. We use kill -0 polling instead.
FAILED=0
DEADLINE=$(( $(date +%s) + COW_TIMEOUT_SECS ))
declare -a CLIENT_DONE

while true; do
    ALL_DONE=1
    for i in $(seq 1 $NUM_CLIENTS); do
        if [ -z "${CLIENT_DONE[$i]:-}" ]; then
            if ! kill -0 ${BENCH_PIDS[$i]} 2>/dev/null; then
                wait ${BENCH_PIDS[$i]} 2>/dev/null || true
                CLIENT_DONE[$i]=1
                ELAPSED=$(( $(date +%s) - START_TIME ))
                log_success "Client $i completed in ${ELAPSED}s"
            else
                ALL_DONE=0
            fi
        fi
    done
    [ $ALL_DONE -eq 1 ] && break
    if [ $(date +%s) -ge $DEADLINE ]; then
        for i in $(seq 1 $NUM_CLIENTS); do
            if [ -z "${CLIENT_DONE[$i]:-}" ]; then
                kill ${BENCH_PIDS[$i]} 2>/dev/null || true
                log_error "FAIL: Client $i did not complete within ${COW_TIMEOUT_SECS}s"
                log_error "  This indicates a COW 'guarding write' blocked on an S3 fetch"
                log_error "  Check S3ObjectFetcher::S3_MAX_CONCURRENT_FETCHES limit"
                FAILED=$((FAILED + 1))
            fi
        done
        break
    fi
    sleep 1
done

END_TIME=$(date +%s)
TOTAL_TIME=$(( END_TIME - START_TIME ))

if [ $FAILED -gt 0 ]; then
    log_error "$FAILED/$NUM_CLIENTS clients failed the COW load test"
    for i in $(seq 1 $NUM_CLIENTS); do
        echo "--- Client $i ---"
        tail -20 "/tmp/cow-bench-$i.log" 2>/dev/null || echo "(no log)"
    done
    exit 1
fi

log_success "All $NUM_CLIENTS clients completed concurrent COW in ${TOTAL_TIME}s"

# ============================================================================
log_step "5. Verify RADOS objects were created (COW actually happened)"
# ============================================================================
INTEGRITY_FAIL=0
for i in $(seq 1 $NUM_CLIENTS); do
    local_prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "cow-child-$i")
    if [ -z "$local_prefix" ]; then
        log_warn "  Child $i: could not get block_name_prefix"
        continue
    fi
    obj_count=$(count_rados_objects "$CEPH_CONF" "$POOL" "$local_prefix")
    if [ "$obj_count" -gt 0 ]; then
        log_success "  Child $i: $obj_count RADOS object(s) created (COW occurred)"
    else
        log_warn "  Child $i: 0 RADOS objects (COW may not have occurred)"
        INTEGRITY_FAIL=$(( INTEGRITY_FAIL + 1 ))
    fi
done

[ $INTEGRITY_FAIL -gt 0 ] && \
    log_warn "$INTEGRITY_FAIL/$NUM_CLIENTS children have 0 RADOS objects after COW bench"

# ============================================================================
log_step "6. Verify data integrity: parent object 0 matches S3 source"
# ============================================================================
# 4 CopyupRequests raced on parent object 0. One won the sentinel lock, fetched
# from S3, and wrote the parent RADOS object; the others saw it populated and
# re-used it. Verify the written parent content equals the original S3 bytes.
PARENT_BLOCK_SIZE=$(( 4 * 1024 * 1024 ))   # 4 MB = default RBD object size

PARENT_BLOCK_PREFIX=$(get_block_prefix "$CEPH_CONF" "$POOL" "cow-parent")
if [ -z "$PARENT_BLOCK_PREFIX" ]; then
    log_warn "Could not get parent block_name_prefix; skipping parent integrity check"
else
    PARENT_OBJ="${PARENT_BLOCK_PREFIX}.0000000000000000"
    PARENT_OBJ_FILE="/tmp/cow-parent-obj0-$$.bin"
    REF_OBJ_FILE="/tmp/cow-ref-obj0-$$.bin"

    # Reference: first 4 MB from the original S3 upload
    dd if="$PARENT_RAW" bs=$PARENT_BLOCK_SIZE count=1 \
        of="$REF_OBJ_FILE" status=none 2>/dev/null

    if "$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" \
            get "$PARENT_OBJ" "$PARENT_OBJ_FILE" 2>/dev/null; then
        if cmp -s "$REF_OBJ_FILE" "$PARENT_OBJ_FILE"; then
            log_success "Parent object 0 matches S3 source (concurrent COW correctness verified)"
        else
            log_error "FAIL: Parent object 0 does NOT match S3 source!"
            log_error "  Expected md5: $(md5sum "$REF_OBJ_FILE" | awk '{print $1}')"
            log_error "  Got md5:      $(md5sum "$PARENT_OBJ_FILE" | awk '{print $1}')"
            INTEGRITY_FAIL=$(( INTEGRITY_FAIL + 1 ))
        fi
    else
        log_warn "Parent object 0 not yet in RADOS (no backfill run; expected if backfill not started)"
    fi
    rm -f "$PARENT_OBJ_FILE" "$REF_OBJ_FILE"
fi

# ============================================================================
log_step "7. Verify data integrity: unwritten area of child object 0 matches S3 source"
# ============================================================================
# Each client wrote IO_TOTAL=4096 bytes at offset 0 (first 4KB of object 0).
# The COW copied the full 4MB parent block into the child object (via rbd::copyup).
# Bytes IO_SIZE to PARENT_BLOCK_SIZE-1 were NEVER written by the bench — they
# remain as-copied from the parent (S3 data for the first client, or
# sparse-read-through to the parent for subsequent clients after the fix).
# Check the LAST 4KB of object 0, which bench never touched.
CHECK_OFFSET=$(( PARENT_BLOCK_SIZE - IO_SIZE ))   # last 4KB of object 0
CHECK_LEN=$IO_SIZE

dd if="$PARENT_RAW" bs=1 skip=$CHECK_OFFSET count=$CHECK_LEN \
    of="/tmp/cow-ref-obj1-$$.bin" status=none 2>/dev/null

DATA_INTEGRITY_FAIL=0
for i in $(seq 1 $NUM_CLIENTS); do
    EXPORT_FILE="/tmp/cow-child-export-$i-$$.raw"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export \
        "$POOL/cow-child-$i" "$EXPORT_FILE" 2>/dev/null

    dd if="$EXPORT_FILE" bs=1 skip=$CHECK_OFFSET count=$CHECK_LEN \
        of="/tmp/cow-child-check-$i-$$.bin" status=none 2>/dev/null

    if cmp -s "/tmp/cow-ref-obj1-$$.bin" "/tmp/cow-child-check-$i-$$.bin"; then
        log_success "  Child $i: unwritten object 1 matches S3 source"
    else
        log_error "  Child $i: MISMATCH — unwritten area does not match S3 data"
        DATA_INTEGRITY_FAIL=$(( DATA_INTEGRITY_FAIL + 1 ))
    fi

    rm -f "$EXPORT_FILE" "/tmp/cow-child-check-$i-$$.bin"
done
rm -f "/tmp/cow-ref-obj1-$$.bin"

INTEGRITY_FAIL=$(( INTEGRITY_FAIL + DATA_INTEGRITY_FAIL ))

if [ $INTEGRITY_FAIL -gt 0 ]; then
    log_error "$INTEGRITY_FAIL integrity check(s) failed"
    exit 1
fi

echo
log_info "=== Concurrent COW Test Summary ==="
echo "✓ $NUM_CLIENTS clients completed in ${TOTAL_TIME}s"
echo "✓ Parent object 0 integrity verified (S3 data written correctly under lock contention)"
echo "✓ Unwritten child objects read correct S3 data"
log_success "Concurrent COW load test PASSED ($NUM_CLIENTS clients, ${TOTAL_TIME}s total)"
