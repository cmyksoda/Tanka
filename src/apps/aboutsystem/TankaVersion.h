/*
 * Copyright 2026, The Tanka Maintainers.
 * Distributed under the terms of the MIT License.
 */
#ifndef TANKA_VERSION_H
#define TANKA_VERSION_H


// Tanka's user-facing release version. Bump this by hand for each release.
#define TANKA_VERSION_RELEASE	"0.1"

// Optional codename shown after the release version (empty string = none).
#define TANKA_VERSION_CODENAME	""


// NOTE: Tanka's internal *build* number is not defined here. It is derived
// automatically at runtime from the git-based Haiku revision that
// determine_haiku_revision bakes into libroot at build time: the commit count
// since the base Haiku hrev tag equals Tanka's own commits on top of Haiku.
// See AboutSystem.cpp:_GetOSVersion(). The build number therefore bumps on
// every commit with no manual upkeep. (Caveat: if the fork is ever rebased
// onto a newer upstream hrev tag, the count restarts from that tag.)


#endif	// TANKA_VERSION_H
