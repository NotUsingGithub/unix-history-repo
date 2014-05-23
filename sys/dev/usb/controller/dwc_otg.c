/* $FreeBSD$ */
/*-
 * Copyright (c) 2012 Hans Petter Selasky. All rights reserved.
 * Copyright (c) 2010-2011 Aleksandr Rybalko. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This file contains the driver for the DesignWare series USB 2.0 OTG
 * Controller.
 */

/*
 * LIMITATION: Drivers must be bound to all OUT endpoints in the
 * active configuration for this driver to work properly. Blocking any
 * OUT endpoint will block all OUT endpoints including the control
 * endpoint. Usually this is not a problem.
 */

/*
 * NOTE: Writing to non-existing registers appears to cause an
 * internal reset.
 */

#ifdef USB_GLOBAL_INCLUDE_FILE
#include USB_GLOBAL_INCLUDE_FILE
#else
#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#define	USB_DEBUG_VAR dwc_otg_debug

#include <dev/usb/usb_core.h>
#include <dev/usb/usb_debug.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_transfer.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_hub.h>
#include <dev/usb/usb_util.h>

#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#endif			/* USB_GLOBAL_INCLUDE_FILE */

#include <dev/usb/controller/dwc_otg.h>
#include <dev/usb/controller/dwc_otgreg.h>

#define	DWC_OTG_BUS2SC(bus) \
   ((struct dwc_otg_softc *)(((uint8_t *)(bus)) - \
    ((uint8_t *)&(((struct dwc_otg_softc *)0)->sc_bus))))

#define	DWC_OTG_PC2UDEV(pc) \
   (USB_DMATAG_TO_XROOT((pc)->tag_parent)->udev)

#define	DWC_OTG_MSK_GINT_ENABLED	\
   (GINTMSK_ENUMDONEMSK |		\
   GINTMSK_USBRSTMSK |			\
   GINTMSK_USBSUSPMSK |			\
   GINTMSK_IEPINTMSK |			\
   GINTMSK_SESSREQINTMSK |		\
   GINTMSK_OTGINTMSK |			\
   GINTMSK_PRTINTMSK)

#define	DWC_OTG_MSK_GINT_THREAD_IRQ				\
   (GINTSTS_USBRST | GINTSTS_ENUMDONE | GINTSTS_PRTINT |	\
   GINTSTS_WKUPINT | GINTSTS_USBSUSP | GINTMSK_OTGINTMSK |	\
   GINTSTS_SESSREQINT)

static int dwc_otg_use_hsic;

static SYSCTL_NODE(_hw_usb, OID_AUTO, dwc_otg, CTLFLAG_RW, 0, "USB DWC OTG");

SYSCTL_INT(_hw_usb_dwc_otg, OID_AUTO, use_hsic, CTLFLAG_RD | CTLFLAG_TUN,
    &dwc_otg_use_hsic, 0, "DWC OTG uses HSIC interface");
TUNABLE_INT("hw.usb.dwc_otg.use_hsic", &dwc_otg_use_hsic);

#ifdef USB_DEBUG
static int dwc_otg_debug;

SYSCTL_INT(_hw_usb_dwc_otg, OID_AUTO, debug, CTLFLAG_RW,
    &dwc_otg_debug, 0, "DWC OTG debug level");
#endif

#define	DWC_OTG_INTR_ENDPT 1

/* prototypes */

struct usb_bus_methods dwc_otg_bus_methods;
struct usb_pipe_methods dwc_otg_device_non_isoc_methods;
struct usb_pipe_methods dwc_otg_device_isoc_methods;

static dwc_otg_cmd_t dwc_otg_setup_rx;
static dwc_otg_cmd_t dwc_otg_data_rx;
static dwc_otg_cmd_t dwc_otg_data_tx;
static dwc_otg_cmd_t dwc_otg_data_tx_sync;

static dwc_otg_cmd_t dwc_otg_host_setup_tx;
static dwc_otg_cmd_t dwc_otg_host_data_tx;
static dwc_otg_cmd_t dwc_otg_host_data_rx;

static void dwc_otg_device_done(struct usb_xfer *, usb_error_t);
static void dwc_otg_do_poll(struct usb_bus *);
static void dwc_otg_standard_done(struct usb_xfer *);
static void dwc_otg_root_intr(struct dwc_otg_softc *);
static void dwc_otg_interrupt_poll(struct dwc_otg_softc *);
static void dwc_otg_host_channel_disable(struct dwc_otg_softc *, uint8_t);

/*
 * Here is a configuration that the chip supports.
 */
static const struct usb_hw_ep_profile dwc_otg_ep_profile[1] = {

	[0] = {
		.max_in_frame_size = 64,/* fixed */
		.max_out_frame_size = 64,	/* fixed */
		.is_simplex = 1,
		.support_control = 1,
	}
};

static void
dwc_otg_get_hw_ep_profile(struct usb_device *udev,
    const struct usb_hw_ep_profile **ppf, uint8_t ep_addr)
{
	struct dwc_otg_softc *sc;

	sc = DWC_OTG_BUS2SC(udev->bus);

	if (ep_addr < sc->sc_dev_ep_max)
		*ppf = &sc->sc_hw_ep_profile[ep_addr].usb;
	else
		*ppf = NULL;
}

static int
dwc_otg_init_fifo(struct dwc_otg_softc *sc, uint8_t mode)
{
	struct dwc_otg_profile *pf;
	uint32_t fifo_size;
	uint32_t fifo_regs;
	uint32_t tx_start;
	uint8_t x;

	fifo_size = sc->sc_fifo_size;

	/*
	 * NOTE: Reserved fixed size area at end of RAM, which must
	 * not be allocated to the FIFOs:
	 */
	fifo_regs = 4 * 16;

	if (fifo_size < fifo_regs) {
		DPRINTF("Too little FIFO\n");
		return (EINVAL);
	}

	/* subtract FIFO regs from total once */
	fifo_size -= fifo_regs;

	/* split equally for IN and OUT */
	fifo_size /= 2;

	/* align to 4 bytes boundary */
	fifo_size &= ~3;

	/* set global receive FIFO size */
	DWC_OTG_WRITE_4(sc, DOTG_GRXFSIZ, fifo_size / 4);

	tx_start = fifo_size;

	if (fifo_size < 64) {
		DPRINTFN(-1, "Not enough data space for EP0 FIFO.\n");
		return (EINVAL);
	}

	/* disable any leftover host channels */
	for (x = 0; x != sc->sc_host_ch_max; x++) {
		if (sc->sc_chan_state[x].wait_sof == 0)
			continue;
		dwc_otg_host_channel_disable(sc, x);
	}

	if (mode == DWC_MODE_HOST) {

		/* reset active endpoints */
		sc->sc_active_rx_ep = 0;

		/* split equally for periodic and non-periodic */
		fifo_size /= 2;

		/* align to 4 bytes boundary */
		fifo_size &= ~3;

		DWC_OTG_WRITE_4(sc, DOTG_GNPTXFSIZ,
		    ((fifo_size / 4) << 16) |
		    (tx_start / 4));

		tx_start += fifo_size;

		for (x = 0; x != sc->sc_host_ch_max; x++) {
			/* disable all host interrupts */
			DWC_OTG_WRITE_4(sc, DOTG_HCINTMSK(x), 0);
		}

		DWC_OTG_WRITE_4(sc, DOTG_HPTXFSIZ,
		    ((fifo_size / 4) << 16) |
		    (tx_start / 4));

		/* reset host channel state */
		memset(sc->sc_chan_state, 0, sizeof(sc->sc_chan_state));

		/* reset FIFO TX levels */
		sc->sc_tx_cur_p_level = 0;
		sc->sc_tx_cur_np_level = 0;

		/* store maximum periodic and non-periodic FIFO TX size */
		sc->sc_tx_max_size = fifo_size;

		/* disable all host channel interrupts */
		DWC_OTG_WRITE_4(sc, DOTG_HAINTMSK, 0);
	}

	if (mode == DWC_MODE_DEVICE) {

	    DWC_OTG_WRITE_4(sc, DOTG_GNPTXFSIZ,
		(0x10 << 16) | (tx_start / 4));
	    fifo_size -= 0x40;
	    tx_start += 0x40;

	    /* setup control endpoint profile */
	    sc->sc_hw_ep_profile[0].usb = dwc_otg_ep_profile[0];

	    /* reset active endpoints */
	    sc->sc_active_rx_ep = 1;

	    for (x = 1; x != sc->sc_dev_ep_max; x++) {

		pf = sc->sc_hw_ep_profile + x;

		pf->usb.max_out_frame_size = 1024 * 3;
		pf->usb.is_simplex = 0;	/* assume duplex */
		pf->usb.support_bulk = 1;
		pf->usb.support_interrupt = 1;
		pf->usb.support_isochronous = 1;
		pf->usb.support_out = 1;

		if (x < sc->sc_dev_in_ep_max) {
			uint32_t limit;

			limit = (x == 1) ? DWC_OTG_MAX_TXN :
			    (DWC_OTG_MAX_TXN / 2);

			if (fifo_size >= limit) {
				DWC_OTG_WRITE_4(sc, DOTG_DIEPTXF(x),
				    ((limit / 4) << 16) |
				    (tx_start / 4));
				tx_start += limit;
				fifo_size -= limit;
				pf->usb.max_in_frame_size = 0x200;
				pf->usb.support_in = 1;
				pf->max_buffer = limit;

			} else if (fifo_size >= 0x80) {
				DWC_OTG_WRITE_4(sc, DOTG_DIEPTXF(x),
				    ((0x80 / 4) << 16) | (tx_start / 4));
				tx_start += 0x80;
				fifo_size -= 0x80;
				pf->usb.max_in_frame_size = 0x40;
				pf->usb.support_in = 1;

			} else {
				pf->usb.is_simplex = 1;
				DWC_OTG_WRITE_4(sc, DOTG_DIEPTXF(x),
				    (0x0 << 16) | (tx_start / 4));
			}
		} else {
			pf->usb.is_simplex = 1;
		}

		DPRINTF("FIFO%d = IN:%d / OUT:%d\n", x,
		    pf->usb.max_in_frame_size,
		    pf->usb.max_out_frame_size);
	    }
	}

	/* reset RX FIFO */
	DWC_OTG_WRITE_4(sc, DOTG_GRSTCTL,
	    GRSTCTL_RXFFLSH);

	if (mode != DWC_MODE_OTG) {
		/* reset all TX FIFOs */
		DWC_OTG_WRITE_4(sc, DOTG_GRSTCTL,
		    GRSTCTL_TXFIFO(0x10) |
		    GRSTCTL_TXFFLSH);
	} else {
		/* reset active endpoints */
		sc->sc_active_rx_ep = 0;

		/* reset periodic and non-periodic FIFO TX size */
		sc->sc_tx_max_size = fifo_size;

		/* reset host channel state */
		memset(sc->sc_chan_state, 0, sizeof(sc->sc_chan_state));

		/* reset FIFO TX levels */
		sc->sc_tx_cur_p_level = 0;
		sc->sc_tx_cur_np_level = 0;
	}
	return (0);
}

static void
dwc_otg_update_host_frame_interval(struct dwc_otg_softc *sc)
{

  /*
   * Disabled until further. Assuming that the register is already
   * programmed correctly by the boot loader.
   */
#if 0
	uint32_t temp;

	/* setup HOST frame interval register, based on existing value */
	temp = DWC_OTG_READ_4(sc, DOTG_HFIR) & HFIR_FRINT_MASK;
	if (temp >= 10000)
		temp /= 1000;
	else
		temp /= 125;

	/* figure out nearest X-tal value */
	if (temp >= 54)
		temp = 60;	/* MHz */
	else if (temp >= 39)
		temp = 48;	/* MHz */
	else
		temp = 30;	/* MHz */

	if (sc->sc_flags.status_high_speed)
		temp *= 125;
	else
		temp *= 1000;

	DPRINTF("HFIR=0x%08x\n", temp);

	DWC_OTG_WRITE_4(sc, DOTG_HFIR, temp);
#endif
}

static void
dwc_otg_clocks_on(struct dwc_otg_softc *sc)
{
	if (sc->sc_flags.clocks_off &&
	    sc->sc_flags.port_powered) {

		DPRINTFN(5, "\n");

		/* TODO - platform specific */

		sc->sc_flags.clocks_off = 0;
	}
}

static void
dwc_otg_clocks_off(struct dwc_otg_softc *sc)
{
	if (!sc->sc_flags.clocks_off) {

		DPRINTFN(5, "\n");

		/* TODO - platform specific */

		sc->sc_flags.clocks_off = 1;
	}
}

static void
dwc_otg_pull_up(struct dwc_otg_softc *sc)
{
	uint32_t temp;

	/* pullup D+, if possible */

	if (!sc->sc_flags.d_pulled_up &&
	    sc->sc_flags.port_powered) {
		sc->sc_flags.d_pulled_up = 1;

		temp = DWC_OTG_READ_4(sc, DOTG_DCTL);
		temp &= ~DCTL_SFTDISCON;
		DWC_OTG_WRITE_4(sc, DOTG_DCTL, temp);
	}
}

static void
dwc_otg_pull_down(struct dwc_otg_softc *sc)
{
	uint32_t temp;

	/* pulldown D+, if possible */

	if (sc->sc_flags.d_pulled_up) {
		sc->sc_flags.d_pulled_up = 0;

		temp = DWC_OTG_READ_4(sc, DOTG_DCTL);
		temp |= DCTL_SFTDISCON;
		DWC_OTG_WRITE_4(sc, DOTG_DCTL, temp);
	}
}

static void
dwc_otg_enable_sof_irq(struct dwc_otg_softc *sc)
{
	/* In device mode we don't use the SOF interrupt */
	if (sc->sc_flags.status_device_mode != 0 ||
	    (sc->sc_irq_mask & GINTMSK_SOFMSK) != 0)
		return;
	sc->sc_irq_mask |= GINTMSK_SOFMSK;
	DWC_OTG_WRITE_4(sc, DOTG_GINTMSK, sc->sc_irq_mask);
}

static void
dwc_otg_resume_irq(struct dwc_otg_softc *sc)
{
	if (sc->sc_flags.status_suspend) {
		/* update status bits */
		sc->sc_flags.status_suspend = 0;
		sc->sc_flags.change_suspend = 1;

		if (sc->sc_flags.status_device_mode) {
			/*
			 * Disable resume interrupt and enable suspend
			 * interrupt:
			 */
			sc->sc_irq_mask &= ~GINTMSK_WKUPINTMSK;
			sc->sc_irq_mask |= GINTMSK_USBSUSPMSK;
			DWC_OTG_WRITE_4(sc, DOTG_GINTMSK, sc->sc_irq_mask);
		}

		/* complete root HUB interrupt endpoint */
		dwc_otg_root_intr(sc);
	}
}

static void
dwc_otg_suspend_irq(struct dwc_otg_softc *sc)
{
	if (!sc->sc_flags.status_suspend) {
		/* update status bits */
		sc->sc_flags.status_suspend = 1;
		sc->sc_flags.change_suspend = 1;

		if (sc->sc_flags.status_device_mode) {
			/*
			 * Disable suspend interrupt and enable resume
			 * interrupt:
			 */
			sc->sc_irq_mask &= ~GINTMSK_USBSUSPMSK;
			sc->sc_irq_mask |= GINTMSK_WKUPINTMSK;
			DWC_OTG_WRITE_4(sc, DOTG_GINTMSK, sc->sc_irq_mask);
		}

		/* complete root HUB interrupt endpoint */
		dwc_otg_root_intr(sc);
	}
}

static void
dwc_otg_wakeup_peer(struct dwc_otg_softc *sc)
{
	if (!sc->sc_flags.status_suspend)
		return;

	DPRINTFN(5, "Remote wakeup\n");

	if (sc->sc_flags.status_device_mode) {
		uint32_t temp;

		/* enable remote wakeup signalling */
		temp = DWC_OTG_READ_4(sc, DOTG_DCTL);
		temp |= DCTL_RMTWKUPSIG;
		DWC_OTG_WRITE_4(sc, DOTG_DCTL, temp);

		/* Wait 8ms for remote wakeup to complete. */
		usb_pause_mtx(&sc->sc_bus.bus_mtx, hz / 125);

		temp &= ~DCTL_RMTWKUPSIG;
		DWC_OTG_WRITE_4(sc, DOTG_DCTL, temp);
	} else {
		/* enable USB port */
		DWC_OTG_WRITE_4(sc, DOTG_PCGCCTL, 0);

		/* wait 10ms */
		usb_pause_mtx(&sc->sc_bus.bus_mtx, hz / 100);

		/* resume port */
		sc->sc_hprt_val |= HPRT_PRTRES;
		DWC_OTG_WRITE_4(sc, DOTG_HPRT, sc->sc_hprt_val);

		/* Wait 100ms for resume signalling to complete. */
		usb_pause_mtx(&sc->sc_bus.bus_mtx, hz / 10);

		/* clear suspend and resume */
		sc->sc_hprt_val &= ~(HPRT_PRTSUSP | HPRT_PRTRES);
		DWC_OTG_WRITE_4(sc, DOTG_HPRT, sc->sc_hprt_val);

		/* Wait 4ms */
		usb_pause_mtx(&sc->sc_bus.bus_mtx, hz / 250);
	}

	/* need to fake resume IRQ */
	dwc_otg_resume_irq(sc);
}

static void
dwc_otg_set_address(struct dwc_otg_softc *sc, uint8_t addr)
{
	uint32_t temp;

	DPRINTFN(5, "addr=%d\n", addr);

	temp = DWC_OTG_READ_4(sc, DOTG_DCFG);
	temp &= ~DCFG_DEVADDR_SET(0x7F);
	temp |= DCFG_DEVADDR_SET(addr);
	DWC_OTG_WRITE_4(sc, DOTG_DCFG, temp);
}

static void
dwc_otg_common_rx_ack(struct dwc_otg_softc *sc)
{
	DPRINTFN(5, "RX status clear\n");

	if (sc->sc_flags.status_device_mode != 0) {
		/* enable RX FIFO level interrupt */
		sc->sc_irq_mask |= GINTMSK_RXFLVLMSK;
		DWC_OTG_WRITE_4(sc, DOTG_GINTMSK, sc->sc_irq_mask);
	}

	/* clear cached status */
	sc->sc_last_rx_status = 0;
}

static void
dwc_otg_clear_hcint(struct dwc_otg_softc *sc, uint8_t x)
{
	uint32_t hcint;

	/* clear all pending interrupts */
	hcint = DWC_OTG_READ_4(sc, DOTG_HCINT(x));
	DWC_OTG_WRITE_4(sc, DOTG_HCINT(x), hcint);

	/* clear buffered interrupts */
	sc->sc_chan_state[x].hcint = 0;
}

