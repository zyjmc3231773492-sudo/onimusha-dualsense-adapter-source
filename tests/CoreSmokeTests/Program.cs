using OnimushaPS5Adapter.Core;


var failures = new List<string>();
void Check(bool condition, string name)
{
    if (!condition)
        failures.Add(name);
}

Check(
    ProductConstants.VersionDisplay ==
        $"{ProductConstants.ProductVersion}  ·  内核 {ProductConstants.BridgeBaselineVersion}",
    "管理器显示版本必须由当前产品和内核版本常量生成");

var defaultSettings = new ProductSettings();
Check(defaultSettings.ControllerSpeakerEnabled, "手柄扬声器总开关应默认开启");
Check(defaultSettings.HapticIntensityPercent == 100, "高清触觉强度应默认保持当前手感");
Check(
    defaultSettings.GuardHapticEnabled &&
    defaultSettings.ParryHapticEnabled &&
    defaultSettings.DeflectHapticEnabled &&
    defaultSettings.IssenHapticEnabled &&
    defaultSettings.GuardHapticIntensityPercent == 100 &&
    defaultSettings.ParryHapticIntensityPercent == 100 &&
    defaultSettings.DeflectHapticIntensityPercent == 100 &&
    defaultSettings.IssenHapticIntensityPercent == 100,
    "格挡、弹开和一闪触觉应默认开启并保持当前强度");
Check(
    defaultSettings.LeftAdaptiveTriggerEnabled &&
    defaultSettings.RightAdaptiveTriggerEnabled,
    "左右自适应扳机应默认独立开启");
Check(
    defaultSettings.GauntletVoiceLanguage == ProductSettings.ChineseGauntletVoice,
    "旧设置和默认设置应继续使用中文笼手语音");
Check(
    defaultSettings.GauntletVoiceVolumePercent == 100 &&
    defaultSettings.SoulSpeakerVolumePercent == 100 &&
    defaultSettings.ClashSpeakerVolumePercent == 100 &&
    defaultSettings.GuardSpeakerVolumePercent == 100 &&
    defaultSettings.DeflectSpeakerVolumePercent == 100 &&
    defaultSettings.ExecutionSpeakerVolumePercent == 100 &&
    defaultSettings.IssenSpeakerVolumePercent == 100,
    "所有扬声器分类应默认保持当前适中音量");
var japaneseSettings = new ProductSettings { GauntletVoiceLanguage = "JA-jp" };
japaneseSettings.Normalize();
Check(
    japaneseSettings.GauntletVoiceLanguage == ProductSettings.JapaneseGauntletVoice,
    "日语设置应规范为 ja-JP");
var invalidSettings = new ProductSettings { GauntletVoiceLanguage = "unsupported" };
invalidSettings.Normalize();
Check(
    invalidSettings.GauntletVoiceLanguage == ProductSettings.ChineseGauntletVoice,
    "不支持的语音语言应安全回退为中文");
var clampedVolumeSettings = new ProductSettings
{
    GauntletVoiceVolumePercent = -5,
    SoulSpeakerVolumePercent = 245,
    HapticIntensityPercent = 220,
    GuardHapticIntensityPercent = -10,
    ParryHapticIntensityPercent = 230,
    DeflectHapticIntensityPercent = 240,
    IssenHapticIntensityPercent = 205
};
clampedVolumeSettings.Normalize();
Check(
    clampedVolumeSettings.GauntletVoiceVolumePercent == 0 &&
    clampedVolumeSettings.SoulSpeakerVolumePercent == 200 &&
    clampedVolumeSettings.HapticIntensityPercent == 200 &&
    clampedVolumeSettings.GuardHapticIntensityPercent == 0 &&
    clampedVolumeSettings.ParryHapticIntensityPercent == 200 &&
    clampedVolumeSettings.DeflectHapticIntensityPercent == 200 &&
    clampedVolumeSettings.IssenHapticIntensityPercent == 200,
    "音量和触觉强度应限制在 0% 到 200%");

var bridgeSyncRoot = Path.Combine(
    Path.GetTempPath(),
    "OnimushaPS5Adapter-BridgeSyncSmoke-" + Guid.NewGuid().ToString("N"));
