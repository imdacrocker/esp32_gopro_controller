/*
 * gopro_usb.h — Open GoPro wired (USB) camera driver.
 *
 * Implements the cam_core camera_driver_t over the Open GoPro HTTP API served
 * by a Hero10+ connected as a USB network gadget (docs/design/wired-variant.md
 * §2, §5).  Registers a single cam_core slot (the wired variant controls one
 * camera).  All HTTP work runs on a dedicated worker task so the vtable
 * methods invoked under the cam_core lock stay non-blocking.
 *
 * Lifecycle:
 *   gopro_usb_init()            — register slot + driver, start worker (boot).
 *   gopro_usb_on_link_up(ip)    — USB link came up (usb_host_net): enable wired
 *                                 control, probe the camera, mark the slot ready.
 *   gopro_usb_on_link_down()    — USB link dropped: mark the slot not-ready.
 *   gopro_usb_sync_time_all()   — push SetDateTime from the CAN UTC anchor.
 *
 * Must be called after cam_core_init().
 */
#pragma once

#include <stdint.h>

/* Register the single USB camera slot + driver with cam_core and start the
 * HTTP worker task.  Call once at boot, before usb_host_net_init() so the slot
 * exists when the first link-up event fires. */
void gopro_usb_init(void);

/* USB camera link is up with the given camera IP (network byte order).  Posts
 * the enable-wired-control + readiness sequence to the worker. */
void gopro_usb_on_link_up(uint32_t camera_ip);

/* USB camera link dropped.  Posts a teardown that clears readiness. */
void gopro_usb_on_link_down(void);

/* Push the current CAN UTC anchor to the camera as a SetDateTime.  No-op if no
 * UTC is available or the link is down. */
void gopro_usb_sync_time_all(void);
