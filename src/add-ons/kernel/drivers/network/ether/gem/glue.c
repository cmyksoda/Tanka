/*
 * Haiku FreeBSD driver glue for the Apple GMAC / Sun GEM Ethernet driver.
 * Distributed under the terms of the MIT License.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/socket.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>

#include <machine/bus.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>

#include <dev/gem/if_gemreg.h>
#include <dev/gem/if_gemvar.h>


HAIKU_FBSD_DRIVER_GLUE(gem, gem, pci);
HAIKU_DRIVER_REQUIREMENTS(FBSD_FAST_TASKQUEUE);


/*
 * Fast interrupt filter. gem registers a threaded handler (gem_intr), so
 * bus_setup_intr() installs the compat intr_wrapper, which calls this on every
 * interrupt to acknowledge/disable it before waking the handler thread. gem's
 * IRQ line is dedicated (its own OpenPIC input on UniNorth), so we simply mask
 * all of the chip's interrupt sources here; the handler thread reads/clears
 * GEM_STATUS and re-enables via __haiku_reenable_interrupts() below.
 *
 * (Do NOT use NO_HAIKU_CHECK_DISABLE_INTERRUPTS() here - that stubs this out
 * with panic("should never be called."), which fires on the first interrupt.)
 */
int
__haiku_disable_interrupts(device_t dev)
{
	struct gem_softc *sc = device_get_softc(dev);

	GEM_BANK1_WRITE_4(sc, GEM_INTMASK, 0xffffffff);
	return 1;
}


void
__haiku_reenable_interrupts(device_t dev)
{
	struct gem_softc *sc = device_get_softc(dev);

	/* Same set gem_init() enables (GEM_DEBUG bits excluded, as in the build). */
	GEM_BANK1_WRITE_4(sc, GEM_INTMASK,
	    ~(GEM_INTR_TX_INTME | GEM_INTR_TX_EMPTY | GEM_INTR_RX_DONE |
	    GEM_INTR_RX_NOBUF | GEM_INTR_RX_TAG_ERR | GEM_INTR_PERR |
	    GEM_INTR_BERR));
}


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