try
{
    var bridgePayload = Path.Combine(bridgeSyncRoot, "payload", "bin");
    var bridgePlugins = Path.Combine(
        bridgeSyncRoot, "game", "reframework", "plugins");
    Directory.CreateDirectory(bridgePayload);
    Directory.CreateDirectory(bridgePlugins);
    var bridgeName = "codex_dualsense_bridge.dll";
    var helperName = "codex_dualsense_hid_helper.exe";
    File.WriteAllBytes(Path.Combine(bridgePayload, bridgeName), [1, 3, 5, 7, 9]);
    File.WriteAllBytes(Path.Combine(bridgePayload, helperName), [2, 4, 6, 8, 10]);
    File.WriteAllBytes(Path.Combine(bridgePlugins, bridgeName), [9, 7, 5, 3, 1]);
    File.WriteAllBytes(Path.Combine(bridgePlugins, helperName), [2, 4, 6, 8, 10]);
    var staleBridge = InstallService.VerifyBridgeSynchronization(
        Path.Combine(bridgeSyncRoot, "game"),
        Path.Combine(bridgeSyncRoot, "payload"));
    Check(!staleBridge.IsSynchronized, "覆盖前的旧桥接必须被同步校验识别");

    File.Copy(
        Path.Combine(bridgePayload, bridgeName),
        Path.Combine(bridgePlugins, bridgeName),
        true);
    var synchronizedBridge = InstallService.VerifyBridgeSynchronization(
        Path.Combine(bridgeSyncRoot, "game"),
        Path.Combine(bridgeSyncRoot, "payload"));
    Check(synchronizedBridge.IsSynchronized, "正确覆盖后桥接同步校验应通过");

    File.WriteAllBytes(Path.Combine(bridgePlugins, helperName), [10, 8, 6, 4, 2]);
    var staleHelper = InstallService.VerifyBridgeSynchronization(
        Path.Combine(bridgeSyncRoot, "game"),
        Path.Combine(bridgeSyncRoot, "payload"));
    Check(!staleHelper.IsSynchronized, "覆盖后被替换的辅助程序必须被同步校验识别");
}
finally
{
    if (Directory.Exists(bridgeSyncRoot))
        Directory.Delete(bridgeSyncRoot, true);
}

// Production assemblies no longer expose or compile activation or machine identity code.
var coreAssembly = typeof(ProductSettings).Assembly;
foreach (var historicalType in new[] { "EntitlementService", "MachineIdentity", "LicenseCodec", "DeviceCodeCodec" })
    Check(coreAssembly.GetType("OnimushaPS5Adapter.Core." + historicalType) is null,
        $"免费运行时不应包含 {historicalType}");
Check(typeof(ProductPaths).GetProperty("LicenseFile") is null &&
      typeof(ProductPaths).GetProperty("TrialStatusFile") is null &&
      typeof(ProductPaths).GetProperty("DeviceSeedFile") is null,
      "免费运行时不应暴露授权、试用或设备种子路径");

var steamFixtureRoot = Path.GetFullPath(Path.Combine(Path.GetTempPath(),
    "OnimushaPS5Adapter-SteamLocatorSmoke-" + Guid.NewGuid().ToString("N")));
