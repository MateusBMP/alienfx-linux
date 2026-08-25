#include "hid_report_descriptor.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace AlienFX_SDK {

namespace fs = std::filesystem;

std::vector<HidCollection> ParseTopLevelCollections(const uint8_t* desc,
                                                     size_t len) {
    std::vector<HidCollection> result;
    // Index into `result` for each currently-open collection, outermost
    // first; a nested (non-top-level) collection pushes a copy of its
    // parent's index, so items inside it fold into the enclosing top-level
    // collection -- no AlienFX-family descriptor observed nests deeper than
    // one Application collection, but this stays correct if one does.
    std::vector<size_t> stack;

    uint16_t usage_page = 0;
    uint32_t report_size = 0, report_count = 0;
    uint8_t report_id = 0;
    std::vector<uint32_t> local_usages;

    // Note: HID Push/Pop (global tags 0xA/0xB) aren't handled -- no
    // AlienFX-family report descriptor observed uses them, and getting this
    // wrong only means a usage/length is misread, not a claim taken.
    size_t i = 0;
    while (i < len) {
        uint8_t item = desc[i++];
        if (item == 0xFE) {  // long item: bTag(0xFE) bSize bLongItemTag data...
            if (i + 1 > len) break;
            uint8_t dataSize = desc[i];
            i += 2 + dataSize;
            continue;
        }
        int size = item & 0x3;
        if (size == 3) size = 4;
        int type = (item >> 2) & 0x3;  // 0=Main, 1=Global, 2=Local
        int tag = (item >> 4) & 0xF;
        if (i + (size_t)size > len) break;
        uint32_t data = 0;
        for (int b = 0; b < size; b++)
            data |= (uint32_t)desc[i + b] << (8 * b);
        i += size;

        if (type == 1) {  // Global
            switch (tag) {
                case 0x0:
                    usage_page = (uint16_t)data;
                    break;
                case 0x7:
                    report_size = data;
                    break;
                case 0x8:
                    report_id = (uint8_t)data;
                    break;
                case 0x9:
                    report_count = data;
                    break;
                default:
                    break;
            }
        } else if (type == 2) {  // Local
            if (tag == 0x0) local_usages.push_back(data);  // Usage
        } else {                                           // Main
            switch (tag) {
                case 0x8:  // Input
                case 0x9:  // Output
                case 0xB: {  // Feature
                    if (!stack.empty()) {
                        HidCollection& c = result[stack.back()];
                        if (c.report_id == 0) c.report_id = report_id;
                        int bits = (int)(report_size * report_count);
                        int bytes = (bits + 7) / 8;
                        int* target = tag == 0x8   ? &c.input_bytes
                                     : tag == 0x9   ? &c.output_bytes
                                                     : &c.feature_bytes;
                        *target += bytes;
                    }
                    local_usages.clear();
                    break;
                }
                case 0xA: {  // Collection
                    HidCollection c{};
                    c.usage_page = usage_page;
                    c.usage =
                        local_usages.empty() ? 0 : (uint16_t)local_usages.front();
                    c.report_id = report_id;
                    if (stack.empty()) {
                        result.push_back(c);
                        stack.push_back(result.size() - 1);
                    } else {
                        // Nested collection: fold into the enclosing
                        // top-level one.
                        stack.push_back(stack.back());
                    }
                    local_usages.clear();
                    break;
                }
                case 0xC:  // End Collection
                    if (!stack.empty()) stack.pop_back();
                    local_usages.clear();
                    break;
                default:
                    local_usages.clear();
                    break;
            }
        }
    }

    // Report ID byte: Windows' *ReportByteLength fields, which this mirrors,
    // include the leading Report ID byte for numbered reports (see the
    // reportIDList[version]==0 compensation this feeds into in
    // AlienFXProbeDevice); the bit-accumulation above only counts the
    // report's data bits, so add it here once per report type actually
    // used.
    for (auto& c : result) {
        if (c.report_id == 0) continue;
        if (c.input_bytes) c.input_bytes += 1;
        if (c.output_bytes) c.output_bytes += 1;
        if (c.feature_bytes) c.feature_bytes += 1;
    }

    return result;
}

// Builds the sysfs directory hid-core creates for one USB interface, e.g.
// /sys/bus/usb/devices/3-8:1.1 for bus 3, port chain [8], config 1,
// interface 1. Returns "" if the device's port chain can't be read (e.g. a
// virtual/root-hub device, which no AlienFX hardware is).
static std::string BuildUsbInterfaceSysfsPath(libusb_device* usbDev,
                                               uint8_t bConfigurationValue,
                                               int interfaceNumber) {
    uint8_t ports[8];
    int n = libusb_get_port_numbers(usbDev, ports, sizeof(ports));
    if (n <= 0) return {};
    std::ostringstream oss;
    oss << "/sys/bus/usb/devices/" << (int)libusb_get_bus_number(usbDev);
    for (int i = 0; i < n; i++) oss << (i == 0 ? "-" : ".") << (int)ports[i];
    oss << ":" << (int)bConfigurationValue << "." << interfaceNumber;
    return oss.str();
}

static bool ReadFileBytes(const fs::path& p, std::vector<uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return !out->empty();
}

bool ReadReportDescriptor(libusb_device* usbDev, int interfaceNumber,
                          std::vector<uint8_t>* out) {
    out->clear();

    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_config_descriptor(usbDev, 0, &cfg) != 0 || !cfg)
        return false;
    std::string ifacePath = BuildUsbInterfaceSysfsPath(
        usbDev, cfg->bConfigurationValue, interfaceNumber);
    libusb_free_config_descriptor(cfg);

    // Primary: hid-core's own world-readable sysfs attribute. Present
    // whenever any kernel HID driver is bound to the interface; reading it
    // needs no open, no claim, no detach -- it can't disturb whatever
    // already owns the interface, which is exactly the case this exists
    // for (e.g. reading the Darfon AlienFX collection's descriptor without
    // touching the keyboard interface it shares).
    if (!ifacePath.empty()) {
        std::error_code ec;
        if (fs::is_directory(ifacePath, ec)) {
            for (auto& entry : fs::directory_iterator(ifacePath, ec)) {
                if (ec || !entry.is_directory()) continue;
                if (ReadFileBytes(entry.path() / "report_descriptor", out))
                    return true;
            }
        }
    }

    // Fallback: only for interfaces with no kernel driver bound at all --
    // claiming one then is harmless because there is no driver ownership to
    // disturb. If a driver *is* bound, hid-core should already have created
    // the sysfs node above; don't detach it just to double-check.
    libusb_device_handle* handle = nullptr;
    if (libusb_open(usbDev, &handle) != 0 || !handle) return false;
    bool ok = false;
    if (libusb_kernel_driver_active(handle, interfaceNumber) == 0 &&
        libusb_claim_interface(handle, interfaceNumber) == 0) {
        unsigned char buf[4096];
        int res = libusb_control_transfer(
            handle,
            (uint8_t)((uint8_t)LIBUSB_ENDPOINT_IN |
                      (uint8_t)LIBUSB_REQUEST_TYPE_STANDARD |
                      (uint8_t)LIBUSB_RECIPIENT_INTERFACE),
            LIBUSB_REQUEST_GET_DESCRIPTOR, (0x22 /* HID Report */ << 8) | 0,
            interfaceNumber, buf, sizeof(buf), 1000);
        if (res > 0) {
            out->assign(buf, buf + res);
            ok = true;
        }
        libusb_release_interface(handle, interfaceNumber);
    }
    libusb_close(handle);
    return ok;
}

}  // namespace AlienFX_SDK
