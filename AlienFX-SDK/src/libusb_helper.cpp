#include "libusb_helper.h"

#include <cstring>

#include "loguru.hpp"

namespace {

// HID class-specific requests (HID 1.11 sec. 7.2).
constexpr uint8_t HID_REQ_GET_REPORT = 0x01;
constexpr uint8_t HID_REQ_SET_REPORT = 0x09;
constexpr uint16_t HID_RT_INPUT = 1;
constexpr uint16_t HID_RT_OUTPUT = 2;
constexpr uint16_t HID_RT_FEATURE = 3;

// Claims an interface for exactly the lifetime of one transfer, with
// libusb's built-in auto-detach so the release below also reattaches
// whatever kernel driver (usbhid, typically) owned the interface --
// unconditionally, the same way hidapi's libusb backend does it, but here
// bounded to a single control/interrupt transfer instead of the life of the
// device handle. This is the actual fix for the keyboard lockup this
// backend used to cause: the interface is never claimed across anything a
// human might be doing, e.g. `probe`'s interactive prompts -- see
// docs/hid-transport.md.
class ScopedClaim {
   public:
    ScopedClaim(libusb_device_handle* h, int iface) : handle_(h), iface_(iface) {
        if (!handle_ || iface_ < 0) return;
        libusb_set_auto_detach_kernel_driver(handle_, 1);
        ok_ = libusb_claim_interface(handle_, iface_) == 0;
        if (!ok_) {
            LOG_S(ERROR) << "Failed to claim USB interface " << iface_;
        }
    }
    ~ScopedClaim() {
        if (ok_) libusb_release_interface(handle_, iface_);
    }
    ScopedClaim(const ScopedClaim&) = delete;
    ScopedClaim& operator=(const ScopedClaim&) = delete;
    bool ok() const { return ok_; }

   private:
    libusb_device_handle* handle_ = nullptr;
    int iface_ = -1;
    bool ok_ = false;
};

// One SET_REPORT/GET_REPORT class transfer, trying the claim-free DEVICE
// recipient first (see UsbHidHandle::deviceRecipientWorks) and falling back
// to INTERFACE recipient -- which needs h.interface claimed -- only if that
// fails. `reportNumber` is buffer[0] by HID convention; when it's 0 (the
// device uses unnumbered reports) that byte is not part of the report data
// and is stripped before the transfer, matching both the kernel's usbhid
// driver (usbhid_set_raw_report) and hidapi's own backends.
int ControlTransferReport(UsbHidHandle& h, uint8_t direction,
                          uint8_t requestCode, uint16_t reportType,
                          uint8_t* buffer, size_t length) {
    if (!h.handle || length == 0) return -1;

    uint8_t reportNumber = buffer[0];
    uint8_t* data = buffer;
    uint16_t dataLen = (uint16_t)length;
    if (reportNumber == 0) {
        data++;
        dataLen--;
    }
    uint16_t wValue = (uint16_t)((reportType << 8) | reportNumber);

    if (!h.deviceRecipientTried || h.deviceRecipientWorks) {
        int rc = libusb_control_transfer(
            h.handle,
            (uint8_t)(direction | LIBUSB_REQUEST_TYPE_CLASS |
                      LIBUSB_RECIPIENT_DEVICE),
            requestCode, wValue, 0, data, dataLen, 2000);
        h.deviceRecipientTried = true;
        h.deviceRecipientWorks = rc >= 0;
        if (rc >= 0) return rc;
    }

    ScopedClaim claim(h.handle, h.interface);
    if (!claim.ok()) return -1;
    return libusb_control_transfer(
        h.handle,
        (uint8_t)(direction | LIBUSB_REQUEST_TYPE_CLASS |
                  LIBUSB_RECIPIENT_INTERFACE),
        requestCode, wValue, (uint16_t)h.interface, data, dataLen, 2000);
}

}  // namespace

