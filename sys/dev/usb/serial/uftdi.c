/*	$NetBSD: uftdi.c,v 1.13 2002/09/23 05:51:23 simonb Exp $	*/

/*-
 * Copyright (c) 2000 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net).
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * NOTE: all function names beginning like "uftdi_cfg_" can only
 * be called from within the config thread function !
 */

/*
 * FTDI FT232x, FT2232x, FT4232x, FT8U100AX and FT8U232xM serial adapters.
 *
 * Note that we specifically do not do a reset or otherwise alter the state of
 * the chip during attach, detach, open, and close, because it could be
 * pre-initialized (via an attached serial eeprom) to power-on into a mode such
 * as bitbang in which the pins are being driven to a specific state which we
 * must not perturb.  The device gets reset at power-on, and doesn't need to be
 * reset again after that to function, except as directed by ioctl() calls.
 */

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
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_ioctl.h>
#include "usbdevs.h"

#define	USB_DEBUG_VAR uftdi_debug
#include <dev/usb/usb_debug.h>
#include <dev/usb/usb_process.h>

#include <dev/usb/serial/usb_serial.h>
#include <dev/usb/serial/uftdi_reg.h>
#include <dev/usb/uftdiio.h>

static SYSCTL_NODE(_hw_usb, OID_AUTO, uftdi, CTLFLAG_RW, 0, "USB uftdi");

#ifdef USB_DEBUG
static int uftdi_debug = 0;
SYSCTL_INT(_hw_usb_uftdi, OID_AUTO, debug, CTLFLAG_RW,
    &uftdi_debug, 0, "Debug level");
#endif

#define	UFTDI_CONFIG_INDEX	0

/*
 * IO buffer sizes and FTDI device procotol sizes.
 *
 * Note that the output packet size in the following defines is not the usb
 * protocol packet size based on bus speed, it is the size dictated by the FTDI
 * device itself, and is used only on older chips.
 *
 * We allocate buffers bigger than the hardware's packet size, and process
 * multiple packets within each buffer.  This allows the controller to make
 * optimal use of the usb bus by conducting multiple transfers with the device
 * during a single bus timeslice to fill or drain the chip's fifos.
 *
 * The output data on newer chips has no packet header, and we are able to pack
 * any number of output bytes into a buffer.  On some older chips, each output
 * packet contains a 1-byte header and up to 63 bytes of payload.  The size is
 * encoded in 6 bits of the header, hence the 64-byte limit on packet size.  We
 * loop to fill the buffer with many of these header+payload packets.
 *
 * The input data on all chips consists of packets which contain a 2-byte header
 * followed by data payload.  The total size of the packet is wMaxPacketSize
 * which can change based on the bus speed (e.g., 64 for full speed, 512 for
 * high speed).  We loop to extract the headers and payloads from the packets
 * packed into an input buffer.
 */
#define	UFTDI_IBUFSIZE	2048
#define	UFTDI_IHDRSIZE	   2
#define	UFTDI_OBUFSIZE	2048
#define	UFTDI_OPKTSIZE	  64

enum {
	UFTDI_BULK_DT_WR,
	UFTDI_BULK_DT_RD,
	UFTDI_N_TRANSFER,
};

enum {
	DEVT_SIO,
	DEVT_232A,
	DEVT_232B,
	DEVT_2232D,	/* Includes 2232C */
	DEVT_232R,
	DEVT_2232H,
	DEVT_4232H,
	DEVT_232H,
	DEVT_230X,
};

#define	DEVF_BAUDBITS_HINDEX	0x01	/* Baud bits in high byte of index. */
#define	DEVF_BAUDCLK_12M	0X02	/* Base baud clock is 12MHz. */

struct uftdi_softc {
	struct ucom_super_softc sc_super_ucom;
	struct ucom_softc sc_ucom;

	struct usb_device *sc_udev;
	struct usb_xfer *sc_xfer[UFTDI_N_TRANSFER];
	device_t sc_dev;
	struct mtx sc_mtx;

	uint32_t sc_unit;

	uint16_t sc_last_lcr;
	uint16_t sc_bcdDevice;

	uint8_t sc_devtype;
	uint8_t sc_devflags;
	uint8_t	sc_hdrlen;
	uint8_t	sc_msr;
	uint8_t	sc_lsr;
};

struct uftdi_param_config {
	uint16_t baud_lobits;
	uint16_t baud_hibits;
	uint16_t lcr;
	uint8_t	v_start;
	uint8_t	v_stop;
	uint8_t	v_flow;
};

/* prototypes */

static device_probe_t uftdi_probe;
static device_attach_t uftdi_attach;
static device_detach_t uftdi_detach;
static void uftdi_free_softc(struct uftdi_softc *);

static usb_callback_t uftdi_write_callback;
static usb_callback_t uftdi_read_callback;

static void	uftdi_free(struct ucom_softc *);
static void	uftdi_cfg_open(struct ucom_softc *);
static void	uftdi_cfg_close(struct ucom_softc *);
static void	uftdi_cfg_set_dtr(struct ucom_softc *, uint8_t);
static void	uftdi_cfg_set_rts(struct ucom_softc *, uint8_t);
static void	uftdi_cfg_set_break(struct ucom_softc *, uint8_t);
static int	uftdi_set_parm_soft(struct ucom_softc *, struct termios *,
		    struct uftdi_param_config *);
static int	uftdi_pre_param(struct ucom_softc *, struct termios *);
static void	uftdi_cfg_param(struct ucom_softc *, struct termios *);
static void	uftdi_cfg_get_status(struct ucom_softc *, uint8_t *,
		    uint8_t *);
static int	uftdi_reset(struct ucom_softc *, int);
static int	uftdi_set_bitmode(struct ucom_softc *, uint8_t, uint8_t);
static int	uftdi_get_bitmode(struct ucom_softc *, uint8_t *);
static int	uftdi_set_latency(struct ucom_softc *, int);
static int	uftdi_get_latency(struct ucom_softc *, int *);
static int	uftdi_set_event_char(struct ucom_softc *, int);
static int	uftdi_set_error_char(struct ucom_softc *, int);
static int	uftdi_ioctl(struct ucom_softc *, uint32_t, caddr_t, int,
		    struct thread *);
static void	uftdi_start_read(struct ucom_softc *);
static void	uftdi_stop_read(struct ucom_softc *);
static void	uftdi_start_write(struct ucom_softc *);
static void	uftdi_stop_write(struct ucom_softc *);
static void	uftdi_poll(struct ucom_softc *ucom);

static const struct usb_config uftdi_config[UFTDI_N_TRANSFER] = {

	[UFTDI_BULK_DT_WR] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_OUT,
		.bufsize = UFTDI_OBUFSIZE,
		.flags = {.pipe_bof = 1,},
		.callback = &uftdi_write_callback,
	},

	[UFTDI_BULK_DT_RD] = {
		.type = UE_BULK,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_IN,
		.bufsize = UFTDI_IBUFSIZE,
		.flags = {.pipe_bof = 1,.short_xfer_ok = 1,},
		.callback = &uftdi_read_callback,
	},
};

static const struct ucom_callback uftdi_callback = {
	.ucom_cfg_get_status = &uftdi_cfg_get_status,
	.ucom_cfg_set_dtr = &uftdi_cfg_set_dtr,
	.ucom_cfg_set_rts = &uftdi_cfg_set_rts,
	.ucom_cfg_set_break = &uftdi_cfg_set_break,
	.ucom_cfg_param = &uftdi_cfg_param,
	.ucom_cfg_open = &uftdi_cfg_open,
	.ucom_cfg_close = &uftdi_cfg_close,
	.ucom_pre_param = &uftdi_pre_param,
	.ucom_ioctl = &uftdi_ioctl,
	.ucom_start_read = &uftdi_start_read,
	.ucom_stop_read = &uftdi_stop_read,
	.ucom_start_write = &uftdi_start_write,
	.ucom_stop_write = &uftdi_stop_write,
	.ucom_poll = &uftdi_poll,
	.ucom_free = &uftdi_free,
};

static device_method_t uftdi_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe, uftdi_probe),
	DEVMETHOD(device_attach, uftdi_attach),
	DEVMETHOD(device_detach, uftdi_detach),
	DEVMETHOD_END
};

static devclass_t uftdi_devclass;

static driver_t uftdi_driver = {
	.name = "uftdi",
	.methods = uftdi_methods,
	.size = sizeof(struct uftdi_softc),
};

DRIVER_MODULE(uftdi, uhub, uftdi_driver, uftdi_devclass, NULL, NULL);
MODULE_DEPEND(uftdi, ucom, 1, 1, 1);
MODULE_DEPEND(uftdi, usb, 1, 1, 1);
MODULE_VERSION(uftdi, 1);