static uint8_t
dwc_otg_host_channel_alloc(struct dwc_otg_softc *sc, struct dwc_otg_td *td, uint8_t which, uint8_t is_out)
{
	uint32_t tx_p_size;
	uint32_t tx_np_size;
	uint8_t x;

	if (td->channel[which] < DWC_OTG_MAX_CHANNELS)
		return (0);		/* already allocated */

	/* check if device is suspended */
	if (DWC_OTG_PC2UDEV(td->pc)->flags.self_suspended != 0)
		return (1);		/* busy - cannot transfer data */

	/* compute needed TX FIFO size */
	if (is_out != 0) {
		if (td->ep_type == UE_INTERRUPT ||
		    td->ep_type == UE_ISOCHRONOUS) {
			tx_p_size = td->max_packet_size;
			tx_np_size = 0;
			if (td->hcsplt != 0 && tx_p_size > HCSPLT_XACTLEN_BURST)
				tx_p_size = HCSPLT_XACTLEN_BURST;
			if ((sc->sc_tx_cur_p_level + tx_p_size) > sc->sc_tx_max_size) {
				DPRINTF("Too little FIFO space\n");
				return (1);	/* too little FIFO */
			}
		} else {
			tx_p_size = 0;
			tx_np_size = td->max_packet_size;
			if (td->hcsplt != 0 && tx_np_size > HCSPLT_XACTLEN_BURST)
				tx_np_size = HCSPLT_XACTLEN_BURST;
			if ((sc->sc_tx_cur_np_level + tx_np_size) > sc->sc_tx_max_size) {
				DPRINTF("Too little FIFO space\n");
				return (1);	/* too little FIFO */
			}
		}
	} else {
		/* not a TX transaction */
		tx_p_size = 0;
		tx_np_size = 0;
	}

	for (x = 0; x != sc->sc_host_ch_max; x++) {
		if (sc->sc_chan_state[x].allocated != 0)
			continue;
		/* check if channel is still enabled */
		if (sc->sc_chan_state[x].wait_sof != 0)
			continue;

		sc->sc_chan_state[x].allocated = 1;
		sc->sc_chan_state[x].tx_p_size = tx_p_size;
		sc->sc_chan_state[x].tx_np_size = tx_np_size;

		/* keep track of used TX FIFO, if any */
		sc->sc_tx_cur_p_level += tx_p_size;
		sc->sc_tx_cur_np_level += tx_np_size;

		/* clear interrupts */
		dwc_otg_clear_hcint(sc, x);

		DPRINTF("CH=%d HCCHAR=0x%08x "
		    "HCSPLT=0x%08x\n", x, td->hcchar, td->hcsplt);

		/* set active channel */
		sc->sc_active_rx_ep |= (1 << x);

		/* set channel */
		td->channel[which] = x;

		return (0);	/* allocated */
	}
	/* wait a bit */
	dwc_otg_enable_sof_irq(sc);
	return (1);	/* busy */
}

static void
dwc_otg_host_channel_free(struct dwc_otg_softc *sc, struct dwc_otg_td *td, uint8_t which)
{
	uint8_t x;

	if (td->channel[which] >= DWC_OTG_MAX_CHANNELS)
		return;		/* already freed */

	/* free channel */
	x = td->channel[which];
	td->channel[which] = DWC_OTG_MAX_CHANNELS;

	DPRINTF("CH=%d\n", x);

	/*
	 * We need to let programmed host channels run till complete
	 * else the host channel will stop functioning. Assume that
	 * after a fixed given amount of time the host channel is no
	 * longer doing any USB traffic:
	 */
	if (td->ep_type == UE_ISOCHRONOUS || td->ep_type == UE_INTERRUPT) {
		/* double buffered */
		sc->sc_chan_state[x].wait_sof = DWC_OTG_SLOT_IDLE_MAX;
	} else {
		/* single buffered */
		sc->sc_chan_state[x].wait_sof = DWC_OTG_SLOT_IDLE_MIN;
	}

	sc->sc_chan_state[x].allocated = 0;

	/* ack any pending messages */
	if (sc->sc_last_rx_status != 0 &&
	    GRXSTSRD_CHNUM_GET(sc->sc_last_rx_status) == x) {
		dwc_otg_common_rx_ack(sc);
	}

	/* clear active channel */
	sc->sc_active_rx_ep &= ~(1 << x);
}

static uint8_t
dwc_otg_host_setup_tx(struct dwc_otg_softc *sc, struct dwc_otg_td *td)
{
	struct usb_device_request req __aligned(4);
	uint32_t hcint;
	uint32_t hcchar;
	uint8_t delta;

	if (td->channel[0] < DWC_OTG_MAX_CHANNELS) {
		hcint = sc->sc_chan_state[td->channel[0]].hcint;

		DPRINTF("CH=%d ST=%d HCINT=0x%08x HCCHAR=0x%08x HCTSIZ=0x%08x\n",
		    td->channel[0], td->state, hcint,
		    DWC_OTG_READ_4(sc, DOTG_HCCHAR(td->channel[0])),
		    DWC_OTG_READ_4(sc, DOTG_HCTSIZ(td->channel[0])));
	} else {
		hcint = 0;
		goto check_state;
	}

	if (hcint & (HCINT_RETRY |
	    HCINT_ACK | HCINT_NYET)) {
		/* give success bits priority over failure bits */
	} else if (hcint & HCINT_STALL) {
		DPRINTF("CH=%d STALL\n", td->channel[0]);
		td->error_stall = 1;
		td->error_any = 1;
		goto complete;
	} else if (hcint & HCINT_ERRORS) {
		DPRINTF("CH=%d ERROR\n", td->channel[0]);
		td->errcnt++;
		if (td->hcsplt != 0 || td->errcnt >= 3) {
			td->error_any = 1;
			goto complete;
		}
	}

	if (hcint & (HCINT_ERRORS | HCINT_RETRY |
	    HCINT_ACK | HCINT_NYET)) {
		if (!(hcint & HCINT_ERRORS))
			td->errcnt = 0;
	}

check_state:
	switch (td->state) {
	case DWC_CHAN_ST_START:
		goto send_pkt;

	case DWC_CHAN_ST_WAIT_ANE:
		if (hcint & (HCINT_RETRY | HCINT_ERRORS)) {
			td->did_nak++;
			td->tt_scheduled = 0;
			goto send_pkt;
		} else if (hcint & (HCINT_ACK | HCINT_NYET)) {
			td->offset += td->tx_bytes;
			td->remainder -= td->tx_bytes;
			td->toggle = 1;
			td->tt_scheduled = 0;
			goto complete;
		}
		break;

	case DWC_CHAN_ST_WAIT_S_ANE:
		if (hcint & (HCINT_RETRY | HCINT_ERRORS)) {
			td->did_nak++;
			td->tt_scheduled = 0;
			goto send_pkt;
		} else if (hcint & (HCINT_ACK | HCINT_NYET)) {
			goto send_cpkt;
		}
		break;

	case DWC_CHAN_ST_WAIT_C_ANE:
		if (hcint & HCINT_NYET) {
			goto send_cpkt;
		} else if (hcint & (HCINT_RETRY | HCINT_ERRORS)) {
			td->did_nak++;
			td->tt_scheduled = 0;
			goto send_pkt;
		} else if (hcint & HCINT_ACK) {
			td->offset += td->tx_bytes;
			td->remainder -= td->tx_bytes;
			td->toggle = 1;
			goto complete;
		}
		break;

	case DWC_CHAN_ST_WAIT_C_PKT:
		goto send_cpkt;

	default:
		break;
	}
	goto busy;

send_pkt:
	/* free existing channel, if any */
	dwc_otg_host_channel_free(sc, td, 0);

	if (sizeof(req) != td->remainder) {
		td->error_any = 1;
		goto complete;
	}

	if (td->hcsplt != 0) {
		delta = td->tt_start_slot - sc->sc_last_frame_num - 1;
		if (td->tt_scheduled == 0 || delta < DWC_OTG_TT_SLOT_MAX) {
			td->state = DWC_CHAN_ST_START;
			goto busy;
		}
		delta = sc->sc_last_frame_num - td->tt_start_slot;
		if (delta > 5) {
			/* missed it */
			td->tt_scheduled = 0;
			td->state = DWC_CHAN_ST_START;
			goto busy;
		}
	}

	/* allocate a new channel */
	if (dwc_otg_host_channel_alloc(sc, td, 0, 1)) {
		td->state = DWC_CHAN_ST_START;
		goto busy;
	}

	if (td->hcsplt != 0) {
		td->hcsplt &= ~HCSPLT_COMPSPLT;
		td->state = DWC_CHAN_ST_WAIT_S_ANE;
	} else {
		td->state = DWC_CHAN_ST_WAIT_ANE;
	}

	usbd_copy_out(td->pc, 0, &req, sizeof(req));

	DWC_OTG_WRITE_4(sc, DOTG_HCTSIZ(td->channel[0]),
	    (sizeof(req) << HCTSIZ_XFERSIZE_SHIFT) |
	    (1 << HCTSIZ_PKTCNT_SHIFT) |
	    (HCTSIZ_PID_SETUP << HCTSIZ_PID_SHIFT));

	DWC_OTG_WRITE_4(sc, DOTG_HCSPLT(td->channel[0]), td->hcsplt);

	hcchar = td->hcchar;
	hcchar &= ~(HCCHAR_EPDIR_IN | HCCHAR_EPTYPE_MASK);
	hcchar |= UE_CONTROL << HCCHAR_EPTYPE_SHIFT;

	/* must enable channel before writing data to FIFO */
	DWC_OTG_WRITE_4(sc, DOTG_HCCHAR(td->channel[0]), hcchar);

	/* transfer data into FIFO */
	bus_space_write_region_4(sc->sc_io_tag, sc->sc_io_hdl,
	    DOTG_DFIFO(td->channel[0]), (uint32_t *)&req, sizeof(req) / 4);

	/* store number of bytes transmitted */
	td->tx_bytes = sizeof(req);
	goto busy;

send_cpkt:
	/* free existing channel, if any */
	dwc_otg_host_channel_free(sc, td, 0);

	delta = td->tt_complete_slot - sc->sc_last_frame_num - 1;
	if (td->tt_scheduled == 0 || delta < DWC_OTG_TT_SLOT_MAX) {
		td->state = DWC_CHAN_ST_WAIT_C_PKT;
		goto busy;
	}
	delta = sc->sc_last_frame_num - td->tt_start_slot;
	if (delta > DWC_OTG_TT_SLOT_MAX) {
		/* we missed the service interval */
		if (td->ep_type != UE_ISOCHRONOUS)
			td->error_any = 1;
		goto complete;
	}
	/* allocate a new channel */
	if (dwc_otg_host_channel_alloc(sc, td, 0, 0)) {
		td->state = DWC_CHAN_ST_WAIT_C_PKT;
		goto busy;
	}

	/* wait until next slot before trying again */
	td->tt_complete_slot++;

	td->hcsplt |= HCSPLT_COMPSPLT;
	td->state = DWC_CHAN_ST_WAIT_C_ANE;

	DWC_OTG_WRITE_4(sc, DOTG_HCTSIZ(td->channel[0]),
	    (HCTSIZ_PID_SETUP << HCTSIZ_PID_SHIFT));

	DWC_OTG_WRITE_4(sc, DOTG_HCSPLT(td->channel[0]), td->hcsplt);

	hcchar = td->hcchar;
	hcchar &= ~(HCCHAR_EPDIR_IN | HCCHAR_EPTYPE_MASK);
	hcchar |= UE_CONTROL << HCCHAR_EPTYPE_SHIFT;

	/* must enable channel before writing data to FIFO */
	DWC_OTG_WRITE_4(sc, DOTG_HCCHAR(td->channel[0]), hcchar);

busy:
	return (1);	/* busy */

complete:
	dwc_otg_host_channel_free(sc, td, 0);
	return (0);	/* complete */
}

static uint8_t
dwc_otg_setup_rx(struct dwc_otg_softc *sc, struct dwc_otg_td *td)
{
	struct usb_device_request req __aligned(4);
	uint32_t temp;
	uint16_t count;

	/* check endpoint status */

	if (sc->sc_last_rx_status == 0)
		goto not_complete;

	if (GRXSTSRD_CHNUM_GET(sc->sc_last_rx_status) != 0)
		goto not_complete;

	if ((sc->sc_last_rx_status & GRXSTSRD_DPID_MASK) !=
	    GRXSTSRD_DPID_DATA0) {
		/* release FIFO */
		dwc_otg_common_rx_ack(sc);
		goto not_complete;
	}

	if ((sc->sc_last_rx_status & GRXSTSRD_PKTSTS_MASK) !=
	    GRXSTSRD_STP_DATA) {
		/* release FIFO */
		dwc_otg_common_rx_ack(sc);
		goto not_complete;
	}

	DPRINTFN(5, "GRXSTSR=0x%08x\n", sc->sc_last_rx_status);

	/* clear did stall */
	td->did_stall = 0;

	/* get the packet byte count */
	count = GRXSTSRD_BCNT_GET(sc->sc_last_rx_status);

	/* verify data length */
	if (count != td->remainder) {
		DPRINTFN(0, "Invalid SETUP packet "
		    "length, %d bytes\n", count);
		/* release FIFO */
		dwc_otg_common_rx_ack(sc);
		goto not_complete;
	}
	if (count != sizeof(req)) {
		DPRINTFN(0, "Unsupported SETUP packet "
		    "length, %d bytes\n", count);
		/* release FIFO */
		dwc_otg_common_rx_ack(sc);
		goto not_complete;
	}

	/* copy in control request */
	memcpy(&req, sc->sc_rx_bounce_buffer, sizeof(req));

	/* copy data into real buffer */
	usbd_copy_in(td->pc, 0, &req, sizeof(req));

	td->offset = sizeof(req);
	td->remainder = 0;

	/* sneak peek the set address */
	if ((req.bmRequestType == UT_WRITE_DEVICE) &&
	    (req.bRequest == UR_SET_ADDRESS)) {
		/* must write address before ZLP */
		dwc_otg_set_address(sc, req.wValue[0] & 0x7F);
	}

	/* don't send any data by default */
	DWC_OTG_WRITE_4(sc, DOTG_DIEPTSIZ(0),
	    DXEPTSIZ_SET_NPKT(0) | 
	    DXEPTSIZ_SET_NBYTES(0));

	temp = sc->sc_in_ctl[0];

	/* enable IN endpoint */
	DWC_OTG_WRITE_4(sc, DOTG_DIEPCTL(0),
	    temp | DIEPCTL_EPENA);
	DWC_OTG_WRITE_4(sc, DOTG_DIEPCTL(0),
	    temp | DIEPCTL_SNAK);

	/* reset IN endpoint buffer */
	DWC_OTG_WRITE_4(sc, DOTG_GRSTCTL,
	    GRSTCTL_TXFIFO(0) |
	    GRSTCTL_TXFFLSH);

	/* acknowledge RX status */
	dwc_otg_common_rx_ack(sc);
	return (0);			/* complete */

not_complete:
	/* abort any ongoing transfer, before enabling again */

	temp = sc->sc_out_ctl[0];

	temp |= DOEPCTL_EPENA |
	    DOEPCTL_SNAK;

	/* enable OUT endpoint */
	DWC_OTG_WRITE_4(sc, DOTG_DOEPCTL(0), temp);

	if (!td->did_stall) {
		td->did_stall = 1;

		DPRINTFN(5, "stalling IN and OUT direction\n");

		/* set stall after enabling endpoint */
		DWC_OTG_WRITE_4(sc, DOTG_DOEPCTL(0),
		    temp | DOEPCTL_STALL);

		temp = sc->sc_in_ctl[0];

		/* set stall assuming endpoint is enabled */
		DWC_OTG_WRITE_4(sc, DOTG_DIEPCTL(0),
		    temp | DIEPCTL_STALL);
	}

	/* setup number of buffers to receive */
	DWC_OTG_WRITE_4(sc, DOTG_DOEPTSIZ(0),
	    DXEPTSIZ_SET_MULTI(3) |
	    DXEPTSIZ_SET_NPKT(1) | 
	    DXEPTSIZ_SET_NBYTES(sizeof(req)));

	return (1);			/* not complete */
}

static uint8_t
dwc_otg_host_rate_check_interrupt(struct dwc_otg_softc *sc, struct dwc_otg_td *td)
{
	uint8_t delta;

	delta = sc->sc_tmr_val - td->tmr_val;
	if (delta >= 128)
		return (1);	/* busy */

	td->tmr_val = sc->sc_tmr_val + td->tmr_res;

	/* set toggle, if any */
	if (td->set_toggle) {
		td->set_toggle = 0;
		td->toggle = 1;
	}
	return (0);
}

static uint8_t
dwc_otg_host_rate_check(struct dwc_otg_softc *sc, struct dwc_otg_td *td)
{
	if (td->ep_type == UE_ISOCHRONOUS) {
		/* non TT isochronous traffic */
		if ((td->tmr_val != 0) ||
		    (sc->sc_last_frame_num & (td->tmr_res - 1))) {
			goto busy;
		}
		td->tmr_val = 1;	/* executed */
		td->toggle = 0;

	} else if (td->ep_type == UE_INTERRUPT) {
		if (!td->tt_scheduled)
			goto busy;
		td->tt_scheduled = 0;
	} else if (td->did_nak >= DWC_OTG_NAK_MAX) {
		goto busy;
	} else if (td->set_toggle) {
		td->set_toggle = 0;
		td->toggle = 1;
	}
	return (0);
busy:
	return (1);
}

