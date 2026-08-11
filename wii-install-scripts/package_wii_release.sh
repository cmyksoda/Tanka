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
OUTPUT_DIR="Haiku-PowerPCii"

if [ ! -f "$BOOT_DOL" ]; then
    echo "Error: Cannot find boot.dol at $BOOT_DOL"
    exit 1
fi

if [ ! -f "$HAIKU_IMG" ]; then
    echo "Error: Cannot find haiku.img at $HAIKU_IMG"
    exit 1
fi

echo "Creating Wii SD Card structure in project root..."

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
    <long_description>Bootloader for the Haiku Operating System on Nintendo Wii. The loader reads the SD card as a raw disk and boots the first Haiku (BFS) partition it finds, so haiku.img must be written to a second partition of the card - see README.txt.</long_description>
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

cat << 'EOF' > "$OUTPUT_DIR/README.txt"
Haiku for the Nintendo Wii
==========================

The loader opens the SD card as a raw disk, walks its MBR partition table and
boots the first Haiku (BFS) partition it finds. It does not read haiku.img as a
file, so the card needs two partitions:

  1. FAT32, holding the Homebrew Channel app:
       apps/HaikuPowerPCii/boot.dol
       apps/HaikuPowerPCii/meta.xml
  2. Anything at least as large as haiku.img, written with the image itself:
       dd if=haiku/haiku.img of=/dev/<second partition> bs=1M status=progress

Copy the apps/ directory onto partition 1, then launch "Haiku OS" from the
Homebrew Channel.

haiku/swap.img is a pre-allocated swap file for later use; the Wii only has
88 MB of RAM. It is not read by the loader.
EOF

echo ""
echo "Done! You can now zip the 'Haiku-PowerPCii' folder in the project root to distribute them."
echo ""
echo "Structure created:"
echo "  Haiku-PowerPCii/README.txt"
echo "  Haiku-PowerPCii/apps/HaikuPowerPCii/boot.dol"
echo "  Haiku-PowerPCii/apps/HaikuPowerPCii/meta.xml"
echo "  Haiku-PowerPCii/haiku/haiku.img"
echo "  Haiku-PowerPCii/haiku/swap.img"
