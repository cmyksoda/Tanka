/*
 * Copyright 2003-2008, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef BFS_ENDIAN_H
#define BFS_ENDIAN_H


#include "system_dependencies.h"


#if !defined(BFS_LITTLE_ENDIAN_ONLY) && !defined(BFS_BIG_ENDIAN_ONLY)
//	default setting; BFS is now primarily a little endian file system
#	define BFS_LITTLE_ENDIAN_ONLY
#endif
#if defined(BFS_LITTLE_ENDIAN_ONLY) && defined(BFS_BIG_ENDIAN_ONLY)
#	error Building BFS with both big and little endian is not supported.
#endif


#if defined(BFS_LITTLE_ENDIAN_ONLY) && B_HOST_IS_LENDIAN \
	|| defined(BFS_BIG_ENDIAN_ONLY) && B_HOST_IS_BENDIAN
		/* host is BFS endian */
#	define BFS_NATIVE_ENDIAN
#	define BFS_ENDIAN_TO_HOST_INT16(value) (value)
#	define BFS_ENDIAN_TO_HOST_INT32(value) (value)
#	define BFS_ENDIAN_TO_HOST_INT64(value) (value)
#	define HOST_ENDIAN_TO_BFS_INT16(value) (value)
#	define HOST_ENDIAN_TO_BFS_INT32(value) (value)
#	define HOST_ENDIAN_TO_BFS_INT64(value) (value)
#elif defined(BFS_LITTLE_ENDIAN_ONLY) && B_HOST_IS_BENDIAN \
	|| defined(BFS_BIG_ENDIAN_ONLY) && B_HOST_IS_LENDIAN
		/* host is big endian, BFS is little endian or vice versa */
#	define BFS_ENDIAN_TO_HOST_INT16(value) __swap_int16(value)
#	define BFS_ENDIAN_TO_HOST_INT32(value) __swap_int32(value)
#	define BFS_ENDIAN_TO_HOST_INT64(value) __swap_int64(value)
#	define HOST_ENDIAN_TO_BFS_INT16(value) __swap_int16(value)
#	define HOST_ENDIAN_TO_BFS_INT32(value) __swap_int32(value)
#	define HOST_ENDIAN_TO_BFS_INT64(value) __swap_int64(value)
#else
	// TODO: maybe build a version that supports both, big & little endian?
	//		But since that will need some kind of global data (to
	//		know of what type this file system is), it's probably 
	//		something for the boot loader; anything else would be
	//		a major pain.
#endif


#ifdef __cplusplus

// Numeric index keys of the expected size are stored in file system byte order.
inline bool
bfs_is_numeric_index_key(uint32 type, size_t length)
{
	switch (type) {
		case B_INT32_TYPE:
		case B_UINT32_TYPE:
		case B_FLOAT_TYPE:
			return length == sizeof(int32);
		case B_INT64_TYPE:
		case B_UINT64_TYPE:
		case B_DOUBLE_TYPE:
			return length == sizeof(int64);
	}
	return false;
}


// In-place host<->file system conversion (its own inverse); strings untouched.
inline void
bfs_convert_index_key(uint32 type, void* key, size_t length)
{
#ifndef BFS_NATIVE_ENDIAN
	if (!bfs_is_numeric_index_key(type, length))
		return;

	uint8* bytes = (uint8*)key;
	for (size_t i = 0, j = length - 1; i < j; i++, j--) {
		uint8 temp = bytes[i];
		bytes[i] = bytes[j];
		bytes[j] = temp;
	}
#endif
}


// Same, but converts into buffer (sized for the largest key); returns the key.
inline const uint8*
bfs_convert_index_key(uint32 type, const uint8* key, size_t length,
	void* buffer)
{
#ifndef BFS_NATIVE_ENDIAN
	if (key != NULL && bfs_is_numeric_index_key(type, length)) {
		uint8* bytes = (uint8*)buffer;
		for (size_t i = 0; i < length; i++)
			bytes[i] = key[length - 1 - i];
		return bytes;
	}
#endif
	return key;
}

#endif	// __cplusplus

#endif	/* BFS_ENDIAN_H */