static uint8_t
dwc_otg_host_data_rx(struct dwc_otg_softc *sc, struct dwc_otg_td *td)
{
	uint32_t hcint;
	uint32_t hcchar;
	uint32_t count;
	uint8_t delta;
	uint8_t channel;

	channel = td->channel[td->tt_channel_tog];

	if (channel < DWC_OTG_MAX_CHANNELS) {
		hcint = sc->sc_chan_state[channel].hcint;

		DPRINTF("CH=%d ST=%d HCINT=0x%08x HCCHAR=0x%08x HCTSIZ=0x%08x\n",
		    channel, td->state, hcint,
		    DWC_OTG_READ_4(sc, DOTG_HCCHAR(channel)),
		    DWC_OTG_READ_4(sc, DOTG_HCTSIZ(channel)));
	} else {
		hcint = 0;
		goto check_state;
	}

	/* check interrupt bits */

	if (hcint & (HCINT_RETRY |
	    HCINT_ACK | HCINT_NYET)) {
		/* give success bits priority over failure bits */
	} else if (hcint & HCINT_STALL) {
		DPRINTF("CH=%d STALL\n", channel);
		td->error_stall = 1;
		td->error_any = 1;
		goto complete;
	} else if (hcint & HCINT_ERRORS) {
		DPRINTF("CH=%d ERROR\n", channel);
		td->errcnt++;
		if (td->hcsplt != 0 || td->errcnt >= 3) {
			if (td->ep_type != UE_ISOCHRONOUS) {
				td->error_any = 1;
				goto complete;
			}
		}
	}

	/* check endpoint status */
	if (sc->sc_last_rx_status == 0)
		goto check_state;

	if (GRXSTSRD_CHNUM_GET(sc->sc_last_rx_status) != channel)
		goto check_state;

	switch (sc->sc_last_rx_status & GRXSTSRD_PKTSTS_MASK) {
	case GRXSTSRH_IN_DATA:

		DPRINTF("DATA ST=%d STATUS=0x%08x\n",
		    (int)td->state, (int)sc->sc_last_rx_status);

		if (hcint & HCINT_SOFTWARE_ONLY) {
			/*
			 * When using SPLIT transactions on interrupt
			 * endpoints, sometimes data occurs twice.
			 */
			DPRINTF("Data already received\n");
			break;
		}

		/* get the packet byte count */
		count = GRXSTSRD_BCNT_GET(sc->sc_last_rx_status);

		/* check for isochronous transfer or high-speed bandwidth endpoint */
		if (td->ep_type == UE_ISOCHRONOUS || td->max_packet_count > 1) {
			if ((sc->sc_last_rx_status & GRXSTSRD_DPID_MASK) != GRXSTSRD_DPID_DATA0) {
				td->tt_xactpos = HCSPLT_XACTPOS_MIDDLE;
			} else {
				td->tt_xactpos = HCSPLT_XACTPOS_BEGIN;

				/* verify the packet byte count */
				if (count < td->max_packet_size) {
					/* we have a short packet */
					td->short_pkt = 1;
					td->got_short = 1;
				}
			}
			td->toggle = 0;
		} else {
			/* verify the packet byte count */
			if (count != td->max_packet_size) {
				if (count < td->max_packet_size) {
					/* we have a short packet */
					td->short_pkt = 1;
					td->got_short = 1;
				} else {
					/* invalid USB packet */
					td->error_any = 1;
			  
					/* release FIFO */
					dwc_otg_common_rx_ack(sc);
					goto complete;
				}
			}
			td->toggle ^= 1;
			td->tt_scheduled = 0;
		}

		/* verify the packet byte count */
		if (count > td->remainder) {
			/* invalid USB packet */
			td->error_any = 1;

			/* release FIFO */
			dwc_otg_common_rx_ack(sc);
			goto complete;
		}

		usbd_copy_in(td->pc, td->offset,
		    sc->sc_rx_bounce_buffer, count);

		td->remainder -= count;
		td->offset += count;
		hcint |= HCINT_SOFTWARE_ONLY;
		sc->sc_chan_state[channel].hcint = hcint;
		break;

	default:
		break;
	}
	/* release FIFO */
	dwc_otg_common_rx_ack(sc);

check_state:
	if (hcint & (HCINT_ERRORS | HCINT_RETRY |
	    HCINT_ACK | HCINT_NYET)) {
		if (!(hcint & HCINT_ERRORS))
			td->errcnt = 0;
	}

	switch (td->state) {
	case DWC_CHAN_ST_START:
		if (td->hcsplt != 0)
			goto receive_spkt;
		else
			goto receive_pkt;

	case DWC_CHAN_ST_WAIT_ANE:
		if (hcint & (HCINT_RETRY | HCINT_ERRORS)) {
			td->did_nak++;
			td->tt_scheduled = 0;
			if (td->hcsplt != 0)
				goto receive_spkt;
			else
				goto receive_pkt;
		} else if (hcint & HCINT_NYET) {
			if (td->hcsplt != 0) {
				/* try again */
				goto receive_pkt;
			} else {
				/* not a valid token for IN endpoints */
				td->error_any = 1;
				goto complete;
			}
		} else if (hcint & HCINT_ACK) {
			/* wait for data - ACK arrived first */
			if (!(hcint & HCINT_SOFTWARE_ONLY))
				goto busy;

			if (td->ep_type == UE_ISOCHRONOUS) {
				/* check if we are complete */
				if ((td->remainder == 0) ||
				    (td->tt_xactpos == HCSPLT_XACTPOS_BEGIN)) {
					goto complete;
				}
				/* get another packet */
				goto receive_pkt;
			} else {
				/* check if we are complete */
				if ((td->remainder == 0) || (td->got_short != 0)) {
					if (td->short_pkt)
						goto complete;

					/*
					 * Else need to receive a zero length
					 * packet.
					 */
				}
				td->tt_scheduled = 0;
				td->did_nak = 0;
				if (td->hcsplt != 0)
					goto receive_spkt;
				else
					goto receive_pkt;
			}
		}
		break;

	case DWC_CHAN_ST_WAIT_S_ANE:
		/*
		 * NOTE: The DWC OTG hardware provides a fake ACK in
		 * case of interrupt and isochronous transfers:
		 */ 
		if (hcint & (HCINT_RETRY | HCINT_ERRORS)) {
			td->did_nak++;
			td->tt_scheduled = 0;
			goto receive_spkt;
		} else if (hcint & HCINT_NYET) {
			td->tt_scheduled = 0;
			goto receive_spkt;
		} else if (hcint & HCINT_ACK) {
			td->did_nak = 0;
			goto receive_pkt;
		}
		break;

	case DWC_CHAN_ST_WAIT_C_PKT:
		goto receive_pkt;

	default:
		break;
	}
	goto busy;

receive_pkt:
	/* free existing channel, if any */
	dwc_otg_host_channel_free(sc, td, td->tt_channel_tog);

  	if (td->hcsplt != 0) {
		delta = td->tt_complete_slot - sc->sc_last_frame_num - 1;
		if (td->tt_scheduled == 0 || delta < DWC_OTG_TT_SLOT_MAX) {
			td->state = DWC_CHAN_ST_WAIT_C_PKT;
			goto busy;
		}
		delta = sc->sc_last_frame_num - td->tt_start_slot;
		if (delta > DWC_OTG_TT_SLOT_MAX) {
			/* we missed the service interval */
			if (td->ep_type != UE_ISOCHRONOUS)
				td->error_any = 1;
			goto complete;
		}
		/* complete split */
		td->hcsplt |= HCSPLT_COMPSPLT;
	} else if (td->tt_xactpos == HCSPLT_XACTPOS_BEGIN &&
	    dwc_otg_host_rate_check(sc, td)) {
		td->state = DWC_CHAN_ST_WAIT_C_PKT;
		goto busy;
	}

	/* allocate a new channel */
	if (dwc_otg_host_channel_alloc(sc, td, td->tt_channel_tog, 0)) {
		td->state = DWC_CHAN_ST_WAIT_C_PKT;
		goto busy;
	}

	channel = td->channel[td->tt_channel_tog];

	/* set toggle, if any */
	if (td->set_toggle) {
		td->set_toggle = 0;
		td->toggle = 1;
	}

	td->state = DWC_CHAN_ST_WAIT_ANE;

	/* receive one packet */
	DWC_OTG_WRITE_4(sc, DOTG_HCTSIZ(channel),
	    (td->max_packet_size << HCTSIZ_XFERSIZE_SHIFT) |
	    (1 << HCTSIZ_PKTCNT_SHIFT) |
	    (td->toggle ? (HCTSIZ_PID_DATA1 << HCTSIZ_PID_SHIFT) :
	    (HCTSIZ_PID_DATA0 << HCTSIZ_PID_SHIFT)));

	DWC_OTG_WRITE_4(sc, DOTG_HCSPLT(channel), td->hcsplt);

	hcchar = td->hcchar;
	hcchar |= HCCHAR_EPDIR_IN;

	/* check if other channel is allocated */
	if (td->channel[td->tt_channel_tog ^ 1] < DWC_OTG_MAX_CHANNELS) {
		/* second - receive after next SOF event */
		if ((sc->sc_last_frame_num & 1) != 0)
			hcchar |= HCCHAR_ODDFRM;
		else
			hcchar &= ~HCCHAR_ODDFRM;

		/* other channel is next */
		td->tt_channel_tog ^= 1;
		td->tt_complete_slot++;

		/* must enable channel before data can be received */
		DWC_OTG_WRITE_4(sc, DOTG_HCCHAR(channel), hcchar);
	} else {
		/* first - receive after second next SOF event */
		if ((sc->sc_last_frame_num & 1) == 0)
			hcchar |= HCCHAR_ODDFRM;
		else
			hcchar &= ~HCCHAR_ODDFRM;

		/* must enable channel before data can be received */
		DWC_OTG_WRITE_4(sc, DOTG_HCCHAR(channel), hcchar);

		if (td->hcsplt != 0) {
			if (td->ep_type == UE_ISOCHRONOUS || td->ep_type == UE_INTERRUPT) {
				/* allocate a second channel */
				td->tt_channel_tog ^= 1;
				goto receive_pkt;
			} else {
				td->tt_complete_slot++;
			}
		}
	}
	goto busy;

receive_spkt:
	/* free existing channel(s), if any */
	dwc_otg_host_channel_free(sc, td, 0);
	dwc_otg_host_channel_free(sc, td, 1);

	delta = td->tt_start_slot - sc->sc_last_frame_num - 1;
	if (td->tt_scheduled == 0 || delta < DWC_OTG_TT_SLOT_MAX) {
		td->state = DWC_CHAN_ST_START;
		goto busy;
	}
	delta = sc->sc_last_frame_num - td->tt_start_slot;
	if (delta > 5) {
		/* missed it */
		td->tt_scheduled = 0;
		td->state = DWC_CHAN_ST_START;
		goto busy;
	}

	/* allocate a new channel */
	if (dwc_otg_host_channel_alloc(sc, td, 0, 0)) {
		td->state = DWC_CHAN_ST_START;
		goto busy;
	}

	channel = td->channel[0];

	td->hcsplt &= ~HCSPLT_COMPSPLT;
	td->state = DWC_CHAN_ST_WAIT_S_ANE;

	/* reset channel toggle */
	td->tt_channel_tog = 0;

	/* receive one packet */
	DWC_OTG_WRITE_4(sc, DOTG_HCTSIZ(channel),
	    (HCTSIZ_PID_DATA0 << HCTSIZ_PID_SHIFT));

	DWC_OTG_WRITE_4(sc, DOTG_HCSPLT(channel), td->hcsplt);

	/* send after next SOF event */
	if ((sc->sc_last_frame_num & 1) == 0)
		td->hcchar |= HCCHAR_ODDFRM;
	else
		td->hcchar &= ~HCCHAR_ODDFRM;

	hcchar = td->hcchar;
	hcchar |= HCCHAR_EPDIR_IN;

	/* must enable channel before data can be received */
	DWC_OTG_WRITE_4(sc, DOTG_HCCHAR(channel), hcchar);
busy:
	return (1);	/* busy */

complete:
	dwc_otg_host_channel_free(sc, td, 0);
	dwc_otg_host_channel_free(sc, td, 1);
	return (0);	/* complete */
}

static uint8_t
dwc_otg_data_rx(struct dwc_otg_softc *sc, struct dwc_otg_td *td)
{
	uint32_t temp;
	uint16_t count;
	uint8_t got_short;

	got_short = 0;

	/* check endpoint status */
	if (sc->sc_last_rx_status == 0)
		goto not_complete;

	if (GRXSTSRD_CHNUM_GET(sc->sc_last_rx_status) != td->ep_no)
		goto not_complete;

	/* check for SETUP packet */
	if ((sc->sc_last_rx_status & GRXSTSRD_PKTSTS_MASK) ==
	    GRXSTSRD_STP_DATA) {
		if (td->remainder == 0) {
			/*
			 * We are actually complete and have
			 * received the next SETUP
			 */
			DPRINTFN(5, "faking complete\n");
			return (0);	/* complete */
		}
		/*
		 * USB Host Aborted the transfer.
		 */
		td->error_any = 1;
		return (0);		/* complete */
	}

	if ((sc->sc_last_rx_status & GRXSTSRD_PKTSTS_MASK) !=
	    GRXSTSRD_OUT_DATA) {
		/* release FIFO */
		dwc_otg_common_rx_ack(sc);
		goto not_complete;
	}

	/* get the packet byte count */
	count = GRXSTSRD_BCNT_GET(sc->sc_last_rx_status);

	/* verify the packet byte count */
	if (count != td->max_packet_size) {
		if (count < td->max_packet_size) {
			/* we have a short packet */
			td->short_pkt = 1;
			got_short = 1;
		} else {
			/* invalid USB packet */
			td->error_any = 1;

			/* release FIFO */
			dwc_otg_common_rx_ack(sc);
			return (0);	/* we are complete */
		}
	}
	/* verify the packet byte count */
	if (count > td->remainder) {
		/* invalid USB packet */
		td->error_any = 1;

		/* release FIFO */
		dwc_otg_common_rx_ack(sc);
		return (0);		/* we are complete */
	}

	usbd_copy_in(td->pc, td->offset, sc->sc_rx_bounce_buffer, count);
	td->remainder -= count;
	td->offset += count;

	/* release FIFO */
	dwc_otg_common_rx_ack(sc);

	/* check if we are complete */
	if ((td->remainder == 0) || got_short) {
		if (td->short_pkt) {
			/* we are complete */
			return (0);
		}
		/* else need to receive a zero length packet */
	}

not_complete:

	temp = sc->sc_out_ctl[td->ep_no];

	temp |= DOEPCTL_EPENA | DOEPCTL_CNAK;

	DWC_OTG_WRITE_4(sc, DOTG_DOEPCTL(td->ep_no), temp);

	/* enable SETUP and transfer complete interrupt */
	if (td->ep_no == 0) {
		DWC_OTG_WRITE_4(sc, DOTG_DOEPTSIZ(0),
		    DXEPTSIZ_SET_NPKT(1) | 
		    DXEPTSIZ_SET_NBYTES(td->max_packet_size));
	} else {
		/* allow reception of multiple packets */
		DWC_OTG_WRITE_4(sc, DOTG_DOEPTSIZ(td->ep_no),
		    DXEPTSIZ_SET_MULTI(1) |
		    DXEPTSIZ_SET_NPKT(4) | 
		    DXEPTSIZ_SET_NBYTES(4 *
		    ((td->max_packet_size + 3) & ~3)));
	}
	return (1);			/* not complete */
}

