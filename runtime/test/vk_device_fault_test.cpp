// The lines the layer prints for a driver's VK_EXT_device_fault report (runtime/src/vulkan_device_fault.h), and that
// report_device_fault (runtime/src/vulkan_backend.h) prints nothing without the entry point:
//   1. A report with address and vendor faults: the description, one line per fault, in the driver's order.
//   2. The two-call shape: counts first with a null info, then the report with the vendor binary size set to 0.
//   3. A failed count query, and a failed report query, each give one line naming the result.
//   4. An empty report says so; a description that is not terminated is cut, not read past.
//   5. More faults than the print limit: the first ones, then how many were left out; the driver is never handed
//      more room than was allocated.
//   6. A driver that writes back larger counts than it was given room for is not read past the allocation.
//   7. report_device_fault with a null vkGetDeviceFaultInfoEXT (the extension is not enabled) prints nothing.
// A stand-in for the driver throughout: needs no GPU and no Vulkan loader. No game data.
#include "../src/vulkan_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::_Exit(1); } \
} while (0)

namespace {

struct FakeDriver {
    std::string description = "a page fault";
    std::vector<VkDeviceFaultAddressInfoEXT> addresses;
    std::vector<VkDeviceFaultVendorInfoEXT> vendors;
    VkResult count_result = VK_SUCCESS;
    VkResult report_result = VK_SUCCESS;
    bool terminate_description = true;
    std::uint32_t overstate_by = 0;  // written back on top of the true counts in the report call
    // What the caller did.
    int calls = 0;
    bool counts_call_had_null_info = false;
    VkDeviceSize binary_size_asked = ~VkDeviceSize{0};
    std::uint32_t address_room = 0, vendor_room = 0;

