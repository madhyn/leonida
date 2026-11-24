// Why a lost Vulkan device was lost, as the driver tells it through VK_EXT_device_fault.
//
// A lost device says only VK_ERROR_DEVICE_LOST. With the extension enabled the driver also keeps a short report: a
// description, the addresses of the faults it saw (a shader's read or write of memory that is not there, for one) and
// its own vendor codes. device_fault_lines() turns that report into the lines the layer prints after its "the device
// was lost" line (vulkan_backend.h, set_device_lost). It takes the query as a callable so a test can stand in for the
// driver; the layer passes vkGetDeviceFaultInfoEXT bound to its device.
//
// The vendor binary (a crash dump for the vendor's own tools) is not asked for: its size is set to 0 in the second
// call, and the deviceFaultVendorBinary feature is left off at device creation.
#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace leonida {

#ifdef VK_EXT_device_fault

// More entries than this are not printed; the line that follows the last one says how many were left out.
constexpr std::uint32_t kDeviceFaultPrintLimit = 16;

inline const char* device_fault_address_type(VkDeviceFaultAddressTypeEXT type) {
    switch (type) {
        case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "no address";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "read of an invalid address";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "write to an invalid address";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "execution of an invalid address";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "instruction pointer, cause unknown";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "invalid instruction pointer";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "instruction pointer of a fault";
        default: return "address fault of an unknown type";
    }
}

// The lines of a driver's fault report, without a prefix or a newline. `query(counts, info)` is
// vkGetDeviceFaultInfoEXT for one device: called once with a null info for the counts and once more for the report.
// Always at least one line: a query that fails, or a report with nothing in it, is said as such.
template <typename Query>
std::vector<std::string> device_fault_lines(Query&& query) {
    std::vector<std::string> lines;
    char text[512];
    VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult result = query(&counts, nullptr);
    if (result != VK_SUCCESS) {
        std::snprintf(text, sizeof text, "the driver gave no fault report (vkGetDeviceFaultInfoEXT returned %d)",
                      static_cast<int>(result));
        lines.emplace_back(text);
        return lines;
    }
    const std::uint32_t reported_addresses = counts.addressInfoCount, reported_vendors = counts.vendorInfoCount;
    counts.addressInfoCount = std::min(reported_addresses, kDeviceFaultPrintLimit);
    counts.vendorInfoCount = std::min(reported_vendors, kDeviceFaultPrintLimit);
    counts.vendorBinarySize = 0;
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
    info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
    result = query(&counts, &info);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {  // INCOMPLETE: more than was asked for, by design here
        std::snprintf(text, sizeof text, "the driver gave no fault report (vkGetDeviceFaultInfoEXT returned %d)",
                      static_cast<int>(result));
        lines.emplace_back(text);
        return lines;
    }
    // The driver writes back how many entries it filled; never read past what was allocated.
    const std::uint32_t address_count = std::min<std::uint32_t>(counts.addressInfoCount, std::uint32_t(addresses.size()));
    const std::uint32_t vendor_count = std::min<std::uint32_t>(counts.vendorInfoCount, std::uint32_t(vendors.size()));
    info.description[VK_MAX_DESCRIPTION_SIZE - 1] = '\0';
    std::snprintf(text, sizeof text, "the driver's fault report (VK_EXT_device_fault): %s",
                  info.description[0] ? info.description : "no description");
    lines.emplace_back(text);
    for (std::uint32_t i = 0; i < address_count; ++i) {
        // The fault is somewhere in [reportedAddress & ~(precision - 1), that + precision): precision is a power of two.
        std::snprintf(text, sizeof text, "  address fault %u: %s, address 0x%llx, precision 0x%llx", i,
                      device_fault_address_type(addresses[i].addressType),
                      static_cast<unsigned long long>(addresses[i].reportedAddress),
                      static_cast<unsigned long long>(addresses[i].addressPrecision));
        lines.emplace_back(text);
    }
    for (std::uint32_t i = 0; i < vendor_count; ++i) {
        vendors[i].description[VK_MAX_DESCRIPTION_SIZE - 1] = '\0';
        std::snprintf(text, sizeof text, "  vendor fault %u: %s, code 0x%llx, data 0x%llx", i,
                      vendors[i].description[0] ? vendors[i].description : "no description",
                      static_cast<unsigned long long>(vendors[i].vendorFaultCode),
                      static_cast<unsigned long long>(vendors[i].vendorFaultData));
        lines.emplace_back(text);
    }
    if (!address_count && !vendor_count) lines.emplace_back("  the report names no address and no vendor fault");
    if (reported_addresses > address_count || reported_vendors > vendor_count) {
        std::snprintf(text, sizeof text, "  not printed: %u more address fault(s), %u more vendor fault(s)",
                      reported_addresses - address_count, reported_vendors - vendor_count);
        lines.emplace_back(text);
    }
    return lines;
}

#endif  // VK_EXT_device_fault

}  // namespace leonida
