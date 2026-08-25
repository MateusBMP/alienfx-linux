#include "libusb_helper.h"

#include <fcntl.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "loguru.hpp"

namespace {

namespace fs = std::filesystem;

// Reads one line (e.g. sysfs's "idVendor"/"idProduct" files, which hold a
// bare 4-hex-digit value and nothing else).
std::string ReadSysfsLine(const fs::path& p) {
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    return s;
}

// HID class-specific requests (HID 1.11 sec. 7.2). Named distinctly from
// <linux/hid.h>'s hid_class_request enum (pulled in transitively via
// <linux/hidraw.h>), which defines the same two names at global scope and
// would otherwise make every call site ambiguous.
constexpr uint8_t kHidReqGetReport = 0x01;
constexpr uint8_t kHidReqSetReport = 0x09;
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
// human might be doing, e.g. `probe`'s interactive prompts.
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
#ifdef DEBUG
        LOG_S(INFO) << "ControlTransferReport device-recipient: dir=0x"
                    << std::hex << (int)direction << " req=0x" << (int)requestCode
                    << " reportType=0x" << reportType << " reportNumber=0x"
                    << (int)reportNumber << " wValue=0x" << wValue
                    << " iface=" << std::dec << h.interface << " rc=" << rc
                    << (rc < 0 ? libusb_error_name(rc) : "");
#endif
        if (rc >= 0) return rc;
    }

    ScopedClaim claim(h.handle, h.interface);
    if (!claim.ok()) {
#ifdef DEBUG
        LOG_S(ERROR) << "ControlTransferReport: failed to claim interface "
                     << h.interface;
#endif
        return -1;
    }
    int rc2 = libusb_control_transfer(
        h.handle,
        (uint8_t)(direction | LIBUSB_REQUEST_TYPE_CLASS |
                  LIBUSB_RECIPIENT_INTERFACE),
        requestCode, wValue, (uint16_t)h.interface, data, dataLen, 2000);
#ifdef DEBUG
    LOG_S(INFO) << "ControlTransferReport interface-recipient: dir=0x"
                << std::hex << (int)direction << " req=0x" << (int)requestCode
                << " reportType=0x" << reportType << " reportNumber=0x"
                << (int)reportNumber << " wValue=0x" << wValue
                << " iface=" << std::dec << h.interface << " rc=" << rc2
                << (rc2 < 0 ? libusb_error_name(rc2) : "");
#endif
    return rc2;
}

// Output reports go out via a plain write(): the kernel's usbhid transport
// (drivers/hid/usbhid/hid-core.c's usbhid_output_report) picks interrupt-OUT
// or a control SET_REPORT itself, whichever the device actually implements
// -- exactly the negotiation this repo's own hardware needs and that a raw
// libusb attempt (control or interrupt, both tried and both confirmed
// failing on the 187c:0550 Alienware LED controller) can't reproduce.
bool HidrawSetOutputReport(const std::string& path, uint8_t* buffer,
                           size_t length) {
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) return false;
    ssize_t n = write(fd, buffer, length);
    close(fd);
#ifdef DEBUG
    LOG_S(INFO) << "HidrawSetOutputReport: path=" << path << " length="
                << length << " written=" << n;
#endif
    return n == (ssize_t)length;
}

// HIDIOCGINPUT mirrors HIDIOCGFEATURE (both class GET_REPORT requests) but
// for the Input report type; available since Linux 4.10.
bool HidrawGetInputReport(const std::string& path, uint8_t* buffer,
                          size_t length) {
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) return false;
    int rc = ioctl(fd, HIDIOCGINPUT(length), buffer);
    close(fd);
#ifdef DEBUG
    LOG_S(INFO) << "HidrawGetInputReport: path=" << path << " length="
                << length << " rc=" << rc;
#endif
    return rc >= 0;
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

