#!/bin/bash
# Test S3 ranged GET implementation with MinIO

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MINIO_BIN="${HOME}/dev/minio/bin"
MINIO_DATA_DIR="/tmp/minio-test-data"
MINIO_PORT=9000
MINIO_CONSOLE_PORT=9001

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

cleanup() {
    log_info "Cleaning up..."

    # Stop MinIO if running
    if [ -f "/tmp/minio.pid" ]; then
        MINIO_PID=$(cat /tmp/minio.pid)
        if kill -0 "$MINIO_PID" 2>/dev/null; then
            log_info "Stopping MinIO (PID: $MINIO_PID)"
            kill "$MINIO_PID" || true
            sleep 2
        fi
        rm -f /tmp/minio.pid
    fi

    # Clean up data directory
    if [ -d "$MINIO_DATA_DIR" ]; then
        log_info "Removing MinIO data directory"
        rm -rf "$MINIO_DATA_DIR"
    fi
}

# Trap cleanup on exit
trap cleanup EXIT

# Check if MinIO binaries exist
if [ ! -f "$MINIO_BIN/minio" ]; then
    log_error "MinIO binary not found at $MINIO_BIN/minio"
    exit 1
fi

if [ ! -f "$MINIO_BIN/mc" ]; then
    log_error "MinIO client (mc) not found at $MINIO_BIN/mc"
    exit 1
fi

log_info "=== Testing S3 Ranged GET Implementation with MinIO ==="
echo

# Create data directory
log_info "Creating MinIO data directory: $MINIO_DATA_DIR"
mkdir -p "$MINIO_DATA_DIR"

# Start MinIO server
log_info "Starting MinIO server on port $MINIO_PORT..."
export MINIO_ROOT_USER="minioadmin"
export MINIO_ROOT_PASSWORD="minioadmin"

"$MINIO_BIN/minio" server "$MINIO_DATA_DIR" \
    --address ":$MINIO_PORT" \
    --console-address ":$MINIO_CONSOLE_PORT" \
    > /tmp/minio.log 2>&1 &

MINIO_PID=$!
echo $MINIO_PID > /tmp/minio.pid
log_info "MinIO started with PID: $MINIO_PID"

# Wait for MinIO to be ready
log_info "Waiting for MinIO to be ready..."
for i in {1..30}; do
    if curl -s http://localhost:$MINIO_PORT/minio/health/live > /dev/null 2>&1; then
        log_info "MinIO is ready!"
        break
    fi
    if [ $i -eq 30 ]; then
        log_error "MinIO failed to start within 30 seconds"
        cat /tmp/minio.log
        exit 1
    fi
    sleep 1
done

# Configure MinIO client
log_info "Configuring MinIO client..."
"$MINIO_BIN/mc" alias set local http://localhost:$MINIO_PORT minioadmin minioadmin

# Create bucket
BUCKET_NAME="rbd-test-bucket"
log_info "Creating bucket: $BUCKET_NAME"
"$MINIO_BIN/mc" mb "local/$BUCKET_NAME" || true

# Set anonymous read policy on bucket
log_info "Setting anonymous read policy on bucket"
"$MINIO_BIN/mc" anonymous set download "local/$BUCKET_NAME"

# Create a test raw image (10MB with known pattern)
IMAGE_FILE="/tmp/test-image.raw"
IMAGE_SIZE=$((10 * 1024 * 1024))  # 10MB
log_info "Creating test raw image: $IMAGE_FILE (size: $IMAGE_SIZE bytes)"

# Create image with a recognizable pattern
# Each 4KB block will have block number repeated
python3 << 'EOF'
import struct

block_size = 4096
num_blocks = (10 * 1024 * 1024) // block_size

