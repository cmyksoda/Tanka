#ifndef _FBSD_COMPAT_MACHINE_OFW_MACHDEP_H_
#define _FBSD_COMPAT_MACHINE_OFW_MACHDEP_H_

#include <sys/bus.h>
#include <dev/ofw/openfirm.h>

void OF_getetheraddr(device_t dev, unsigned char* addr);

#endif
