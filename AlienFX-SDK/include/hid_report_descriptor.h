#pragma once
#include <cstdint>
#include <vector>

#include "libusb.h"

// Parses the HID report descriptor the same way Windows' HidP_GetCaps does,
// so AlienFXProbeDevice can select a device by *usage* (e.g. the AlienFX
// lighting collection, usage page 0xFF89 / usage 0xCC on Darfon keyboards)
// instead of by VID alone -- see AlienFXProbeDevice's case 0x0d62 in
// AlienFX_SDK.cpp. Upstream gets this from HIDP_CAPS on a per-collection
// Windows device node; Linux has no such node, only one report descriptor
// per USB interface covering every top-level collection on it, so this
// parses it directly.

namespace AlienFX_SDK {

// One top-level (Application) HID collection, with the byte length of each
// report type it declares -- the Linux equivalent of a single Windows
// HIDP_CAPS. Byte lengths already include the leading Report ID byte when
// the collection uses numbered reports (report_id != 0), matching how
// Windows' *ReportByteLength fields are used elsewhere in this SDK (see the
// reportIDList[version]==0 compensation in AlienFXProbeDevice).
struct HidCollection {
    uint16_t usage_page = 0;
    uint16_t usage = 0;
    uint8_t report_id = 0;
    int input_bytes = 0;
    int output_bytes = 0;
    int feature_bytes = 0;
};

// Walks a raw HID report descriptor (short items only -- long items, tag
// 0xFE, are skipped; no report descriptor observed on AlienFX-family
// hardware uses them) and returns one HidCollection per top-level
// Collection(Application). Items nested inside (Physical/Logical
// sub-collections, or bare Input/Output/Feature items with no enclosing
// collection) accumulate into whichever top-level collection contains them.
std::vector<HidCollection> ParseTopLevelCollections(const uint8_t* desc,
                                                     size_t len);

// Reads the report descriptor for one USB HID interface without opening an
// exclusive handle to the device where avoidable:
//
//   1. hid-core's own sysfs attribute --
//      /sys/bus/usb/devices/<bus>-<ports>:<config>.<iface>/<hid-device>/report_descriptor
//      -- world-readable, needs no open/claim/detach at all. Present
//      whenever a kernel HID driver (usbhid or otherwise) is bound to the
//      interface, which is the case this function exists for: reading the
//      descriptor of an interface a kernel driver already owns (e.g. the
//      keyboard interface a Darfon AlienFX collection lives on) without
//      touching that ownership.
//   2. USB GET_DESCRIPTOR(HID Report), standard/interface recipient, as a
//      fallback for interfaces with no kernel driver bound (nothing to
//      detach, so claiming is harmless) or no sysfs node reachable.
//
// Returns false if the descriptor could not be obtained via either path.
bool ReadReportDescriptor(libusb_device* usbDev, int interfaceNumber,
                          std::vector<uint8_t>* out);

}  // namespace AlienFX_SDK