static uint8_t
dwc_otg_host_data_tx(struct dwc_otg_softc *sc, struct dwc_otg_td *td)
{
	uint32_t count;
	uint32_t hcint;
	uint32_t hcchar;
	uint8_t delta;
	uint8_t channel;

	channel = td->channel[td->tt_channel_tog];

	if (channel < DWC_OTG_MAX_CHANNELS) {
		hcint = sc->sc_chan_state[channel].hcint;

		DPRINTF("CH=%d ST=%d HCINT=0x%08x HCCHAR=0x%08x HCTSIZ=0x%08x\n",
		    channel, td->state, hcint,
		    DWC_OTG_READ_4(sc, DOTG_HCCHAR(channel)),
		    DWC_OTG_READ_4(sc, DOTG_HCTSIZ(channel)));
	} else {
		hcint = 0;
		goto check_state;
	}

	if (hcint & (HCINT_RETRY |
	    HCINT_ACK | HCINT_NYET)) {
		/* give success bits priority over failure bits */
	} else if (hcint & HCINT_STALL) {
		DPRINTF("CH=%d STALL\n", channel);
		td->error_stall = 1;
		td->error_any = 1;
		goto complete;
	} else if (hcint & HCINT_ERRORS) {
		DPRINTF("CH=%d ERROR\n", channel);
		td->errcnt++;
		if (td->hcsplt != 0 || td->errcnt >= 3) {
			td->error_any = 1;
			goto complete;
		}
	}

	if (hcint & (HCINT_ERRORS | HCINT_RETRY |
	    HCINT_ACK | HCINT_NYET)) {

		if (!(hcint & HCINT_ERRORS))
			td->errcnt = 0;
	}

check_state:
	switch (td->state) {
	case DWC_CHAN_ST_START:
		goto send_pkt;

	case DWC_CHAN_ST_WAIT_ANE:
		if (hcint & (HCINT_RETRY | HCINT_ERRORS)) {
			td->did_nak++;
			td->tt_scheduled = 0;
			goto send_pkt;
		} else if (hcint & (HCINT_ACK | HCINT_NYET)) {
			td->offset += td->tx_bytes;
			td->remainder -= td->tx_bytes;
			td->toggle ^= 1;
			td->did_nak = 0;
			td->tt_scheduled = 0;

			/* check remainder */
			if (td->remainder == 0) {
				if (td->short_pkt)
					goto complete;

				/*
				 * Else we need to transmit a short
				 * packet:
				 */
			}
			goto send_pkt;
		}
		break;

	case DWC_CHAN_ST_WAIT_S_ANE:
		if (hcint & (HCINT_RETRY | HCINT_ERRORS)) {
			td->did_nak++;
			td->tt_scheduled = 0;
			goto send_pkt;
		} else if (hcint & (HCINT_ACK | HCINT_NYET)) {
			td->did_nak = 0;
			goto send_cpkt;
		}
		break;

	case DWC_CHAN_ST_WAIT_C_ANE:
		if (hcint & HCINT_NYET) {
			goto send_cpkt;
		} else if (hcint & (HCINT_RETRY | HCINT_ERRORS)) {
			td->did_nak++;
			td->tt_scheduled = 0;
			goto send_pkt;
		} else if (hcint & HCINT_ACK) {
			td->offset += td->tx_bytes;
			td->remainder -= td->tx_bytes;
			td->toggle ^= 1;
			td->did_nak = 0;
			td->tt_scheduled = 0;

			/* check remainder */
			if (td->remainder == 0) {
				if (td->short_pkt)
					goto complete;

				/* else we need to transmit a short packet */
			}
			goto send_pkt;
		}
		break;

	case DWC_CHAN_ST_WAIT_C_PKT:
		goto send_cpkt;

	case DWC_CHAN_ST_TX_WAIT_ISOC:

		/* Check if isochronous OUT traffic is complete */
		if ((hcint & HCINT_HCH_DONE_MASK) == 0)
			break;

		td->offset += td->tx_bytes;
		td->remainder -= td->tx_bytes;

		if (td->hcsplt != 0 || td->remainder == 0)
			goto complete;

		/* check for next packet */
		if (td->max_packet_count > 1)
			td->tt_xactpos++;

		/* free existing channel, if any */
		dwc_otg_host_channel_free(sc, td, td->tt_channel_tog);

		td->state = DWC_CHAN_ST_TX_PKT_ISOC;

		/* FALLTHROUGH */

	case DWC_CHAN_ST_TX_PKT_ISOC:
		if (dwc_otg_host_channel_alloc(sc, td, 0, 1))
			break;
		channel = td->channel[0];
		goto send_isoc_pkt;
	default:
		break;
	}
	goto busy;

send_pkt:
	/* free existing channel(s), if any */
	dwc_otg_host_channel_free(sc, td, 0);
	dwc_otg_host_channel_free(sc, td, 1);

	if (td->hcsplt != 0) {
		delta = td->tt_start_slot - sc->sc_last_frame_num - 1;
		if (td->tt_scheduled == 0 || delta < DWC_OTG_TT_SLOT_MAX) {
			td->state = DWC_CHAN_ST_START;
			goto busy;
		}
		delta = sc->sc_last_frame_num - td->tt_start_slot;
		if (delta > 5) {
			/* missed it */
			td->tt_scheduled = 0;
			td->state = DWC_CHAN_ST_START;
			goto busy;
		}
	} else if (dwc_otg_host_rate_check(sc, td)) {
		td->state = DWC_CHAN_ST_START;
		goto busy;
	}

	/* allocate a new channel */
	if (dwc_otg_host_channel_alloc(sc, td, 0, 1)) {
		td->state = DWC_CHAN_ST_START;
		goto busy;
	}

	channel = td->channel[0];

	/* set toggle, if any */
	if (td->set_toggle) {
		td->set_toggle = 0;
		td->toggle = 1;
	}

	if (td->ep_type == UE_ISOCHRONOUS) {
send_isoc_pkt:
		/* Isochronous OUT transfers don't have any ACKs */
		td->state = DWC_CHAN_ST_TX_WAIT_ISOC;
		td->hcsplt &= ~HCSPLT_COMPSPLT;
		if (td->hcsplt != 0) {
			/* get maximum transfer length */
			count = td->remainder;
			if (count > HCSPLT_XACTLEN_BURST) {
				DPRINTF("TT overflow\n");
				td->error = 1;
				goto complete;
			}
			/* Update transaction position */
			td->hcsplt &= ~HCSPLT_XACTPOS_MASK;
			td->hcsplt |= (HCSPLT_XACTPOS_ALL << HCSPLT_XACTPOS_SHIFT);
		} else {
			/* send one packet at a time */
			count = td->max_packet_size;
			if (td->remainder < count) {
				/* we have a short packet */
				td->short_pkt = 1;
				count = td->remainder;
			}
		}
	} else if (td->hcsplt != 0) {

		td->hcsplt &= ~HCSPLT_COMPSPLT;

		/* Wait for ACK/NAK/ERR from TT */
		td->state = DWC_CHAN_ST_WAIT_S_ANE;

		/* send one packet at a time */
		count = td->max_packet_size;
		if (td->remainder < count) {
			/* we have a short packet */
			td->short_pkt = 1;
			count = td->remainder;
		}
	} else {
		/* Wait for ACK/NAK/STALL from device */
		td->state = DWC_CHAN_ST_WAIT_ANE;

		/* send one packet at a time */
		count = td->max_packet_size;
		if (td->remainder < count) {
			/* we have a short packet */
			td->short_pkt = 1;
			count = td->remainder;
		}
	}

	/* check for High-Speed multi-packets */
	if ((td->hcsplt == 0) && (td->max_packet_count > 1)) {
		if (td->npkt == 0) {
			if (td->remainder >= (3 * td->max_packet_size))
				td->npkt = 3;
			else if (td->remainder >= (2 * td->max_packet_size))
				td->npkt = 2;
			else
				td->npkt = 1;

			if (td->npkt > td->max_packet_count)
				td->npkt = td->max_packet_count;

			td->tt_xactpos = 1;	/* overload */
		}
		if (td->tt_xactpos == td->npkt) {
			if (td->npkt == 1) {
				DWC_OTG_WRITE_4(sc, DOTG_HCTSIZ(channel),
				    (count << HCTSIZ_XFERSIZE_SHIFT) |
				    (1 << HCTSIZ_PKTCNT_SHIFT) |
				    (HCTSIZ_PID_DATA0 << HCTSIZ_PID_SHIFT));
			} else if (td->npkt == 2) {
				DWC_OTG_WRITE_4(sc, DOTG_HCTSIZ(channel),
				    (count << HCTSIZ_XFERSIZE_SHIFT) |
				    (1 << HCTSIZ_PKTCNT_SHIFT) |
				    (HCTSIZ_PID_DATA1 << HCTSIZ_PID_SHIFT));
			} else {
				DWC_OTG_WRITE_4(sc, DOTG_HCTSIZ(channel),
				    (count << HCTSIZ_XFERSIZE_SHIFT) |
				    (1 << HCTSIZ_PKTCNT_SHIFT) |
				    (HCTSIZ_PID_DATA2 << HCTSIZ_PID_SHIFT));
			}
			td->npkt = 0;
		} else {
			DWC_OTG_WRITE_4(sc, DOTG_HCTSIZ(channel),
			    (count << HCTSIZ_XFERSIZE_SHIFT) |
			    (1 << HCTSIZ_PKTCNT_SHIFT) |
			    (HCTSIZ_PID_MDATA << HCTSIZ_PID_SHIFT));
		}
	} else {
		/* TODO: HCTSIZ_DOPNG */

		DWC_OTG_WRITE_4(sc, DOTG_HCTSIZ(channel),
		    (count << HCTSIZ_XFERSIZE_SHIFT) |
		    (1 << HCTSIZ_PKTCNT_SHIFT) |
		    (td->toggle ? (HCTSIZ_PID_DATA1 << HCTSIZ_PID_SHIFT) :
		    (HCTSIZ_PID_DATA0 << HCTSIZ_PID_SHIFT)));
	}

	DWC_OTG_WRITE_4(sc, DOTG_HCSPLT(channel), td->hcsplt);

	hcchar = td->hcchar;
	hcchar &= ~HCCHAR_EPDIR_IN;

	/* send after next SOF event */
	if ((sc->sc_last_frame_num & 1) == 0)
		hcchar |= HCCHAR_ODDFRM;
	else
		hcchar &= ~HCCHAR_ODDFRM;

	/* must enable before writing data to FIFO */
	DWC_OTG_WRITE_4(sc, DOTG_HCCHAR(channel), hcchar);

	if (count != 0) {

		/* clear topmost word before copy */
		sc->sc_tx_bounce_buffer[(count - 1) / 4] = 0;

		/* copy out data */
		usbd_copy_out(td->pc, td->offset,
		    sc->sc_tx_bounce_buffer, count);

		/* transfer data into FIFO */
		bus_space_write_region_4(sc->sc_io_tag, sc->sc_io_hdl,
		    DOTG_DFIFO(channel),
		    sc->sc_tx_bounce_buffer, (count + 3) / 4);
	}

	/* store number of bytes transmitted */
	td->tx_bytes = count;
	goto busy;

send_cpkt:
	/* free existing channel, if any */
	dwc_otg_host_channel_free(sc, td, td->tt_channel_tog);

	delta = td->tt_complete_slot - sc->sc_last_frame_num - 1;
	if (td->tt_scheduled == 0 || delta < DWC_OTG_TT_SLOT_MAX) {
		td->state = DWC_CHAN_ST_WAIT_C_PKT;
		goto busy;
	}
	delta = sc->sc_last_frame_num - td->tt_start_slot;
	if (delta > DWC_OTG_TT_SLOT_MAX) {
		/* we missed the service interval */
		if (td->ep_type != UE_ISOCHRONOUS)
			td->error_any = 1;
		goto complete;
	}

	/* allocate a new channel */
	if (dwc_otg_host_channel_alloc(sc, td, td->tt_channel_tog, 0)) {
		td->state = DWC_CHAN_ST_WAIT_C_PKT;
		goto busy;
	}

	channel = td->channel[td->tt_channel_tog];

 	td->hcsplt |= HCSPLT_COMPSPLT;
	td->state = DWC_CHAN_ST_WAIT_C_ANE;

	DWC_OTG_WRITE_4(sc, DOTG_HCTSIZ(channel),
	    (HCTSIZ_PID_DATA0 << HCTSIZ_PID_SHIFT));

	DWC_OTG_WRITE_4(sc, DOTG_HCSPLT(channel), td->hcsplt);

	hcchar = td->hcchar;
	hcchar &= ~HCCHAR_EPDIR_IN;

	/* check if other channel is allocated */
	if (td->channel[td->tt_channel_tog ^ 1] < DWC_OTG_MAX_CHANNELS) {
		/* second - receive after next SOF event */
		if ((sc->sc_last_frame_num & 1) != 0)
			hcchar |= HCCHAR_ODDFRM;
		else
			hcchar &= ~HCCHAR_ODDFRM;

		/* other channel is next */
		td->tt_channel_tog ^= 1;
		/* wait until next slot before trying again */
		td->tt_complete_slot++;

		/* must enable channel before data can be received */
		DWC_OTG_WRITE_4(sc, DOTG_HCCHAR(channel), hcchar);
	} else {
		/* first - receive after second next SOF event */
		if ((sc->sc_last_frame_num & 1) == 0)
			hcchar |= HCCHAR_ODDFRM;
		else
			hcchar &= ~HCCHAR_ODDFRM;

		/* must enable channel before data can be received */
		DWC_OTG_WRITE_4(sc, DOTG_HCCHAR(channel), hcchar);

		if (td->hcsplt != 0) {
			if (td->ep_type == UE_ISOCHRONOUS || td->ep_type == UE_INTERRUPT) {
				/* allocate a second channel */
				td->tt_channel_tog ^= 1;
				goto send_cpkt;
			} else {
				/* wait until next slot before trying again */
				td->tt_complete_slot++;
			}
		}
	}
busy:
	return (1);	/* busy */

complete:
	dwc_otg_host_channel_free(sc, td, 0);
	dwc_otg_host_channel_free(sc, td, 1);
	return (0);	/* complete */
}

static uint8_t
dwc_otg_data_tx(struct dwc_otg_softc *sc, struct dwc_otg_td *td)
{
	uint32_t max_buffer;
	uint32_t count;
	uint32_t fifo_left;
	uint32_t mpkt;
	uint32_t temp;
	uint8_t to;

	to = 3;				/* don't loop forever! */

	max_buffer = sc->sc_hw_ep_profile[td->ep_no].max_buffer;

repeat:
	/* check for for endpoint 0 data */

	temp = sc->sc_last_rx_status;

	if ((td->ep_no == 0) && (temp != 0) &&
	    (GRXSTSRD_CHNUM_GET(temp) == 0)) {

		if ((temp & GRXSTSRD_PKTSTS_MASK) !=
		    GRXSTSRD_STP_DATA) {

			/* dump data - wrong direction */
			dwc_otg_common_rx_ack(sc);
		} else {
			/*
			 * The current transfer was cancelled
			 * by the USB Host:
			 */
			td->error_any = 1;
			return (0);		/* complete */
		}
	}

	/* fill in more TX data, if possible */
	if (td->tx_bytes != 0) {

		uint16_t cpkt;

		/* check if packets have been transferred */
		temp = DWC_OTG_READ_4(sc, DOTG_DIEPTSIZ(td->ep_no));

		/* get current packet number */
		cpkt = DXEPTSIZ_GET_NPKT(temp);

		if (cpkt >= td->npkt) {
			fifo_left = 0;
		} else {
			if (max_buffer != 0) {
				fifo_left = (td->npkt - cpkt) *
				    td->max_packet_size;

				if (fifo_left > max_buffer)
					fifo_left = max_buffer;
			} else {
				fifo_left = td->max_packet_size;
			}
		}

		count = td->tx_bytes;
		if (count > fifo_left)
			count = fifo_left;

		if (count != 0) {

			/* clear topmost word before copy */
			sc->sc_tx_bounce_buffer[(count - 1) / 4] = 0;

			/* copy out data */
			usbd_copy_out(td->pc, td->offset,
			    sc->sc_tx_bounce_buffer, count);

			/* transfer data into FIFO */
			bus_space_write_region_4(sc->sc_io_tag, sc->sc_io_hdl,
			    DOTG_DFIFO(td->ep_no),
			    sc->sc_tx_bounce_buffer, (count + 3) / 4);

			td->tx_bytes -= count;
			td->remainder -= count;
			td->offset += count;
			td->npkt = cpkt;
		}
		if (td->tx_bytes != 0)
			goto not_complete;

		/* check remainder */
		if (td->remainder == 0) {
			if (td->short_pkt)
				return (0);	/* complete */

			/* else we need to transmit a short packet */
		}
	}

	if (!to--)
		goto not_complete;

	/* check if not all packets have been transferred */
	temp = DWC_OTG_READ_4(sc, DOTG_DIEPTSIZ(td->ep_no));

	if (DXEPTSIZ_GET_NPKT(temp) != 0) {

		DPRINTFN(5, "busy ep=%d npkt=%d DIEPTSIZ=0x%08x "
		    "DIEPCTL=0x%08x\n", td->ep_no,
		    DXEPTSIZ_GET_NPKT(temp),
		    temp, DWC_OTG_READ_4(sc, DOTG_DIEPCTL(td->ep_no)));

		goto not_complete;
	}

	DPRINTFN(5, "rem=%u ep=%d\n", td->remainder, td->ep_no);

	/* try to optimise by sending more data */
	if ((max_buffer != 0) && ((td->max_packet_size & 3) == 0)) {

		/* send multiple packets at the same time */
		mpkt = max_buffer / td->max_packet_size;

		if (mpkt > 0x3FE)
			mpkt = 0x3FE;

		count = td->remainder;
		if (count > 0x7FFFFF)
			count = 0x7FFFFF - (0x7FFFFF % td->max_packet_size);

		td->npkt = count / td->max_packet_size;

		/*
		 * NOTE: We could use 0x3FE instead of "mpkt" in the
		 * check below to get more throughput, but then we
		 * have a dependency towards non-generic chip features
		 * to disable the TX-FIFO-EMPTY interrupts on a per
		 * endpoint basis. Increase the maximum buffer size of
		 * the IN endpoint to increase the performance.
		 */
		if (td->npkt > mpkt) {
			td->npkt = mpkt;
			count = td->max_packet_size * mpkt;
		} else if ((count == 0) || (count % td->max_packet_size)) {
			/* we are transmitting a short packet */
			td->npkt++;
			td->short_pkt = 1;
		}
	} else {
		/* send one packet at a time */
		mpkt = 1;
		count = td->max_packet_size;
		if (td->remainder < count) {
			/* we have a short packet */
			td->short_pkt = 1;
			count = td->remainder;
		}
		td->npkt = 1;
	}
	DWC_OTG_WRITE_4(sc, DOTG_DIEPTSIZ(td->ep_no),
	    DXEPTSIZ_SET_MULTI(1) |
	    DXEPTSIZ_SET_NPKT(td->npkt) | 
	    DXEPTSIZ_SET_NBYTES(count));

	/* make room for buffering */
	td->npkt += mpkt;

	temp = sc->sc_in_ctl[td->ep_no];

	/* must enable before writing data to FIFO */
	DWC_OTG_WRITE_4(sc, DOTG_DIEPCTL(td->ep_no), temp |
	    DIEPCTL_EPENA |
	    DIEPCTL_CNAK);

	td->tx_bytes = count;

	/* check remainder */
	if (td->tx_bytes == 0 &&
	    td->remainder == 0) {
		if (td->short_pkt)
			return (0);	/* complete */

		/* else we need to transmit a short packet */
	}
	goto repeat;

not_complete:
	return (1);			/* not complete */
}

static uint8_t
dwc_otg_data_tx_sync(struct dwc_otg_softc *sc, struct dwc_otg_td *td)
{
	uint32_t temp;

	/*
	 * If all packets are transferred we are complete:
	 */
	temp = DWC_OTG_READ_4(sc, DOTG_DIEPTSIZ(td->ep_no));

	/* check that all packets have been transferred */
	if (DXEPTSIZ_GET_NPKT(temp) != 0) {
		DPRINTFN(5, "busy ep=%d\n", td->ep_no);
		goto not_complete;
	}
	return (0);

not_complete:

	/* we only want to know if there is a SETUP packet or free IN packet */

	temp = sc->sc_last_rx_status;

	if ((td->ep_no == 0) && (temp != 0) &&
	    (GRXSTSRD_CHNUM_GET(temp) == 0)) {

		if ((temp & GRXSTSRD_PKTSTS_MASK) ==
		    GRXSTSRD_STP_DATA) {
			DPRINTFN(5, "faking complete\n");
			/*
			 * Race condition: We are complete!
			 */
			return (0);
		} else {
			/* dump data - wrong direction */
			dwc_otg_common_rx_ack(sc);
		}
	}
	return (1);			/* not complete */
}

static void
dwc_otg_xfer_do_fifo(struct dwc_otg_softc *sc, struct usb_xfer *xfer)
{
	struct dwc_otg_td *td;
	uint8_t toggle;
	uint8_t tmr_val;
	uint8_t tmr_res;

	DPRINTFN(9, "\n");

	td = xfer->td_transfer_cache;
	if (td == NULL)
		return;

	while (1) {
		if ((td->func) (sc, td)) {
			/* operation in progress */
			break;
		}
		if (((void *)td) == xfer->td_transfer_last) {
			goto done;
		}
		if (td->error_any) {
			goto done;
		} else if (td->remainder > 0) {
			/*
			 * We had a short transfer. If there is no alternate
			 * next, stop processing !
			 */
			if (!td->alt_next)
				goto done;
		}

		/*
		 * Fetch the next transfer descriptor and transfer
		 * some flags to the next transfer descriptor
		 */
		tmr_res = td->tmr_res;
		tmr_val = td->tmr_val;
		toggle = td->toggle;
		td = td->obj_next;
		xfer->td_transfer_cache = td;
		td->toggle = toggle;	/* transfer toggle */
		td->tmr_res = tmr_res;
		td->tmr_val = tmr_val;
	}
	return;

done:
	xfer->td_transfer_cache = NULL;
	sc->sc_xfer_complete = 1;
}

static uint8_t
dwc_otg_xfer_do_complete(struct dwc_otg_softc *sc, struct usb_xfer *xfer)
{
	struct dwc_otg_td *td;

	DPRINTFN(9, "\n");

	td = xfer->td_transfer_cache;
	if (td == NULL) {
		/* compute all actual lengths */
		dwc_otg_standard_done(xfer);
		return (1);
	}
	return (0);
}

static void
dwc_otg_timer(void *_sc)
{
	struct dwc_otg_softc *sc = _sc;
	struct usb_xfer *xfer;
	struct dwc_otg_td *td;

	USB_BUS_LOCK_ASSERT(&sc->sc_bus, MA_OWNED);

	DPRINTF("\n");

	USB_BUS_SPIN_LOCK(&sc->sc_bus);

	/* increment timer value */
	sc->sc_tmr_val++;

	TAILQ_FOREACH(xfer, &sc->sc_bus.intr_q.head, wait_entry) {
		td = xfer->td_transfer_cache;
		if (td != NULL) {
			/* reset NAK counter */ 
			td->did_nak = 0;
		}
	}

	/* enable SOF interrupt, which will poll jobs */
	dwc_otg_enable_sof_irq(sc);

	USB_BUS_SPIN_UNLOCK(&sc->sc_bus);

	if (sc->sc_timer_active) {
		/* restart timer */
		usb_callout_reset(&sc->sc_timer,
		    hz / (1000 / DWC_OTG_HOST_TIMER_RATE),
		    &dwc_otg_timer, sc);
	}
}

static void
dwc_otg_timer_start(struct dwc_otg_softc *sc)
{
	if (sc->sc_timer_active != 0)
		return;

	sc->sc_timer_active = 1;

	/* restart timer */
	usb_callout_reset(&sc->sc_timer,
	    hz / (1000 / DWC_OTG_HOST_TIMER_RATE),
	    &dwc_otg_timer, sc);
}

static void
dwc_otg_timer_stop(struct dwc_otg_softc *sc)
{
	if (sc->sc_timer_active == 0)
		return;

	sc->sc_timer_active = 0;

	/* stop timer */
	usb_callout_stop(&sc->sc_timer);
}

static void
dwc_otg_host_channel_disable(struct dwc_otg_softc *sc, uint8_t x)
{
	uint32_t hcchar;

	hcchar = DWC_OTG_READ_4(sc, DOTG_HCCHAR(x));

	/* disable host channel, if any */
	if (hcchar & (HCCHAR_CHENA | HCCHAR_CHDIS)) {
		/* disable channel */
		DWC_OTG_WRITE_4(sc, DOTG_HCCHAR(x),
		    HCCHAR_CHENA | HCCHAR_CHDIS);
		/* wait for chip to get its brains in order */
		sc->sc_chan_state[x].wait_sof = 2;
	}

	/* release TX FIFO usage, if any */
	sc->sc_tx_cur_p_level -= sc->sc_chan_state[x].tx_p_size;
	sc->sc_tx_cur_np_level -= sc->sc_chan_state[x].tx_np_size;

	/* don't release TX FIFO usage twice */
	sc->sc_chan_state[x].tx_p_size = 0;
	sc->sc_chan_state[x].tx_np_size = 0;
}