try
{
    var steamRoot = Path.Combine(steamFixtureRoot, "Steam");
    var secondLibrary = Path.Combine(steamFixtureRoot, "OtherLibrary");
    var firstGame = Path.Combine(steamRoot, "steamapps", "common", "OnimushaWotS");
    var renamedGame = Path.Combine(secondLibrary, "steamapps", "common", "Renamed Onimusha");
    Directory.CreateDirectory(firstGame);
    File.WriteAllBytes(Path.Combine(firstGame, "OnimushaWotS.exe"), [0]);
    var libraryFile = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
    string VdfPath(string value) => value.Replace("\\", "\\\\");
    File.WriteAllText(libraryFile,
        "\"libraryfolders\" { \"0\" { \"path\" \"" + VdfPath(steamRoot) + "\" } }");
    var rootAliases = new[]
    {
        steamRoot.Replace('\\', '/'),
        steamRoot.ToUpperInvariant() + "\\",
        Path.Combine(steamRoot, ".") + "/"
    };
    var aliasDetection = SteamLocator.FindGameDirectories(rootAliases);
    Check(aliasDetection.Count == 1 &&
          string.Equals(aliasDetection[0], firstGame, StringComparison.OrdinalIgnoreCase) &&
          !aliasDetection[0].Contains('/') && !Path.EndsInDirectorySeparator(aliasDetection[0]),
          "同一 Steam 目录的大小写、斜杠和尾部分隔符应归一化为一个游戏路径");

    Directory.CreateDirectory(renamedGame);
    File.WriteAllBytes(Path.Combine(renamedGame, "OnimushaWotS.exe"), [0]);
    var secondManifest = Path.Combine(secondLibrary, "steamapps", $"appmanifest_{ProductConstants.SteamAppId}.acf");
    File.WriteAllText(secondManifest, "\"AppState\" { \"installdir\" \"Renamed Onimusha\" }");
    File.WriteAllText(libraryFile,
        "\"libraryfolders\" { \"0\" { \"path\" \"" + VdfPath(steamRoot) +
        "\" } \"1\" { \"path\" \"" + VdfPath(secondLibrary) +
        "\" } \"2\" { \"path\" \"" + secondLibrary.Replace('\\', '/').ToUpperInvariant() + "/\" } }");
    var distinctDetection = SteamLocator.FindGameDirectories(rootAliases);
    Check(distinctDetection.Count == 2 &&
          distinctDetection.Contains(firstGame, StringComparer.OrdinalIgnoreCase) &&
          distinctDetection.Contains(renamedGame, StringComparer.OrdinalIgnoreCase),
          "两个真实 Steam 库应保留两个游戏，并按应用清单识别更名目录");

    var fallbackGame = Path.Combine(secondLibrary, "steamapps", "common", "OnimushaWotS");
    Directory.CreateDirectory(fallbackGame);
    File.WriteAllBytes(Path.Combine(fallbackGame, "OnimushaWotS.exe"), [0]);
    Check(SteamLocator.FindGameDirectories([secondLibrary]).SequenceEqual([renamedGame], StringComparer.OrdinalIgnoreCase),
          "清单有效时应优先使用清单目录，不把同库旧默认文件夹误判为另一份安装");
    using (var lockedManifest = new FileStream(secondManifest, FileMode.Open, FileAccess.Read, FileShare.None))
        Check(SteamLocator.FindGameDirectories([secondLibrary]).SequenceEqual([fallbackGame], StringComparer.OrdinalIgnoreCase),
              "应用清单不可读取时应继续检测安全的默认目录");
    File.Delete(secondManifest);
    Check(SteamLocator.FindGameDirectories([secondLibrary]).SequenceEqual([fallbackGame], StringComparer.OrdinalIgnoreCase),
          "清单缺失时应检测原默认目录");

    using (var lockedLibraryFile = new FileStream(libraryFile, FileMode.Open, FileAccess.Read, FileShare.None))
    {
        var survivingRoots = SteamLocator.FindGameDirectories([steamRoot, secondLibrary, "invalid\0path"]);
        Check(survivingRoots.Count == 2 && survivingRoots.Contains(fallbackGame, StringComparer.OrdinalIgnoreCase),
              "一个根目录的库清单读取失败或路径无效不应阻断其他根目录检测");
    }

    var unsafeLibrary = Path.Combine(steamFixtureRoot, "UnsafeLibrary");
    var unsafeApps = Path.Combine(unsafeLibrary, "steamapps");
    var outsideCommon = Path.Combine(unsafeApps, "common-other", "escaped");
    Directory.CreateDirectory(outsideCommon);
    File.WriteAllBytes(Path.Combine(outsideCommon, "OnimushaWotS.exe"), [0]);
    var unsafeManifest = Path.Combine(unsafeApps, $"appmanifest_{ProductConstants.SteamAppId}.acf");
    foreach (var unsafeName in new[] { "../common-other/escaped", outsideCommon })
    {
        File.WriteAllText(unsafeManifest, "\"AppState\" { \"installdir\" \"" + VdfPath(unsafeName) + "\" }");
        Check(SteamLocator.FindGameDirectories([unsafeLibrary]).Count == 0,
              "应用清单不应允许绝对路径或跳出 common 目录的相邻路径");
    }
}
finally
{
    var tempRoot = Path.GetFullPath(Path.GetTempPath());
    if (!steamFixtureRoot.StartsWith(tempRoot, StringComparison.OrdinalIgnoreCase) ||
        !Path.GetFileName(steamFixtureRoot).StartsWith("OnimushaPS5Adapter-SteamLocatorSmoke-", StringComparison.Ordinal))
        throw new InvalidOperationException("Steam 测试目录超出临时目录，拒绝清理。");
    if (Directory.Exists(steamFixtureRoot))
        Directory.Delete(steamFixtureRoot, true);
}

var freeInstallRoot = Path.GetFullPath(Path.Combine(
    Path.GetTempPath(), "OnimushaPS5Adapter-FreeInstallSmoke-" + Guid.NewGuid().ToString("N")));
