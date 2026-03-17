#!/bin/bash
# run-s3-tests.sh — Master runner for S3-backed standalone clone test suite
#
# Usage:
#   ./run-s3-tests.sh                   # All tests except cross-cluster (requires docker)
#   ./run-s3-tests.sh --quick           # edge-cases + matrix scenario 1 only (~30s)
#   ./run-s3-tests.sh --full            # Everything including cross-cluster
#   ./run-s3-tests.sh --suite <name>    # One suite: edge|failure|matrix|backfill|cow|preemption|cross
#
# Test order (dependency-safe):
#   1. edge-cases        — metadata, s3-config roundtrip, rbd info, ranged GET
#   2. failure-scenarios — bad creds, S3 down, 404, sparse, invalid config
#   3. e2e-matrix        — 3 cache scenarios × read/write/flatten (+ cross mode)
#   4. backfill          — daemon lifecycle, data integrity, object naming, regressions
#   5. concurrent-cow    — 4 clients, COW throttle test
#   6. preemption        — daemon vs client lock contention
#   7. cross-cluster     — Docker dual-cluster (requires docker)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
log_info()    { echo -e "${GREEN}[INFO]${NC}  $1"; }
log_step()    { echo -e "${BLUE}[STEP]${NC}  $1"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC}  $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1" >&2; }
log_success() { echo -e "${GREEN}[PASS]${NC}  $1"; }

# ── Option parsing ────────────────────────────────────────────────────────────
MODE="default"        # default | quick | full
SUITE=""
CEPH_CONF_ARG=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)        MODE="quick";  shift ;;
        --full)         MODE="full";   shift ;;
        --suite)        SUITE="$2";    MODE="suite"; shift 2 ;;
        --conf)         CEPH_CONF_ARG="--conf $2"; shift 2 ;;
        -h|--help)
            sed -n '2,12p' "$0" | sed 's/^# //'
            exit 0 ;;
        *) log_warn "Unknown option: $1"; shift ;;
    esac
done

# ── Suite runner ──────────────────────────────────────────────────────────────
PASS_COUNT=0
FAIL_COUNT=0
FAILED_SUITES=()
START_WALL=$(date +%s)

run_suite() {
    local name=$1
    local script=$2
    shift 2
    local extra_args=("$@")

    log_step "Suite: $name"
    local t0=$(date +%s)
    local exit_code=0

    bash "$SCRIPT_DIR/$script" $CEPH_CONF_ARG "${extra_args[@]}" || exit_code=$?

    local elapsed=$(( $(date +%s) - t0 ))
    if [ $exit_code -eq 0 ]; then
        log_success "$name PASSED (${elapsed}s)"
        PASS_COUNT=$(( PASS_COUNT + 1 ))
    else
        log_error "$name FAILED (${elapsed}s, exit $exit_code)"
        FAIL_COUNT=$(( FAIL_COUNT + 1 ))
        FAILED_SUITES+=("$name")
    fi
    echo
}

# ── Suites ────────────────────────────────────────────────────────────────────
run_edge()       { run_suite "edge-cases"         "test-s3-edge-cases.sh"; }
run_failure()    { run_suite "failure-scenarios"  "test-s3-failure-scenarios.sh"; }
run_matrix()     { run_suite "e2e-matrix"         "test-s3-e2e-matrix.sh"; }
run_matrix_q()   { run_suite "e2e-matrix-quick"   "test-s3-e2e-matrix.sh" "--scenario" "1"; }
run_backfill()   { run_suite "backfill"           "test-s3-backfill.sh"; }
run_cow()        { run_suite "concurrent-cow"     "test-s3-concurrent-cow.sh"; }
run_preemption() { run_suite "preemption"         "test-s3-preemption.sh"; }
run_cross()      { run_suite "cross-cluster"      "test-s3-cross-cluster.sh"; }

# ── Mode dispatch ─────────────────────────────────────────────────────────────
echo
log_info "=== S3-Backed Standalone Clone Test Suite ==="
log_info "Mode: $MODE"
echo

case $MODE in
    quick)
        run_edge
        run_matrix_q
        ;;
    full)
        run_edge
        run_failure
        run_matrix
        run_backfill
        run_cow
        run_preemption
        run_cross
        ;;
    suite)
        case $SUITE in
            edge)        run_edge ;;
            failure)     run_failure ;;
            matrix)      run_matrix ;;
            backfill)    run_backfill ;;
            cow)         run_cow ;;
            preemption)  run_preemption ;;
            cross)       run_cross ;;
            *)
                log_error "Unknown suite: '$SUITE'"
                log_error "Valid suites: edge | failure | matrix | backfill | cow | preemption | cross"
                exit 1
                ;;
        esac
        ;;
    default)
        run_edge
        run_failure
        run_matrix
        run_backfill
        run_cow
        run_preemption
        ;;
esac

# ── Summary ───────────────────────────────────────────────────────────────────
TOTAL_WALL=$(( $(date +%s) - START_WALL ))
echo "────────────────────────────────────────────"
log_info "Suite summary  (${TOTAL_WALL}s wall time)"
echo "  Passed: $PASS_COUNT"
echo "  Failed: $FAIL_COUNT"
if [ ${#FAILED_SUITES[@]} -gt 0 ]; then
    echo "  Failed suites:"
    for s in "${FAILED_SUITES[@]}"; do
        echo "    ✗ $s"
    done
fi
echo "────────────────────────────────────────────"

[ $FAIL_COUNT -eq 0 ] && log_success "All suites PASSED" || { log_error "Some suites FAILED"; exit 1; }