static uint8_t
dwc_otg_update_host_transfer_schedule(struct dwc_otg_softc *sc)
{
	TAILQ_HEAD(, usb_xfer) head;
	struct usb_xfer *xfer;
	struct usb_xfer *xfer_next;
	struct dwc_otg_td *td;
	uint16_t temp;
	uint8_t x;

	temp = DWC_OTG_READ_4(sc, DOTG_HFNUM) & DWC_OTG_FRAME_MASK;

	if (sc->sc_last_frame_num == temp)
		return (0);

	sc->sc_last_frame_num = temp;

	TAILQ_INIT(&head);

	for (x = 0; x != sc->sc_host_ch_max; x++) {
		if (sc->sc_chan_state[x].wait_sof == 0)
			continue;

		sc->sc_needsof = 1;
		if (--(sc->sc_chan_state[x].wait_sof) == 0)
			dwc_otg_host_channel_disable(sc, x);
	}

	if ((temp & 7) == 0) {
		TAILQ_FOREACH_SAFE(xfer, &sc->sc_bus.intr_q.head, wait_entry, xfer_next) {
			td = xfer->td_transfer_cache;
			if (td == NULL || td->ep_type != UE_ISOCHRONOUS)
				continue;

			/* execute more frames */
			td->tmr_val = 0;

			sc->sc_needsof = 1;

			if (td->hcsplt == 0 || td->tt_scheduled != 0)
				continue;

			/* Start ASAP */
			td->tt_start_slot = temp + 0;
			td->tt_complete_slot = temp + 2;
			td->tt_scheduled = 1;
			TAILQ_REMOVE(&sc->sc_bus.intr_q.head, xfer, wait_entry);
			TAILQ_INSERT_TAIL(&head, xfer, wait_entry);
		}

		TAILQ_FOREACH_SAFE(xfer, &sc->sc_bus.intr_q.head, wait_entry, xfer_next) {
			td = xfer->td_transfer_cache;
			if (td == NULL || td->ep_type != UE_INTERRUPT)
				continue;

			if (td->tt_scheduled != 0) {
				sc->sc_needsof = 1;
				continue;
			}

			if (dwc_otg_host_rate_check_interrupt(sc, td))
				continue;

			if (td->hcsplt == 0) {
				sc->sc_needsof = 1;
				td->tt_scheduled = 1;
				continue;
			}

			/* start ASAP */
			td->tt_start_slot = temp + 0;
			td->tt_complete_slot = temp + 2;
			sc->sc_needsof = 1;
			td->tt_scheduled = 1;
			TAILQ_REMOVE(&sc->sc_bus.intr_q.head, xfer, wait_entry);
			TAILQ_INSERT_TAIL(&head, xfer, wait_entry);
		}

		TAILQ_FOREACH_SAFE(xfer, &sc->sc_bus.intr_q.head, wait_entry, xfer_next) {
			td = xfer->td_transfer_cache;
			if (td == NULL ||
			    td->ep_type != UE_CONTROL ||
			    td->did_nak >= DWC_OTG_NAK_MAX) {
				continue;
			}

			sc->sc_needsof = 1;

			if (td->hcsplt == 0 || td->tt_scheduled != 0)
				continue;

			/* start ASAP */
			td->tt_start_slot = temp + 0;
			td->tt_complete_slot = temp + 1;
			td->tt_scheduled = 1;
			TAILQ_REMOVE(&sc->sc_bus.intr_q.head, xfer, wait_entry);
			TAILQ_INSERT_TAIL(&head, xfer, wait_entry);
		}
	}
	if ((temp & 7) < 6) {
		TAILQ_FOREACH_SAFE(xfer, &sc->sc_bus.intr_q.head, wait_entry, xfer_next) {
			td = xfer->td_transfer_cache;
			if (td == NULL ||
			    td->ep_type != UE_BULK ||
			    td->did_nak >= DWC_OTG_NAK_MAX) {
				continue;
			}

			sc->sc_needsof = 1;

			if (td->hcsplt == 0 || td->tt_scheduled != 0)
				continue;

			/* start ASAP */
			td->tt_start_slot = temp + 0;
			td->tt_complete_slot = temp + 1;
			td->tt_scheduled = 1;
			TAILQ_REMOVE(&sc->sc_bus.intr_q.head, xfer, wait_entry);
			TAILQ_INSERT_TAIL(&head, xfer, wait_entry);
		}
	}

	/* Put TT transfers in execution order at the end */
	TAILQ_CONCAT(&sc->sc_bus.intr_q.head, &head, wait_entry);

	/* move all TT transfers in front, keeping the current order */
	TAILQ_FOREACH_SAFE(xfer, &sc->sc_bus.intr_q.head, wait_entry, xfer_next) {
		td = xfer->td_transfer_cache;
		if (td == NULL || td->hcsplt == 0)
			continue;
		TAILQ_REMOVE(&sc->sc_bus.intr_q.head, xfer, wait_entry);
		TAILQ_INSERT_TAIL(&head, xfer, wait_entry);
	}
	TAILQ_CONCAT(&head, &sc->sc_bus.intr_q.head, wait_entry);
	TAILQ_CONCAT(&sc->sc_bus.intr_q.head, &head, wait_entry);

	/* put non-TT BULK transfers last */
	TAILQ_FOREACH_SAFE(xfer, &sc->sc_bus.intr_q.head, wait_entry, xfer_next) {
		td = xfer->td_transfer_cache;
		if (td == NULL || td->hcsplt != 0 || td->ep_type != UE_BULK)
			continue;
		TAILQ_REMOVE(&sc->sc_bus.intr_q.head, xfer, wait_entry);
		TAILQ_INSERT_TAIL(&head, xfer, wait_entry);
	}
	TAILQ_CONCAT(&sc->sc_bus.intr_q.head, &head, wait_entry);

	if ((temp & 7) == 0) {

		DPRINTFN(12, "SOF interrupt #%d, needsof=%d\n",
		    (int)temp, (int)sc->sc_needsof);

		/* update SOF IRQ mask */
		if (sc->sc_irq_mask & GINTMSK_SOFMSK) {
			if (sc->sc_needsof == 0) {
				sc->sc_irq_mask &= ~GINTMSK_SOFMSK; 
				DWC_OTG_WRITE_4(sc, DOTG_GINTMSK, sc->sc_irq_mask);
			}
		} else {
			if (sc->sc_needsof != 0) {
				sc->sc_irq_mask |= GINTMSK_SOFMSK; 
				DWC_OTG_WRITE_4(sc, DOTG_GINTMSK, sc->sc_irq_mask);
			}
		}

		/* clear need SOF flag */
		sc->sc_needsof = 0;
	}
	return (1);
}

static void
dwc_otg_interrupt_poll(struct dwc_otg_softc *sc)
{
	struct usb_xfer *xfer;
	uint32_t temp;
	uint8_t got_rx_status;
	uint8_t x;

repeat:
	/* get all channel interrupts */
	for (x = 0; x != sc->sc_host_ch_max; x++) {
		temp = DWC_OTG_READ_4(sc, DOTG_HCINT(x));
		if (temp != 0) {
			DWC_OTG_WRITE_4(sc, DOTG_HCINT(x), temp);
			temp &= ~HCINT_SOFTWARE_ONLY;
			sc->sc_chan_state[x].hcint |= temp;
		}
	}

	if (sc->sc_last_rx_status == 0) {

		temp = DWC_OTG_READ_4(sc, DOTG_GINTSTS);
		if (temp & GINTSTS_RXFLVL) {
			/* pop current status */
			sc->sc_last_rx_status =
			    DWC_OTG_READ_4(sc, DOTG_GRXSTSPD);
		}

		if (sc->sc_last_rx_status != 0) {

			uint8_t ep_no;

			temp = sc->sc_last_rx_status &
			    GRXSTSRD_PKTSTS_MASK;

			/* non-data messages we simply skip */
			if (temp != GRXSTSRD_STP_DATA &&
			    temp != GRXSTSRD_OUT_DATA) {
				dwc_otg_common_rx_ack(sc);
				goto repeat;
			}

			temp = GRXSTSRD_BCNT_GET(
			    sc->sc_last_rx_status);
			ep_no = GRXSTSRD_CHNUM_GET(
			    sc->sc_last_rx_status);

			/* receive data, if any */
			if (temp != 0) {
				DPRINTF("Reading %d bytes from ep %d\n", temp, ep_no);
				bus_space_read_region_4(sc->sc_io_tag, sc->sc_io_hdl,
				    DOTG_DFIFO(ep_no),
				    sc->sc_rx_bounce_buffer, (temp + 3) / 4);
			}

			/* check if we should dump the data */
			if (!(sc->sc_active_rx_ep & (1U << ep_no))) {
				dwc_otg_common_rx_ack(sc);
				goto repeat;
			}

			got_rx_status = 1;

			DPRINTFN(5, "RX status = 0x%08x: ch=%d pid=%d bytes=%d sts=%d\n",
			    sc->sc_last_rx_status, ep_no,
			    (sc->sc_last_rx_status >> 15) & 3,
			    GRXSTSRD_BCNT_GET(sc->sc_last_rx_status),
			    (sc->sc_last_rx_status >> 17) & 15);
		} else {
			got_rx_status = 0;
		}
	} else {
		uint8_t ep_no;

		ep_no = GRXSTSRD_CHNUM_GET(
		    sc->sc_last_rx_status);

		/* check if we should dump the data */
		if (!(sc->sc_active_rx_ep & (1U << ep_no))) {
			dwc_otg_common_rx_ack(sc);
			goto repeat;
		}

		got_rx_status = 1;
	}

	/* execute FIFOs */
	TAILQ_FOREACH(xfer, &sc->sc_bus.intr_q.head, wait_entry)
		dwc_otg_xfer_do_fifo(sc, xfer);

	if (got_rx_status) {
		/* check if data was consumed */
		if (sc->sc_last_rx_status == 0)
			goto repeat;

		/* disable RX FIFO level interrupt */
		sc->sc_irq_mask &= ~GINTMSK_RXFLVLMSK;
		DWC_OTG_WRITE_4(sc, DOTG_GINTMSK, sc->sc_irq_mask);
	}

	if (sc->sc_flags.status_device_mode == 0 && sc->sc_xfer_complete == 0) {
		/* update host transfer schedule, so that new transfers can be issued */
		if (dwc_otg_update_host_transfer_schedule(sc))
			goto repeat;
	}
}

static void
dwc_otg_interrupt_complete(struct dwc_otg_softc *sc)
{
	struct usb_xfer *xfer;
repeat:
	/* scan for completion events */
	TAILQ_FOREACH(xfer, &sc->sc_bus.intr_q.head, wait_entry) {
		if (dwc_otg_xfer_do_complete(sc, xfer))
			goto repeat;
	}
}

static void
dwc_otg_vbus_interrupt(struct dwc_otg_softc *sc, uint8_t is_on)
{
	DPRINTFN(5, "vbus = %u\n", is_on);

	if (is_on) {
		if (!sc->sc_flags.status_vbus) {
			sc->sc_flags.status_vbus = 1;

			/* complete root HUB interrupt endpoint */

			dwc_otg_root_intr(sc);
		}
	} else {
		if (sc->sc_flags.status_vbus) {
			sc->sc_flags.status_vbus = 0;
			sc->sc_flags.status_bus_reset = 0;
			sc->sc_flags.status_suspend = 0;
			sc->sc_flags.change_suspend = 0;
			sc->sc_flags.change_connect = 1;

			/* complete root HUB interrupt endpoint */

			dwc_otg_root_intr(sc);
		}
	}
}

int
dwc_otg_filter_interrupt(void *arg)
{
	struct dwc_otg_softc *sc = arg;
	int retval = FILTER_HANDLED;
	uint32_t status;

	/* read and clear interrupt status */
	status = DWC_OTG_READ_4(sc, DOTG_GINTSTS);

	/* clear interrupts we are handling here */
	DWC_OTG_WRITE_4(sc, DOTG_GINTSTS, status & ~DWC_OTG_MSK_GINT_THREAD_IRQ);

	/* check for USB state change interrupts */
	if ((status & DWC_OTG_MSK_GINT_THREAD_IRQ) != 0)
		retval = FILTER_SCHEDULE_THREAD;

	/* clear all IN endpoint interrupts */
	if (status & GINTSTS_IEPINT) {
		uint32_t temp;
		uint8_t x;

		for (x = 0; x != sc->sc_dev_in_ep_max; x++) {
			temp = DWC_OTG_READ_4(sc, DOTG_DIEPINT(x));
			if (temp & DIEPMSK_XFERCOMPLMSK) {
				DWC_OTG_WRITE_4(sc, DOTG_DIEPINT(x),
				    DIEPMSK_XFERCOMPLMSK);
			}
		}
	}

	USB_BUS_SPIN_LOCK(&sc->sc_bus);

	/* poll FIFOs, if any */
	dwc_otg_interrupt_poll(sc);

	if (sc->sc_xfer_complete != 0)
		retval = FILTER_SCHEDULE_THREAD;

	USB_BUS_SPIN_UNLOCK(&sc->sc_bus);

	return (retval);
}

void
dwc_otg_interrupt(void *arg)
{
	struct dwc_otg_softc *sc = arg;
	uint32_t status;

	USB_BUS_LOCK(&sc->sc_bus);
	USB_BUS_SPIN_LOCK(&sc->sc_bus);

	/* read and clear interrupt status */
	status = DWC_OTG_READ_4(sc, DOTG_GINTSTS);

	/* clear interrupts we are handling here */
	DWC_OTG_WRITE_4(sc, DOTG_GINTSTS, status & DWC_OTG_MSK_GINT_THREAD_IRQ);

	DPRINTFN(14, "GINTSTS=0x%08x HAINT=0x%08x HFNUM=0x%08x\n",
	    status, DWC_OTG_READ_4(sc, DOTG_HAINT),
	    DWC_OTG_READ_4(sc, DOTG_HFNUM));

	if (status & GINTSTS_USBRST) {

		/* set correct state */
		sc->sc_flags.status_device_mode = 1;
		sc->sc_flags.status_bus_reset = 0;
		sc->sc_flags.status_suspend = 0;
		sc->sc_flags.change_suspend = 0;
		sc->sc_flags.change_connect = 1;

		/* Disable SOF interrupt */
		sc->sc_irq_mask &= ~GINTMSK_SOFMSK;
		/* Enable RX frame interrupt */
		sc->sc_irq_mask |= GINTMSK_RXFLVLMSK;
		DWC_OTG_WRITE_4(sc, DOTG_GINTMSK, sc->sc_irq_mask);

		/* complete root HUB interrupt endpoint */
		dwc_otg_root_intr(sc);
	}

	/* check for any bus state change interrupts */
	if (status & GINTSTS_ENUMDONE) {

		uint32_t temp;

		DPRINTFN(5, "end of reset\n");

		/* set correct state */
		sc->sc_flags.status_device_mode = 1;
		sc->sc_flags.status_bus_reset = 1;
		sc->sc_flags.status_suspend = 0;
		sc->sc_flags.change_suspend = 0;
		sc->sc_flags.change_connect = 1;
		sc->sc_flags.status_low_speed = 0;
		sc->sc_flags.port_enabled = 1;

		/* reset FIFOs */
		(void) dwc_otg_init_fifo(sc, DWC_MODE_DEVICE);

		/* reset function address */
		dwc_otg_set_address(sc, 0);

		/* figure out enumeration speed */
		temp = DWC_OTG_READ_4(sc, DOTG_DSTS);
		if (DSTS_ENUMSPD_GET(temp) == DSTS_ENUMSPD_HI)
			sc->sc_flags.status_high_speed = 1;
		else
			sc->sc_flags.status_high_speed = 0;

		/*
		 * Disable resume and SOF interrupt, and enable
		 * suspend and RX frame interrupt:
		 */
		sc->sc_irq_mask &= ~(GINTMSK_WKUPINTMSK | GINTMSK_SOFMSK);
		sc->sc_irq_mask |= (GINTMSK_USBSUSPMSK | GINTMSK_RXFLVLMSK);
		DWC_OTG_WRITE_4(sc, DOTG_GINTMSK, sc->sc_irq_mask);

		/* complete root HUB interrupt endpoint */
		dwc_otg_root_intr(sc);
	}

	if (status & GINTSTS_PRTINT) {
		uint32_t hprt;

		hprt = DWC_OTG_READ_4(sc, DOTG_HPRT);

		/* clear change bits */
		DWC_OTG_WRITE_4(sc, DOTG_HPRT, (hprt & (
		    HPRT_PRTPWR | HPRT_PRTENCHNG |
		    HPRT_PRTCONNDET | HPRT_PRTOVRCURRCHNG)) |
		    sc->sc_hprt_val);

		DPRINTFN(12, "GINTSTS=0x%08x, HPRT=0x%08x\n", status, hprt);

		sc->sc_flags.status_device_mode = 0;

		if (hprt & HPRT_PRTCONNSTS)
			sc->sc_flags.status_bus_reset = 1;
		else
			sc->sc_flags.status_bus_reset = 0;

		if (hprt & HPRT_PRTENCHNG)
			sc->sc_flags.change_enabled = 1;

		if (hprt & HPRT_PRTENA)
			sc->sc_flags.port_enabled = 1;
		else
			sc->sc_flags.port_enabled = 0;

		if (hprt & HPRT_PRTOVRCURRCHNG)
			sc->sc_flags.change_over_current = 1;

		if (hprt & HPRT_PRTOVRCURRACT)
			sc->sc_flags.port_over_current = 1;
		else
			sc->sc_flags.port_over_current = 0;

		if (hprt & HPRT_PRTPWR)
			sc->sc_flags.port_powered = 1;
		else
			sc->sc_flags.port_powered = 0;

		if (((hprt & HPRT_PRTSPD_MASK)
		    >> HPRT_PRTSPD_SHIFT) == HPRT_PRTSPD_LOW)
			sc->sc_flags.status_low_speed = 1;
		else
			sc->sc_flags.status_low_speed = 0;

		if (((hprt & HPRT_PRTSPD_MASK)
		    >> HPRT_PRTSPD_SHIFT) == HPRT_PRTSPD_HIGH)
			sc->sc_flags.status_high_speed = 1;
		else
			sc->sc_flags.status_high_speed = 0;

		if (hprt & HPRT_PRTCONNDET)
			sc->sc_flags.change_connect = 1;

		if (hprt & HPRT_PRTSUSP)
			dwc_otg_suspend_irq(sc);
		else
			dwc_otg_resume_irq(sc);

		/* complete root HUB interrupt endpoint */
		dwc_otg_root_intr(sc);

		/* disable RX FIFO level interrupt */
		sc->sc_irq_mask &= ~GINTMSK_RXFLVLMSK;
		DWC_OTG_WRITE_4(sc, DOTG_GINTMSK, sc->sc_irq_mask);

		/* update host frame interval */
		dwc_otg_update_host_frame_interval(sc);
	}

	/*
	 * If resume and suspend is set at the same time we interpret
	 * that like RESUME. Resume is set when there is at least 3
	 * milliseconds of inactivity on the USB BUS.
	 */
	if (status & GINTSTS_WKUPINT) {

		DPRINTFN(5, "resume interrupt\n");

		dwc_otg_resume_irq(sc);

	} else if (status & GINTSTS_USBSUSP) {

		DPRINTFN(5, "suspend interrupt\n");

		dwc_otg_suspend_irq(sc);
	}
	/* check VBUS */
	if (status & (GINTSTS_USBSUSP |
	    GINTSTS_USBRST |
	    GINTMSK_OTGINTMSK |
	    GINTSTS_SESSREQINT)) {
		uint32_t temp;

		temp = DWC_OTG_READ_4(sc, DOTG_GOTGCTL);

		DPRINTFN(5, "GOTGCTL=0x%08x\n", temp);

		dwc_otg_vbus_interrupt(sc,
		    (temp & (GOTGCTL_ASESVLD | GOTGCTL_BSESVLD)) ? 1 : 0);
	}

	if (sc->sc_xfer_complete != 0) {
		sc->sc_xfer_complete = 0;

		/* complete FIFOs, if any */
		dwc_otg_interrupt_complete(sc);

		if (sc->sc_flags.status_device_mode == 0) {
			/* update host transfer schedule, so that new transfers can be issued */
			if (dwc_otg_update_host_transfer_schedule(sc))
				dwc_otg_interrupt_poll(sc);
		}
	}

	USB_BUS_SPIN_UNLOCK(&sc->sc_bus);
	USB_BUS_UNLOCK(&sc->sc_bus);
}

