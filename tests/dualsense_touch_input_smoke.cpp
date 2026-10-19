#include <array>
#include <cstdint>
#include <cstdio>

#include "dualsense_touch_input.hpp"

using namespace dualsense_touch_input;

int main() {
    for (const bool bluetooth : {false, true}) {
        std::array<uint8_t, 78> report{};
        report[0] = bluetooth ? 0x31 : 0x01;
        const size_t first = (bluetooth ? 2 : 1) + 32;
        report[first + 4] = 0x80;
        Gesture gesture;
        const size_t length = bluetooth ? 78 : 64;
        report[first] = 7;
        report[first + 1] = 0x20; // x=800
        report[first + 2] = 0x03;
        if (gesture.Update(Parse(report.data(), length)) != Left) return 1;
        report[first + 1] = 0xB0; // x=1200, same contact remains left
        report[first + 2] = 0x04;
        if (gesture.Update(Parse(report.data(), length)) != Left) return 2;
        report[first + 4] = 9;
        report[first + 5] = 0xB0;
        report[first + 6] = 0x04;
        if (gesture.Update(Parse(report.data(), length)) != 0) return 3;
        report[first] |= 0x80;
        if (gesture.Update(Parse(report.data(), length)) != 0) return 4;
        report[first + 4] |= 0x80;
        if (gesture.Update(Parse(report.data(), length)) != 0) return 5;
        report[first] = 10;
        if (gesture.Update(Parse(report.data(), length)) != Right) return 6;
        if (gesture.Update(Parse(report.data(), 10)) != 0) return 7;
    }
    const uint64_t packed = Pack(1000, Right);
    if (FreshFlags(packed, 1100) != Right || FreshFlags(packed, 1200) != 0)
        return 8;
    std::puts("DualSense USB/Bluetooth touch input smoke tests passed.");
    return 0;
}