static const STRUCT_USB_HOST_ID uftdi_devs[] = {
#define	UFTDI_DEV(v, p, i) \
  { USB_VPI(USB_VENDOR_##v, USB_PRODUCT_##v##_##p, i) }
	UFTDI_DEV(ACTON, SPECTRAPRO, 0),
	UFTDI_DEV(ALTI2, N3, 0),
	UFTDI_DEV(ANALOGDEVICES, GNICE, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(ANALOGDEVICES, GNICEPLUS, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(ATMEL, STK541, 0),
	UFTDI_DEV(BAYER, CONTOUR_CABLE, 0),
	UFTDI_DEV(BBELECTRONICS, 232USB9M, 0),
	UFTDI_DEV(BBELECTRONICS, 485USB9F_2W, 0),
	UFTDI_DEV(BBELECTRONICS, 485USB9F_4W, 0),
	UFTDI_DEV(BBELECTRONICS, 485USBTB_2W, 0),
	UFTDI_DEV(BBELECTRONICS, 485USBTB_4W, 0),
	UFTDI_DEV(BBELECTRONICS, TTL3USB9M, 0),
	UFTDI_DEV(BBELECTRONICS, TTL5USB9M, 0),
	UFTDI_DEV(BBELECTRONICS, USO9ML2, 0),
	UFTDI_DEV(BBELECTRONICS, USO9ML2DR, 0),
	UFTDI_DEV(BBELECTRONICS, USO9ML2DR_2, 0),
	UFTDI_DEV(BBELECTRONICS, USOPTL4, 0),
	UFTDI_DEV(BBELECTRONICS, USOPTL4DR, 0),
	UFTDI_DEV(BBELECTRONICS, USOPTL4DR2, 0),
	UFTDI_DEV(BBELECTRONICS, USOTL4, 0),
	UFTDI_DEV(BBELECTRONICS, USPTL4, 0),
	UFTDI_DEV(BBELECTRONICS, USTL4, 0),
	UFTDI_DEV(BBELECTRONICS, ZZ_PROG1_USB, 0),
	UFTDI_DEV(CONTEC, COM1USBH, 0),
	UFTDI_DEV(DRESDENELEKTRONIK, SENSORTERMINALBOARD, 0),
	UFTDI_DEV(DRESDENELEKTRONIK, WIRELESSHANDHELDTERMINAL, 0),
	UFTDI_DEV(DRESDENELEKTRONIK, DE_RFNODE, 0),
	UFTDI_DEV(DRESDENELEKTRONIK, LEVELSHIFTERSTICKLOWCOST, 0),
	UFTDI_DEV(ELEKTOR, FT323R, 0),
	UFTDI_DEV(EVOLUTION, ER1, 0),
	UFTDI_DEV(EVOLUTION, HYBRID, 0),
	UFTDI_DEV(EVOLUTION, RCM4, 0),
	UFTDI_DEV(FALCOM, SAMBA, 0),
	UFTDI_DEV(FALCOM, TWIST, 0),
	UFTDI_DEV(FIC, NEO1973_DEBUG, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FIC, NEO1973_DEBUG, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, 232EX, 0),
	UFTDI_DEV(FTDI, 232H, 0),
	UFTDI_DEV(FTDI, 232RL, 0),
	UFTDI_DEV(FTDI, 4N_GALAXY_DE_1, 0),
	UFTDI_DEV(FTDI, 4N_GALAXY_DE_2, 0),
	UFTDI_DEV(FTDI, 4N_GALAXY_DE_3, 0),
	UFTDI_DEV(FTDI, 8U232AM_ALT, 0),
	UFTDI_DEV(FTDI, ACCESSO, 0),
	UFTDI_DEV(FTDI, ACG_HFDUAL, 0),
	UFTDI_DEV(FTDI, ACTIVE_ROBOTS, 0),
	UFTDI_DEV(FTDI, ACTZWAVE, 0),
	UFTDI_DEV(FTDI, AMC232, 0),
	UFTDI_DEV(FTDI, ARTEMIS, 0),
	UFTDI_DEV(FTDI, ASK_RDR400, 0),
	UFTDI_DEV(FTDI, ATIK_ATK16, 0),
	UFTDI_DEV(FTDI, ATIK_ATK16C, 0),
	UFTDI_DEV(FTDI, ATIK_ATK16HR, 0),
	UFTDI_DEV(FTDI, ATIK_ATK16HRC, 0),
	UFTDI_DEV(FTDI, ATIK_ATK16IC, 0),
	UFTDI_DEV(FTDI, BCS_SE923, 0),
	UFTDI_DEV(FTDI, CANDAPTER, 0),
	UFTDI_DEV(FTDI, CANUSB, 0),
	UFTDI_DEV(FTDI, CCSICDU20_0, 0),
	UFTDI_DEV(FTDI, CCSICDU40_1, 0),
	UFTDI_DEV(FTDI, CCSICDU64_4, 0),
	UFTDI_DEV(FTDI, CCSLOAD_N_GO_3, 0),
	UFTDI_DEV(FTDI, CCSMACHX_2, 0),
	UFTDI_DEV(FTDI, CCSPRIME8_5, 0),
	UFTDI_DEV(FTDI, CFA_631, 0),
	UFTDI_DEV(FTDI, CFA_632, 0),
	UFTDI_DEV(FTDI, CFA_633, 0),
	UFTDI_DEV(FTDI, CFA_634, 0),
	UFTDI_DEV(FTDI, CFA_635, 0),
	UFTDI_DEV(FTDI, CHAMSYS_24_MASTER_WING, 0),
	UFTDI_DEV(FTDI, CHAMSYS_MAXI_WING, 0),
	UFTDI_DEV(FTDI, CHAMSYS_MEDIA_WING, 0),
	UFTDI_DEV(FTDI, CHAMSYS_MIDI_TIMECODE, 0),
	UFTDI_DEV(FTDI, CHAMSYS_MINI_WING, 0),
	UFTDI_DEV(FTDI, CHAMSYS_PC_WING, 0),
	UFTDI_DEV(FTDI, CHAMSYS_USB_DMX, 0),
	UFTDI_DEV(FTDI, CHAMSYS_WING, 0),
	UFTDI_DEV(FTDI, COM4SM, 0),
	UFTDI_DEV(FTDI, CONVERTER_0, 0),
	UFTDI_DEV(FTDI, CONVERTER_1, 0),
	UFTDI_DEV(FTDI, CONVERTER_2, 0),
	UFTDI_DEV(FTDI, CONVERTER_3, 0),
	UFTDI_DEV(FTDI, CONVERTER_4, 0),
	UFTDI_DEV(FTDI, CONVERTER_5, 0),
	UFTDI_DEV(FTDI, CONVERTER_6, 0),
	UFTDI_DEV(FTDI, CONVERTER_7, 0),
	UFTDI_DEV(FTDI, CTI_USB_MINI_485, 0),
	UFTDI_DEV(FTDI, CTI_USB_NANO_485, 0),
	UFTDI_DEV(FTDI, DMX4ALL, 0),
	UFTDI_DEV(FTDI, DOMINTELL_DGQG, 0),
	UFTDI_DEV(FTDI, DOMINTELL_DUSB, 0),
	UFTDI_DEV(FTDI, DOTEC, 0),
	UFTDI_DEV(FTDI, ECLO_COM_1WIRE, 0),
	UFTDI_DEV(FTDI, ECO_PRO_CDS, 0),
	UFTDI_DEV(FTDI, EISCOU, 0),
	UFTDI_DEV(FTDI, ELSTER_UNICOM, 0),
	UFTDI_DEV(FTDI, ELV_ALC8500, 0),
	UFTDI_DEV(FTDI, ELV_CLI7000, 0),
	UFTDI_DEV(FTDI, ELV_CSI8, 0),
	UFTDI_DEV(FTDI, ELV_EC3000, 0),
	UFTDI_DEV(FTDI, ELV_EM1000DL, 0),
	UFTDI_DEV(FTDI, ELV_EM1010PC, 0),
	UFTDI_DEV(FTDI, ELV_FEM, 0),
	UFTDI_DEV(FTDI, ELV_FHZ1000PC, 0),
	UFTDI_DEV(FTDI, ELV_FHZ1300PC, 0),
	UFTDI_DEV(FTDI, ELV_FM3RX, 0),
	UFTDI_DEV(FTDI, ELV_FS20SIG, 0),
	UFTDI_DEV(FTDI, ELV_HS485, 0),
	UFTDI_DEV(FTDI, ELV_KL100, 0),
	UFTDI_DEV(FTDI, ELV_MSM1, 0),
	UFTDI_DEV(FTDI, ELV_PCD200, 0),
	UFTDI_DEV(FTDI, ELV_PCK100, 0),
	UFTDI_DEV(FTDI, ELV_PPS7330, 0),
	UFTDI_DEV(FTDI, ELV_RFP500, 0),
	UFTDI_DEV(FTDI, ELV_T1100, 0),
	UFTDI_DEV(FTDI, ELV_TFD128, 0),
	UFTDI_DEV(FTDI, ELV_TFM100, 0),
	UFTDI_DEV(FTDI, ELV_TWS550, 0),
	UFTDI_DEV(FTDI, ELV_UAD8, 0),
	UFTDI_DEV(FTDI, ELV_UDA7, 0),
	UFTDI_DEV(FTDI, ELV_UDF77, 0),
	UFTDI_DEV(FTDI, ELV_UIO88, 0),
	UFTDI_DEV(FTDI, ELV_ULA200, 0),
	UFTDI_DEV(FTDI, ELV_UM100, 0),
	UFTDI_DEV(FTDI, ELV_UMS100, 0),
	UFTDI_DEV(FTDI, ELV_UO100, 0),
	UFTDI_DEV(FTDI, ELV_UR100, 0),
	UFTDI_DEV(FTDI, ELV_USI2, 0),
	UFTDI_DEV(FTDI, ELV_USR, 0),
	UFTDI_DEV(FTDI, ELV_UTP8, 0),
	UFTDI_DEV(FTDI, ELV_WS300PC, 0),
	UFTDI_DEV(FTDI, ELV_WS444PC, 0),
	UFTDI_DEV(FTDI, ELV_WS500, 0),
	UFTDI_DEV(FTDI, ELV_WS550, 0),
	UFTDI_DEV(FTDI, ELV_WS777, 0),
	UFTDI_DEV(FTDI, ELV_WS888, 0),
	UFTDI_DEV(FTDI, EMCU2D, 0),
	UFTDI_DEV(FTDI, EMCU2H, 0),
	UFTDI_DEV(FTDI, FUTURE_0, 0),
	UFTDI_DEV(FTDI, FUTURE_1, 0),
	UFTDI_DEV(FTDI, FUTURE_2, 0),
	UFTDI_DEV(FTDI, GAMMASCOUT, 0),
	UFTDI_DEV(FTDI, GENERIC, 0),
	UFTDI_DEV(FTDI, GUDEADS_E808, 0),
	UFTDI_DEV(FTDI, GUDEADS_E809, 0),
	UFTDI_DEV(FTDI, GUDEADS_E80A, 0),
	UFTDI_DEV(FTDI, GUDEADS_E80B, 0),
	UFTDI_DEV(FTDI, GUDEADS_E80C, 0),
	UFTDI_DEV(FTDI, GUDEADS_E80D, 0),
	UFTDI_DEV(FTDI, GUDEADS_E80E, 0),
	UFTDI_DEV(FTDI, GUDEADS_E80F, 0),
	UFTDI_DEV(FTDI, GUDEADS_E88D, 0),
	UFTDI_DEV(FTDI, GUDEADS_E88E, 0),
	UFTDI_DEV(FTDI, GUDEADS_E88F, 0),
	UFTDI_DEV(FTDI, HD_RADIO, 0),
	UFTDI_DEV(FTDI, HO720, 0),
	UFTDI_DEV(FTDI, HO730, 0),
	UFTDI_DEV(FTDI, HO820, 0),
	UFTDI_DEV(FTDI, HO870, 0),
	UFTDI_DEV(FTDI, IBS_APP70, 0),
	UFTDI_DEV(FTDI, IBS_PCMCIA, 0),
	UFTDI_DEV(FTDI, IBS_PEDO, 0),
	UFTDI_DEV(FTDI, IBS_PICPRO, 0),
	UFTDI_DEV(FTDI, IBS_PK1, 0),
	UFTDI_DEV(FTDI, IBS_PROD, 0),
	UFTDI_DEV(FTDI, IBS_RS232MON, 0),
	UFTDI_DEV(FTDI, IBS_US485, 0),
	UFTDI_DEV(FTDI, IPLUS, 0),
	UFTDI_DEV(FTDI, IPLUS2, 0),
	UFTDI_DEV(FTDI, IRTRANS, 0),
	UFTDI_DEV(FTDI, KBS, 0),
	UFTDI_DEV(FTDI, KTLINK, 0),
	UFTDI_DEV(FTDI, LENZ_LIUSB, 0),
	UFTDI_DEV(FTDI, LK202, 0),
	UFTDI_DEV(FTDI, LK204, 0),
	UFTDI_DEV(FTDI, LM3S_DEVEL_BOARD, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, LM3S_EVAL_BOARD, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, LM3S_ICDI_B_BOARD, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, MASTERDEVEL2, 0),
	UFTDI_DEV(FTDI, MAXSTREAM, 0),
	UFTDI_DEV(FTDI, MHAM_DB9, 0),
	UFTDI_DEV(FTDI, MHAM_IC, 0),
	UFTDI_DEV(FTDI, MHAM_KW, 0),
	UFTDI_DEV(FTDI, MHAM_RS232, 0),
	UFTDI_DEV(FTDI, MHAM_Y6, 0),
	UFTDI_DEV(FTDI, MHAM_Y8, 0),
	UFTDI_DEV(FTDI, MHAM_Y9, 0),
	UFTDI_DEV(FTDI, MHAM_YS, 0),
	UFTDI_DEV(FTDI, MICRO_CHAMELEON, 0),
	UFTDI_DEV(FTDI, MTXORB_5, 0),
	UFTDI_DEV(FTDI, MTXORB_6, 0),
	UFTDI_DEV(FTDI, MX2_3, 0),
	UFTDI_DEV(FTDI, MX4_5, 0),
	UFTDI_DEV(FTDI, NXTCAM, 0),
	UFTDI_DEV(FTDI, OCEANIC, 0),
	UFTDI_DEV(FTDI, OOCDLINK, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, OPENDCC, 0),
	UFTDI_DEV(FTDI, OPENDCC_GATEWAY, 0),
	UFTDI_DEV(FTDI, OPENDCC_GBM, 0),
	UFTDI_DEV(FTDI, OPENDCC_SNIFFER, 0),
	UFTDI_DEV(FTDI, OPENDCC_THROTTLE, 0),
	UFTDI_DEV(FTDI, PCDJ_DAC2, 0),
	UFTDI_DEV(FTDI, PCMSFU, 0),
	UFTDI_DEV(FTDI, PERLE_ULTRAPORT, 0),
	UFTDI_DEV(FTDI, PHI_FISCO, 0),
	UFTDI_DEV(FTDI, PIEGROUP, 0),
	UFTDI_DEV(FTDI, PROPOX_JTAGCABLEII, 0),
	UFTDI_DEV(FTDI, R2000KU_TRUE_RNG, 0),
	UFTDI_DEV(FTDI, R2X0, 0),
	UFTDI_DEV(FTDI, RELAIS, 0),
	UFTDI_DEV(FTDI, REU_TINY, 0),
	UFTDI_DEV(FTDI, RMP200, 0),
	UFTDI_DEV(FTDI, RM_CANVIEW, 0),
	UFTDI_DEV(FTDI, RRCIRKITS_LOCOBUFFER, 0),
	UFTDI_DEV(FTDI, SCIENCESCOPE_HS_LOGBOOK, 0),
	UFTDI_DEV(FTDI, SCIENCESCOPE_LOGBOOKML, 0),
	UFTDI_DEV(FTDI, SCIENCESCOPE_LS_LOGBOOK, 0),
	UFTDI_DEV(FTDI, SCS_DEVICE_0, 0),
	UFTDI_DEV(FTDI, SCS_DEVICE_1, 0),
	UFTDI_DEV(FTDI, SCS_DEVICE_2, 0),
	UFTDI_DEV(FTDI, SCS_DEVICE_3, 0),
	UFTDI_DEV(FTDI, SCS_DEVICE_4, 0),
	UFTDI_DEV(FTDI, SCS_DEVICE_5, 0),
	UFTDI_DEV(FTDI, SCS_DEVICE_6, 0),
	UFTDI_DEV(FTDI, SCS_DEVICE_7, 0),
	UFTDI_DEV(FTDI, SDMUSBQSS, 0),
	UFTDI_DEV(FTDI, SEMC_DSS20, 0),
	UFTDI_DEV(FTDI, SERIAL_2232C, UFTDI_JTAG_CHECK_STRING),
	UFTDI_DEV(FTDI, SERIAL_2232D, 0),
	UFTDI_DEV(FTDI, SERIAL_232RL, 0),
	UFTDI_DEV(FTDI, SERIAL_4232H, 0),
	UFTDI_DEV(FTDI, SERIAL_8U100AX, 0),
	UFTDI_DEV(FTDI, SERIAL_8U232AM, 0),
	UFTDI_DEV(FTDI, SERIAL_8U232AM4, 0),
	UFTDI_DEV(FTDI, SIGNALYZER_SH2, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, SIGNALYZER_SH4, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, SIGNALYZER_SLITE, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, SIGNALYZER_ST, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, SPECIAL_1, 0),
	UFTDI_DEV(FTDI, SPECIAL_3, 0),
	UFTDI_DEV(FTDI, SPECIAL_4, 0),
	UFTDI_DEV(FTDI, SPROG_II, 0),
	UFTDI_DEV(FTDI, SR_RADIO, 0),
	UFTDI_DEV(FTDI, SUUNTO_SPORTS, 0),
	UFTDI_DEV(FTDI, TACTRIX_OPENPORT_13M, 0),
	UFTDI_DEV(FTDI, TACTRIX_OPENPORT_13S, 0),
	UFTDI_DEV(FTDI, TACTRIX_OPENPORT_13U, 0),
	UFTDI_DEV(FTDI, TAVIR_STK500, 0),
	UFTDI_DEV(FTDI, TERATRONIK_D2XX, 0),
	UFTDI_DEV(FTDI, TERATRONIK_VCP, 0),
	UFTDI_DEV(FTDI, THORLABS, 0),
	UFTDI_DEV(FTDI, TNC_X, 0),
	UFTDI_DEV(FTDI, TTUSB, 0),
	UFTDI_DEV(FTDI, TURTELIZER2, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, UOPTBR, 0),
	UFTDI_DEV(FTDI, USBSERIAL, 0),
	UFTDI_DEV(FTDI, USBX_707, 0),
	UFTDI_DEV(FTDI, USB_UIRT, 0),
	UFTDI_DEV(FTDI, USINT_CAT, 0),
	UFTDI_DEV(FTDI, USINT_RS232, 0),
	UFTDI_DEV(FTDI, USINT_WKEY, 0),
	UFTDI_DEV(FTDI, VARDAAN, 0),
	UFTDI_DEV(FTDI, VNHCPCUSB_D, 0),
	UFTDI_DEV(FTDI, WESTREX_MODEL_777, 0),
	UFTDI_DEV(FTDI, WESTREX_MODEL_8900F, 0),
	UFTDI_DEV(FTDI, XDS100V2, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, XDS100V3, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(FTDI, XF_547, 0),
	UFTDI_DEV(FTDI, XF_640, 0),
	UFTDI_DEV(FTDI, XF_642, 0),
	UFTDI_DEV(FTDI, XM_RADIO, 0),
	UFTDI_DEV(FTDI, YEI_SERVOCENTER31, 0),
	UFTDI_DEV(GNOTOMETRICS, USB, 0),
	UFTDI_DEV(ICOM, SP1, 0),
	UFTDI_DEV(ICOM, OPC_U_UC, 0),
	UFTDI_DEV(ICOM, RP2C1, 0),
	UFTDI_DEV(ICOM, RP2C2, 0),
	UFTDI_DEV(ICOM, RP2D, 0),
	UFTDI_DEV(ICOM, RP2KVR, 0),
	UFTDI_DEV(ICOM, RP2KVT, 0),
	UFTDI_DEV(ICOM, RP2VR, 0),
	UFTDI_DEV(ICOM, RP2VT, 0),
	UFTDI_DEV(ICOM, RP4KVR, 0),
	UFTDI_DEV(ICOM, RP4KVT, 0),
	UFTDI_DEV(IDTECH, IDT1221U, 0),
	UFTDI_DEV(INTERBIOMETRICS, IOBOARD, 0),
	UFTDI_DEV(INTERBIOMETRICS, MINI_IOBOARD, 0),
	UFTDI_DEV(INTREPIDCS, NEOVI, 0),
	UFTDI_DEV(INTREPIDCS, VALUECAN, 0),
	UFTDI_DEV(IONICS, PLUGCOMPUTER, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(JETI, SPC1201, 0),
	UFTDI_DEV(KOBIL, CONV_B1, 0),
	UFTDI_DEV(KOBIL, CONV_KAAN, 0),
	UFTDI_DEV(LARSENBRUSGAARD, ALTITRACK, 0),
	UFTDI_DEV(MARVELL, SHEEVAPLUG, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0100, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0101, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0102, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0103, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0104, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0105, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0106, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0107, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0108, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0109, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_010A, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_010B, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_010C, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_010D, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_010E, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_010F, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0110, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0111, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0112, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0113, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0114, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0115, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0116, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0117, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0118, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0119, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_011A, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_011B, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_011C, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_011D, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_011E, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_011F, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0120, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0121, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0122, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0123, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0124, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0125, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0126, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0128, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0129, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_012A, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_012B, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_012D, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_012E, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_012F, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0130, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0131, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0132, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0133, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0134, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0135, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0136, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0137, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0138, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0139, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_013A, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_013B, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_013C, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_013D, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_013E, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_013F, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0140, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0141, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0142, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0143, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0144, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0145, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0146, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0147, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0148, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0149, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_014A, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_014B, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_014C, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_014D, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_014E, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_014F, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0150, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0151, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0152, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0159, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_015A, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_015B, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_015C, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_015D, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_015E, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_015F, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0160, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0161, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0162, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0163, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0164, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0165, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0166, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0167, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0168, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0169, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_016A, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_016B, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_016C, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_016D, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_016E, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_016F, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0170, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0171, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0172, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0173, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0174, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0175, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0176, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0177, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0178, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0179, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_017A, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_017B, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_017C, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_017D, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_017E, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_017F, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0180, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0181, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0182, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0183, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0184, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0185, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0186, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0187, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0188, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0189, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_018A, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_018B, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_018C, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_018D, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_018E, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_018F, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0190, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0191, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0192, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0193, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0194, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0195, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0196, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0197, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0198, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_0199, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_019A, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_019B, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_019C, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_019D, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_019E, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_019F, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01A0, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01A1, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01A2, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01A3, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01A4, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01A5, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01A6, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01A7, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01A8, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01A9, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01AA, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01AB, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01AC, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01AD, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01AE, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01AF, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01B0, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01B1, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01B2, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01B3, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01B4, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01B5, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01B6, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01B7, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01B8, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01B9, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01BA, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01BB, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01BC, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01BD, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01BE, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01BF, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01C0, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01C1, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01C2, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01C3, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01C4, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01C5, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01C6, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01C7, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01C8, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01C9, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01CA, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01CB, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01CC, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01CD, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01CE, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01CF, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01D0, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01D1, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01D2, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01D3, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01D4, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01D5, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01D6, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01D7, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01D8, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01D9, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01DA, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01DB, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01DC, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01DD, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01DE, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01DF, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01E0, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01E1, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01E2, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01E3, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01E4, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01E5, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01E6, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01E7, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01E8, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01E9, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01EA, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01EB, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01EC, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01ED, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01EE, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01EF, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01F0, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01F1, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01F2, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01F3, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01F4, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01F5, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01F6, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01F7, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01F8, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01F9, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01FA, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01FB, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01FC, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01FD, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01FE, 0),
	UFTDI_DEV(MATRIXORBITAL, FTDI_RANGE_01FF, 0),
	UFTDI_DEV(MATRIXORBITAL, MOUA, 0),
	UFTDI_DEV(MELCO, PCOPRS1, 0),
	UFTDI_DEV(METAGEEK, TELLSTICK, 0),
	UFTDI_DEV(MOBILITY, USB_SERIAL, 0),
	UFTDI_DEV(OLIMEX, ARM_USB_OCD, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(OLIMEX, ARM_USB_OCD_H, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(OPTO, CRD7734, 0),
	UFTDI_DEV(OPTO, CRD7734_1, 0),
	UFTDI_DEV(PAPOUCH, AD4USB, 0),
	UFTDI_DEV(PAPOUCH, AP485, 0),
	UFTDI_DEV(PAPOUCH, AP485_2, 0),
	UFTDI_DEV(PAPOUCH, DRAK5, 0),
	UFTDI_DEV(PAPOUCH, DRAK6, 0),
	UFTDI_DEV(PAPOUCH, GMSR, 0),
	UFTDI_DEV(PAPOUCH, GMUX, 0),
	UFTDI_DEV(PAPOUCH, IRAMP, 0),
	UFTDI_DEV(PAPOUCH, LEC, 0),
	UFTDI_DEV(PAPOUCH, MU, 0),
	UFTDI_DEV(PAPOUCH, QUIDO10X1, 0),
	UFTDI_DEV(PAPOUCH, QUIDO2X16, 0),
	UFTDI_DEV(PAPOUCH, QUIDO2X2, 0),
	UFTDI_DEV(PAPOUCH, QUIDO30X3, 0),
	UFTDI_DEV(PAPOUCH, QUIDO3X32, 0),
	UFTDI_DEV(PAPOUCH, QUIDO4X4, 0),
	UFTDI_DEV(PAPOUCH, QUIDO60X3, 0),
	UFTDI_DEV(PAPOUCH, QUIDO8X8, 0),
	UFTDI_DEV(PAPOUCH, SB232, 0),
	UFTDI_DEV(PAPOUCH, SB422, 0),
	UFTDI_DEV(PAPOUCH, SB422_2, 0),
	UFTDI_DEV(PAPOUCH, SB485, 0),
	UFTDI_DEV(PAPOUCH, SB485C, 0),
	UFTDI_DEV(PAPOUCH, SB485S, 0),
	UFTDI_DEV(PAPOUCH, SB485_2, 0),
	UFTDI_DEV(PAPOUCH, SIMUKEY, 0),
	UFTDI_DEV(PAPOUCH, TMU, 0),
	UFTDI_DEV(PAPOUCH, UPSUSB, 0),
	UFTDI_DEV(POSIFLEX, PP7000, 0),
	UFTDI_DEV(QIHARDWARE, JTAGSERIAL, UFTDI_JTAG_IFACE(0)),
	UFTDI_DEV(RATOC, REXUSB60F, 0),
	UFTDI_DEV(RTSYSTEMS, CT29B, 0),
	UFTDI_DEV(RTSYSTEMS, SERIAL_VX7, 0),
	UFTDI_DEV(SEALEVEL, 2101, 0),
	UFTDI_DEV(SEALEVEL, 2102, 0),
	UFTDI_DEV(SEALEVEL, 2103, 0),
	UFTDI_DEV(SEALEVEL, 2104, 0),
	UFTDI_DEV(SEALEVEL, 2106, 0),
	UFTDI_DEV(SEALEVEL, 2201_1, 0),
	UFTDI_DEV(SEALEVEL, 2201_2, 0),
	UFTDI_DEV(SEALEVEL, 2202_1, 0),
	UFTDI_DEV(SEALEVEL, 2202_2, 0),
	UFTDI_DEV(SEALEVEL, 2203_1, 0),
	UFTDI_DEV(SEALEVEL, 2203_2, 0),
	UFTDI_DEV(SEALEVEL, 2401_1, 0),
	UFTDI_DEV(SEALEVEL, 2401_2, 0),
	UFTDI_DEV(SEALEVEL, 2401_3, 0),
	UFTDI_DEV(SEALEVEL, 2401_4, 0),
	UFTDI_DEV(SEALEVEL, 2402_1, 0),
	UFTDI_DEV(SEALEVEL, 2402_2, 0),
	UFTDI_DEV(SEALEVEL, 2402_3, 0),
	UFTDI_DEV(SEALEVEL, 2402_4, 0),
	UFTDI_DEV(SEALEVEL, 2403_1, 0),
	UFTDI_DEV(SEALEVEL, 2403_2, 0),
	UFTDI_DEV(SEALEVEL, 2403_3, 0),
	UFTDI_DEV(SEALEVEL, 2403_4, 0),
	UFTDI_DEV(SEALEVEL, 2801_1, 0),
	UFTDI_DEV(SEALEVEL, 2801_2, 0),
	UFTDI_DEV(SEALEVEL, 2801_3, 0),
	UFTDI_DEV(SEALEVEL, 2801_4, 0),
	UFTDI_DEV(SEALEVEL, 2801_5, 0),
	UFTDI_DEV(SEALEVEL, 2801_6, 0),
	UFTDI_DEV(SEALEVEL, 2801_7, 0),
	UFTDI_DEV(SEALEVEL, 2801_8, 0),
	UFTDI_DEV(SEALEVEL, 2802_1, 0),
	UFTDI_DEV(SEALEVEL, 2802_2, 0),
	UFTDI_DEV(SEALEVEL, 2802_3, 0),
	UFTDI_DEV(SEALEVEL, 2802_4, 0),
	UFTDI_DEV(SEALEVEL, 2802_5, 0),
	UFTDI_DEV(SEALEVEL, 2802_6, 0),
	UFTDI_DEV(SEALEVEL, 2802_7, 0),
	UFTDI_DEV(SEALEVEL, 2802_8, 0),
	UFTDI_DEV(SEALEVEL, 2803_1, 0),
	UFTDI_DEV(SEALEVEL, 2803_2, 0),
	UFTDI_DEV(SEALEVEL, 2803_3, 0),
	UFTDI_DEV(SEALEVEL, 2803_4, 0),
	UFTDI_DEV(SEALEVEL, 2803_5, 0),
	UFTDI_DEV(SEALEVEL, 2803_6, 0),
	UFTDI_DEV(SEALEVEL, 2803_7, 0),
	UFTDI_DEV(SEALEVEL, 2803_8, 0),
	UFTDI_DEV(SIIG2, DK201, 0),
	UFTDI_DEV(SIIG2, US2308, 0),
	UFTDI_DEV(TESTO, USB_INTERFACE, 0),
	UFTDI_DEV(TML, USB_SERIAL, 0),
	UFTDI_DEV(TTI, QL355P, 0),
	UFTDI_DEV(UNKNOWN4, NF_RIC, 0),
#undef UFTDI_DEV
};

/*
 * Jtag product name strings table.  Some products have one or more interfaces
 * dedicated to jtag or gpio, but use a product ID that's the same as other
 * products which don't.  They are marked with a flag in the table above, and
 * the following string table is checked for flagged products.  The string check
 * is done with strstr(); in effect there is an implicit wildcard at the
 * beginning and end of each product name string in table.
 */
static const struct jtag_by_name {
	const char *	product_name;
	uint32_t	jtag_interfaces;
} jtag_products_by_name[] = {
        /* TI Beaglebone and TI XDS100Vn jtag product line. */
	{"XDS100V",	UFTDI_JTAG_IFACE(0)},
};

/*
 * Set up a sysctl and tunable to en/disable the feature of skipping the
 * creation of tty devices for jtag interfaces.  Enabled by default.
 */
static int skip_jtag_interfaces = 1;
TUNABLE_INT("hw.usb.uftdi.skip_jtag_interfaces", &skip_jtag_interfaces);
SYSCTL_INT(_hw_usb_uftdi, OID_AUTO, skip_jtag_interfaces, CTLFLAG_RWTUN,
    &skip_jtag_interfaces, 1, "Skip creating tty devices for jtag interfaces");

static boolean_t
is_jtag_interface(struct usb_attach_arg *uaa, const struct usb_device_id *id)
{
	int i, iface_bit;
	const char * product_name;
	const struct jtag_by_name *jbn;

	/* We only allocate 8 flag bits for jtag interface flags. */
	if (uaa->info.bIfaceIndex >= UFTDI_JTAG_IFACES_MAX)
		return (0);
	iface_bit = UFTDI_JTAG_IFACE(uaa->info.bIfaceIndex);

	/*
	 * If requested, search the name strings table and use the interface
	 * bits from that table when the product name string matches, else use
	 * the jtag interface bits from the main ID table.
	 */
	if ((id->driver_info & UFTDI_JTAG_MASK) == UFTDI_JTAG_CHECK_STRING) {
		product_name = usb_get_product(uaa->device);
		for (i = 0; i < nitems(jtag_products_by_name); i++) {
			jbn = &jtag_products_by_name[i];
			if (strstr(product_name, jbn->product_name) != NULL &&
			    (jbn->jtag_interfaces & iface_bit) != 0)
				return (1);
		}
	} else if ((id->driver_info & iface_bit) != 0)
		return (1);

	return (0);
}

/*
 * Set up softc fields whose value depends on the device type.
 *
 * Note that the 2232C and 2232D devices are the same for our purposes.  In the
 * silicon the difference is that the D series has CPU FIFO mode and C doesn't.
 * I haven't found any way of determining the C/D difference from info provided
 * by the chip other than trying to set CPU FIFO mode and having it work or not.
 * 
 * Due to a hardware bug, a 232B chip without an eeprom reports itself as a 
 * 232A, but if the serial number is also zero we know it's really a 232B. 
 */
static void
uftdi_devtype_setup(struct uftdi_softc *sc, struct usb_attach_arg *uaa)
{
	struct usb_device_descriptor *dd;

	sc->sc_bcdDevice = uaa->info.bcdDevice;

	switch (uaa->info.bcdDevice) {
	case 0x200:
		dd = usbd_get_device_descriptor(sc->sc_udev);
		if (dd->iSerialNumber == 0) {
			sc->sc_devtype = DEVT_232B;
		} else {
			sc->sc_devtype = DEVT_232A;
		}
		sc->sc_ucom.sc_portno = 0;
		break;
	case 0x400:
		sc->sc_devtype = DEVT_232B;
		sc->sc_ucom.sc_portno = 0;
		break;
	case 0x500:
		sc->sc_devtype = DEVT_2232D;
		sc->sc_devflags |= DEVF_BAUDBITS_HINDEX;
		sc->sc_ucom.sc_portno = FTDI_PIT_SIOA + uaa->info.bIfaceNum;
		break;
	case 0x600:
		sc->sc_devtype = DEVT_232R;
		sc->sc_ucom.sc_portno = 0;
		break;
	case 0x700:
		sc->sc_devtype = DEVT_2232H;
		sc->sc_devflags |= DEVF_BAUDBITS_HINDEX | DEVF_BAUDCLK_12M;
		sc->sc_ucom.sc_portno = FTDI_PIT_SIOA + uaa->info.bIfaceNum;
		break;
	case 0x800:
		sc->sc_devtype = DEVT_4232H;
		sc->sc_devflags |= DEVF_BAUDBITS_HINDEX | DEVF_BAUDCLK_12M;
		sc->sc_ucom.sc_portno = FTDI_PIT_SIOA + uaa->info.bIfaceNum;
		break;
	case 0x900:
		sc->sc_devtype = DEVT_232H;
		sc->sc_devflags |= DEVF_BAUDBITS_HINDEX | DEVF_BAUDCLK_12M;
		sc->sc_ucom.sc_portno = FTDI_PIT_SIOA + uaa->info.bIfaceNum;
		break;
	case 0x1000:
		sc->sc_devtype = DEVT_230X;
		sc->sc_devflags |= DEVF_BAUDBITS_HINDEX;
		sc->sc_ucom.sc_portno = FTDI_PIT_SIOA + uaa->info.bIfaceNum;
		break;
	default:
		if (uaa->info.bcdDevice < 0x200) {
			sc->sc_devtype = DEVT_SIO;
			sc->sc_hdrlen = 1;
		} else {
			sc->sc_devtype = DEVT_232R;
			device_printf(sc->sc_dev, "Warning: unknown FTDI "
			    "device type, bcdDevice=0x%04x, assuming 232R\n", 
			    uaa->info.bcdDevice);
		}
		sc->sc_ucom.sc_portno = 0;
		break;
	}
}

static int
uftdi_probe(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	const struct usb_device_id *id;

	if (uaa->usb_mode != USB_MODE_HOST) {
		return (ENXIO);
	}
	if (uaa->info.bConfigIndex != UFTDI_CONFIG_INDEX) {
		return (ENXIO);
	}

	/*
	 * Attach to all present interfaces unless this is a JTAG one, which
	 * we leave for userland.
	 */
	id = usbd_lookup_id_by_info(uftdi_devs, sizeof(uftdi_devs),
	    &uaa->info);
	if (id == NULL)
		return (ENXIO);
	if (skip_jtag_interfaces && is_jtag_interface(uaa, id)) {
		printf("%s: skipping JTAG interface #%d for '%s' at %u.%u\n",
		    device_get_name(dev),
		    uaa->info.bIfaceIndex,
		    usb_get_product(uaa->device),
		    usbd_get_bus_index(uaa->device),
		    usbd_get_device_index(uaa->device));
		return (ENXIO);
	}
	uaa->driver_info = id->driver_info;
	return (BUS_PROBE_SPECIFIC);
}

static int
uftdi_attach(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	struct uftdi_softc *sc = device_get_softc(dev);
	int error;

	DPRINTF("\n");

	sc->sc_udev = uaa->device;
	sc->sc_dev = dev;
	sc->sc_unit = device_get_unit(dev);

	device_set_usb_desc(dev);
	mtx_init(&sc->sc_mtx, "uftdi", NULL, MTX_DEF);
	ucom_ref(&sc->sc_super_ucom);


	uftdi_devtype_setup(sc, uaa);

	error = usbd_transfer_setup(uaa->device,
	    &uaa->info.bIfaceIndex, sc->sc_xfer, uftdi_config,
	    UFTDI_N_TRANSFER, sc, &sc->sc_mtx);

	if (error) {
		device_printf(dev, "allocating USB "
		    "transfers failed\n");
		goto detach;
	}
	/* clear stall at first run */
	mtx_lock(&sc->sc_mtx);
	usbd_xfer_set_stall(sc->sc_xfer[UFTDI_BULK_DT_WR]);
	usbd_xfer_set_stall(sc->sc_xfer[UFTDI_BULK_DT_RD]);
	mtx_unlock(&sc->sc_mtx);

	/* set a valid "lcr" value */

	sc->sc_last_lcr =
	    (FTDI_SIO_SET_DATA_STOP_BITS_2 |
	    FTDI_SIO_SET_DATA_PARITY_NONE |
	    FTDI_SIO_SET_DATA_BITS(8));

	error = ucom_attach(&sc->sc_super_ucom, &sc->sc_ucom, 1, sc,
	    &uftdi_callback, &sc->sc_mtx);
	if (error) {
		goto detach;
	}
	ucom_set_pnpinfo_usb(&sc->sc_super_ucom, dev);

	return (0);			/* success */

detach:
	uftdi_detach(dev);
	return (ENXIO);
}

static int
uftdi_detach(device_t dev)
{
	struct uftdi_softc *sc = device_get_softc(dev);

	ucom_detach(&sc->sc_super_ucom, &sc->sc_ucom);
	usbd_transfer_unsetup(sc->sc_xfer, UFTDI_N_TRANSFER);

	device_claim_softc(dev);

	uftdi_free_softc(sc);

	return (0);
}

UCOM_UNLOAD_DRAIN(uftdi);

static void
uftdi_free_softc(struct uftdi_softc *sc)
{
	if (ucom_unref(&sc->sc_super_ucom)) {
		mtx_destroy(&sc->sc_mtx);
		device_free_softc(sc);
	}
}

static void
uftdi_free(struct ucom_softc *ucom)
{
	uftdi_free_softc(ucom->sc_parent);
}

static void
uftdi_cfg_open(struct ucom_softc *ucom)
{

	/*
	 * This do-nothing open routine exists for the sole purpose of this
	 * DPRINTF() so that you can see the point at which open gets called
	 * when debugging is enabled.
	 */
	DPRINTF("");
}

static void
uftdi_cfg_close(struct ucom_softc *ucom)
{

	/*
	 * This do-nothing close routine exists for the sole purpose of this
	 * DPRINTF() so that you can see the point at which close gets called
	 * when debugging is enabled.
	 */
	DPRINTF("");
}

static void
uftdi_write_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct uftdi_softc *sc = usbd_xfer_softc(xfer);
	struct usb_page_cache *pc;
	uint32_t pktlen;
	uint32_t buflen;
	uint8_t buf[1];

	switch (USB_GET_STATE(xfer)) {
	default:			/* Error */
		if (error != USB_ERR_CANCELLED) {
			/* try to clear stall first */
			usbd_xfer_set_stall(xfer);
		}
		/* FALLTHROUGH */
	case USB_ST_SETUP:
	case USB_ST_TRANSFERRED:
		/*
		 * If output packets don't require headers (the common case) we
		 * can just load the buffer up with payload bytes all at once.
		 * Otherwise, loop to format packets into the buffer while there
		 * is data available, and room for a packet header and at least
		 * one byte of payload.
		 *
		 * NOTE: The FTDI chip doesn't accept zero length
		 * packets. This cannot happen because the "pktlen"
		 * will always be non-zero when "ucom_get_data()"
		 * returns non-zero which we check below.
		 */
		pc = usbd_xfer_get_frame(xfer, 0);
		if (sc->sc_hdrlen == 0) {
			if (ucom_get_data(&sc->sc_ucom, pc, 0, UFTDI_OBUFSIZE, 
			    &buflen) == 0)
				break;
		} else {
			buflen = 0;
			while (buflen < UFTDI_OBUFSIZE - sc->sc_hdrlen - 1 &&
			    ucom_get_data(&sc->sc_ucom, pc, buflen + 
			    sc->sc_hdrlen, UFTDI_OPKTSIZE - sc->sc_hdrlen, 
			    &pktlen) != 0) {
				buf[0] = FTDI_OUT_TAG(pktlen, 
				    sc->sc_ucom.sc_portno);
				usbd_copy_in(pc, buflen, buf, 1);
				buflen += pktlen + sc->sc_hdrlen;
			}
		}
		if (buflen != 0) {
			usbd_xfer_set_frame_len(xfer, 0, buflen);
			usbd_transfer_submit(xfer);
		}
		break;
	}
}

static void
uftdi_read_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct uftdi_softc *sc = usbd_xfer_softc(xfer);
	struct usb_page_cache *pc;
	uint8_t buf[2];
	uint8_t ftdi_msr;
	uint8_t msr;
	uint8_t lsr;
	int buflen;
	int pktlen;
	int pktmax;
	int offset;

	usbd_xfer_status(xfer, &buflen, NULL, NULL, NULL);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		if (buflen < UFTDI_IHDRSIZE)
			goto tr_setup;
		pc = usbd_xfer_get_frame(xfer, 0);
		pktmax = xfer->max_packet_size - UFTDI_IHDRSIZE;
		lsr = 0;
		msr = 0;
		offset = 0;
		/*
		 * Extract packet headers and payload bytes from the buffer.
		 * Feed payload bytes to ucom/tty layer; OR-accumulate header
		 * status bits which are transient and could toggle with each
		 * packet. After processing all packets in the buffer, process
		 * the accumulated transient MSR and LSR values along with the
		 * non-transient bits from the last packet header.
		 */
		while (buflen >= UFTDI_IHDRSIZE) {
			usbd_copy_out(pc, offset, buf, UFTDI_IHDRSIZE);
			offset += UFTDI_IHDRSIZE;
			buflen -= UFTDI_IHDRSIZE;
			lsr |= FTDI_GET_LSR(buf);
			if (FTDI_GET_MSR(buf) & FTDI_SIO_RI_MASK)
				msr |= SER_RI;
			pktlen = min(buflen, pktmax);
			if (pktlen != 0) {
				ucom_put_data(&sc->sc_ucom, pc, offset, 
				    pktlen);
				offset += pktlen;
				buflen -= pktlen;
			}
		}
		ftdi_msr = FTDI_GET_MSR(buf);

		if (ftdi_msr & FTDI_SIO_CTS_MASK)
			msr |= SER_CTS;
		if (ftdi_msr & FTDI_SIO_DSR_MASK)
			msr |= SER_DSR;
		if (ftdi_msr & FTDI_SIO_RI_MASK)
			msr |= SER_RI;
		if (ftdi_msr & FTDI_SIO_RLSD_MASK)
			msr |= SER_DCD;

		if ((sc->sc_msr != msr) ||
		    ((sc->sc_lsr & FTDI_LSR_MASK) != (lsr & FTDI_LSR_MASK))) {
			DPRINTF("status change msr=0x%02x (0x%02x) "
			    "lsr=0x%02x (0x%02x)\n", msr, sc->sc_msr,
			    lsr, sc->sc_lsr);

			sc->sc_msr = msr;
			sc->sc_lsr = lsr;

			ucom_status_change(&sc->sc_ucom);
		}
		/* FALLTHROUGH */
	case USB_ST_SETUP:
tr_setup:
		usbd_xfer_set_frame_len(xfer, 0, usbd_xfer_max_len(xfer));
		usbd_transfer_submit(xfer);
		return;

	default:			/* Error */
		if (error != USB_ERR_CANCELLED) {
			/* try to clear stall first */
			usbd_xfer_set_stall(xfer);
			goto tr_setup;
		}
		return;
	}
}

static void
uftdi_cfg_set_dtr(struct ucom_softc *ucom, uint8_t onoff)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	uint16_t wIndex = ucom->sc_portno;
	uint16_t wValue;
	struct usb_device_request req;

	wValue = onoff ? FTDI_SIO_SET_DTR_HIGH : FTDI_SIO_SET_DTR_LOW;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_MODEM_CTRL;
	USETW(req.wValue, wValue);
	USETW(req.wIndex, wIndex);
	USETW(req.wLength, 0);
	ucom_cfg_do_request(sc->sc_udev, &sc->sc_ucom, 
	    &req, NULL, 0, 1000);
}

static void
uftdi_cfg_set_rts(struct ucom_softc *ucom, uint8_t onoff)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	uint16_t wIndex = ucom->sc_portno;
	uint16_t wValue;
	struct usb_device_request req;

	wValue = onoff ? FTDI_SIO_SET_RTS_HIGH : FTDI_SIO_SET_RTS_LOW;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_MODEM_CTRL;
	USETW(req.wValue, wValue);
	USETW(req.wIndex, wIndex);
	USETW(req.wLength, 0);
	ucom_cfg_do_request(sc->sc_udev, &sc->sc_ucom, 
	    &req, NULL, 0, 1000);
}

static void
uftdi_cfg_set_break(struct ucom_softc *ucom, uint8_t onoff)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	uint16_t wIndex = ucom->sc_portno;
	uint16_t wValue;
	struct usb_device_request req;

	if (onoff) {
		sc->sc_last_lcr |= FTDI_SIO_SET_BREAK;
	} else {
		sc->sc_last_lcr &= ~FTDI_SIO_SET_BREAK;
	}

	wValue = sc->sc_last_lcr;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_SET_DATA;
	USETW(req.wValue, wValue);
	USETW(req.wIndex, wIndex);
	USETW(req.wLength, 0);
	ucom_cfg_do_request(sc->sc_udev, &sc->sc_ucom, 
	    &req, NULL, 0, 1000);
}

/*
 * Return true if the given speed is within operational tolerance of the target
 * speed.  FTDI recommends that the hardware speed be within 3% of nominal.
 */
static inline boolean_t
uftdi_baud_within_tolerance(uint64_t speed, uint64_t target)
{
	return ((speed >= (target * 100) / 103) &&
	    (speed <= (target * 100) / 97));
}

static int
uftdi_sio_encode_baudrate(struct uftdi_softc *sc, speed_t speed,
	struct uftdi_param_config *cfg)
{
	u_int i;
	const speed_t sio_speeds[] = {
		300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200
	};

	/*
	 * The original SIO chips were limited to a small choice of speeds
	 * listed in an internal table of speeds chosen by an index value.
	 */
	for (i = 0; i < nitems(sio_speeds); ++i) {
		if (speed == sio_speeds[i]) {
			cfg->baud_lobits = i;
			cfg->baud_hibits = 0;
			return (0);
		}
	}
	return (ERANGE);
}

static int
uftdi_encode_baudrate(struct uftdi_softc *sc, speed_t speed,
	struct uftdi_param_config *cfg)
{
	static const uint8_t encoded_fraction[8] = {0, 3, 2, 4, 1, 5, 6, 7};
	static const uint8_t roundoff_232a[16] = {
		0,  1,  0,  1,  0, -1,  2,  1,
		0, -1, -2, -3,  4,  3,  2,  1,
	};
	uint32_t clk, divisor, fastclk_flag, frac, hwspeed;

	/*
	 * If this chip has the fast clock capability and the speed is within
	 * range, use the 12MHz clock, otherwise the standard clock is 3MHz.
	 */
	if ((sc->sc_devflags & DEVF_BAUDCLK_12M) && speed >= 1200) {
		clk = 12000000;
		fastclk_flag = (1 << 17);
	} else {
		clk = 3000000;
		fastclk_flag = 0;
	}

	/*
	 * Make sure the requested speed is reachable with the available clock
	 * and a 14-bit divisor.
	 */
	if (speed < (clk >> 14) || speed > clk)
		return (ERANGE);

	/*
	 * Calculate the divisor, initially yielding a fixed point number with a
	 * 4-bit (1/16ths) fraction, then round it to the nearest fraction the
	 * hardware can handle.  When the integral part of the divisor is
	 * greater than one, the fractional part is in 1/8ths of the base clock.
	 * The FT8U232AM chips can handle only 0.125, 0.250, and 0.5 fractions.
	 * Later chips can handle all 1/8th fractions.
	 *
	 * If the integral part of the divisor is 1, a special rule applies: the
	 * fractional part can only be .0 or .5 (this is a limitation of the
	 * hardware).  We handle this by truncating the fraction rather than
	 * rounding, because this only applies to the two fastest speeds the
	 * chip can achieve and rounding doesn't matter, either you've asked for
	 * that exact speed or you've asked for something the chip can't do.
	 *
	 * For the FT8U232AM chips, use a roundoff table to adjust the result
	 * to the nearest 1/8th fraction that is supported by the hardware,
	 * leaving a fixed-point number with a 3-bit fraction which exactly
	 * represents the math the hardware divider will do.  For later-series
	 * chips that support all 8 fractional divisors, just round 16ths to
	 * 8ths by adding 1 and dividing by 2.
	 */
	divisor = (clk << 4) / speed;
	if ((divisor & 0xf) == 1)
		divisor &= 0xfffffff8;
	else if (sc->sc_devtype == DEVT_232A)
		divisor += roundoff_232a[divisor & 0x0f];
	else
		divisor += 1;  /* Rounds odd 16ths up to next 8th. */
	divisor >>= 1;

	/*
	 * Ensure the resulting hardware speed will be within operational
	 * tolerance (within 3% of nominal).
	 */
	hwspeed = (clk << 3) / divisor;
	if (!uftdi_baud_within_tolerance(hwspeed, speed))
		return (ERANGE);

	/*
	 * Re-pack the divisor into hardware format. The lower 14-bits hold the
	 * integral part, while the upper bits specify the fraction by indexing
	 * a table of fractions within the hardware which is laid out as:
	 *    {0.0, 0.5, 0.25, 0.125, 0.325, 0.625, 0.725, 0.875}
	 * The A-series chips only have the first four table entries; the
	 * roundoff table logic above ensures that the fractional part for those
	 * chips will be one of the first four values.
	 *
	 * When the divisor is 1 a special encoding applies:  1.0 is encoded as
	 * 0.0, and 1.5 is encoded as 1.0.  The rounding logic above has already
	 * ensured that the fraction is either .0 or .5 if the integral is 1.
	 */
	frac = divisor & 0x07;
	divisor >>= 3;
	if (divisor == 1) {
		if (frac == 0)
			divisor = 0;  /* 1.0 becomes 0.0 */
		else
			frac = 0;     /* 1.5 becomes 1.0 */
	}
	divisor |= (encoded_fraction[frac] << 14) | fastclk_flag;
        
	cfg->baud_lobits = (uint16_t)divisor;
	cfg->baud_hibits = (uint16_t)(divisor >> 16);

	/*
	 * If this chip requires the baud bits to be in the high byte of the
	 * index word, move the bits up to that location.
	 */
	if (sc->sc_devflags & DEVF_BAUDBITS_HINDEX) {
		cfg->baud_hibits <<= 8;
	}

	return (0);
}

static int
uftdi_set_parm_soft(struct ucom_softc *ucom, struct termios *t,
    struct uftdi_param_config *cfg)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	int err;

	memset(cfg, 0, sizeof(*cfg));

	if (sc->sc_devtype == DEVT_SIO)
		err = uftdi_sio_encode_baudrate(sc, t->c_ospeed, cfg);
	else
		err = uftdi_encode_baudrate(sc, t->c_ospeed, cfg);
	if (err != 0)
		return (err);

	if (t->c_cflag & CSTOPB)
		cfg->lcr = FTDI_SIO_SET_DATA_STOP_BITS_2;
	else
		cfg->lcr = FTDI_SIO_SET_DATA_STOP_BITS_1;

	if (t->c_cflag & PARENB) {
		if (t->c_cflag & PARODD) {
			cfg->lcr |= FTDI_SIO_SET_DATA_PARITY_ODD;
		} else {
			cfg->lcr |= FTDI_SIO_SET_DATA_PARITY_EVEN;
		}
	} else {
		cfg->lcr |= FTDI_SIO_SET_DATA_PARITY_NONE;
	}

	switch (t->c_cflag & CSIZE) {
	case CS5:
		cfg->lcr |= FTDI_SIO_SET_DATA_BITS(5);
		break;

	case CS6:
		cfg->lcr |= FTDI_SIO_SET_DATA_BITS(6);
		break;

	case CS7:
		cfg->lcr |= FTDI_SIO_SET_DATA_BITS(7);
		break;

	case CS8:
		cfg->lcr |= FTDI_SIO_SET_DATA_BITS(8);
		break;
	}

	if (t->c_cflag & CRTSCTS) {
		cfg->v_flow = FTDI_SIO_RTS_CTS_HS;
	} else if (t->c_iflag & (IXON | IXOFF)) {
		cfg->v_flow = FTDI_SIO_XON_XOFF_HS;
		cfg->v_start = t->c_cc[VSTART];
		cfg->v_stop = t->c_cc[VSTOP];
	} else {
		cfg->v_flow = FTDI_SIO_DISABLE_FLOW_CTRL;
	}

	return (0);
}

static int
uftdi_pre_param(struct ucom_softc *ucom, struct termios *t)
{
	struct uftdi_param_config cfg;

	DPRINTF("\n");

	return (uftdi_set_parm_soft(ucom, t, &cfg));
}

static void
uftdi_cfg_param(struct ucom_softc *ucom, struct termios *t)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	uint16_t wIndex = ucom->sc_portno;
	struct uftdi_param_config cfg;
	struct usb_device_request req;

	if (uftdi_set_parm_soft(ucom, t, &cfg)) {
		/* should not happen */
		return;
	}
	sc->sc_last_lcr = cfg.lcr;

	DPRINTF("\n");

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_SET_BAUD_RATE;
	USETW(req.wValue, cfg.baud_lobits);
	USETW(req.wIndex, cfg.baud_hibits | wIndex);
	USETW(req.wLength, 0);
	ucom_cfg_do_request(sc->sc_udev, &sc->sc_ucom, 
	    &req, NULL, 0, 1000);

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_SET_DATA;
	USETW(req.wValue, cfg.lcr);
	USETW(req.wIndex, wIndex);
	USETW(req.wLength, 0);
	ucom_cfg_do_request(sc->sc_udev, &sc->sc_ucom, 
	    &req, NULL, 0, 1000);

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_SET_FLOW_CTRL;
	USETW2(req.wValue, cfg.v_stop, cfg.v_start);
	USETW2(req.wIndex, cfg.v_flow, wIndex);
	USETW(req.wLength, 0);
	ucom_cfg_do_request(sc->sc_udev, &sc->sc_ucom, 
	    &req, NULL, 0, 1000);
}

