/*
 * Copyright 2018, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *			Augustin Cavalier <waddlesplash>
 */

extern "C" {
#include <sys/cdefs.h>
#include <sys/callout.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usb_device.h>
}

// undo name remappings, so we can use both these and USB3.h in this file
#undef usb_device
#undef usb_interface
#undef usb_endpoint_descriptor

#include <USB3.h>


// static members
static usb_module_info* sUSB;


status_t
init_usb_stack()
{
	if (get_module(B_USB_MODULE_NAME, (module_info**)&sUSB) != B_OK) {
		dprintf("cannot get module \"%s\"\n", B_USB_MODULE_NAME);
		return B_ERROR;
	}
	return B_OK;
}


extern "C" usb_error_t
usbd_do_request_flags(struct freebsd_usb_device* udev, struct mtx *mtx,
	struct usb_device_request* req, void* data, uint16_t flags,
    uint16_t *actlen, usb_timeout_t timeout)
{
	usb_device device = (usb_device)udev->haiku_usb_device;

	// FIXME: send_request is default pipe only, but this is probably used for other things?
	size_t actualLen = 0;
	status_t ret = sUSB->send_request(device, req->bmRequestType, req->bRequest,
		UGETW(req->wValue), UGETW(req->wIndex), UGETW(req->wLength),
		data, &actualLen);
	*actlen = actualLen;

	usb_error_t err;
	if (ret != B_OK)
		err = USB_ERR_INVAL; /* TODO: convert errors */
	return err;
}


enum usb_dev_speed
usbd_get_speed(struct freebsd_usb_device* udev)
{
	/* MUST_FIXME */
}


struct usb_xfer {
	struct mtx* mutex;
	void* priv_sc;
	usb_callback_t* callback;

	usb_device device;
	usb_pipe pipe;

	uint8 usb_state;
	usb_xfer_flags flags;
};


extern "C" usb_error_t
usbd_transfer_setup(struct freebsd_usb_device* udev,
	const uint8_t* ifaces, struct usb_xfer** ppxfer,
	const struct usb_config* setup_start, uint16_t n_setup,
	void* priv_sc, struct mtx* xfer_mtx)
{
	for (const struct usb_config* setup = setup_start;
			setup < (setup_start + n_setup); setup++) {
		/* skip transfers w/o callbacks */
		if (setup->callback == NULL)
			continue;

		struct usb_xfer* xfer = new usb_xfer;
		xfer->mutex = xfer_mtx;
		xfer->priv_sc = priv_sc;
		xfer->callback = setup->callback;
		xfer->device = (usb_device)udev->haiku_usb_device;
		xfer->pipe = setup->endpoint;

		ppxfer[setup - setup_start] = xfer;
	}

	return USB_ERR_NORMAL_COMPLETION;
}


extern "C" void
usbd_transfer_unsetup(struct usb_xfer** pxfer, uint16_t n_setup)
{
	for (int i = 0; i < n_setup; i++) {
		struct usb_xfer* xfer = pxfer[i];
		usbd_transfer_stop(xfer);
		delete xfer;
	}
}


extern "C" usb_frlength_t
usbd_xfer_max_len(struct usb_xfer* xfer)
{
	/* return (xfer->max_data_length); */
}


extern "C" void*
usbd_xfer_softc(struct usb_xfer* xfer)
{
	return xfer->priv_sc;
}


extern "C" uint8_t
usbd_xfer_state(struct usb_xfer *xfer)
{
	return xfer->usb_state;
}


extern "C" void
usbd_xfer_set_frame_data(struct usb_xfer *xfer, usb_frcount_t frindex,
	void *ptr, usb_frlength_t len)
{
	/* MUST_FIXME
	KASSERT(frindex < xfer->max_frame_count, ("frame index overflow"));
	xfer->frbuffers[frindex].buffer = ptr;
	xfer->frlengths[frindex] = len;
	*/
}


extern "C" void
usbd_xfer_set_stall(struct usb_xfer *xfer)
{
	/* this isn't a real soft-stall. still, better than nothing */
	sUSB->cancel_queued_transfers(xfer->pipe);
}


static void
usbd_callback(void* arg, status_t status, void* data, size_t actualLength)
{
	struct usb_xfer* xfer = (struct usb_xfer*)arg;
	xfer->queued = false;

	usb_error_t err = map_usb_error(status);
	//xfer->callback(xfer);
}


extern "C" void
usbd_transfer_submit(struct usb_xfer* xfer)
{
	// FIXME: queue as proper type not just bulk!
	sUSB->queue_bulk(xfer->pipe, xfer->data,
		xfer->dataLength, usbd_callback, xfer);
}


extern "C" void
usbd_transfer_stop(struct usb_xfer* xfer)
{
	sUSB->cancel_queued_transfers(xfer->pipe);
}


extern "C" void
usbd_transfer_drain(struct usb_xfer* xfer)
{
	if (xfer == NULL) {
		/* transfer is gone */
		return;
	}

	usbd_transfer_stop(xfer);
	// while (usbd_transfer_pending(xfer)) {}
}


extern "C" void
usbd_transfer_start(struct usb_xfer* xfer)
{
	if (xfer == NULL) {
		/* transfer is gone */
		return;
	}

	/* FIXME */
}


void
usbd_xfer_status(struct usb_xfer* xfer, int* actlen, int* sumlen, int* aframes,
	int* nframes)
{
	/* FIXME */
}