with open('/tmp/test-image.raw', 'wb') as f:
    for block_num in range(num_blocks):
        # Fill each 4KB block with block number
        block_data = struct.pack('<Q', block_num) * (block_size // 8)
        f.write(block_data)

print(f"Created test image with {num_blocks} blocks")
EOF

# Upload image to MinIO
IMAGE_KEY="test-parent-image.raw"
log_info "Uploading image to MinIO: $IMAGE_KEY"
"$MINIO_BIN/mc" cp "$IMAGE_FILE" "local/$BUCKET_NAME/$IMAGE_KEY"

# Verify upload
log_info "Verifying uploaded image..."
"$MINIO_BIN/mc" stat "local/$BUCKET_NAME/$IMAGE_KEY"

# Test ranged GET using curl
log_info "Testing HTTP ranged GET with curl..."

# Test 1: Get first 4KB block
log_info "Test 1: Fetching first 4KB (bytes 0-4095)"
curl -s -r 0-4095 "http://localhost:$MINIO_PORT/$BUCKET_NAME/$IMAGE_KEY" -o /tmp/test-range-0.bin
FETCHED_SIZE=$(stat -c%s /tmp/test-range-0.bin)
if [ "$FETCHED_SIZE" -eq 4096 ]; then
    log_info "✓ Fetched correct size: $FETCHED_SIZE bytes"
else
    log_error "✗ Expected 4096 bytes, got $FETCHED_SIZE bytes"
    exit 1
fi

# Verify content
python3 << 'EOF'
import struct

with open('/tmp/test-range-0.bin', 'rb') as f:
    data = f.read()
    expected = struct.pack('<Q', 0) * (4096 // 8)
    if data == expected:
        print("✓ Content matches expected pattern for block 0")
    else:
        print("✗ Content does not match expected pattern")
        exit(1)
EOF

# Test 2: Get second 4KB block
log_info "Test 2: Fetching second 4KB (bytes 4096-8191)"
curl -s -r 4096-8191 "http://localhost:$MINIO_PORT/$BUCKET_NAME/$IMAGE_KEY" -o /tmp/test-range-1.bin
FETCHED_SIZE=$(stat -c%s /tmp/test-range-1.bin)
if [ "$FETCHED_SIZE" -eq 4096 ]; then
    log_info "✓ Fetched correct size: $FETCHED_SIZE bytes"
else
    log_error "✗ Expected 4096 bytes, got $FETCHED_SIZE bytes"
    exit 1
fi

# Verify content
python3 << 'EOF'
import struct

with open('/tmp/test-range-1.bin', 'rb') as f:
    data = f.read()
    expected = struct.pack('<Q', 1) * (4096 // 8)
    if data == expected:
        print("✓ Content matches expected pattern for block 1")
    else:
        print("✗ Content does not match expected pattern")
        exit(1)
EOF

# Test 3: Get middle block
log_info "Test 3: Fetching block 100 (bytes 409600-413695)"
BLOCK_100_START=$((100 * 4096))
BLOCK_100_END=$((BLOCK_100_START + 4095))
curl -s -r $BLOCK_100_START-$BLOCK_100_END "http://localhost:$MINIO_PORT/$BUCKET_NAME/$IMAGE_KEY" -o /tmp/test-range-100.bin
FETCHED_SIZE=$(stat -c%s /tmp/test-range-100.bin)
if [ "$FETCHED_SIZE" -eq 4096 ]; then
    log_info "✓ Fetched correct size: $FETCHED_SIZE bytes"
else
    log_error "✗ Expected 4096 bytes, got $FETCHED_SIZE bytes"
    exit 1
fi

# Verify content
python3 << 'EOF'
import struct

with open('/tmp/test-range-100.bin', 'rb') as f:
    data = f.read()
    expected = struct.pack('<Q', 100) * (4096 // 8)
    if data == expected:
        print("✓ Content matches expected pattern for block 100")
    else:
        print("✗ Content does not match expected pattern")
        exit(1)
EOF

log_info ""
log_info "=== All MinIO ranged GET tests passed! ==="
log_info ""
log_info "S3 Configuration for RBD:"
log_info "  Endpoint: http://localhost:$MINIO_PORT"
log_info "  Bucket: $BUCKET_NAME"
log_info "  Image Name: $IMAGE_KEY"
log_info "  Image Format: raw"
log_info "  Access Key: minioadmin"
log_info "  Secret Key: minioadmin"
log_info ""
log_info "To configure an RBD parent image with S3:"
log_info "  rbd image-meta set <parent-image> s3.enabled true"
log_info "  rbd image-meta set <parent-image> s3.endpoint http://localhost:$MINIO_PORT"
log_info "  rbd image-meta set <parent-image> s3.bucket $BUCKET_NAME"
log_info "  rbd image-meta set <parent-image> s3.image_name $IMAGE_KEY"
log_info "  rbd image-meta set <parent-image> s3.image_format raw"
log_info "  rbd image-meta set <parent-image> s3.access_key minioadmin"
log_info "  rbd image-meta set <parent-image> s3.secret_key minioadmin"
log_info ""

log_info "Test completed successfully!"
exit 0
