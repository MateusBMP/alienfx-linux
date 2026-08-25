#pragma once
#include <cstddef>
#include <cstdint>

#include "libusb.h"

// Windows-HID-API-shaped shims (see AlienFX_SDK.cpp's PrepareAndSend and
// GetDeviceStatus, which are otherwise a straight port of the upstream
// Windows source) reimplemented directly on libusb control/interrupt
// transfers instead of hidapi. Reasons this talks to libusb instead of
// hidapi, and instead of hidapi's own libusb backend:
//
//   - hidapi's libusb backend claims -- and unconditionally *detaches the
//     kernel driver from* -- one fixed USB interface for the life of the
//     open handle (libusb/hid.c's DETACH_KERNEL_DRIVER, reattached only on
//     hid_close()). On a Darfon RGB keyboard (VID 0x0d62) the AlienFX
//     lighting collection lives on the *same* interface as the keyboard
//     itself; that combination can disable the keyboard for as long as the
//     process holds the handle open.
//   - hidapi's hidraw backend avoids that (it never claims anything), but
//     every output-report write then goes over hidraw's interrupt-OUT path
//     (drivers/hid/hidraw.c) instead of a control transfer -- measured
//     ~425x slower on hardware whose OUT endpoint has a long bInterval.
//
// Every function here scopes any interface claim it needs to the single
// transfer it's making -- see UsbHidHandle's negotiation below and
// ScopedClaim in the .cpp -- so a claim (and the kernel-driver detach that
// comes with it, where one is unavoidable) never outlives one HID report
// send/receive, and in particular is never held across the interactive
// prompts in `probe` the way the original lockup needed.

// Everything AlienFX_SDK::Functions needs to address one HID interface over
// libusb, resolved once by AlienFXProbeDevice and reused for every
// PrepareAndSend/GetDeviceStatus call on that device.
struct UsbHidHandle {
    libusb_device_handle* handle = nullptr;
    int interface = -1;  // USB interface number the HID reports live on
    uint8_t ep_in = 0;   // interrupt IN endpoint address, 0 if none
    uint8_t ep_out = 0;  // interrupt OUT endpoint address, 0 if none

    // Class requests (Set/Get Feature, Set Output Report) are tried first
    // at DEVICE recipient, which needs no interface claim at all -- some
    // firmware accepts that, and it's strictly safer when it works. It's
    // negotiated once per device rather than retried on every call: some
    // firmware answers a device-recipient request with a STALL that still
    // costs the full 2s control-transfer timeout, and retrying that on
    // every SetColor would reintroduce exactly the kind of stall this
    // rewrite exists to avoid.
    bool deviceRecipientTried = false;
    bool deviceRecipientWorks = false;
};

// [Linux Compatibility] Gets the maximum packet size for IN endpoint for the
// device. Pure libusb descriptor read, independent of the transfer path
// below -- kept for the non-Darfon API-version detection in
// AlienFXProbeDevice, which this SDK's own testing has verified produces
// the correct on-wire packet length.
int GetMaxPacketSize(libusb_context* ctxx, unsigned short vidd,
                     unsigned short pidd);

// Populates ep_in/ep_out on h from the device's active config descriptor,
// for the WriteFile/ReadFile (interrupt transfer) paths below. Safe to call
// before the interface is claimed -- it only reads descriptors.
void PopulateEndpoints(libusb_device* usbDev, UsbHidHandle* h);

// Below: HID Set/Get Report override for hid_send_feature_report.
bool HidD_SetFeature(UsbHidHandle& h, uint8_t* buffer, size_t length);

// Override for hid_send_output_report.
bool HidD_SetOutputReport(UsbHidHandle& h, uint8_t* buffer, size_t length);

// Override for hid_write (interrupt OUT transfer; always claims the
// interface for the duration of the call -- endpoint transfers have no
// device-recipient equivalent to try first).
bool WriteFile(UsbHidHandle& h, uint8_t* buffer, size_t length);

// Override for hid_read (interrupt IN transfer; same claim scoping as
// WriteFile).
bool ReadFile(UsbHidHandle& h, uint8_t* buffer, size_t length);

// Override for hid_get_feature_report.
bool HidD_GetFeature(UsbHidHandle& h, uint8_t* buffer, size_t length);

// Override for hid_get_input_report.
bool HidD_GetInputReport(UsbHidHandle& h, uint8_t* buffer, size_t length);
