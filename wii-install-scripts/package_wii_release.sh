#!/bin/bash
# Copyright 2026, Haiku, Inc.
# Distributed under the terms of the MIT License.
#
# package_wii_release.sh - Creates the deployable SD card folder structure
# for the Nintendo Wii Haiku port.

set -e

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <path_to_boot_dol> <path_to_tanka_img>"
    echo "Example: $0 generated.ppc/objects/haiku/powerpc/release/system/boot/boot.dol generated.ppc/tanka.img"
    exit 1
fi

BOOT_DOL="$1"
TANKA_IMG="$2"
OUTPUT_DIR="Tanka"

if [ ! -f "$BOOT_DOL" ]; then
    echo "Error: Cannot find boot.dol at $BOOT_DOL"
    exit 1
fi

if [ ! -f "$TANKA_IMG" ]; then
    echo "Error: Cannot find tanka.img at $TANKA_IMG"
    exit 1
fi

echo "Creating Wii SD Card structure in project root..."

# 1. Create Homebrew Channel App Directory
mkdir -p "$OUTPUT_DIR/apps/Tanka"

# Copy bootloader
cp "$BOOT_DOL" "$OUTPUT_DIR/apps/Tanka/boot.dol"

# Create HBC meta.xml
cat << 'EOF' > "$OUTPUT_DIR/apps/Tanka/meta.xml"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<app version="1">
    <name>Tanka</name>
    <coder>cmyksoda</coder>
    <version></version>
    <release_date></release_date>
    <short_description>Operating system based on Haiku</short_description>
    <long_description>This is an experimental operating system derived from Haiku®, the free and open-source operating system that serves as the spiritual successor to BeOS.

Haiku® and the HAIKU logo® are registered trademarks of Haiku, Inc. and are developed by the Haiku Project.</long_description>
    <ahb_access/>
</app>
EOF

if [ -f "$(dirname "$0")/icon.png" ]; then
    cp "$(dirname "$0")/icon.png" "$OUTPUT_DIR/apps/Tanka/icon.png"
fi

# 2. Create Tanka System Directory
mkdir -p "$OUTPUT_DIR/tanka"

# Copy main BFS image
cp "$TANKA_IMG" "$OUTPUT_DIR/tanka/tanka.img"

# Pre-allocate a 256MB swap file (Wii only has 88MB RAM, swap is highly recommended)
echo "Pre-allocating 256MB swap file (swap.img)..."
dd if=/dev/zero of="$OUTPUT_DIR/tanka/swap.img" bs=1M count=256 status=none

cat << 'EOF' > "$OUTPUT_DIR/README.txt"
Tanka for the Nintendo Wii
==========================

The loader opens the SD card as a raw disk, walks its MBR partition table and
boots the first Tanka (BFS) partition it finds. It does not read tanka.img as a
file, so the card needs two partitions:

  1. FAT32, holding the Homebrew Channel app:
       apps/Tanka/boot.dol
       apps/Tanka/meta.xml
  2. Anything at least as large as tanka.img, written with the image itself:
       dd if=tanka/tanka.img of=/dev/<second partition> bs=1M status=progress

Copy the apps/ directory onto partition 1, then launch "Tanka" from the
Homebrew Channel.

tanka/swap.img is a pre-allocated swap file for later use; the Wii only has
88 MB of RAM. It is not read by the loader.
EOF

echo ""
echo "Done! You can now zip the 'Tanka' folder in the project root to distribute them."
echo ""
echo "Structure created:"
echo "  Tanka/README.txt"
echo "  Tanka/apps/Tanka/boot.dol"
echo "  Tanka/apps/Tanka/meta.xml"
if [ -f "$(dirname "$0")/icon.png" ]; then
    echo "  Tanka/apps/Tanka/icon.png"
fi
echo "  Tanka/tanka/tanka.img"
echo "  Tanka/tanka/swap.img"
