using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace OnimushaPS5Adapter.Core;

public sealed class InstalledFileRecord
{
    public required string Path { get; set; }
    public required string Sha256 { get; set; }
    public bool OwnedByProduct { get; set; }
}

public sealed class ConfigValueBackup
{
    public bool Existed { get; set; }
    public string? Value { get; set; }
}

public sealed class InstallManifest
{
    public int SchemaVersion { get; set; } = 1;
    public string ProductVersion { get; set; } = ProductConstants.ProductVersion;
    public string BridgeBaselineVersion { get; set; } = ProductConstants.BridgeBaselineVersion;
    public required string GameRoot { get; set; }
    public DateTimeOffset InstalledAt { get; set; } = DateTimeOffset.UtcNow;
    public List<InstalledFileRecord> Files { get; set; } = [];
    public Dictionary<string, ConfigValueBackup> ReframeworkConfigBackup { get; set; } =
        new(StringComparer.OrdinalIgnoreCase);
}

[JsonSerializable(typeof(InstallManifest))]
[JsonSerializable(typeof(List<Dictionary<string, JsonElement>>))]
internal partial class InstallJsonContext : JsonSerializerContext;

public sealed record InstallResult(bool Success, string Message, InstallManifest? Manifest = null);
public sealed record BridgeSyncVerification(bool IsSynchronized, string Message);

public sealed class InstallService
{
    private readonly string _manifestFile;
    private readonly string _settingsFile;

    public InstallService(string? manifestFile = null, string? settingsFile = null)
    {
        _manifestFile = Path.GetFullPath(manifestFile ?? ProductPaths.InstallManifestFile);
        _settingsFile = Path.GetFullPath(settingsFile ?? ProductPaths.SettingsFile);
    }

    private static readonly string[] BridgeComponentNames =
    [
        "codex_dualsense_bridge.dll",
        "codex_dualsense_hid_helper.exe"
    ];

    private static readonly Dictionary<string, string> HiddenReframeworkValues =
        new(StringComparer.OrdinalIgnoreCase)
        {
            ["REFrameworkConfig_RememberMenuState"] = "true",
            ["REFrameworkConfig_MenuOpen"] = "false",
            ["REFrameworkConfig_MenuKey_V2"] = "45",
            ["ScriptRunner_OpenDebugConsoleAtStartup"] = "false"
        };

    public InstallResult InstallOrRepair(string gameRoot, string payloadRoot)
    {
        gameRoot = Path.GetFullPath(gameRoot);
        payloadRoot = Path.GetFullPath(payloadRoot);
        if (!File.Exists(Path.Combine(gameRoot, "OnimushaWotS.exe")))
            return new(false, "所选目录不是《鬼武者 剑之道》游戏目录。");
        if (Process.GetProcessesByName("OnimushaWotS").Length != 0)
            return new(false, "请先退出游戏再安装或修复。");

        var previous = LoadManifest(_manifestFile);
        var manifest = new InstallManifest
        {
            GameRoot = gameRoot,
            ReframeworkConfigBackup = previous is not null &&
                string.Equals(previous.GameRoot, gameRoot, StringComparison.OrdinalIgnoreCase)
                ? previous.ReframeworkConfigBackup
                : new(StringComparer.OrdinalIgnoreCase)
        };

        try
        {
            var pluginDirectory = Path.Combine(gameRoot, "reframework", "plugins");
            var dataDirectory = Path.Combine(gameRoot, "reframework", "data", "codex_dualsense_haptics");
            var voiceDirectory = Path.Combine(dataDirectory, "gauntlet_voice");
            Directory.CreateDirectory(pluginDirectory);
            Directory.CreateDirectory(dataDirectory);
            Directory.CreateDirectory(voiceDirectory);

            var loaderTarget = Path.Combine(gameRoot, "dinput8.dll");
            if (!File.Exists(loaderTarget))
            {
                CopyOwnedFile(Path.Combine(payloadRoot, "reframework", "dinput8.dll"), loaderTarget, manifest);
            }
            else
            {
                manifest.Files.Add(new InstalledFileRecord
                {
                    Path = loaderTarget,
                    Sha256 = HashFile(loaderTarget),
                    OwnedByProduct = previous?.Files.Any(file =>
                        file.OwnedByProduct &&
                        string.Equals(file.Path, loaderTarget, StringComparison.OrdinalIgnoreCase)) == true
                });
            }

            CopyOwnedFile(
                Path.Combine(payloadRoot, "bin", "codex_dualsense_bridge.dll"),
                Path.Combine(pluginDirectory, "codex_dualsense_bridge.dll"),
                manifest);
            CopyOwnedFile(
                Path.Combine(payloadRoot, "bin", "codex_dualsense_hid_helper.exe"),
                Path.Combine(pluginDirectory, "codex_dualsense_hid_helper.exe"),
                manifest);

            foreach (var source in Directory.EnumerateFiles(Path.Combine(payloadRoot, "haptics"), "*.wav"))
                CopyOwnedFile(source, Path.Combine(dataDirectory, Path.GetFileName(source)), manifest);

            var sourceVoiceDirectory = Path.Combine(payloadRoot, "gauntlet_voice");
            CopyVoiceSet(sourceVoiceDirectory, voiceDirectory, manifest);
            CopyVoiceSet(
                Path.Combine(sourceVoiceDirectory, ProductSettings.JapaneseGauntletVoice),
                Path.Combine(voiceDirectory, ProductSettings.JapaneseGauntletVoice),
                manifest);

            ApplyHiddenReframeworkConfig(gameRoot, manifest);
            var bridgeVerification = VerifyBridgeSynchronization(gameRoot, payloadRoot);
            if (!bridgeVerification.IsSynchronized)
                throw new IOException(bridgeVerification.Message);
            SaveManifest(manifest);
            return new(true,
                $"版本 {ProductConstants.ProductVersion} 已安装到游戏目录。\n" +
                bridgeVerification.Message,
                manifest);
        }
        catch (Exception exception)
        {
            return new(false, "安装失败：" + exception.Message);
        }
    }

