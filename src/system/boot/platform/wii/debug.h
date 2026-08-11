/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef WII_DEBUG_H
#define WII_DEBUG_H


#include <SupportDefs.h>


void debug_init(void);
void debug_write(const char* buffer, size_t length);


#endif	// WII_DEBUG_H
