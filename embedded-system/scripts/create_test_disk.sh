#!/bin/bash
# ============================================================================
# create_test_disk.sh
# Create a test disk image using a loop device for integration testing.
#
# Usage:
#   ./create_test_disk.sh [--size SIZE_MB] [--output PATH]
#
# Default: 1024 MiB disk at /tmp/test_disk.img
# ============================================================================

set -euo pipefail

SIZE_MB=1024
OUTPUT="/tmp/test_disk.img"
PARTITION_LAYOUT="gpt"

print_usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Create a test disk image for installer integration testing.

Options:
  --size SIZE_MB      Disk size in MiB (default: 1024)
  --output PATH       Output image path (default: /tmp/test_disk.img)
  --layout gpt|mbr    Partition table type (default: gpt)
  --force             Overwrite existing image without prompting
  --help              Show this help message

Examples:
  $0 --size 2048 --output /tmp/big_disk.img
  $0 --layout mbr --force
EOF
}

FORCE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --size)
            SIZE_MB="$2"
            shift 2
            ;;
        --output)
            OUTPUT="$2"
            shift 2
            ;;
        --layout)
            PARTITION_LAYOUT="$2"
            shift 2
            ;;
        --force)
            FORCE=true
            shift
            ;;
        --help|-h)
            print_usage
            exit 0
            ;;
        *)
            echo "Error: Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

# Check for required tools
for tool in dd losetup sgdisk mkfs.vfat mkfs.ext4; do
    if ! command -v "$tool" &>/dev/null; then
        echo "Error: Required tool '$tool' not found. Please install it."
        exit 1
    fi
done

# Check if output already exists
if [[ -f "$OUTPUT" ]]; then
    if $FORCE; then
        echo "Removing existing image: $OUTPUT"
        rm -f "$OUTPUT"
    else
        echo "Error: File '$OUTPUT' already exists. Use --force to overwrite."
        exit 1
    fi
fi

echo "========================================"
echo "  Creating Test Disk Image"
echo "========================================"
echo "  Size:        ${SIZE_MB} MiB"
echo "  Output:      ${OUTPUT}"
echo "  Layout:      ${PARTITION_LAYOUT}"
echo "========================================"

# Create the empty image file
echo "[1/5] Creating empty image file..."
dd if=/dev/zero of="$OUTPUT" bs=1M count="$SIZE_MB" status=progress

# Set up loop device
echo "[2/5] Setting up loop device..."
LOOP_DEV=$(sudo losetup --find --show "$OUTPUT")
echo "  Loop device: $LOOP_DEV"

# Cleanup function
cleanup() {
    echo ""
    echo "Cleaning up loop device..."
    sudo losetup -d "$LOOP_DEV" 2>/dev/null || true
}

trap cleanup EXIT

# Create partition table
echo "[3/5] Creating partition table ($PARTITION_LAYOUT)..."
if [[ "$PARTITION_LAYOUT" == "gpt" ]]; then
    sudo sgdisk --zap-all "$LOOP_DEV"
    sudo sgdisk --new=1:0:+128M --typecode=1:EF00 --change-name=1:BOOT_A "$LOOP_DEV"
    sudo sgdisk --new=2:0:+128M --typecode=2:EF00 --change-name=2:BOOT_B "$LOOP_DEV"
    sudo sgdisk --new=3:0:+384M --typecode=3:8300 --change-name=3:ROOTFS_A "$LOOP_DEV"
    sudo sgdisk --new=4:0:+384M --typecode=4:8300 --change-name=4:ROOTFS_B "$LOOP_DEV"
    sudo sgdisk --new=5:0:0    --typecode=5:8300 --change-name=5:DATA "$LOOP_DEV"
else
    sudo parted -s "$LOOP_DEV" mklabel msdos
    sudo parted -s "$LOOP_DEV" mkpart primary fat32 1MiB 129MiB
    sudo parted -s "$LOOP_DEV" mkpart primary fat32 129MiB 257MiB
    sudo parted -s "$LOOP_DEV" mkpart primary ext4 257MiB 641MiB
    sudo parted -s "$LOOP_DEV" mkpart primary ext4 641MiB 1025MiB
    sudo parted -s "$LOOP_DEV" mkpart primary ext4 1025MiB 100%
    sudo parted -s "$LOOP_DEV" set 1 boot on
fi

# Wait for kernel to detect partitions
echo "[4/5] Waiting for partition detection..."
sudo partprobe "$LOOP_DEV"
sleep 2

# Determine partition suffix (p for mmcblk/nvme, empty for sd/vd)
PART_SUFFIX=""
if [[ "$LOOP_DEV" =~ (mmcblk|nvme|loop) ]]; then
    PART_SUFFIX="p"
fi

# Format partitions
echo "[5/5] Formatting partitions..."
sudo mkfs.vfat -F 32 -n BOOT_A "${LOOP_DEV}${PART_SUFFIX}1" 2>/dev/null || echo "  Warning: BOOT_A format may have failed"
sudo mkfs.vfat -F 32 -n BOOT_B "${LOOP_DEV}${PART_SUFFIX}2" 2>/dev/null || echo "  Warning: BOOT_B format may have failed"
sudo mkfs.ext4 -F -L ROOTFS_A "${LOOP_DEV}${PART_SUFFIX}3" 2>/dev/null || echo "  Warning: ROOTFS_A format may have failed"
sudo mkfs.ext4 -F -L ROOTFS_B "${LOOP_DEV}${PART_SUFFIX}4" 2>/dev/null || echo "  Warning: ROOTFS_B format may have failed"
sudo mkfs.ext4 -F -L DATA "${LOOP_DEV}${PART_SUFFIX}5" 2>/dev/null || echo "  Warning: DATA format may have failed"

echo ""
echo "========================================"
echo "  Test disk image created successfully!"
echo "========================================"
echo "  Image:    $OUTPUT"
echo "  Size:     ${SIZE_MB} MiB"
echo ""
echo "To attach manually:"
echo "  sudo losetup --find --show $OUTPUT"
echo "To detach:"
echo "  sudo losetup -d <loop_device>"
echo "To remove:"
echo "  rm -f $OUTPUT"
echo "========================================"

# Don't let cleanup detach — keep loop for tests
trap - EXIT
echo ""
echo "Loop device $LOOP_DEV is still attached for testing."
echo "Run 'sudo losetup -d $LOOP_DEV' when done."
