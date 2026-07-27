#ifndef _FBSD_COMPAT_DEV_OFW_OPENFIRM_H_
#define _FBSD_COMPAT_DEV_OFW_OPENFIRM_H_

#include <sys/types.h>

typedef intptr_t phandle_t;
typedef intptr_t ihandle_t;

phandle_t OF_finddevice(const char* path);
phandle_t OF_child(phandle_t node);
phandle_t OF_peer(phandle_t node);
phandle_t OF_parent(phandle_t node);
ssize_t   OF_getprop(phandle_t node, const char* prop, void* buf, size_t len);
ssize_t   OF_getproplen(phandle_t node, const char* prop);

#endif