    public InstallResult UninstallBridge()
    {
        var manifest = LoadManifest(_manifestFile);
        if (manifest is null)
            return new(true, "没有找到已安装的桥接组件。");
        if (Process.GetProcessesByName("OnimushaWotS").Length != 0)
            return new(false, "请先退出游戏再卸载。");

        try
        {
            foreach (var file in manifest.Files
                         .Where(item => item.OwnedByProduct)
                         .OrderByDescending(item => item.Path.Length))
            {
                if (!File.Exists(file.Path))
                    continue;
                if (!string.Equals(HashFile(file.Path), file.Sha256, StringComparison.OrdinalIgnoreCase))
                    continue;
                File.Delete(file.Path);
            }

            RestoreReframeworkConfig(manifest);
            TryDeleteEmptyDirectories(manifest.GameRoot);
            if (File.Exists(_settingsFile))
                File.Delete(_settingsFile);
            if (File.Exists(_manifestFile))
                File.Delete(_manifestFile);
            return new(true, "桥接组件已卸载。");
        }
        catch (Exception exception)
        {
            return new(false, "卸载失败：" + exception.Message);
        }
    }

    public static InstallManifest? LoadManifest()
        => LoadManifest(ProductPaths.InstallManifestFile);

    public static InstallManifest? LoadManifest(string manifestFile)
    {
        try
        {
            if (!File.Exists(manifestFile))
                return null;
            return JsonSerializer.Deserialize(
                File.ReadAllText(manifestFile),
                InstallJsonContext.Default.InstallManifest);
        }
        catch
        {
            return null;
        }
    }

    public static BridgeSyncVerification VerifyBridgeSynchronization(
        string gameRoot,
        string payloadRoot)
    {
        try
        {
            gameRoot = Path.GetFullPath(gameRoot);
            payloadRoot = Path.GetFullPath(payloadRoot);
            var pluginDirectory = Path.Combine(gameRoot, "reframework", "plugins");
            foreach (var name in BridgeComponentNames)
            {
                var source = Path.Combine(payloadRoot, "bin", name);
                var destination = Path.Combine(pluginDirectory, name);
                if (!File.Exists(source))
                    return new(false, $"桥接同步校验失败：安装负载缺少 {name}。");
                if (!File.Exists(destination))
                    return new(false, $"桥接同步校验失败：游戏目录缺少 {name}。");
                var expectedHash = HashFile(source);
                var installedHash = HashFile(destination);
                if (!string.Equals(expectedHash, installedHash,
                        StringComparison.OrdinalIgnoreCase))
                {
                    return new(false,
                        $"桥接同步校验失败：{name} 未更新为当前安装包版本，请关闭游戏后重试。");
                }
            }
            return new(true, "桥接同步校验通过：DLL 与辅助程序均已更新为当前安装包版本。");
        }
        catch (Exception exception)
        {
            return new(false, "桥接同步校验失败：" + exception.Message);
        }
    }

