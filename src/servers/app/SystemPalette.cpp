/*
 * Copyright 2001-2006, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		DarkWyrm <bpmagic@columbus.rr.com>
 *		Stefano Ceccherini (burton666@libero.it)
 */

//! Methods to initialize and get the system color_map.


#include "SystemPalette.h"

#include <stdio.h>
#include <string.h>

#include <Palette.h>

#include "SystemPaletteMap.h"

// TODO: BWindowScreen has a method to set the palette.
// maybe we should have a lock to protect this variable.
static color_map sColorMap;


static void
FillColorMap(const rgb_color *palette, color_map *map)
{
	memcpy((void*)map->color_list, palette, sizeof(map->color_list));

	// The index_map (nearest palette entry for every RGB15 color) and the
	// inversion_map depend ONLY on the compile-time-constant system palette,
	// so they are precomputed at build time (see generate_palette_map.cpp ->
	// SystemPaletteMap.h). Computing them here was an 8.4M-iteration
	// nearest-color search (32768 * 256) that cost ~22s at every app_server
	// startup on slow/emulated PowerPC. The tables are byte-for-byte identical
	// to the former runtime result.
	memcpy(map->index_map, kSystemColorMapIndex, sizeof(map->index_map));
	memcpy(map->inversion_map, kSystemColorMapInversion,
		sizeof(map->inversion_map));
}


/*!	\brief Initializes the system color_map.
*/
void
InitializeColorMap()
{
	FillColorMap(kSystemPalette, &sColorMap);
}


/*!	\brief Returns a pointer to the system palette.
	\return A pointer to the system palette.
*/
const rgb_color *
SystemPalette()
{
	return sColorMap.color_list;
}


/*!	\brief Returns a pointer to the system color_map structure.
	\return A pointer to the system color_map.
*/
const color_map *
SystemColorMap()
{
	return &sColorMap;
}