    VkResult operator()(VkDeviceFaultCountsEXT* counts, VkDeviceFaultInfoEXT* info) {
        ++calls;
        if (!info) {
            counts_call_had_null_info = true;
            if (count_result != VK_SUCCESS) return count_result;
            counts->addressInfoCount = std::uint32_t(addresses.size());
            counts->vendorInfoCount = std::uint32_t(vendors.size());
            counts->vendorBinarySize = 4096;  // a driver that has one; the layer must not ask for it
            return VK_SUCCESS;
        }
        binary_size_asked = counts->vendorBinarySize;
        address_room = counts->addressInfoCount;
        vendor_room = counts->vendorInfoCount;
        if (report_result != VK_SUCCESS && report_result != VK_INCOMPLETE) return report_result;
        std::memset(info->description, terminate_description ? 0 : 'x', sizeof info->description);
        if (terminate_description) std::snprintf(info->description, sizeof info->description, "%s", description.c_str());
        const std::uint32_t a = std::min<std::uint32_t>(address_room, std::uint32_t(addresses.size()));
        const std::uint32_t v = std::min<std::uint32_t>(vendor_room, std::uint32_t(vendors.size()));
        for (std::uint32_t i = 0; i < a; ++i) info->pAddressInfos[i] = addresses[i];
        for (std::uint32_t i = 0; i < v; ++i) info->pVendorInfos[i] = vendors[i];
        counts->addressInfoCount = a + overstate_by;
        counts->vendorInfoCount = v + overstate_by;
        return a < addresses.size() || v < vendors.size() ? VK_INCOMPLETE : report_result;
    }
};

VkDeviceFaultVendorInfoEXT vendor(const char* text, std::uint64_t code, std::uint64_t data) {
    VkDeviceFaultVendorInfoEXT v{};
    std::snprintf(v.description, sizeof v.description, "%s", text);
    v.vendorFaultCode = code;
    v.vendorFaultData = data;
    return v;
}

bool has(const std::vector<std::string>& lines, const char* part) {
    for (const std::string& line : lines)
        if (line.find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

int main() {
    using leonida::device_fault_lines;

    {   // 1 and 2
        FakeDriver driver;
        driver.addresses = {{VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x12345000, 0x1000},
                            {VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0xABCD0000, 0x10000}};
        driver.vendors = {vendor("channel error", 31, 0x55)};
        const auto lines = device_fault_lines(driver);
        EXPECT(driver.calls == 2);
        EXPECT(driver.counts_call_had_null_info);
        EXPECT(driver.binary_size_asked == 0);
        EXPECT(driver.address_room == 2 && driver.vendor_room == 1);
        EXPECT(lines.size() == 4);
        EXPECT(lines[0] == "the driver's fault report (VK_EXT_device_fault): a page fault");
        EXPECT(lines[1] == "  address fault 0: read of an invalid address, address 0x12345000, precision 0x1000");
        EXPECT(lines[2] == "  address fault 1: write to an invalid address, address 0xabcd0000, precision 0x10000");
        EXPECT(lines[3] == "  vendor fault 0: channel error, code 0x1f, data 0x55");
    }
    {   // 3: the count query fails
        FakeDriver driver;
        driver.count_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        const auto lines = device_fault_lines(driver);
        EXPECT(driver.calls == 1);
        EXPECT(lines.size() == 1);
        EXPECT(lines[0] == "the driver gave no fault report (vkGetDeviceFaultInfoEXT returned -1)");
    }
    {   // 3: the report query fails
        FakeDriver driver;
        driver.addresses = {{VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 1, 1}};
        driver.report_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        const auto lines = device_fault_lines(driver);
        EXPECT(driver.calls == 2);
        EXPECT(lines.size() == 1);
        EXPECT(has(lines, "returned -1"));
    }
    {   // 4: nothing in the report
        FakeDriver driver;
        driver.description.clear();
        const auto lines = device_fault_lines(driver);
        EXPECT(lines.size() == 2);
        EXPECT(lines[0] == "the driver's fault report (VK_EXT_device_fault): no description");
        EXPECT(lines[1] == "  the report names no address and no vendor fault");
    }
    {   // 4: a description without a terminator
        FakeDriver driver;
        driver.terminate_description = false;
        const auto lines = device_fault_lines(driver);
        EXPECT(lines.size() == 2);
        EXPECT(lines[0].size() < 600);   // one bounded line, however long the unterminated text
        EXPECT(has(lines, "xxxx"));
    }
    {   // 5: more than the print limit
        FakeDriver driver;
        for (std::uint32_t i = 0; i < leonida::kDeviceFaultPrintLimit + 5; ++i)
            driver.addresses.push_back({VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x1000 * (i + 1), 0x1000});
        for (std::uint32_t i = 0; i < leonida::kDeviceFaultPrintLimit + 2; ++i) driver.vendors.push_back(vendor("v", i, i));
        const auto lines = device_fault_lines(driver);
        EXPECT(driver.address_room == leonida::kDeviceFaultPrintLimit);
        EXPECT(driver.vendor_room == leonida::kDeviceFaultPrintLimit);
        EXPECT(lines.size() == 1 + 2 * leonida::kDeviceFaultPrintLimit + 1);
        EXPECT(lines.back() == "  not printed: 5 more address fault(s), 2 more vendor fault(s)");
        EXPECT(has(lines, "address fault 15:"));
        EXPECT(!has(lines, "address fault 16:"));
    }
    {   // 6: counts written back larger than the room given
        FakeDriver driver;
        driver.addresses = {{VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT, 0x40, 1}};
        driver.overstate_by = 1000;
        const auto lines = device_fault_lines(driver);
        EXPECT(lines.size() == 2);
        EXPECT(lines[1] == "  address fault 0: execution of an invalid address, address 0x40, precision 0x1");
    }
    {   // an address type this code does not know is still printed
        FakeDriver driver;
        driver.addresses = {{static_cast<VkDeviceFaultAddressTypeEXT>(0x7000), 0x8, 0x8}};
        EXPECT(has(device_fault_lines(driver), "address fault of an unknown type, address 0x8"));
    }
    {   // 7: no entry point, no report
        leonida::VulkanDeviceFunctions vk{};
        const unsigned before = leonida::device_fault_reports.load();
        leonida::report_device_fault(vk, reinterpret_cast<VkDevice>(std::uintptr_t{1}));
        EXPECT(leonida::device_fault_reports.load() == before);
    }
    std::puts("vk_device_fault: ok");
    return 0;
}
