/* Haiku-ppc resurrection shim: the 7.4.4 fork declares everything in glapi.h;
 * the newer OpenGL kit #includes glapi_priv.h and expects PUBLIC (from
 * glheader.h) + the dispatch table to be available. */
#ifndef GLAPI_PRIV_H
#define GLAPI_PRIV_H
#include "glheader.h"
#include "glapi.h"
#include "glapitable.h"
#endif