try
{
    var gameRoot = Path.Combine(freeInstallRoot, "game");
    var payloadRoot = Path.Combine(freeInstallRoot, "payload");
    var machineRoot = Path.Combine(freeInstallRoot, "old-machine-state");
    var manifestFile = Path.Combine(freeInstallRoot, "installation", "install-manifest.json");
    var settingsFile = Path.Combine(freeInstallRoot, "user-settings", "settings.json");
    Directory.CreateDirectory(gameRoot);
    File.WriteAllBytes(Path.Combine(gameRoot, "OnimushaWotS.exe"), [0]);
    foreach (var folder in new[] { "reframework", "bin", "haptics", "gauntlet_voice", "gauntlet_voice/ja-JP" })
        Directory.CreateDirectory(Path.Combine(payloadRoot, folder));
    File.WriteAllBytes(Path.Combine(payloadRoot, "reframework", "dinput8.dll"), [1, 2, 3]);
    File.WriteAllBytes(Path.Combine(payloadRoot, "bin", "codex_dualsense_bridge.dll"), [4, 5, 6]);
    File.WriteAllBytes(Path.Combine(payloadRoot, "bin", "codex_dualsense_hid_helper.exe"), [7, 8, 9]);
    File.WriteAllBytes(Path.Combine(payloadRoot, "haptics", "test.wav"), [10, 11, 12]);
    File.WriteAllText(Path.Combine(payloadRoot, "gauntlet_voice", "manifest.json"), "[]");
    File.WriteAllText(Path.Combine(payloadRoot, "gauntlet_voice", "ja-JP", "manifest.json"), "[]");
    var install = new InstallService(manifestFile, settingsFile);
    Check(install.InstallOrRepair(gameRoot, payloadRoot).Success,
        "没有授权或试用记录时也应安装成功");
    Check(!Directory.Exists(machineRoot), "安装不应创建旧授权记录目录");
    Check(InstallService.LoadManifest(manifestFile)?.GameRoot == gameRoot,
        "安装必须保留用于校验和卸载的组件清单");

    foreach (var trialText in new[] { "{broken json", "{\"usedSeconds\":600,\"consumed\":true}" })
    {
        Directory.CreateDirectory(machineRoot);
        var oldRecords = new Dictionary<string, string>
        {
            ["license.dat"] = "invalid activation",
            ["trial-state.bin"] = "unreadable legacy state",
            ["trial-status.json"] = trialText
        };
        foreach (var record in oldRecords)
        {
            var path = Path.Combine(machineRoot, record.Key);
            if (File.Exists(path)) File.SetAttributes(path, FileAttributes.Normal);
            File.WriteAllText(path, record.Value);
            File.SetAttributes(path, FileAttributes.ReadOnly);
        }
        Check(install.InstallOrRepair(gameRoot, payloadRoot).Success,
            "旧版已过期或损坏记录不应阻止安装和修复");
        foreach (var record in oldRecords)
            Check(File.ReadAllText(Path.Combine(machineRoot, record.Key)) == record.Value,
                $"修复不应读取、修改或删除旧记录 {record.Key}");
        Check(!File.Exists(Path.Combine(machineRoot, "device-seed.dat")),
            "修复不应创建本机设备身份");
    }
    Check(install.UninstallBridge().Success, "免费版安装的桥接应可正常卸载");
    Check(!File.Exists(manifestFile), "卸载应删除自己的组件清单");
    Check(File.Exists(Path.Combine(machineRoot, "license.dat")) &&
          File.Exists(Path.Combine(machineRoot, "trial-status.json")),
          "卸载不应访问旧版授权或试用记录");
}
finally
{
    var tempRoot = Path.GetFullPath(Path.GetTempPath());
    if (!freeInstallRoot.StartsWith(tempRoot, StringComparison.OrdinalIgnoreCase) ||
        !Path.GetFileName(freeInstallRoot).StartsWith("OnimushaPS5Adapter-FreeInstallSmoke-", StringComparison.Ordinal))
        throw new InvalidOperationException("测试目录超出临时目录，拒绝清理。");
    if (Directory.Exists(freeInstallRoot))
    {
        foreach (var file in Directory.EnumerateFiles(freeInstallRoot, "*", SearchOption.AllDirectories))
            File.SetAttributes(file, FileAttributes.Normal);
        Directory.Delete(freeInstallRoot, true);
    }
}
if (failures.Count > 0)
{
    foreach (var failure in failures)
        Console.Error.WriteLine("FAIL: " + failure);
    return 1;
}

Console.WriteLine("Core smoke tests passed.");
return 0;