void PopulateHidrawPath(unsigned short vidd, unsigned short pidd,
                        UsbHidHandle* h) {
    h->hidrawPath.clear();
    std::error_code ec;
    for (const auto& entry :
        fs::directory_iterator("/sys/class/hidraw", ec)) {
        fs::path hidDevDir = fs::canonical(entry.path() / "device", ec);
        if (ec) continue;
        // hidDevDir looks like .../<bus>-<port>:<config>.<iface>/<hid-id>;
        // its parent is the usb_interface, its grandparent the usb_device.
        fs::path ifaceDir = hidDevDir.parent_path();
        std::string ifaceName = ifaceDir.filename().string();
        size_t colon = ifaceName.rfind(':');
        size_t dot = ifaceName.rfind('.');
        if (colon == std::string::npos || dot == std::string::npos ||
            dot < colon)
            continue;
        int iface = -1;
        try {
            iface = std::stoi(ifaceName.substr(dot + 1));
        } catch (...) {
            continue;
        }
        if (iface != h->interface) continue;

        fs::path usbDevDir = ifaceDir.parent_path();
        unsigned int foundVid = 0, foundPid = 0;
        std::istringstream(ReadSysfsLine(usbDevDir / "idVendor")) >>
            std::hex >> foundVid;
        std::istringstream(ReadSysfsLine(usbDevDir / "idProduct")) >>
            std::hex >> foundPid;
        if (foundVid != vidd || foundPid != pidd) continue;

        h->hidrawPath = "/dev/" + entry.path().filename().string();
#ifdef DEBUG
        LOG_S(INFO) << "PopulateHidrawPath: VID 0x" << std::hex << vidd
                    << " PID 0x" << pidd << " iface " << std::dec
                    << h->interface << " -> " << h->hidrawPath;
#endif
        return;
    }
}

bool HidD_SetFeature(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    return ControlTransferReport(h, LIBUSB_ENDPOINT_OUT, kHidReqSetReport,
                                 HID_RT_FEATURE, buffer, length) >= 0;
}

// NOTE: HID 1.11 sec. 7.2 makes Set_Report/Get_Report over the control pipe
// mandatory only for report types a device has no other pipe for (Feature
// reports always go through HidD_SetFeature/HidD_GetFeature above). Once a
// device declares its own interrupt OUT/IN endpoints for Output/Input
// reports, firmware is free to leave the equivalent control request
// unimplemented -- and on this repo's own Alienware LED controller
// (187c:0550) it does: every Set_Report(Output)/Get_Report(Input) attempt at
// both DEVICE and INTERFACE recipient came back LIBUSB_ERROR_PIPE (a STALL).
// Its firmware turned out just as unwilling to answer a *raw* libusb
// interrupt transfer to those same endpoints (LIBUSB_ERROR_IO, every time) --
// only the kernel's own hidraw path (usbhid picking the transport itself)
// gets a report through, so that's tried first via h.hidrawPath, with the
// interrupt transfer and then ControlTransferReport as fallbacks for a
// device with no hidraw node bound.
bool HidD_SetOutputReport(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    if (!h.hidrawPath.empty())
        return HidrawSetOutputReport(h.hidrawPath, buffer, length);
    if (h.ep_out) return WriteFile(h, buffer, length);
    return ControlTransferReport(h, LIBUSB_ENDPOINT_OUT, kHidReqSetReport,
                                 HID_RT_OUTPUT, buffer, length) >= 0;
}

bool HidD_GetFeature(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    return ControlTransferReport(h, LIBUSB_ENDPOINT_IN, kHidReqGetReport,
                                 HID_RT_FEATURE, buffer, length) >= 0;
}

bool HidD_GetInputReport(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    if (!h.hidrawPath.empty())
        return HidrawGetInputReport(h.hidrawPath, buffer, length);
    if (h.ep_in) return ReadFile(h, buffer, length);
    return ControlTransferReport(h, LIBUSB_ENDPOINT_IN, kHidReqGetReport,
                                 HID_RT_INPUT, buffer, length) >= 0;
}

bool WriteFile(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    if (!h.handle || !h.ep_out) return false;
    ScopedClaim claim(h.handle, h.interface);
    if (!claim.ok()) return false;
    int actual = 0;
    int rc = libusb_interrupt_transfer(h.handle, h.ep_out, buffer,
                                       (int)length, &actual, 2000);
#ifdef DEBUG
    LOG_S(INFO) << "WriteFile: ep=0x" << std::hex << (int)h.ep_out
                << " iface=" << std::dec << h.interface << " length="
                << length << " actual=" << actual << " rc=" << rc
                << (rc != 0 ? libusb_error_name(rc) : "");
#endif
    return rc == 0;
}

bool ReadFile(UsbHidHandle& h, uint8_t* buffer, size_t length) {
    if (!h.handle || !h.ep_in) return false;
    ScopedClaim claim(h.handle, h.interface);
    if (!claim.ok()) return false;
    int actual = 0;
    int rc = libusb_interrupt_transfer(h.handle, h.ep_in, buffer,
                                       (int)length, &actual, 2000);
#ifdef DEBUG
    LOG_S(INFO) << "ReadFile: ep=0x" << std::hex << (int)h.ep_in
                << " iface=" << std::dec << h.interface << " length="
                << length << " actual=" << actual << " rc=" << rc
                << (rc != 0 ? libusb_error_name(rc) : "");
#endif
    return rc == 0;
}
