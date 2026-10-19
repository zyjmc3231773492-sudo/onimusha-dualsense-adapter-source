using OnimushaPS5Adapter.Core;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace OnimushaPS5Adapter.Manager;

internal sealed record DeviceConnectionSnapshot(
    bool IsConnected,
    bool DetectionAvailable,
    string Status,
    string Detail);

internal static class DeviceConnectionService
{
    private const uint DigcfPresent = 0x00000002;
    private const uint DigcfDeviceInterface = 0x00000010;
    private const uint FileShareRead = 0x00000001;
    private const uint FileShareWrite = 0x00000002;
    private const uint OpenExisting = 3;
    private const int HidpStatusSuccess = 0x00110000;
    private const int ErrorNoMoreItems = 259;
    private static readonly IntPtr InvalidHandleValue = new(-1);

    public static DeviceConnectionSnapshot GetSnapshot()
    {
        try
        {
            string? detectedModel = null;
            string? inspectionFailure = null;
#if ONIMUSHA_BLUETOOTH_ENABLED
            DeviceConnectionSnapshot? bluetoothSnapshot = null;
#endif
            foreach (var devicePath in EnumerateHidDevicePaths())
            {
                var inspection = InspectCollection(devicePath);
                if (!inspection.IsSupportedController) continue;
                var model = inspection.Model;
                detectedModel ??= model;
                if (inspection.IsCompatible)
                {
#if ONIMUSHA_BLUETOOTH_ENABLED
                    if (inspection.IsBluetooth)
                    {
                        bluetoothSnapshot ??= new DeviceConnectionSnapshot(true, true, UiText.Get("BluetoothHidReady"),
                            UiText.Get("0IsConnectedOverBluetoothAdaptiveTriggersStereoHapticsAnd", model));
                        continue; // matches the native helper's USB preference
                    }
#endif
                    return new DeviceConnectionSnapshot(
                        true,
                        true,
                        UiText.Get("HidReady"),
                        UiText.Get("Detected0UsbGamepadHidCollection1ByteOutputReport", model, inspection.OutputReportLength, ProductConstants.BridgeBaselineVersion));
                }

                if (!string.IsNullOrWhiteSpace(inspection.Error))
                    inspectionFailure = inspection.Error;
            }

#if ONIMUSHA_BLUETOOTH_ENABLED
            if (bluetoothSnapshot is not null) return bluetoothSnapshot;
#endif
            if (detectedModel is not null)
            {
                var reason = string.IsNullOrWhiteSpace(inspectionFailure)
                    ? UiText.Get("NoUsbGamepadHidCollectionWithUsagepage0x01Usage0x05")
                    : inspectionFailure;
                return new DeviceConnectionSnapshot(
                    false,
                    true,
                    UiText.Get("DetectedHidNotReady"),
#if ONIMUSHA_BLUETOOTH_ENABLED
                    UiText.Get("WindowsDetected0ButItsHidCollectionDidNotMatch", detectedModel));
#else
                    UiText.Get("WindowsDetected0ButThisConnectionCannotProvideFullSupport", detectedModel, reason));
#endif
            }

            return new DeviceConnectionSnapshot(
                false,
                true,
                UiText.Get("NotConnected"),
#if ONIMUSHA_BLUETOOTH_ENABLED
                UiText.Get("NoCompatibleDualsenseFoundConnectOverUsbOrPairIt"));
#else
                UiText.Get("NoCompatibleDualsenseUsbGamepadHidCollectionFoundConnectWith"));
#endif
        }
        catch (Exception exception)
        {
            return new DeviceConnectionSnapshot(
                false,
                false,
                UiText.Get("DetectionUnavailable"),
                UiText.Get("CouldNotReadWindowsHidDevices") + exception.Message);
        }
    }

    private static HidCollectionInspection InspectCollection(string devicePath)
    {
        var handle = CreateFile(
            devicePath,
            0,
            FileShareRead | FileShareWrite,
            IntPtr.Zero,
            OpenExisting,
            0,
            IntPtr.Zero);
        if (handle == InvalidHandleValue)
        {
            return new HidCollectionInspection(
                false,
                0,
                UiText.Get("CouldNotReadHidCollection") + new Win32Exception(Marshal.GetLastWin32Error()).Message + "。");
        }

        try
        {
            var attributes = new HidAttributes { Size = Marshal.SizeOf<HidAttributes>() };
            if (!HidD_GetAttributes(handle, ref attributes) || attributes.VendorId != 0x054C ||
                (attributes.ProductId != 0x0CE6 && attributes.ProductId != 0x0DF2))
                return new HidCollectionInspection(false, 0, null);
            var model = attributes.ProductId == 0x0DF2 ? "DualSense Edge" : "DualSense";
            if (!HidD_GetPreparsedData(handle, out var preparsedData))
            {
                return new HidCollectionInspection(
                    false,
                    0,
                    UiText.Get("CouldNotReadHidCapabilities") + new Win32Exception(Marshal.GetLastWin32Error()).Message + "。");
            }

            try
            {
                var status = HidP_GetCaps(preparsedData, out var caps);
                if (status != HidpStatusSuccess)
                    return new HidCollectionInspection(false, 0, UiText.Get("HidCapabilityQueryFailed0x0X8", status));

                var compatible = caps.UsagePage == 0x01 &&
                    caps.Usage == 0x05 &&
                    (caps.OutputReportByteLength == 48 || caps.OutputReportByteLength == 64);
                var bluetooth = false;
#if ONIMUSHA_BLUETOOTH_ENABLED
                bluetooth = caps.UsagePage == 0x01 && caps.Usage == 0x05 &&
                    caps.InputReportByteLength > 64 && caps.OutputReportByteLength >= 398 &&
                    caps.OutputReportByteLength <= 1024;
                compatible |= bluetooth;
#endif
                return new HidCollectionInspection(
                    compatible,
                    caps.OutputReportByteLength,
                    compatible ? null : string.Empty) {
                        IsSupportedController = true, Model = model, IsBluetooth = bluetooth
                    };
            }
            finally
            {
                HidD_FreePreparsedData(preparsedData);
            }
        }
        finally
        {
            CloseHandle(handle);
        }
    }

    private static IEnumerable<string> EnumerateHidDevicePaths()
    {
        HidD_GetHidGuid(out var hidGuid);
        var deviceInfoSet = SetupDiGetClassDevs(
            ref hidGuid,
            null,
            IntPtr.Zero,
            DigcfPresent | DigcfDeviceInterface);

        if (deviceInfoSet == InvalidHandleValue)
            throw new Win32Exception(Marshal.GetLastWin32Error());

        try
        {
            for (uint index = 0; ; index++)
            {
                var interfaceData = new SpDeviceInterfaceData
                {
                    Size = (uint)Marshal.SizeOf<SpDeviceInterfaceData>()
                };

                if (!SetupDiEnumDeviceInterfaces(
                        deviceInfoSet,
                        IntPtr.Zero,
                        ref hidGuid,
                        index,
                        ref interfaceData))
                {
                    var error = Marshal.GetLastWin32Error();
                    if (error == ErrorNoMoreItems)
                        yield break;
                    throw new Win32Exception(error);
                }

                SetupDiGetDeviceInterfaceDetail(
                    deviceInfoSet,
                    ref interfaceData,
                    IntPtr.Zero,
                    0,
                    out var requiredSize,
                    IntPtr.Zero);

                if (requiredSize == 0)
                    continue;

                var detailData = Marshal.AllocHGlobal((int)requiredSize);
                try
                {
                    Marshal.WriteInt32(detailData, IntPtr.Size == 8 ? 8 : 6);
                    if (!SetupDiGetDeviceInterfaceDetail(
                            deviceInfoSet,
                            ref interfaceData,
                            detailData,
                            requiredSize,
                            out _,
                            IntPtr.Zero))
                    {
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    }

                    var devicePath = Marshal.PtrToStringUni(IntPtr.Add(detailData, 4));
                    if (!string.IsNullOrWhiteSpace(devicePath))
                        yield return devicePath;
                }
                finally
                {
                    Marshal.FreeHGlobal(detailData);
                }
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(deviceInfoSet);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SpDeviceInterfaceData
    {
        public uint Size;
        public Guid InterfaceClassGuid;
        public uint Flags;
        public UIntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct HidpCaps
    {
        public ushort Usage;
        public ushort UsagePage;
        public ushort InputReportByteLength;
        public ushort OutputReportByteLength;
        public ushort FeatureReportByteLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)]
        public ushort[] Reserved;
        public ushort NumberLinkCollectionNodes;
        public ushort NumberInputButtonCaps;
        public ushort NumberInputValueCaps;
        public ushort NumberInputDataIndices;
        public ushort NumberOutputButtonCaps;
        public ushort NumberOutputValueCaps;
        public ushort NumberOutputDataIndices;
        public ushort NumberFeatureButtonCaps;
        public ushort NumberFeatureValueCaps;
        public ushort NumberFeatureDataIndices;
    }

    private sealed record HidCollectionInspection(
        bool IsCompatible,
        ushort OutputReportLength,
        string? Error) {
        public bool IsSupportedController { get; init; }
        public bool IsBluetooth { get; init; }
        public string Model { get; init; } = "DualSense";
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct HidAttributes {
        public int Size;
        public ushort VendorId;
        public ushort ProductId;
        public ushort VersionNumber;
    }

    [DllImport("hid.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool HidD_GetAttributes(IntPtr handle, ref HidAttributes attributes);

    [DllImport("hid.dll")]
    private static extern void HidD_GetHidGuid(out Guid hidGuid);

    [DllImport("hid.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool HidD_GetPreparsedData(
        IntPtr hidDeviceObject,
        out IntPtr preparsedData);

    [DllImport("hid.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool HidD_FreePreparsedData(IntPtr preparsedData);

    [DllImport("hid.dll")]
    private static extern int HidP_GetCaps(IntPtr preparsedData, out HidpCaps capabilities);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateFile(
        string fileName,
        uint desiredAccess,
        uint shareMode,
        IntPtr securityAttributes,
        uint creationDisposition,
        uint flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevs(
        ref Guid classGuid,
        string? enumerator,
        IntPtr parentWindow,
        uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiEnumDeviceInterfaces(
        IntPtr deviceInfoSet,
        IntPtr deviceInfoData,
        ref Guid interfaceClassGuid,
        uint memberIndex,
        ref SpDeviceInterfaceData deviceInterfaceData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(
        IntPtr deviceInfoSet,
        ref SpDeviceInterfaceData deviceInterfaceData,
        IntPtr deviceInterfaceDetailData,
        uint deviceInterfaceDetailDataSize,
        out uint requiredSize,
        IntPtr deviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);
}
