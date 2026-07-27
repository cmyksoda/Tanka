/*
 * Haiku FreeBSD driver glue for the Apple GMAC / Sun GEM Ethernet driver.
 * Distributed under the terms of the MIT License.
 */

#include <sys/bus.h>


HAIKU_FBSD_DRIVER_GLUE(gem, gem, pci);
HAIKU_DRIVER_REQUIREMENTS(FBSD_FAST_TASKQUEUE);
NO_HAIKU_CHECK_DISABLE_INTERRUPTS();
NO_HAIKU_REENABLE_INTERRUPTS();


extern driver_t *DRIVER_MODULE_NAME(ukphy, miibus);


driver_t *
__haiku_select_miibus_driver(device_t dev)
{
	driver_t *drivers[] = {
		DRIVER_MODULE_NAME(ukphy, miibus),
		NULL
	};

	return __haiku_probe_drivers(dev, drivers);
}
