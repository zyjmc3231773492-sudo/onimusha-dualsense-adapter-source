#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace dualsense_hid_report {

inline constexpr size_t LogicalReportLength = 48;
inline constexpr size_t MaximumTransportReportLength = 64;

using LogicalReport = std::array<uint8_t, LogicalReportLength>;
using TransportReport = std::array<uint8_t, MaximumTransportReportLength>;

inline constexpr bool IsSupportedTransportLength(size_t length) {
    return length == LogicalReportLength ||
        length == MaximumTransportReportLength;
}

inline bool PrepareTransportReport(
    const LogicalReport& logicalReport,
    size_t transportLength,
    TransportReport& transportReport) {
    if (!IsSupportedTransportLength(transportLength)) return false;

    transportReport.fill(0);
    std::copy(logicalReport.begin(), logicalReport.end(), transportReport.begin());
    return true;
}

} // namespace dualsense_hid_report
