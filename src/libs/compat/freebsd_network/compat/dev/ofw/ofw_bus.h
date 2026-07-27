#ifndef _FBSD_COMPAT_DEV_OFW_OFW_BUS_H_
#define _FBSD_COMPAT_DEV_OFW_OFW_BUS_H_

#include <sys/bus.h>
#include <dev/ofw/openfirm.h>

phandle_t ofw_bus_get_node(device_t dev);

#endif
