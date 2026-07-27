/*
 * Open Firmware glue for ppc network drivers (Apple GMAC MAC address, etc.).
 * Bridges the FreeBSD dev/ofw OF_* API onto Haiku's kernel of_* interface.
 * Distributed under the terms of the MIT License.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <string.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <machine/ofw_machdep.h>

/* Haiku kernel Open Firmware interface (extern "C" in the kernel). */
extern intptr_t of_finddevice(const char* device);
extern intptr_t of_child(intptr_t node);
extern intptr_t of_peer(intptr_t node);
extern intptr_t of_parent(intptr_t node);
extern bool ppc_get_gmac_mac(unsigned char* address);
extern intptr_t of_getprop(intptr_t package, const char* property, void* buffer,
	intptr_t bufferSize);
extern intptr_t of_getproplen(intptr_t package, const char* property);


phandle_t
OF_finddevice(const char* path)
{
	return of_finddevice(path);
}


phandle_t
OF_child(phandle_t node)
{
	return of_child(node);
}


phandle_t
OF_peer(phandle_t node)
{
	return of_peer(node);
}


phandle_t
OF_parent(phandle_t node)
{
	return of_parent(node);
}


ssize_t
OF_getprop(phandle_t node, const char* prop, void* buf, size_t len)
{
	return of_getprop(node, prop, buf, len);
}


ssize_t
OF_getproplen(phandle_t node, const char* prop)
{
	return of_getproplen(node, prop);
}


static phandle_t
find_network_node(phandle_t node)
{
	char type[32];
	phandle_t child;

	for (child = of_child(node); child > 0; child = of_peer(child)) {
		phandle_t found;
		if (of_getprop(child, "device_type", type, sizeof(type)) > 0) {
			type[sizeof(type) - 1] = '\0';
			if (strcmp(type, "network") == 0)
				return child;
		}
		found = find_network_node(child);
		if (found > 0)
			return found;
	}
	return 0;
}


phandle_t
ofw_bus_get_node(device_t dev)
{
	static phandle_t sEnet = -1;

	(void)dev;
	if (sEnet == (phandle_t)-1) {
		sEnet = of_finddevice("enet");
		if (sEnet <= 0)
			sEnet = find_network_node(of_finddevice("/"));
		if (sEnet <= 0)
			sEnet = 0;
	}
	return sEnet;
}


void
OF_getetheraddr(device_t dev, unsigned char* addr)
{
	// Open Firmware is not reliably callable from the kernel on this port,
	// so the MAC address is captured by the boot loader and handed over
	// through kernel_args. Prefer that; fall back to a live OF read.
	if (ppc_get_gmac_mac(addr))
		return;
	phandle_t node = ofw_bus_get_node(dev);
	if (node <= 0)
		return;
	if (of_getprop(node, "local-mac-address", addr, 6) == 6)
		return;
	of_getprop(node, "mac-address", addr, 6);
}