int GetMaxPacketSize(libusb_context* ctx, unsigned short vid,
                     unsigned short pid) {
    int maxPacketSize = -1;
    libusb_device** devs = nullptr;

    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0) {
        LOG_S(ERROR) << "Failed to get device list";
        return -1;
    }

    libusb_device* device = nullptr;
    for (ssize_t i = 0; i < cnt; i++) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) == 0 &&
            desc.idVendor == vid && desc.idProduct == pid) {
            device = devs[i];
            break;
        }
    }

    if (!device) {
        libusb_free_device_list(devs, 1);
        return -1;
    }

    libusb_config_descriptor* config = nullptr;
    int result = libusb_get_config_descriptor(device, 0, &config);
    if (result != 0) {
        libusb_free_device_list(devs, 1);
        return -1;
    }

    for (int ifc = 0; ifc < config->bNumInterfaces; ifc++) {
        const libusb_interface& iface = config->interface[ifc];

        for (int alt = 0; alt < iface.num_altsetting; alt++) {
            const libusb_interface_descriptor& altset = iface.altsetting[alt];

            if (altset.bInterfaceClass != LIBUSB_CLASS_HID) continue;

            for (int ep = 0; ep < altset.bNumEndpoints; ep++) {
                const libusb_endpoint_descriptor& e = altset.endpoint[ep];

                if ((e.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
                        LIBUSB_ENDPOINT_IN &&
                    (e.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
                        LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                    maxPacketSize = e.wMaxPacketSize;
                }
            }
        }
    }

    libusb_free_config_descriptor(config);
    libusb_free_device_list(devs, 1);

    return maxPacketSize;
}

void PopulateEndpoints(libusb_device* usbDev, UsbHidHandle* h) {
    h->ep_in = h->ep_out = 0;
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_config_descriptor(usbDev, 0, &cfg) != 0 || !cfg) return;

    for (int ifc = 0; ifc < cfg->bNumInterfaces; ifc++) {
        const libusb_interface& iface = cfg->interface[ifc];
        for (int alt = 0; alt < iface.num_altsetting; alt++) {
            const libusb_interface_descriptor& a = iface.altsetting[alt];
            if (a.bInterfaceNumber != h->interface) continue;
            for (int ep = 0; ep < a.bNumEndpoints; ep++) {
                const libusb_endpoint_descriptor& e = a.endpoint[ep];
                if ((e.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                    LIBUSB_TRANSFER_TYPE_INTERRUPT)
                    continue;
                if ((e.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) ==
                    LIBUSB_ENDPOINT_IN)
                    h->ep_in = e.bEndpointAddress;
                else
                    h->ep_out = e.bEndpointAddress;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
}

bool HidD_SetFeature(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    return ControlTransferReport(h, LIBUSB_ENDPOINT_OUT, HID_REQ_SET_REPORT,
                                 HID_RT_FEATURE, buffer, length) >= 0;
}

bool HidD_SetOutputReport(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    return ControlTransferReport(h, LIBUSB_ENDPOINT_OUT, HID_REQ_SET_REPORT,
                                 HID_RT_OUTPUT, buffer, length) >= 0;
}

bool HidD_GetFeature(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    return ControlTransferReport(h, LIBUSB_ENDPOINT_IN, HID_REQ_GET_REPORT,
                                 HID_RT_FEATURE, buffer, length) >= 0;
}

bool HidD_GetInputReport(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    return ControlTransferReport(h, LIBUSB_ENDPOINT_IN, HID_REQ_GET_REPORT,
                                 HID_RT_INPUT, buffer, length) >= 0;
}

bool WriteFile(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    if (!h.handle || !h.ep_out) return false;
    ScopedClaim claim(h.handle, h.interface);
    if (!claim.ok()) return false;
    int actual = 0;
    int rc = libusb_interrupt_transfer(h.handle, h.ep_out, buffer,
                                       (int)length, &actual, 2000);
    return rc == 0;
}

bool ReadFile(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    if (!h.handle || !h.ep_in) return false;
    ScopedClaim claim(h.handle, h.interface);
    if (!claim.ok()) return false;
    int actual = 0;
    int rc = libusb_interrupt_transfer(h.handle, h.ep_in, buffer,
                                       (int)length, &actual, 2000);
    return rc == 0;
}
