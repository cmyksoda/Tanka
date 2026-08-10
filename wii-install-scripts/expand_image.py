#!/usr/bin/env python3
"""
Haiku on Wii - SD Card Image Expander
This script takes the shipped minimal compressed image and expands it 
into a full size image suitable for the Wii's FAT32 SD Card.
"""

import sys
import os
import gzip
import shutil

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <source_compressed_image.gz> <target_sd_mount>/haiku/")
        print(f"Example: {sys.argv[0]} haiku-wii-base.img.gz E:/haiku/")
        sys.exit(1)

    source_img = sys.argv[1]
    target_dir = sys.argv[2]
    img_size_mb = 2048
    swap_size_mb = 256

    if not os.path.isfile(source_img):
        print(f"Error: Source image {source_img} not found.")
        sys.exit(1)

    os.makedirs(target_dir, exist_ok=True)
    target_img = os.path.join(target_dir, "haiku.img")
    target_swap = os.path.join(target_dir, "swap.img")

    print(f"Expanding Haiku base image to {target_img} (Target size: {img_size_mb}MB)...")
    
    # Pre-allocate and extract
    with open(target_img, 'wb') as f_out:
        print("Extracting base image...")
        with gzip.open(source_img, 'rb') as f_in:
            shutil.copyfileobj(f_in, f_out)
        
        current_size = f_out.tell()
        target_size_bytes = img_size_mb * 1024 * 1024
        
        if current_size < target_size_bytes:
            print(f"Padding file to {img_size_mb}MB (this may take a minute)...")
            f_out.seek(target_size_bytes - 1)
            f_out.write(b'\0')
    
    print(f"Creating {swap_size_mb}MB swap file at {target_swap}...")
    with open(target_swap, 'wb') as f_out:
        f_out.seek((swap_size_mb * 1024 * 1024) - 1)
        f_out.write(b'\0')

    print("Expansion complete! You can now launch boot.dol from the Homebrew Channel.")

if __name__ == "__main__":
    main()
