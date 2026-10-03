/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <platform/wii/wii_sd_image.h>

#include <stdlib.h>
#include <string.h>


static const uint32 kSectorSize = 512;
static const uint32 kSectorsPerRead = 64;
static const uint32 kFATEntriesPerSector = kSectorSize / 4;

static const uint8 kAttributeVolumeLabel = 0x08;
static const uint8 kAttributeDirectory = 0x10;
static const uint8 kEntryDeleted = 0xe5;

// 8.3 names as stored, which is how any OS writes these two lowercase names.
static const char kDirectoryName[] = "TANKA      ";
static const char kImageName[] = "TANKA   IMG";


struct fat32_volume {
	wii_sd_read_sectors	read;
	void*				cookie;
	uint8*				buffer;
	uint32				window_start;
	uint32				window_count;
	uint32				fat_start;
	uint32				data_start;
	uint32				sectors_per_cluster;
	uint32				cluster_count;
	uint32				root_cluster;
};


static inline uint16
read16(const uint8* data)
{
	return data[0] | (data[1] << 8);
}


static inline uint32
read32(const uint8* data)
{
	return data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32)data[3] << 24);
}


static status_t
read_sector(fat32_volume* volume, uint32 sector)
{
	volume->window_count = 0;
	return volume->read(volume->cookie, sector, 1, volume->buffer);
}


static bool
parse_boot_sector(fat32_volume* volume, uint32 start)
{
	const uint8* sector = volume->buffer;
	if (sector[510] != 0x55 || sector[511] != 0xaa)
		return false;

	uint32 sectorsPerCluster = sector[13];
	uint32 reserved = read16(sector + 14);
	uint32 fatCount = sector[16];
	uint32 totalSectors = read16(sector + 19);
	if (totalSectors == 0)
		totalSectors = read32(sector + 32);
	uint32 fatSize = read32(sector + 36);

	if (read16(sector + 11) != kSectorSize || sectorsPerCluster == 0
		|| (sectorsPerCluster & (sectorsPerCluster - 1)) != 0
		|| reserved == 0 || fatCount == 0 || read16(sector + 17) != 0
		|| read16(sector + 22) != 0 || fatSize == 0)
		return false;

	uint32 metadata = reserved + fatCount * fatSize;
	if (totalSectors <= metadata)
		return false;

	volume->fat_start = start + reserved;
	volume->data_start = start + metadata;
	volume->sectors_per_cluster = sectorsPerCluster;
	volume->cluster_count = (totalSectors - metadata) / sectorsPerCluster;
	volume->root_cluster = read32(sector + 44);

	return volume->cluster_count > 0;
}


static status_t
find_volume(fat32_volume* volume)
{
	status_t status = read_sector(volume, 0);
	if (status != B_OK)
		return status;

	// A card formatted without a partition table starts with the volume.
	if (parse_boot_sector(volume, 0))
		return B_OK;

	const uint8* mbr = volume->buffer;
	if (mbr[510] != 0x55 || mbr[511] != 0xaa)
		return B_ENTRY_NOT_FOUND;

	uint32 starts[4];
	for (int32 i = 0; i < 4; i++) {
		const uint8* entry = mbr + 446 + i * 16;
		starts[i] = entry[4] != 0 ? read32(entry + 8) : 0;
	}

	// The type byte is often wrong on cards, so trust the boot sector instead.
	for (int32 i = 0; i < 4; i++) {
		if (starts[i] == 0)
			continue;

		status = read_sector(volume, starts[i]);
		if (status != B_OK)
			return status;
		if (parse_boot_sector(volume, starts[i]))
			return B_OK;
	}

	return B_ENTRY_NOT_FOUND;
}


static inline bool
is_data_cluster(const fat32_volume* volume, uint32 cluster)
{
	return cluster >= 2 && cluster < volume->cluster_count + 2;
}


static status_t
next_cluster(fat32_volume* volume, uint32 cluster, uint32* _next)
{
	uint32 sector = volume->fat_start + cluster / kFATEntriesPerSector;
	if (volume->window_count == 0 || sector < volume->window_start
		|| sector >= volume->window_start + volume->window_count) {
		status_t status = volume->read(volume->cookie, sector, kSectorsPerRead,
			volume->buffer);
		if (status != B_OK) {
			volume->window_count = 0;
			return status;
		}

		volume->window_start = sector;
		volume->window_count = kSectorsPerRead;
	}

	const uint8* entry = volume->buffer
		+ (sector - volume->window_start) * kSectorSize
		+ (cluster % kFATEntriesPerSector) * 4;
	*_next = read32(entry) & 0x0fffffff;
	return B_OK;
}


