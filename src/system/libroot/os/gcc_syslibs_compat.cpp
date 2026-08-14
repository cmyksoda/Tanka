/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Compatibility shims for C++ runtime entry points that the current ppc
	cross-compiler (GCC 13) emits calls to, but which the frozen prebuilt
	gcc_syslibs package (libstdc++/libsupc++ 8.3.0, 2019) does not provide.
	Without them no GCC-13-compiled C++ shared library (libbe.so and every
	server/app that uses it) can be relocated - runtime_loader fails with
	"Could not resolve symbol".

	This is a stopgap: the real fix is to rebuild gcc_syslibs for ppc from the
	current compiler so the shipped libstdc++/libsupc++ match it. Defined only
	for ppc, so it never collides with the up-to-date libstdc++ on other
	architectures.
*/

#ifdef __powerpc__

#include <stdlib.h>


namespace std {

// GCC 11+ emits a call to this when a new[] expression's computed size would
// overflow. libsupc++ throws std::bad_array_new_length here; the 8.3.0 package
// predates the entry point. Exceptions may be disabled for this translation
// unit, and this is a never-hit-in-normal-operation error path, so abort.
extern "C++" __attribute__((noreturn)) void
__throw_bad_array_new_length()
{
	abort();
}


// GCC 13's eh_alloc.o (GLIBCXX_TUNABLES parsing) needs this libstdc++-only
// thrower; hidden, so the real libstdc++.so export is never shadowed.
extern "C++" __attribute__((noreturn, visibility("hidden"))) void
__throw_out_of_range_fmt(const char*, ...)
{
	abort();
}

}	// namespace std

#endif	// __powerpc__