static void
uftdi_cfg_get_status(struct ucom_softc *ucom, uint8_t *lsr, uint8_t *msr)
{
	struct uftdi_softc *sc = ucom->sc_parent;

	DPRINTF("msr=0x%02x lsr=0x%02x\n",
	    sc->sc_msr, sc->sc_lsr);

	*msr = sc->sc_msr;
	*lsr = sc->sc_lsr;
}

static int
uftdi_reset(struct ucom_softc *ucom, int reset_type)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	usb_device_request_t req;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_RESET;

	USETW(req.wIndex, sc->sc_ucom.sc_portno);
	USETW(req.wLength, 0);
	USETW(req.wValue, reset_type);

	return (usbd_do_request(sc->sc_udev, &sc->sc_mtx, &req, NULL));
}

static int
uftdi_set_bitmode(struct ucom_softc *ucom, uint8_t bitmode, uint8_t iomask)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	usb_device_request_t req;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_SET_BITMODE;

	USETW(req.wIndex, sc->sc_ucom.sc_portno);
	USETW(req.wLength, 0);

	if (bitmode == UFTDI_BITMODE_NONE)
	    USETW2(req.wValue, 0, 0);
	else
	    USETW2(req.wValue, (1 << bitmode), iomask);

	return (usbd_do_request(sc->sc_udev, &sc->sc_mtx, &req, NULL));
}