static void
dwc_otg_setup_standard_chain_sub(struct dwc_otg_std_temp *temp)
{
	struct dwc_otg_td *td;

	/* get current Transfer Descriptor */
	td = temp->td_next;
	temp->td = td;

	/* prepare for next TD */
	temp->td_next = td->obj_next;

	/* fill out the Transfer Descriptor */
	td->func = temp->func;
	td->pc = temp->pc;
	td->offset = temp->offset;
	td->remainder = temp->len;
	td->tx_bytes = 0;
	td->error_any = 0;
	td->error_stall = 0;
	td->npkt = 0;
	td->did_stall = temp->did_stall;
	td->short_pkt = temp->short_pkt;
	td->alt_next = temp->setup_alt_next;
	td->set_toggle = 0;
	td->got_short = 0;
	td->did_nak = 0;
	td->channel[0] = DWC_OTG_MAX_CHANNELS;
	td->channel[1] = DWC_OTG_MAX_CHANNELS;
	td->state = 0;
	td->errcnt = 0;
	td->tt_scheduled = 0;
	td->tt_channel_tog = 0;
	td->tt_xactpos = HCSPLT_XACTPOS_BEGIN;
}

static void
dwc_otg_setup_standard_chain(struct usb_xfer *xfer)
{
	struct dwc_otg_std_temp temp;
	struct dwc_otg_td *td;
	uint32_t x;
	uint8_t need_sync;
	uint8_t is_host;

	DPRINTFN(9, "addr=%d endpt=%d sumlen=%d speed=%d\n",
	    xfer->address, UE_GET_ADDR(xfer->endpointno),
	    xfer->sumlen, usbd_get_speed(xfer->xroot->udev));

	temp.max_frame_size = xfer->max_frame_size;

	td = xfer->td_start[0];
	xfer->td_transfer_first = td;
	xfer->td_transfer_cache = td;

	/* setup temp */

	temp.pc = NULL;
	temp.td = NULL;
	temp.td_next = xfer->td_start[0];
	temp.offset = 0;
	temp.setup_alt_next = xfer->flags_int.short_frames_ok ||
	    xfer->flags_int.isochronous_xfr;
	temp.did_stall = !xfer->flags_int.control_stall;

	is_host = (xfer->xroot->udev->flags.usb_mode == USB_MODE_HOST);

	/* check if we should prepend a setup message */

	if (xfer->flags_int.control_xfr) {
		if (xfer->flags_int.control_hdr) {

			if (is_host)
				temp.func = &dwc_otg_host_setup_tx;
			else
				temp.func = &dwc_otg_setup_rx;

			temp.len = xfer->frlengths[0];
			temp.pc = xfer->frbuffers + 0;
			temp.short_pkt = temp.len ? 1 : 0;

			/* check for last frame */
			if (xfer->nframes == 1) {
				/* no STATUS stage yet, SETUP is last */
				if (xfer->flags_int.control_act)
					temp.setup_alt_next = 0;
			}

			dwc_otg_setup_standard_chain_sub(&temp);
		}
		x = 1;
	} else {
		x = 0;
	}

	if (x != xfer->nframes) {
		if (xfer->endpointno & UE_DIR_IN) {
			if (is_host) {
				temp.func = &dwc_otg_host_data_rx;
				need_sync = 0;
			} else {
				temp.func = &dwc_otg_data_tx;
				need_sync = 1;
			}
		} else {
			if (is_host) {
				temp.func = &dwc_otg_host_data_tx;
				need_sync = 0;
			} else {
				temp.func = &dwc_otg_data_rx;
				need_sync = 0;
			}
		}

		/* setup "pc" pointer */
		temp.pc = xfer->frbuffers + x;
	} else {
		need_sync = 0;
	}
	while (x != xfer->nframes) {

		/* DATA0 / DATA1 message */

		temp.len = xfer->frlengths[x];

		x++;

		if (x == xfer->nframes) {
			if (xfer->flags_int.control_xfr) {
				if (xfer->flags_int.control_act) {
					temp.setup_alt_next = 0;
				}
			} else {
				temp.setup_alt_next = 0;
			}
		}
		if (temp.len == 0) {

			/* make sure that we send an USB packet */

			temp.short_pkt = 0;

		} else {

			/* regular data transfer */

			temp.short_pkt = (xfer->flags.force_short_xfer ? 0 : 1);
		}

		dwc_otg_setup_standard_chain_sub(&temp);

		if (xfer->flags_int.isochronous_xfr) {
			temp.offset += temp.len;
		} else {
			/* get next Page Cache pointer */
			temp.pc = xfer->frbuffers + x;
		}
	}

	if (xfer->flags_int.control_xfr) {

		/* always setup a valid "pc" pointer for status and sync */
		temp.pc = xfer->frbuffers + 0;
		temp.len = 0;
		temp.short_pkt = 0;
		temp.setup_alt_next = 0;

		/* check if we need to sync */
		if (need_sync) {
			/* we need a SYNC point after TX */
			temp.func = &dwc_otg_data_tx_sync;
			dwc_otg_setup_standard_chain_sub(&temp);
		}

		/* check if we should append a status stage */
		if (!xfer->flags_int.control_act) {

			/*
			 * Send a DATA1 message and invert the current
			 * endpoint direction.
			 */
			if (xfer->endpointno & UE_DIR_IN) {
				if (is_host) {
					temp.func = &dwc_otg_host_data_tx;
					need_sync = 0;
				} else {
					temp.func = &dwc_otg_data_rx;
					need_sync = 0;
				}
			} else {
				if (is_host) {
					temp.func = &dwc_otg_host_data_rx;
					need_sync = 0;
				} else {
					temp.func = &dwc_otg_data_tx;
					need_sync = 1;
				}
			}

			dwc_otg_setup_standard_chain_sub(&temp);

			/* data toggle should be DATA1 */
			td = temp.td;
			td->set_toggle = 1;

			if (need_sync) {
				/* we need a SYNC point after TX */
				temp.func = &dwc_otg_data_tx_sync;
				dwc_otg_setup_standard_chain_sub(&temp);
			}
		}
	} else {
		/* check if we need to sync */
		if (need_sync) {

			temp.pc = xfer->frbuffers + 0;
			temp.len = 0;
			temp.short_pkt = 0;
			temp.setup_alt_next = 0;

			/* we need a SYNC point after TX */
			temp.func = &dwc_otg_data_tx_sync;
			dwc_otg_setup_standard_chain_sub(&temp);
		}
	}

	/* must have at least one frame! */
	td = temp.td;
	xfer->td_transfer_last = td;

	if (is_host) {

		struct dwc_otg_softc *sc;
		uint32_t hcchar;
		uint32_t hcsplt;

		sc = DWC_OTG_BUS2SC(xfer->xroot->bus);

		/* get first again */
		td = xfer->td_transfer_first;
		td->toggle = (xfer->endpoint->toggle_next ? 1 : 0);

		hcchar =
			(xfer->address << HCCHAR_DEVADDR_SHIFT) |
			((xfer->endpointno & UE_ADDR) << HCCHAR_EPNUM_SHIFT) |
			(xfer->max_packet_size << HCCHAR_MPS_SHIFT) |
			HCCHAR_CHENA;

		/* XXX stability hack - possible HW issue */
		if (td->ep_type == UE_CONTROL)
			hcchar |= (UE_BULK << HCCHAR_EPTYPE_SHIFT);
		else
			hcchar |= (td->ep_type << HCCHAR_EPTYPE_SHIFT);

		if (usbd_get_speed(xfer->xroot->udev) == USB_SPEED_LOW)
			hcchar |= HCCHAR_LSPDDEV;
		if (UE_GET_DIR(xfer->endpointno) == UE_DIR_IN)
			hcchar |= HCCHAR_EPDIR_IN;

		switch (xfer->xroot->udev->speed) {
		case USB_SPEED_FULL:
		case USB_SPEED_LOW:
			/* check if root HUB port is running High Speed */
			if (xfer->xroot->udev->parent_hs_hub != NULL) {
				hcsplt = HCSPLT_SPLTENA |
				    (xfer->xroot->udev->hs_port_no <<
				    HCSPLT_PRTADDR_SHIFT) |
				    (xfer->xroot->udev->hs_hub_addr <<
				    HCSPLT_HUBADDR_SHIFT);
			} else {
				hcsplt = 0;
			}
			if (td->ep_type == UE_INTERRUPT) {
				uint32_t ival;
				ival = xfer->interval / DWC_OTG_HOST_TIMER_RATE;
				if (ival == 0)
					ival = 1;
				else if (ival > 127)
					ival = 127;
				td->tmr_val = sc->sc_tmr_val + ival;
				td->tmr_res = ival;
			} else if (td->ep_type == UE_ISOCHRONOUS) {
				td->tmr_val = 0;
				td->tmr_res = 1;
			} else {
				td->tmr_val = 0;
				td->tmr_res = 0;
			}
			break;
		case USB_SPEED_HIGH:
			hcsplt = 0;
			if (td->ep_type == UE_ISOCHRONOUS ||
			    td->ep_type == UE_INTERRUPT) {
				hcchar |= ((xfer->max_packet_count & 3)
				    << HCCHAR_MC_SHIFT);
			}
			if (td->ep_type == UE_INTERRUPT) {
				uint32_t ival;
				ival = xfer->interval / DWC_OTG_HOST_TIMER_RATE;
				if (ival == 0)
					ival = 1;
				else if (ival > 127)
					ival = 127;
				td->tmr_val = sc->sc_tmr_val + ival;
				td->tmr_res = ival;
			} else if (td->ep_type == UE_ISOCHRONOUS) {
				td->tmr_val = 0;
				td->tmr_res = 1 << usbd_xfer_get_fps_shift(xfer);
			} else {
				td->tmr_val = 0;
				td->tmr_res = 0;
			}
			break;
		default:
			hcsplt = 0;
			td->tmr_val = 0;
			td->tmr_res = 0;
			break;
		}

		/* store configuration in all TD's */
		while (1) {
			td->hcchar = hcchar;
			td->hcsplt = hcsplt;

			if (((void *)td) == xfer->td_transfer_last)
				break;

			td = td->obj_next;
		}
	}
}

static void
dwc_otg_timeout(void *arg)
{
	struct usb_xfer *xfer = arg;

	DPRINTF("xfer=%p\n", xfer);

	USB_BUS_LOCK_ASSERT(xfer->xroot->bus, MA_OWNED);

	/* transfer is transferred */
	dwc_otg_device_done(xfer, USB_ERR_TIMEOUT);
}

static void
dwc_otg_start_standard_chain(struct usb_xfer *xfer)
{
	struct dwc_otg_softc *sc = DWC_OTG_BUS2SC(xfer->xroot->bus);
	struct usb_xfer_root *xroot;
	struct dwc_otg_td *td;

	DPRINTFN(9, "\n");

	/*
	 * Poll one time in device mode, which will turn on the
	 * endpoint interrupts. Else wait for SOF interrupt in host
	 * mode.
	 */
	if (sc->sc_flags.status_device_mode != 0) {
		dwc_otg_xfer_do_fifo(sc, xfer);
		if (dwc_otg_xfer_do_complete(sc, xfer))
			return;
	}
	USB_BUS_SPIN_LOCK(&sc->sc_bus);

	/* put transfer on interrupt queue */
	usbd_transfer_enqueue(&xfer->xroot->bus->intr_q, xfer);

	/* start timeout, if any */
	if (xfer->timeout != 0) {
		usbd_transfer_timeout_ms(xfer,
		    &dwc_otg_timeout, xfer->timeout);
	}

	if (sc->sc_flags.status_device_mode != 0)
		goto done;

	/* enable SOF interrupt, if any */
	dwc_otg_enable_sof_irq(sc);

	td = xfer->td_transfer_cache;
	if (td->ep_type != UE_BULK)
		goto done;

	xroot = xfer->xroot;

	/*
	 * Optimise the ping-pong effect by waking up other BULK
	 * transfers belonging to the same device group:
	 */
	TAILQ_FOREACH(xfer, &sc->sc_bus.intr_q.head, wait_entry) {
		td = xfer->td_transfer_cache;
		if (td == NULL || td->ep_type != UE_BULK || xfer->xroot != xroot)
			continue;
		/* reset NAK counter */ 
		td->did_nak = 0;
	}
done:
	USB_BUS_SPIN_UNLOCK(&sc->sc_bus);
}

static void
dwc_otg_root_intr(struct dwc_otg_softc *sc)
{
	DPRINTFN(9, "\n");

	USB_BUS_LOCK_ASSERT(&sc->sc_bus, MA_OWNED);

	/* set port bit */
	sc->sc_hub_idata[0] = 0x02;	/* we only have one port */

	uhub_root_intr(&sc->sc_bus, sc->sc_hub_idata,
	    sizeof(sc->sc_hub_idata));
}

static usb_error_t
dwc_otg_standard_done_sub(struct usb_xfer *xfer)
{
	struct dwc_otg_td *td;
	uint32_t len;
	usb_error_t error;

	DPRINTFN(9, "\n");

	td = xfer->td_transfer_cache;

	do {
		len = td->remainder;

		/* store last data toggle */
		xfer->endpoint->toggle_next = td->toggle;

		if (xfer->aframes != xfer->nframes) {
			/*
			 * Verify the length and subtract
			 * the remainder from "frlengths[]":
			 */
			if (len > xfer->frlengths[xfer->aframes]) {
				td->error_any = 1;
			} else {
				xfer->frlengths[xfer->aframes] -= len;
			}
		}
		/* Check for transfer error */
		if (td->error_any) {
			/* the transfer is finished */
			error = (td->error_stall ?
			    USB_ERR_STALLED : USB_ERR_IOERROR);
			td = NULL;
			break;
		}
		/* Check for short transfer */
		if (len > 0) {
			if (xfer->flags_int.short_frames_ok ||
			    xfer->flags_int.isochronous_xfr) {
				/* follow alt next */
				if (td->alt_next) {
					td = td->obj_next;
				} else {
					td = NULL;
				}
			} else {
				/* the transfer is finished */
				td = NULL;
			}
			error = 0;
			break;
		}
		td = td->obj_next;

		/* this USB frame is complete */
		error = 0;
		break;

	} while (0);

	/* update transfer cache */

	xfer->td_transfer_cache = td;

	return (error);
}

static void
dwc_otg_standard_done(struct usb_xfer *xfer)
{
	usb_error_t err = 0;

	DPRINTFN(13, "xfer=%p endpoint=%p transfer done\n",
	    xfer, xfer->endpoint);

	/* reset scanner */

	xfer->td_transfer_cache = xfer->td_transfer_first;

	if (xfer->flags_int.control_xfr) {

		if (xfer->flags_int.control_hdr) {

			err = dwc_otg_standard_done_sub(xfer);
		}
		xfer->aframes = 1;

		if (xfer->td_transfer_cache == NULL) {
			goto done;
		}
	}
	while (xfer->aframes != xfer->nframes) {

		err = dwc_otg_standard_done_sub(xfer);
		xfer->aframes++;

		if (xfer->td_transfer_cache == NULL) {
			goto done;
		}
	}

	if (xfer->flags_int.control_xfr &&
	    !xfer->flags_int.control_act) {

		err = dwc_otg_standard_done_sub(xfer);
	}
done:
	dwc_otg_device_done(xfer, err);
}

/*------------------------------------------------------------------------*
 *	dwc_otg_device_done
 *
 * NOTE: this function can be called more than one time on the
 * same USB transfer!
 *------------------------------------------------------------------------*/
static void
dwc_otg_device_done(struct usb_xfer *xfer, usb_error_t error)
{
	struct dwc_otg_softc *sc = DWC_OTG_BUS2SC(xfer->xroot->bus);

	DPRINTFN(9, "xfer=%p, endpoint=%p, error=%d\n",
	    xfer, xfer->endpoint, error);

	if (xfer->flags_int.usb_mode == USB_MODE_DEVICE) {
		/* Interrupts are cleared by the interrupt handler */
	} else {
		struct dwc_otg_td *td;

		td = xfer->td_transfer_first;

		if (td != NULL) {
			dwc_otg_host_channel_free(sc, td, 0);
			dwc_otg_host_channel_free(sc, td, 1);
		}
	}
	/* dequeue transfer and start next transfer */
	usbd_transfer_done(xfer, error);
}

static void
dwc_otg_xfer_stall(struct usb_xfer *xfer)
{
	dwc_otg_device_done(xfer, USB_ERR_STALLED);
}

static void
dwc_otg_set_stall(struct usb_device *udev,
    struct usb_endpoint *ep, uint8_t *did_stall)
{
	struct dwc_otg_softc *sc;
	uint32_t temp;
	uint32_t reg;
	uint8_t ep_no;

	USB_BUS_LOCK_ASSERT(udev->bus, MA_OWNED);

	/* check mode */
	if (udev->flags.usb_mode != USB_MODE_DEVICE) {
		/* not supported */
		return;
	}

	sc = DWC_OTG_BUS2SC(udev->bus);

	/* get endpoint address */
	ep_no = ep->edesc->bEndpointAddress;

	DPRINTFN(5, "endpoint=0x%x\n", ep_no);

	if (ep_no & UE_DIR_IN) {
		reg = DOTG_DIEPCTL(ep_no & UE_ADDR);
		temp = sc->sc_in_ctl[ep_no & UE_ADDR];
	} else {
		reg = DOTG_DOEPCTL(ep_no & UE_ADDR);
		temp = sc->sc_out_ctl[ep_no & UE_ADDR];
	}

	/* disable and stall endpoint */
	DWC_OTG_WRITE_4(sc, reg, temp | DOEPCTL_EPDIS);
	DWC_OTG_WRITE_4(sc, reg, temp | DOEPCTL_STALL);

	/* clear active OUT ep */
	if (!(ep_no & UE_DIR_IN)) {

		sc->sc_active_rx_ep &= ~(1U << (ep_no & UE_ADDR));

		if (sc->sc_last_rx_status != 0 &&
		    (ep_no & UE_ADDR) == GRXSTSRD_CHNUM_GET(
		    sc->sc_last_rx_status)) {
			/* dump data */
			dwc_otg_common_rx_ack(sc);
			/* poll interrupt */
			dwc_otg_interrupt_poll(sc);
			dwc_otg_interrupt_complete(sc);
		}
	}
}

static void
dwc_otg_clear_stall_sub(struct dwc_otg_softc *sc, uint32_t mps,
    uint8_t ep_no, uint8_t ep_type, uint8_t ep_dir)
{
	uint32_t reg;
	uint32_t temp;

	if (ep_type == UE_CONTROL) {
		/* clearing stall is not needed */
		return;
	}

	if (ep_dir) {
		reg = DOTG_DIEPCTL(ep_no);
	} else {
		reg = DOTG_DOEPCTL(ep_no);
		sc->sc_active_rx_ep |= (1U << ep_no);
	}

	/* round up and mask away the multiplier count */
	mps = (mps + 3) & 0x7FC;

	if (ep_type == UE_BULK) {
		temp = DIEPCTL_EPTYPE_SET(
		    DIEPCTL_EPTYPE_BULK) |
		    DIEPCTL_USBACTEP;
	} else if (ep_type == UE_INTERRUPT) {
		temp = DIEPCTL_EPTYPE_SET(
		    DIEPCTL_EPTYPE_INTERRUPT) |
		    DIEPCTL_USBACTEP;
	} else {
		temp = DIEPCTL_EPTYPE_SET(
		    DIEPCTL_EPTYPE_ISOC) |
		    DIEPCTL_USBACTEP;
	}

	temp |= DIEPCTL_MPS_SET(mps);
	temp |= DIEPCTL_TXFNUM_SET(ep_no);

	if (ep_dir)
		sc->sc_in_ctl[ep_no] = temp;
	else
		sc->sc_out_ctl[ep_no] = temp;

	DWC_OTG_WRITE_4(sc, reg, temp | DOEPCTL_EPDIS);
	DWC_OTG_WRITE_4(sc, reg, temp | DOEPCTL_SETD0PID);
	DWC_OTG_WRITE_4(sc, reg, temp | DIEPCTL_SNAK);

	/* we only reset the transmit FIFO */
	if (ep_dir) {
		DWC_OTG_WRITE_4(sc, DOTG_GRSTCTL,
		    GRSTCTL_TXFIFO(ep_no) |
		    GRSTCTL_TXFFLSH);

		DWC_OTG_WRITE_4(sc,
		    DOTG_DIEPTSIZ(ep_no), 0);
	}

	/* poll interrupt */
	dwc_otg_interrupt_poll(sc);
	dwc_otg_interrupt_complete(sc);
}

