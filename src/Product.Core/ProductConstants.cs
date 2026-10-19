namespace OnimushaPS5Adapter.Core;

public static class ProductConstants
{
    public const string DisplayName = "鬼武者 PS5 手柄适配工具（非官方）";
    public const string ProductVersion = "2.3";
    public const string BridgeBaselineVersion = "4.1";
    public static string VersionDisplay =>
        $"{ProductVersion}  ·  内核 {BridgeBaselineVersion}";
    public const string ProductDirectoryName = "OnimushaPS5Adapter";
    public const int SteamAppId = 2638890;
}

public static class ProductPaths
{
    public static string LocalDataDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        ProductConstants.ProductDirectoryName);

    public static string MachineDataDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
        ProductConstants.ProductDirectoryName);

    public static string SettingsFile => Path.Combine(LocalDataDirectory, "settings.json");
    public static string InstallManifestFile => Path.Combine(MachineDataDirectory, "install-manifest.json");
}
