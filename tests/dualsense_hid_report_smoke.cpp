#include <cstdint>
#include <cstdio>

#include "dualsense_hid_report.hpp"

int main() {
    dualsense_hid_report::LogicalReport logical{};
    for (size_t index = 0; index < logical.size(); ++index) {
        logical[index] = static_cast<uint8_t>(index + 1);
    }

    dualsense_hid_report::TransportReport transport{};
    if (!dualsense_hid_report::PrepareTransportReport(logical, 48, transport)) {
        std::fputs("FAIL: normal DualSense transport length rejected\n", stderr);
        return 2;
    }
    for (size_t index = 0; index < logical.size(); ++index) {
        if (transport[index] != logical[index]) {
            std::fputs("FAIL: 48-byte report payload changed\n", stderr);
            return 3;
        }
    }

    transport.fill(0xA5);
    if (!dualsense_hid_report::PrepareTransportReport(logical, 64, transport)) {
        std::fputs("FAIL: DualSense Edge transport length rejected\n", stderr);
        return 4;
    }
    for (size_t index = 0; index < logical.size(); ++index) {
        if (transport[index] != logical[index]) {
            std::fputs("FAIL: Edge padding changed the 48-byte effect payload\n", stderr);
            return 5;
        }
    }
    for (size_t index = logical.size(); index < transport.size(); ++index) {
        if (transport[index] != 0) {
            std::fputs("FAIL: Edge report padding is not zero-filled\n", stderr);
            return 6;
        }
    }

    if (dualsense_hid_report::PrepareTransportReport(logical, 47, transport) ||
        dualsense_hid_report::PrepareTransportReport(logical, 63, transport) ||
        dualsense_hid_report::PrepareTransportReport(logical, 65, transport)) {
        std::fputs("FAIL: unsupported HID transport length accepted\n", stderr);
        return 7;
    }

    std::puts("DualSense 48/64-byte HID report smoke tests passed.");
    return 0;
}