static void
dwc_otg_clear_stall(struct usb_device *udev, struct usb_endpoint *ep)
{
	struct dwc_otg_softc *sc;
	struct usb_endpoint_descriptor *ed;

	DPRINTFN(5, "endpoint=%p\n", ep);

	USB_BUS_LOCK_ASSERT(udev->bus, MA_OWNED);

	/* check mode */
	if (udev->flags.usb_mode != USB_MODE_DEVICE) {
		/* not supported */
		return;
	}
	/* get softc */
	sc = DWC_OTG_BUS2SC(udev->bus);

	/* get endpoint descriptor */
	ed = ep->edesc;

	/* reset endpoint */
	dwc_otg_clear_stall_sub(sc,
	    UGETW(ed->wMaxPacketSize),
	    (ed->bEndpointAddress & UE_ADDR),
	    (ed->bmAttributes & UE_XFERTYPE),
	    (ed->bEndpointAddress & (UE_DIR_IN | UE_DIR_OUT)));
}

static void
dwc_otg_device_state_change(struct usb_device *udev)
{
	struct dwc_otg_softc *sc;
	uint8_t x;

	/* check mode */
	if (udev->flags.usb_mode != USB_MODE_DEVICE) {
		/* not supported */
		return;
	}

	/* get softc */
	sc = DWC_OTG_BUS2SC(udev->bus);

	/* deactivate all other endpoint but the control endpoint */
	if (udev->state == USB_STATE_CONFIGURED ||
	    udev->state == USB_STATE_ADDRESSED) {

		USB_BUS_LOCK(&sc->sc_bus);

		for (x = 1; x != sc->sc_dev_ep_max; x++) {

			if (x < sc->sc_dev_in_ep_max) {
				DWC_OTG_WRITE_4(sc, DOTG_DIEPCTL(x),
				    DIEPCTL_EPDIS);
				DWC_OTG_WRITE_4(sc, DOTG_DIEPCTL(x), 0);
			}

			DWC_OTG_WRITE_4(sc, DOTG_DOEPCTL(x),
			    DOEPCTL_EPDIS);
			DWC_OTG_WRITE_4(sc, DOTG_DOEPCTL(x), 0);
		}
		USB_BUS_UNLOCK(&sc->sc_bus);
	}
}

int
dwc_otg_init(struct dwc_otg_softc *sc)
{
	uint32_t temp;

	DPRINTF("start\n");

	/* set up the bus structure */
	sc->sc_bus.usbrev = USB_REV_2_0;
	sc->sc_bus.methods = &dwc_otg_bus_methods;

	usb_callout_init_mtx(&sc->sc_timer,
	    &sc->sc_bus.bus_mtx, 0);

	USB_BUS_LOCK(&sc->sc_bus);

	/* turn on clocks */
	dwc_otg_clocks_on(sc);

	temp = DWC_OTG_READ_4(sc, DOTG_GSNPSID);
	DPRINTF("Version = 0x%08x\n", temp);

	/* disconnect */
	DWC_OTG_WRITE_4(sc, DOTG_DCTL,
	    DCTL_SFTDISCON);

	/* wait for host to detect disconnect */
	usb_pause_mtx(&sc->sc_bus.bus_mtx, hz / 32);

	DWC_OTG_WRITE_4(sc, DOTG_GRSTCTL,
	    GRSTCTL_CSFTRST);

	/* wait a little bit for block to reset */
	usb_pause_mtx(&sc->sc_bus.bus_mtx, hz / 128);

	switch (sc->sc_mode) {
	case DWC_MODE_DEVICE:
		temp = GUSBCFG_FORCEDEVMODE;
		break;
	case DWC_MODE_HOST:
		temp = GUSBCFG_FORCEHOSTMODE;
		break;
	default:
		temp = 0;
		break;
	}

	/* select HSIC or non-HSIC mode */
	if (dwc_otg_use_hsic) {
		DWC_OTG_WRITE_4(sc, DOTG_GUSBCFG,
		    GUSBCFG_PHYIF |
		    GUSBCFG_TRD_TIM_SET(5) | temp);
		DWC_OTG_WRITE_4(sc, DOTG_GOTGCTL,
		    0x000000EC);

		temp = DWC_OTG_READ_4(sc, DOTG_GLPMCFG);
		DWC_OTG_WRITE_4(sc, DOTG_GLPMCFG,
		    temp & ~GLPMCFG_HSIC_CONN);
		DWC_OTG_WRITE_4(sc, DOTG_GLPMCFG,
		    temp | GLPMCFG_HSIC_CONN);
	} else {
		DWC_OTG_WRITE_4(sc, DOTG_GUSBCFG,
		    GUSBCFG_ULPI_UTMI_SEL |
		    GUSBCFG_TRD_TIM_SET(5) | temp);
		DWC_OTG_WRITE_4(sc, DOTG_GOTGCTL, 0);

		temp = DWC_OTG_READ_4(sc, DOTG_GLPMCFG);
		DWC_OTG_WRITE_4(sc, DOTG_GLPMCFG,
		    temp & ~GLPMCFG_HSIC_CONN);
	}

	/* clear global nak */
	DWC_OTG_WRITE_4(sc, DOTG_DCTL,
	    DCTL_CGOUTNAK |
	    DCTL_CGNPINNAK);

	/* disable USB port */
	DWC_OTG_WRITE_4(sc, DOTG_PCGCCTL, 0xFFFFFFFF);

	/* wait 10ms */
	usb_pause_mtx(&sc->sc_bus.bus_mtx, hz / 100);

	/* enable USB port */
	DWC_OTG_WRITE_4(sc, DOTG_PCGCCTL, 0);

	/* wait 10ms */
	usb_pause_mtx(&sc->sc_bus.bus_mtx, hz / 100);

	/* pull up D+ */
	dwc_otg_pull_up(sc);

	temp = DWC_OTG_READ_4(sc, DOTG_GHWCFG3);

	sc->sc_fifo_size = 4 * GHWCFG3_DFIFODEPTH_GET(temp);

	temp = DWC_OTG_READ_4(sc, DOTG_GHWCFG2);

	sc->sc_dev_ep_max = GHWCFG2_NUMDEVEPS_GET(temp);

	if (sc->sc_dev_ep_max > DWC_OTG_MAX_ENDPOINTS)
		sc->sc_dev_ep_max = DWC_OTG_MAX_ENDPOINTS;

	sc->sc_host_ch_max = GHWCFG2_NUMHSTCHNL_GET(temp);

	if (sc->sc_host_ch_max > DWC_OTG_MAX_CHANNELS)
		sc->sc_host_ch_max = DWC_OTG_MAX_CHANNELS;

	temp = DWC_OTG_READ_4(sc, DOTG_GHWCFG4);

	sc->sc_dev_in_ep_max = GHWCFG4_NUM_IN_EP_GET(temp);

	DPRINTF("Total FIFO size = %d bytes, Device EPs = %d/%d Host CHs = %d\n",
	    sc->sc_fifo_size, sc->sc_dev_ep_max, sc->sc_dev_in_ep_max,
	    sc->sc_host_ch_max);

	/* setup FIFO */
	if (dwc_otg_init_fifo(sc, DWC_MODE_OTG)) {
		USB_BUS_UNLOCK(&sc->sc_bus);
		return (EINVAL);
	}

	/* enable interrupts */
	sc->sc_irq_mask = DWC_OTG_MSK_GINT_ENABLED;
	DWC_OTG_WRITE_4(sc, DOTG_GINTMSK, sc->sc_irq_mask);

	if (sc->sc_mode == DWC_MODE_OTG || sc->sc_mode == DWC_MODE_DEVICE) {

		/* enable all endpoint interrupts */
		temp = DWC_OTG_READ_4(sc, DOTG_GHWCFG2);
		if (temp & GHWCFG2_MPI) {
			uint8_t x;

			DPRINTF("Multi Process Interrupts\n");

			for (x = 0; x != sc->sc_dev_in_ep_max; x++) {
				DWC_OTG_WRITE_4(sc, DOTG_DIEPEACHINTMSK(x),
				    DIEPMSK_XFERCOMPLMSK);
				DWC_OTG_WRITE_4(sc, DOTG_DOEPEACHINTMSK(x), 0);
			}
			DWC_OTG_WRITE_4(sc, DOTG_DEACHINTMSK, 0xFFFF);
		} else {
			DWC_OTG_WRITE_4(sc, DOTG_DIEPMSK,
			    DIEPMSK_XFERCOMPLMSK);
			DWC_OTG_WRITE_4(sc, DOTG_DOEPMSK, 0);
			DWC_OTG_WRITE_4(sc, DOTG_DAINTMSK, 0xFFFF);
		}
	}

	if (sc->sc_mode == DWC_MODE_OTG || sc->sc_mode == DWC_MODE_HOST) {
		/* setup clocks */
		temp = DWC_OTG_READ_4(sc, DOTG_HCFG);
		temp &= ~(HCFG_FSLSSUPP | HCFG_FSLSPCLKSEL_MASK);
		temp |= (1 << HCFG_FSLSPCLKSEL_SHIFT);
		DWC_OTG_WRITE_4(sc, DOTG_HCFG, temp);
	}

	/* only enable global IRQ */
	DWC_OTG_WRITE_4(sc, DOTG_GAHBCFG,
	    GAHBCFG_GLBLINTRMSK);

	/* turn off clocks */
	dwc_otg_clocks_off(sc);

	/* read initial VBUS state */

	temp = DWC_OTG_READ_4(sc, DOTG_GOTGCTL);

	DPRINTFN(5, "GOTCTL=0x%08x\n", temp);

	dwc_otg_vbus_interrupt(sc,
	    (temp & (GOTGCTL_ASESVLD | GOTGCTL_BSESVLD)) ? 1 : 0);

	USB_BUS_UNLOCK(&sc->sc_bus);

	/* catch any lost interrupts */

	dwc_otg_do_poll(&sc->sc_bus);

	return (0);			/* success */
}

void
dwc_otg_uninit(struct dwc_otg_softc *sc)
{
	USB_BUS_LOCK(&sc->sc_bus);

	/* stop host timer */
	dwc_otg_timer_stop(sc);

	/* set disconnect */
	DWC_OTG_WRITE_4(sc, DOTG_DCTL,
	    DCTL_SFTDISCON);

	/* turn off global IRQ */
	DWC_OTG_WRITE_4(sc, DOTG_GAHBCFG, 0);

	sc->sc_flags.port_enabled = 0;
	sc->sc_flags.port_powered = 0;
	sc->sc_flags.status_vbus = 0;
	sc->sc_flags.status_bus_reset = 0;
	sc->sc_flags.status_suspend = 0;
	sc->sc_flags.change_suspend = 0;
	sc->sc_flags.change_connect = 1;

	dwc_otg_pull_down(sc);
	dwc_otg_clocks_off(sc);

	USB_BUS_UNLOCK(&sc->sc_bus);

	usb_callout_drain(&sc->sc_timer);
}

static void
dwc_otg_suspend(struct dwc_otg_softc *sc)
{
	return;
}

static void
dwc_otg_resume(struct dwc_otg_softc *sc)
{
	return;
}

static void
dwc_otg_do_poll(struct usb_bus *bus)
{
	struct dwc_otg_softc *sc = DWC_OTG_BUS2SC(bus);

	USB_BUS_LOCK(&sc->sc_bus);
	USB_BUS_SPIN_LOCK(&sc->sc_bus);
	dwc_otg_interrupt_poll(sc);
	dwc_otg_interrupt_complete(sc);
	if (sc->sc_flags.status_device_mode == 0) {
		/* update host transfer schedule, so that new transfers can be issued */
		if (dwc_otg_update_host_transfer_schedule(sc))
			dwc_otg_interrupt_poll(sc);
	}
	USB_BUS_SPIN_UNLOCK(&sc->sc_bus);
	USB_BUS_UNLOCK(&sc->sc_bus);
}

/*------------------------------------------------------------------------*
 * DWC OTG bulk support
 * DWC OTG control support
 * DWC OTG interrupt support
 *------------------------------------------------------------------------*/
static void
dwc_otg_device_non_isoc_open(struct usb_xfer *xfer)
{
}

static void
dwc_otg_device_non_isoc_close(struct usb_xfer *xfer)
{
	dwc_otg_device_done(xfer, USB_ERR_CANCELLED);
}

static void
dwc_otg_device_non_isoc_enter(struct usb_xfer *xfer)
{
}

static void
dwc_otg_device_non_isoc_start(struct usb_xfer *xfer)
{
	/* setup TDs */
	dwc_otg_setup_standard_chain(xfer);
	dwc_otg_start_standard_chain(xfer);
}

struct usb_pipe_methods dwc_otg_device_non_isoc_methods =
{
	.open = dwc_otg_device_non_isoc_open,
	.close = dwc_otg_device_non_isoc_close,
	.enter = dwc_otg_device_non_isoc_enter,
	.start = dwc_otg_device_non_isoc_start,
};

/*------------------------------------------------------------------------*
 * DWC OTG full speed isochronous support
 *------------------------------------------------------------------------*/
static void
dwc_otg_device_isoc_open(struct usb_xfer *xfer)
{
}

static void
dwc_otg_device_isoc_close(struct usb_xfer *xfer)
{
	dwc_otg_device_done(xfer, USB_ERR_CANCELLED);
}

static void
dwc_otg_device_isoc_enter(struct usb_xfer *xfer)
{
}

static void
dwc_otg_device_isoc_start(struct usb_xfer *xfer)
{
	struct dwc_otg_softc *sc = DWC_OTG_BUS2SC(xfer->xroot->bus);
	uint32_t temp;
	uint32_t msframes;
	uint32_t framenum;
	uint8_t shift = usbd_xfer_get_fps_shift(xfer);

	DPRINTFN(6, "xfer=%p next=%d nframes=%d\n",
	    xfer, xfer->endpoint->isoc_next, xfer->nframes);

	if (xfer->xroot->udev->flags.usb_mode == USB_MODE_HOST) {
		temp = DWC_OTG_READ_4(sc, DOTG_HFNUM);

		/* get the current frame index */
		framenum = (temp & HFNUM_FRNUM_MASK);
	} else {
		temp = DWC_OTG_READ_4(sc, DOTG_DSTS);

		/* get the current frame index */
		framenum = DSTS_SOFFN_GET(temp);
	}

	if (xfer->xroot->udev->parent_hs_hub != NULL)
		framenum /= 8;

	framenum &= DWC_OTG_FRAME_MASK;

	/*
	 * Compute number of milliseconds worth of data traffic for
	 * this USB transfer:
	 */ 
	if (xfer->xroot->udev->speed == USB_SPEED_HIGH)
		msframes = ((xfer->nframes << shift) + 7) / 8;
	else
		msframes = xfer->nframes;

	/*
	 * check if the frame index is within the window where the frames
	 * will be inserted
	 */
	temp = (framenum - xfer->endpoint->isoc_next) & DWC_OTG_FRAME_MASK;

	if ((xfer->endpoint->is_synced == 0) || (temp < msframes)) {
		/*
		 * If there is data underflow or the pipe queue is
		 * empty we schedule the transfer a few frames ahead
		 * of the current frame position. Else two isochronous
		 * transfers might overlap.
		 */
		xfer->endpoint->isoc_next = (framenum + 3) & DWC_OTG_FRAME_MASK;
		xfer->endpoint->is_synced = 1;
		DPRINTFN(3, "start next=%d\n", xfer->endpoint->isoc_next);
	}
	/*
	 * compute how many milliseconds the insertion is ahead of the
	 * current frame position:
	 */
	temp = (xfer->endpoint->isoc_next - framenum) & DWC_OTG_FRAME_MASK;

	/*
	 * pre-compute when the isochronous transfer will be finished:
	 */
	xfer->isoc_time_complete =
		usb_isoc_time_expand(&sc->sc_bus, framenum) + temp + msframes;

	/* setup TDs */
	dwc_otg_setup_standard_chain(xfer);

	/* compute frame number for next insertion */
	xfer->endpoint->isoc_next += msframes;

	/* start TD chain */
	dwc_otg_start_standard_chain(xfer);
}

struct usb_pipe_methods dwc_otg_device_isoc_methods =
{
	.open = dwc_otg_device_isoc_open,
	.close = dwc_otg_device_isoc_close,
	.enter = dwc_otg_device_isoc_enter,
	.start = dwc_otg_device_isoc_start,
};

/*------------------------------------------------------------------------*
 * DWC OTG root control support
 *------------------------------------------------------------------------*
 * Simulate a hardware HUB by handling all the necessary requests.
 *------------------------------------------------------------------------*/

static const struct usb_device_descriptor dwc_otg_devd = {
	.bLength = sizeof(struct usb_device_descriptor),
	.bDescriptorType = UDESC_DEVICE,
	.bcdUSB = {0x00, 0x02},
	.bDeviceClass = UDCLASS_HUB,
	.bDeviceSubClass = UDSUBCLASS_HUB,
	.bDeviceProtocol = UDPROTO_HSHUBSTT,
	.bMaxPacketSize = 64,
	.bcdDevice = {0x00, 0x01},
	.iManufacturer = 1,
	.iProduct = 2,
	.bNumConfigurations = 1,
};

static const struct dwc_otg_config_desc dwc_otg_confd = {
	.confd = {
		.bLength = sizeof(struct usb_config_descriptor),
		.bDescriptorType = UDESC_CONFIG,
		.wTotalLength[0] = sizeof(dwc_otg_confd),
		.bNumInterface = 1,
		.bConfigurationValue = 1,
		.iConfiguration = 0,
		.bmAttributes = UC_SELF_POWERED,
		.bMaxPower = 0,
	},
	.ifcd = {
		.bLength = sizeof(struct usb_interface_descriptor),
		.bDescriptorType = UDESC_INTERFACE,
		.bNumEndpoints = 1,
		.bInterfaceClass = UICLASS_HUB,
		.bInterfaceSubClass = UISUBCLASS_HUB,
		.bInterfaceProtocol = 0,
	},
	.endpd = {
		.bLength = sizeof(struct usb_endpoint_descriptor),
		.bDescriptorType = UDESC_ENDPOINT,
		.bEndpointAddress = (UE_DIR_IN | DWC_OTG_INTR_ENDPT),
		.bmAttributes = UE_INTERRUPT,
		.wMaxPacketSize[0] = 8,
		.bInterval = 255,
	},
};

#define	HSETW(ptr, val) ptr = { (uint8_t)(val), (uint8_t)((val) >> 8) }

static const struct usb_hub_descriptor_min dwc_otg_hubd = {
	.bDescLength = sizeof(dwc_otg_hubd),
	.bDescriptorType = UDESC_HUB,
	.bNbrPorts = 1,
	HSETW(.wHubCharacteristics, (UHD_PWR_NO_SWITCH | UHD_OC_INDIVIDUAL)),
	.bPwrOn2PwrGood = 50,
	.bHubContrCurrent = 0,
	.DeviceRemovable = {0},		/* port is removable */
};