static int
uftdi_get_bitmode(struct ucom_softc *ucom, uint8_t *iomask)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	usb_device_request_t req;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_GET_BITMODE;

	USETW(req.wIndex, sc->sc_ucom.sc_portno);
	USETW(req.wLength, 1);
	USETW(req.wValue,  0);

	return (usbd_do_request(sc->sc_udev, &sc->sc_mtx, &req, iomask));
}

static int
uftdi_set_latency(struct ucom_softc *ucom, int latency)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	usb_device_request_t req;

	if (latency < 0 || latency > 255)
		return (USB_ERR_INVAL);

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_SET_LATENCY;

	USETW(req.wIndex, sc->sc_ucom.sc_portno);
	USETW(req.wLength, 0);
	USETW2(req.wValue, 0, latency);

	return (usbd_do_request(sc->sc_udev, &sc->sc_mtx, &req, NULL));
}

static int
uftdi_get_latency(struct ucom_softc *ucom, int *latency)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	usb_device_request_t req;
	usb_error_t err;
	uint8_t buf;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_GET_LATENCY;

	USETW(req.wIndex, sc->sc_ucom.sc_portno);
	USETW(req.wLength, 1);
	USETW(req.wValue, 0);

	err = usbd_do_request(sc->sc_udev, &sc->sc_mtx, &req, &buf);
	*latency = buf;

	return (err);
}