    private static void CopyOwnedFile(string source, string destination, InstallManifest manifest)
    {
        if (!File.Exists(source))
            throw new FileNotFoundException("安装负载缺少文件。", source);
        var expectedHash = HashFile(source);
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        File.Copy(source, destination, true);
        var installedHash = HashFile(destination);
        if (!string.Equals(expectedHash, installedHash, StringComparison.OrdinalIgnoreCase))
            throw new IOException($"文件覆盖后校验失败：{Path.GetFileName(destination)}");
        manifest.Files.RemoveAll(item =>
            string.Equals(item.Path, destination, StringComparison.OrdinalIgnoreCase));
        manifest.Files.Add(new InstalledFileRecord
        {
            Path = destination,
            Sha256 = installedHash,
            OwnedByProduct = true
        });
    }

    private static void CopyVoiceSet(
        string sourceDirectory,
        string destinationDirectory,
        InstallManifest manifest)
    {
        var voiceManifestSource = Path.Combine(sourceDirectory, "manifest.json");
        using (var document = JsonDocument.Parse(File.ReadAllText(voiceManifestSource)))
        {
            foreach (var entry in document.RootElement.EnumerateArray())
            {
                var triggerId = entry.GetProperty("triggerId").GetUInt32();
                var name = triggerId + ".wav";
                CopyOwnedFile(
                    Path.Combine(sourceDirectory, name),
                    Path.Combine(destinationDirectory, name),
                    manifest);
            }
        }
        CopyOwnedFile(
            voiceManifestSource,
            Path.Combine(destinationDirectory, "manifest.json"),
            manifest);
    }

    private static string HashFile(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    private void SaveManifest(InstallManifest manifest)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_manifestFile)!);
        var temporary = _manifestFile + ".tmp";
        File.WriteAllText(
            temporary,
            JsonSerializer.Serialize(manifest, InstallJsonContext.Default.InstallManifest));
        File.Move(temporary, _manifestFile, true);
    }

    private static void ApplyHiddenReframeworkConfig(string gameRoot, InstallManifest manifest)
    {
        var configPath = Path.Combine(gameRoot, "re2_fw_config.txt");
        var lines = File.Exists(configPath) ? File.ReadAllLines(configPath).ToList() : [];
        foreach (var pair in HiddenReframeworkValues)
        {
            var prefix = pair.Key + "=";
            var index = lines.FindIndex(line => line.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
            if (!manifest.ReframeworkConfigBackup.ContainsKey(pair.Key))
            {
                manifest.ReframeworkConfigBackup[pair.Key] = new ConfigValueBackup
                {
                    Existed = index >= 0,
                    Value = index >= 0 ? lines[index][prefix.Length..] : null
                };
            }
            if (index >= 0)
                lines[index] = prefix + pair.Value;
            else
                lines.Add(prefix + pair.Value);
        }
        File.WriteAllLines(configPath, lines);
    }

    private static void RestoreReframeworkConfig(InstallManifest manifest)
    {
        var configPath = Path.Combine(manifest.GameRoot, "re2_fw_config.txt");
        if (!File.Exists(configPath))
            return;
        var lines = File.ReadAllLines(configPath).ToList();
        foreach (var pair in manifest.ReframeworkConfigBackup)
        {
            var prefix = pair.Key + "=";
            var current = HiddenReframeworkValues.GetValueOrDefault(pair.Key);
            var index = lines.FindIndex(line => line.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
            if (index < 0 || current is null ||
                !string.Equals(lines[index], prefix + current, StringComparison.OrdinalIgnoreCase))
                continue;
            if (pair.Value.Existed)
                lines[index] = prefix + pair.Value.Value;
            else
                lines.RemoveAt(index);
        }
        File.WriteAllLines(configPath, lines);
    }

    private static void TryDeleteEmptyDirectories(string gameRoot)
    {
        var directories = new[]
        {
            Path.Combine(
                gameRoot,
                "reframework",
                "data",
                "codex_dualsense_haptics",
                "gauntlet_voice",
                ProductSettings.JapaneseGauntletVoice),
            Path.Combine(gameRoot, "reframework", "data", "codex_dualsense_haptics", "gauntlet_voice"),
            Path.Combine(gameRoot, "reframework", "data", "codex_dualsense_haptics")
        };
        foreach (var directory in directories)
        {
            if (Directory.Exists(directory) && !Directory.EnumerateFileSystemEntries(directory).Any())
                Directory.Delete(directory);
        }
    }
}