#define	STRING_VENDOR \
  "D\0W\0C\0O\0T\0G"

#define	STRING_PRODUCT \
  "O\0T\0G\0 \0R\0o\0o\0t\0 \0H\0U\0B"

USB_MAKE_STRING_DESC(STRING_VENDOR, dwc_otg_vendor);
USB_MAKE_STRING_DESC(STRING_PRODUCT, dwc_otg_product);

static usb_error_t
dwc_otg_roothub_exec(struct usb_device *udev,
    struct usb_device_request *req, const void **pptr, uint16_t *plength)
{
	struct dwc_otg_softc *sc = DWC_OTG_BUS2SC(udev->bus);
	const void *ptr;
	uint16_t len;
	uint16_t value;
	uint16_t index;
	usb_error_t err;

	USB_BUS_LOCK_ASSERT(&sc->sc_bus, MA_OWNED);

	/* buffer reset */
	ptr = (const void *)&sc->sc_hub_temp;
	len = 0;
	err = 0;

	value = UGETW(req->wValue);
	index = UGETW(req->wIndex);

	/* demultiplex the control request */

	switch (req->bmRequestType) {
	case UT_READ_DEVICE:
		switch (req->bRequest) {
		case UR_GET_DESCRIPTOR:
			goto tr_handle_get_descriptor;
		case UR_GET_CONFIG:
			goto tr_handle_get_config;
		case UR_GET_STATUS:
			goto tr_handle_get_status;
		default:
			goto tr_stalled;
		}
		break;

	case UT_WRITE_DEVICE:
		switch (req->bRequest) {
		case UR_SET_ADDRESS:
			goto tr_handle_set_address;
		case UR_SET_CONFIG:
			goto tr_handle_set_config;
		case UR_CLEAR_FEATURE:
			goto tr_valid;	/* nop */
		case UR_SET_DESCRIPTOR:
			goto tr_valid;	/* nop */
		case UR_SET_FEATURE:
		default:
			goto tr_stalled;
		}
		break;

	case UT_WRITE_ENDPOINT:
		switch (req->bRequest) {
		case UR_CLEAR_FEATURE:
			switch (UGETW(req->wValue)) {
			case UF_ENDPOINT_HALT:
				goto tr_handle_clear_halt;
			case UF_DEVICE_REMOTE_WAKEUP:
				goto tr_handle_clear_wakeup;
			default:
				goto tr_stalled;
			}
			break;
		case UR_SET_FEATURE:
			switch (UGETW(req->wValue)) {
			case UF_ENDPOINT_HALT:
				goto tr_handle_set_halt;
			case UF_DEVICE_REMOTE_WAKEUP:
				goto tr_handle_set_wakeup;
			default:
				goto tr_stalled;
			}
			break;
		case UR_SYNCH_FRAME:
			goto tr_valid;	/* nop */
		default:
			goto tr_stalled;
		}
		break;

	case UT_READ_ENDPOINT:
		switch (req->bRequest) {
		case UR_GET_STATUS:
			goto tr_handle_get_ep_status;
		default:
			goto tr_stalled;
		}
		break;

	case UT_WRITE_INTERFACE:
		switch (req->bRequest) {
		case UR_SET_INTERFACE:
			goto tr_handle_set_interface;
		case UR_CLEAR_FEATURE:
			goto tr_valid;	/* nop */
		case UR_SET_FEATURE:
		default:
			goto tr_stalled;
		}
		break;

	case UT_READ_INTERFACE:
		switch (req->bRequest) {
		case UR_GET_INTERFACE:
			goto tr_handle_get_interface;
		case UR_GET_STATUS:
			goto tr_handle_get_iface_status;
		default:
			goto tr_stalled;
		}
		break;

	case UT_WRITE_CLASS_INTERFACE:
	case UT_WRITE_VENDOR_INTERFACE:
		/* XXX forward */
		break;

	case UT_READ_CLASS_INTERFACE:
	case UT_READ_VENDOR_INTERFACE:
		/* XXX forward */
		break;

	case UT_WRITE_CLASS_DEVICE:
		switch (req->bRequest) {
		case UR_CLEAR_FEATURE:
			goto tr_valid;
		case UR_SET_DESCRIPTOR:
		case UR_SET_FEATURE:
			break;
		default:
			goto tr_stalled;
		}
		break;

	case UT_WRITE_CLASS_OTHER:
		switch (req->bRequest) {
		case UR_CLEAR_FEATURE:
			goto tr_handle_clear_port_feature;
		case UR_SET_FEATURE:
			goto tr_handle_set_port_feature;
		case UR_CLEAR_TT_BUFFER:
		case UR_RESET_TT:
		case UR_STOP_TT:
			goto tr_valid;

		default:
			goto tr_stalled;
		}
		break;

	case UT_READ_CLASS_OTHER:
		switch (req->bRequest) {
		case UR_GET_TT_STATE:
			goto tr_handle_get_tt_state;
		case UR_GET_STATUS:
			goto tr_handle_get_port_status;
		default:
			goto tr_stalled;
		}
		break;

	case UT_READ_CLASS_DEVICE:
		switch (req->bRequest) {
		case UR_GET_DESCRIPTOR:
			goto tr_handle_get_class_descriptor;
		case UR_GET_STATUS:
			goto tr_handle_get_class_status;

		default:
			goto tr_stalled;
		}
		break;
	default:
		goto tr_stalled;
	}
	goto tr_valid;

tr_handle_get_descriptor:
	switch (value >> 8) {
	case UDESC_DEVICE:
		if (value & 0xff) {
			goto tr_stalled;
		}
		len = sizeof(dwc_otg_devd);
		ptr = (const void *)&dwc_otg_devd;
		goto tr_valid;
	case UDESC_CONFIG:
		if (value & 0xff) {
			goto tr_stalled;
		}
		len = sizeof(dwc_otg_confd);
		ptr = (const void *)&dwc_otg_confd;
		goto tr_valid;
	case UDESC_STRING:
		switch (value & 0xff) {
		case 0:		/* Language table */
			len = sizeof(usb_string_lang_en);
			ptr = (const void *)&usb_string_lang_en;
			goto tr_valid;

		case 1:		/* Vendor */
			len = sizeof(dwc_otg_vendor);
			ptr = (const void *)&dwc_otg_vendor;
			goto tr_valid;

		case 2:		/* Product */
			len = sizeof(dwc_otg_product);
			ptr = (const void *)&dwc_otg_product;
			goto tr_valid;
		default:
			break;
		}
		break;
	default:
		goto tr_stalled;
	}
	goto tr_stalled;

tr_handle_get_config:
	len = 1;
	sc->sc_hub_temp.wValue[0] = sc->sc_conf;
	goto tr_valid;

tr_handle_get_status:
	len = 2;
	USETW(sc->sc_hub_temp.wValue, UDS_SELF_POWERED);
	goto tr_valid;

tr_handle_set_address:
	if (value & 0xFF00) {
		goto tr_stalled;
	}
	sc->sc_rt_addr = value;
	goto tr_valid;

tr_handle_set_config:
	if (value >= 2) {
		goto tr_stalled;
	}
	sc->sc_conf = value;
	goto tr_valid;

tr_handle_get_interface:
	len = 1;
	sc->sc_hub_temp.wValue[0] = 0;
	goto tr_valid;

tr_handle_get_tt_state:
tr_handle_get_class_status:
tr_handle_get_iface_status:
tr_handle_get_ep_status:
	len = 2;
	USETW(sc->sc_hub_temp.wValue, 0);
	goto tr_valid;

tr_handle_set_halt:
tr_handle_set_interface:
tr_handle_set_wakeup:
tr_handle_clear_wakeup:
tr_handle_clear_halt:
	goto tr_valid;

tr_handle_clear_port_feature:
	if (index != 1)
		goto tr_stalled;

	DPRINTFN(9, "UR_CLEAR_PORT_FEATURE on port %d\n", index);

	switch (value) {
	case UHF_PORT_SUSPEND:
		dwc_otg_wakeup_peer(sc);
		break;

	case UHF_PORT_ENABLE:
		if (sc->sc_flags.status_device_mode == 0) {
			DWC_OTG_WRITE_4(sc, DOTG_HPRT,
			    sc->sc_hprt_val | HPRT_PRTENA);
		}
		sc->sc_flags.port_enabled = 0;
		break;

	case UHF_C_PORT_RESET:
		sc->sc_flags.change_reset = 0;
		break;

	case UHF_C_PORT_ENABLE:
		sc->sc_flags.change_enabled = 0;
		break;

	case UHF_C_PORT_OVER_CURRENT:
		sc->sc_flags.change_over_current = 0;
		break;

	case UHF_PORT_TEST:
	case UHF_PORT_INDICATOR:
		/* nops */
		break;

	case UHF_PORT_POWER:
		sc->sc_flags.port_powered = 0;
		if (sc->sc_mode == DWC_MODE_HOST || sc->sc_mode == DWC_MODE_OTG) {
			sc->sc_hprt_val = 0;
			DWC_OTG_WRITE_4(sc, DOTG_HPRT, HPRT_PRTENA);
		}
		dwc_otg_pull_down(sc);
		dwc_otg_clocks_off(sc);
		break;

	case UHF_C_PORT_CONNECTION:
		/* clear connect change flag */
		sc->sc_flags.change_connect = 0;
		break;

	case UHF_C_PORT_SUSPEND:
		sc->sc_flags.change_suspend = 0;
		break;

	default:
		err = USB_ERR_IOERROR;
		goto done;
	}
	goto tr_valid;

tr_handle_set_port_feature:
	if (index != 1) {
		goto tr_stalled;
	}
	DPRINTFN(9, "UR_SET_PORT_FEATURE\n");

	switch (value) {
	case UHF_PORT_ENABLE:
		break;

	case UHF_PORT_SUSPEND:
		if (sc->sc_flags.status_device_mode == 0) {
			/* set suspend BIT */
			sc->sc_hprt_val |= HPRT_PRTSUSP;
			DWC_OTG_WRITE_4(sc, DOTG_HPRT, sc->sc_hprt_val);

			/* generate HUB suspend event */
			dwc_otg_suspend_irq(sc);
		}
		break;

	case UHF_PORT_RESET:
		if (sc->sc_flags.status_device_mode == 0) {

			DPRINTF("PORT RESET\n");

			/* enable PORT reset */
			DWC_OTG_WRITE_4(sc, DOTG_HPRT, sc->sc_hprt_val | HPRT_PRTRST);

			/* Wait 62.5ms for reset to complete */
			usb_pause_mtx(&sc->sc_bus.bus_mtx, hz / 16);

			DWC_OTG_WRITE_4(sc, DOTG_HPRT, sc->sc_hprt_val);

			/* Wait 62.5ms for reset to complete */
			usb_pause_mtx(&sc->sc_bus.bus_mtx, hz / 16);

			/* reset FIFOs */
			(void) dwc_otg_init_fifo(sc, DWC_MODE_HOST);

			sc->sc_flags.change_reset = 1;
		} else {
			err = USB_ERR_IOERROR;
		}
		break;

	case UHF_PORT_TEST:
	case UHF_PORT_INDICATOR:
		/* nops */
		break;
	case UHF_PORT_POWER:
		if (sc->sc_mode == DWC_MODE_HOST || sc->sc_mode == DWC_MODE_OTG) {
			sc->sc_hprt_val |= HPRT_PRTPWR;
			DWC_OTG_WRITE_4(sc, DOTG_HPRT, sc->sc_hprt_val);
		}
		sc->sc_flags.port_powered = 1;
		break;
	default:
		err = USB_ERR_IOERROR;
		goto done;
	}
	goto tr_valid;

tr_handle_get_port_status:

	DPRINTFN(9, "UR_GET_PORT_STATUS\n");

	if (index != 1)
		goto tr_stalled;

	if (sc->sc_flags.status_vbus)
		dwc_otg_clocks_on(sc);
	else
		dwc_otg_clocks_off(sc);

	/* Select Device Side Mode */

	if (sc->sc_flags.status_device_mode) {
		value = UPS_PORT_MODE_DEVICE;
		dwc_otg_timer_stop(sc);
	} else {
		value = 0;
		dwc_otg_timer_start(sc);
	}

	if (sc->sc_flags.status_high_speed)
		value |= UPS_HIGH_SPEED;
	else if (sc->sc_flags.status_low_speed)
		value |= UPS_LOW_SPEED;

	if (sc->sc_flags.port_powered)
		value |= UPS_PORT_POWER;

	if (sc->sc_flags.port_enabled)
		value |= UPS_PORT_ENABLED;

	if (sc->sc_flags.port_over_current)
		value |= UPS_OVERCURRENT_INDICATOR;

	if (sc->sc_flags.status_vbus &&
	    sc->sc_flags.status_bus_reset)
		value |= UPS_CURRENT_CONNECT_STATUS;

	if (sc->sc_flags.status_suspend)
		value |= UPS_SUSPEND;

	USETW(sc->sc_hub_temp.ps.wPortStatus, value);

	value = 0;

	if (sc->sc_flags.change_connect)
		value |= UPS_C_CONNECT_STATUS;
	if (sc->sc_flags.change_suspend)
		value |= UPS_C_SUSPEND;
	if (sc->sc_flags.change_reset)
		value |= UPS_C_PORT_RESET;
	if (sc->sc_flags.change_over_current)
		value |= UPS_C_OVERCURRENT_INDICATOR;

	USETW(sc->sc_hub_temp.ps.wPortChange, value);
	len = sizeof(sc->sc_hub_temp.ps);
	goto tr_valid;

tr_handle_get_class_descriptor:
	if (value & 0xFF) {
		goto tr_stalled;
	}
	ptr = (const void *)&dwc_otg_hubd;
	len = sizeof(dwc_otg_hubd);
	goto tr_valid;

tr_stalled:
	err = USB_ERR_STALLED;
tr_valid:
done:
	*plength = len;
	*pptr = ptr;
	return (err);
}

static void
dwc_otg_xfer_setup(struct usb_setup_params *parm)
{
	struct usb_xfer *xfer;
	void *last_obj;
	uint32_t ntd;
	uint32_t n;
	uint8_t ep_no;
	uint8_t ep_type;

	xfer = parm->curr_xfer;

	/*
	 * NOTE: This driver does not use any of the parameters that
	 * are computed from the following values. Just set some
	 * reasonable dummies:
	 */
	parm->hc_max_packet_size = 0x500;
	parm->hc_max_packet_count = 3;
	parm->hc_max_frame_size = 3 * 0x500;

	usbd_transfer_setup_sub(parm);

	/*
	 * compute maximum number of TDs
	 */
	ep_type = (xfer->endpoint->edesc->bmAttributes & UE_XFERTYPE);

	if (ep_type == UE_CONTROL) {

		ntd = xfer->nframes + 1 /* STATUS */ + 1 /* SYNC 1 */
		    + 1 /* SYNC 2 */ + 1 /* SYNC 3 */;
	} else {

		ntd = xfer->nframes + 1 /* SYNC */ ;
	}

	/*
	 * check if "usbd_transfer_setup_sub" set an error
	 */
	if (parm->err)
		return;

	/*
	 * allocate transfer descriptors
	 */
	last_obj = NULL;

	ep_no = xfer->endpointno & UE_ADDR;

	/*
	 * Check for a valid endpoint profile in USB device mode:
	 */
	if (xfer->flags_int.usb_mode == USB_MODE_DEVICE) {
		const struct usb_hw_ep_profile *pf;

		dwc_otg_get_hw_ep_profile(parm->udev, &pf, ep_no);

		if (pf == NULL) {
			/* should not happen */
			parm->err = USB_ERR_INVAL;
			return;
		}
	}

	/* align data */
	parm->size[0] += ((-parm->size[0]) & (USB_HOST_ALIGN - 1));

	for (n = 0; n != ntd; n++) {

		struct dwc_otg_td *td;

		if (parm->buf) {

			td = USB_ADD_BYTES(parm->buf, parm->size[0]);

			/* init TD */
			td->max_packet_size = xfer->max_packet_size;
			td->max_packet_count = xfer->max_packet_count;
			td->ep_no = ep_no;
			td->ep_type = ep_type;
			td->obj_next = last_obj;

			last_obj = td;
		}
		parm->size[0] += sizeof(*td);
	}

	xfer->td_start[0] = last_obj;
}

static void
dwc_otg_xfer_unsetup(struct usb_xfer *xfer)
{
	return;
}

static void
dwc_otg_ep_init(struct usb_device *udev, struct usb_endpoint_descriptor *edesc,
    struct usb_endpoint *ep)
{
	struct dwc_otg_softc *sc = DWC_OTG_BUS2SC(udev->bus);

	DPRINTFN(2, "endpoint=%p, addr=%d, endpt=%d, mode=%d (%d,%d)\n",
	    ep, udev->address,
	    edesc->bEndpointAddress, udev->flags.usb_mode,
	    sc->sc_rt_addr, udev->device_index);

	if (udev->device_index != sc->sc_rt_addr) {

		if (udev->flags.usb_mode == USB_MODE_DEVICE) {
			if (udev->speed != USB_SPEED_FULL &&
			    udev->speed != USB_SPEED_HIGH) {
				/* not supported */
				return;
			}
		} else {
			if (udev->speed == USB_SPEED_HIGH) {
				if ((UGETW(edesc->wMaxPacketSize) >> 11) & 3) {
					/* high bandwidth endpoint - not tested */
					DPRINTF("High Bandwidth Endpoint - not tested\n");
					return;
				}
			}
		}
		if ((edesc->bmAttributes & UE_XFERTYPE) == UE_ISOCHRONOUS)
			ep->methods = &dwc_otg_device_isoc_methods;
		else
			ep->methods = &dwc_otg_device_non_isoc_methods;
	}
}

static void
dwc_otg_set_hw_power_sleep(struct usb_bus *bus, uint32_t state)
{
	struct dwc_otg_softc *sc = DWC_OTG_BUS2SC(bus);

	switch (state) {
	case USB_HW_POWER_SUSPEND:
		dwc_otg_suspend(sc);
		break;
	case USB_HW_POWER_SHUTDOWN:
		dwc_otg_uninit(sc);
		break;
	case USB_HW_POWER_RESUME:
		dwc_otg_resume(sc);
		break;
	default:
		break;
	}
}

static void
dwc_otg_get_dma_delay(struct usb_device *udev, uint32_t *pus)
{
	/* DMA delay - wait until any use of memory is finished */
	*pus = (2125);			/* microseconds */
}

static void
dwc_otg_device_resume(struct usb_device *udev)
{
	DPRINTF("\n");

	/* poll all transfers again to restart resumed ones */
	dwc_otg_do_poll(udev->bus);
}

static void
dwc_otg_device_suspend(struct usb_device *udev)
{
	DPRINTF("\n");
}

struct usb_bus_methods dwc_otg_bus_methods =
{
	.endpoint_init = &dwc_otg_ep_init,
	.xfer_setup = &dwc_otg_xfer_setup,
	.xfer_unsetup = &dwc_otg_xfer_unsetup,
	.get_hw_ep_profile = &dwc_otg_get_hw_ep_profile,
	.xfer_stall = &dwc_otg_xfer_stall,
	.set_stall = &dwc_otg_set_stall,
	.clear_stall = &dwc_otg_clear_stall,
	.roothub_exec = &dwc_otg_roothub_exec,
	.xfer_poll = &dwc_otg_do_poll,
	.device_state_change = &dwc_otg_device_state_change,
	.set_hw_power_sleep = &dwc_otg_set_hw_power_sleep,
	.get_dma_delay = &dwc_otg_get_dma_delay,
	.device_resume = &dwc_otg_device_resume,
	.device_suspend = &dwc_otg_device_suspend,
};
