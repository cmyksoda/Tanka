#!/bin/bash
# Haiku on Wii - SD Card Image Expander
# This script takes the shipped minimal compressed image and expands it 
# into a full size image suitable for the Wii's FAT32 SD Card.

set -e

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <source_compressed_image.gz> <target_sd_mount>/haiku/"
    echo "Example: $0 haiku-wii-base.img.gz /media/user/SDCARD/haiku/"
    exit 1
fi

SOURCE_IMG="$1"
TARGET_DIR="$2"
IMG_SIZE_MB=2048
SWAP_SIZE_MB=256

if [ ! -f "$SOURCE_IMG" ]; then
    echo "Error: Source image $SOURCE_IMG not found."
    exit 1
fi

mkdir -p "$TARGET_DIR"
TARGET_IMG="$TARGET_DIR/haiku.img"
TARGET_SWAP="$TARGET_DIR/swap.img"

echo "Expanding Haiku base image to $TARGET_IMG (Target size: ${IMG_SIZE_MB}MB)..."

# Create a full size empty file on FAT32 (dd is used for compatibility across OSes)
# We fill with zeros to preallocate contiguous space on FAT32.
echo "Pre-allocating ${IMG_SIZE_MB}MB file (this may take a few minutes)..."
dd if=/dev/zero of="$TARGET_IMG" bs=1M count=$IMG_SIZE_MB status=progress

echo "Inflating base image into pre-allocated file..."
# Extract the compressed minimal BFS image over the beginning of the zeroed file
gunzip -c "$SOURCE_IMG" | dd of="$TARGET_IMG" bs=1M conv=notrunc status=progress

echo "Creating ${SWAP_SIZE_MB}MB swap file at $TARGET_SWAP..."
dd if=/dev/zero of="$TARGET_SWAP" bs=1M count=$SWAP_SIZE_MB status=progress

echo "Expansion complete! You can now launch boot.dol from the Homebrew Channel."