static status_t
find_entry(fat32_volume* volume, uint32 directory, const char* name,
	bool isDirectory, uint8* _entry)
{
	uint32 cluster = directory;
	for (uint32 visited = 0; is_data_cluster(volume, cluster)
			&& visited < volume->cluster_count; visited++) {
		uint32 first = volume->data_start
			+ (cluster - 2) * volume->sectors_per_cluster;

		for (uint32 i = 0; i < volume->sectors_per_cluster; i++) {
			status_t status = read_sector(volume, first + i);
			if (status != B_OK)
				return status;

			for (uint32 offset = 0; offset < kSectorSize; offset += 32) {
				const uint8* entry = volume->buffer + offset;
				if (entry[0] == 0)
					return B_ENTRY_NOT_FOUND;

				// Long-name entries carry the volume label bit too.
				uint8 attributes = entry[11];
				if (entry[0] == kEntryDeleted
					|| (attributes & kAttributeVolumeLabel) != 0)
					continue;

				if (memcmp(entry, name, 11) == 0
					&& ((attributes & kAttributeDirectory) != 0)
						== isDirectory) {
					memcpy(_entry, entry, 32);
					return B_OK;
				}
			}
		}

		status_t status = next_cluster(volume, cluster, &cluster);
		if (status != B_OK)
			return status;
	}

	return B_ENTRY_NOT_FOUND;
}


static status_t
map_file(fat32_volume* volume, uint32 cluster, uint32 size,
	wii_sd_image* image)
{
	uint32 sectors = size / kSectorSize;
	if (sectors == 0)
		return B_BAD_DATA;

	uint32 clusterSectors = volume->sectors_per_cluster;
	uint32 clusters = (sectors + clusterSectors - 1) / clusterSectors;

	image->sectors = sectors;
	image->run_count = 0;

	uint32 offset = 0;
	for (uint32 i = 0; i < clusters; i++) {
		if (!is_data_cluster(volume, cluster))
			return B_BAD_DATA;

		uint32 sector = volume->data_start + (cluster - 2) * clusterSectors;
		uint32 count = sectors - offset < clusterSectors
			? sectors - offset : clusterSectors;

		wii_sd_image_run* run = image->run_count > 0
			? &image->runs[image->run_count - 1] : NULL;
		if (run != NULL && run->sector + run->count == sector)
			run->count += count;
		else {
			if (image->run_count == WII_SD_IMAGE_MAX_RUNS)
				return B_BUFFER_OVERFLOW;

			run = &image->runs[image->run_count++];
			run->offset = offset;
			run->sector = sector;
			run->count = count;
		}
		offset += count;

		if (i + 1 < clusters) {
			status_t status = next_cluster(volume, cluster, &cluster);
			if (status != B_OK)
				return status;
		}
	}

	return B_OK;
}


status_t
wii_sd_image_find(wii_sd_read_sectors read, void* cookie, wii_sd_image* image)
{
	fat32_volume volume;
	memset(&volume, 0, sizeof(volume));
	volume.read = read;
	volume.cookie = cookie;
	volume.buffer = (uint8*)malloc(kSectorsPerRead * kSectorSize);
	if (volume.buffer == NULL)
		return B_NO_MEMORY;

	uint8 entry[32];
	status_t status = find_volume(&volume);
	if (status == B_OK) {
		status = find_entry(&volume, volume.root_cluster, kDirectoryName, true,
			entry);
	}
	if (status == B_OK) {
		uint32 directory = ((uint32)read16(entry + 20) << 16)
			| read16(entry + 26);
		status = find_entry(&volume, directory, kImageName, false, entry);
	}
	if (status == B_OK) {
		uint32 first = ((uint32)read16(entry + 20) << 16) | read16(entry + 26);
		status = map_file(&volume, first, read32(entry + 28), image);
	}

	free(volume.buffer);
	return status;
}


uint32
wii_sd_image_map(const wii_sd_image* image, uint32 sector, uint32* _count)
{
	for (uint32 i = 0; i < image->run_count; i++) {
		const wii_sd_image_run& run = image->runs[i];
		if (sector < run.offset || sector - run.offset >= run.count)
			continue;

		uint32 left = run.count - (sector - run.offset);
		if (*_count > left)
			*_count = left;
		return run.sector + sector - run.offset;
	}

	*_count = 0;
	return 0;
}
