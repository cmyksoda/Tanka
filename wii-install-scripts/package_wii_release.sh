#!/bin/bash
# Copyright 2026, Haiku, Inc.
# Distributed under the terms of the MIT License.
#
# package_wii_release.sh - Creates the deployable SD card folder structure
# for the Nintendo Wii Haiku port.

set -e

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <path_to_boot_dol> <path_to_haiku_img>"
    echo "Example: $0 generated.ppc/objects/haiku/powerpc/release/system/boot/boot.dol generated.ppc/haiku.img"
    exit 1
fi

BOOT_DOL="$1"
HAIKU_IMG="$2"
OUTPUT_DIR="wii_sd_card_release"

if [ ! -f "$BOOT_DOL" ]; then
    echo "Error: Cannot find boot.dol at $BOOT_DOL"
    exit 1
fi

if [ ! -f "$HAIKU_IMG" ]; then
    echo "Error: Cannot find haiku.img at $HAIKU_IMG"
    exit 1
fi

echo "Creating Wii SD Card structure in $OUTPUT_DIR..."

# 1. Create Homebrew Channel App Directory
mkdir -p "$OUTPUT_DIR/apps/HaikuPowerPCii"

# Copy bootloader
cp "$BOOT_DOL" "$OUTPUT_DIR/apps/HaikuPowerPCii/boot.dol"

# Create HBC meta.xml
cat << 'EOF' > "$OUTPUT_DIR/apps/HaikuPowerPCii/meta.xml"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<app version="1">
    <name>Haiku OS</name>
    <coder>Haiku Project</coder>
    <version>R1/beta</version>
    <release_date></release_date>
    <short_description>Haiku Operating System</short_description>
    <long_description>Bootloader for the Haiku Operating System on Nintendo Wii. Haiku requires a valid haiku.img located at sd:/haiku/haiku.img to boot.</long_description>
    <ahb_access/>
</app>
EOF

# 2. Create Haiku System Directory
mkdir -p "$OUTPUT_DIR/haiku"

# Copy main BFS image
cp "$HAIKU_IMG" "$OUTPUT_DIR/haiku/haiku.img"

# Pre-allocate a 256MB swap file (Wii only has 88MB RAM, swap is highly recommended)
echo "Pre-allocating 256MB swap file (swap.img)..."
dd if=/dev/zero of="$OUTPUT_DIR/haiku/swap.img" bs=1M count=256 status=none

echo ""
echo "Done! Copy the contents of the '$OUTPUT_DIR' folder to the root of your FAT32 SD card."
echo ""
echo "Structure created:"
echo "  sd:/apps/HaikuPowerPCii/boot.dol"
echo "  sd:/apps/HaikuPowerPCii/meta.xml"
echo "  sd:/haiku/haiku.img"
echo "  sd:/haiku/swap.img"