static int
uftdi_set_event_char(struct ucom_softc *ucom, int echar)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	usb_device_request_t req;
	uint8_t enable;

	enable = (echar == -1) ? 0 : 1;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_SET_EVENT_CHAR;

	USETW(req.wIndex, sc->sc_ucom.sc_portno);
	USETW(req.wLength, 0);
	USETW2(req.wValue, enable, echar & 0xff);

	return (usbd_do_request(sc->sc_udev, &sc->sc_mtx, &req, NULL));
}

static int
uftdi_set_error_char(struct ucom_softc *ucom, int echar)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	usb_device_request_t req;
	uint8_t enable;

	enable = (echar == -1) ? 0 : 1;

	req.bmRequestType = UT_WRITE_VENDOR_DEVICE;
	req.bRequest = FTDI_SIO_SET_ERROR_CHAR;

	USETW(req.wIndex, sc->sc_ucom.sc_portno);
	USETW(req.wLength, 0);
	USETW2(req.wValue, enable, echar & 0xff);

	return (usbd_do_request(sc->sc_udev, &sc->sc_mtx, &req, NULL));
}

static int
uftdi_ioctl(struct ucom_softc *ucom, uint32_t cmd, caddr_t data,
    int flag, struct thread *td)
{
	struct uftdi_softc *sc = ucom->sc_parent;
	int err;
	struct uftdi_bitmode * mode;

	DPRINTF("portno: %d cmd: %#x\n", ucom->sc_portno, cmd);

	switch (cmd) {
	case UFTDIIOC_RESET_IO:
	case UFTDIIOC_RESET_RX:
	case UFTDIIOC_RESET_TX:
		err = uftdi_reset(ucom, 
		    cmd == UFTDIIOC_RESET_IO ? FTDI_SIO_RESET_SIO :
		    (cmd == UFTDIIOC_RESET_RX ? FTDI_SIO_RESET_PURGE_RX :
		    FTDI_SIO_RESET_PURGE_TX));
		break;
	case UFTDIIOC_SET_BITMODE:
		mode = (struct uftdi_bitmode *)data;
		err = uftdi_set_bitmode(ucom, mode->mode, mode->iomask);
		break;
	case UFTDIIOC_GET_BITMODE:
		mode = (struct uftdi_bitmode *)data;
		err = uftdi_get_bitmode(ucom, &mode->iomask);
		break;
	case UFTDIIOC_SET_LATENCY:
		err = uftdi_set_latency(ucom, *((int *)data));
		break;
	case UFTDIIOC_GET_LATENCY:
		err = uftdi_get_latency(ucom, (int *)data);
		break;
	case UFTDIIOC_SET_ERROR_CHAR:
		err = uftdi_set_error_char(ucom, *(int *)data);
		break;
	case UFTDIIOC_SET_EVENT_CHAR:
		err = uftdi_set_event_char(ucom, *(int *)data);
		break;
	case UFTDIIOC_GET_HWREV:
		*(int *)data = sc->sc_bcdDevice;
		err = 0;
		break;
	default:
		return (ENOIOCTL);
	}
	if (err != USB_ERR_NORMAL_COMPLETION)
		return (EIO);
	return (0);
}

static void
uftdi_start_read(struct ucom_softc *ucom)
{
	struct uftdi_softc *sc = ucom->sc_parent;

	usbd_transfer_start(sc->sc_xfer[UFTDI_BULK_DT_RD]);
}

static void
uftdi_stop_read(struct ucom_softc *ucom)
{
	struct uftdi_softc *sc = ucom->sc_parent;

	usbd_transfer_stop(sc->sc_xfer[UFTDI_BULK_DT_RD]);
}

static void
uftdi_start_write(struct ucom_softc *ucom)
{
	struct uftdi_softc *sc = ucom->sc_parent;

	usbd_transfer_start(sc->sc_xfer[UFTDI_BULK_DT_WR]);
}

static void
uftdi_stop_write(struct ucom_softc *ucom)
{
	struct uftdi_softc *sc = ucom->sc_parent;

	usbd_transfer_stop(sc->sc_xfer[UFTDI_BULK_DT_WR]);
}

static void
uftdi_poll(struct ucom_softc *ucom)
{
	struct uftdi_softc *sc = ucom->sc_parent;

	usbd_transfer_poll(sc->sc_xfer, UFTDI_N_TRANSFER);
}
