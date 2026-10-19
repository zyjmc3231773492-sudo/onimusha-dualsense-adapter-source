#if ONIMUSHA_STARTUP_REPAIR
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using OnimushaPS5Adapter.Core;

namespace OnimushaPS5Adapter.Manager;

internal static class StartupRepairMaintenance
{
    internal static string StateRoot => Path.Combine(ProductPaths.LocalDataDirectory, "StartupDiagnosticR1");
    internal static string RecoveryHash = "__RECOVERY_HASH__";
    internal static IReadOnlyList<string> LocateGames()
    {
        var candidates = new List<string>();
        void Add(string? root)
        {
            try
            {
                if (string.IsNullOrWhiteSpace(root)) return;
                root = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar);
                if (File.Exists(Path.Combine(root, "OnimushaWotS.exe"))
                    && !candidates.Contains(root, StringComparer.OrdinalIgnoreCase)) candidates.Add(root);
            }
            catch (ArgumentException) { }
            catch (IOException) { }
        }
        // Diagnostics know this customer's manually selected, non-Steam game path.
        var active = Path.Combine(StateRoot, "active-session.json");
        var records = new List<string> { active };
        var sessions = Path.Combine(StateRoot, "sessions");
        if (Directory.Exists(sessions))
            records.AddRange(Directory.GetDirectories(sessions)
                .OrderByDescending(Directory.GetLastWriteTimeUtc).Take(40)
                .Select(folder => Path.Combine(folder, "session.json")));
        foreach (var record in records)
        {
            try
            {
                if (!File.Exists(record) || new FileInfo(record).Length > 128 * 1024) continue;
                using var document = JsonDocument.Parse(File.ReadAllText(record));
                Add(document.RootElement.GetProperty("GameRoot").GetString());
            }
            catch (Exception error) when (error is IOException or JsonException or KeyNotFoundException or InvalidOperationException) { }
        }
        Add(InstallService.LoadManifest()?.GameRoot);
        foreach (var root in SteamLocator.FindGameDirectories()) Add(root);
        return candidates;
    }

    internal static InstallResult Install(string gameRoot, string payloadRoot, string? diagnosticState = null)
    {
        try
        {
            gameRoot = Path.GetFullPath(gameRoot);
            payloadRoot = Path.GetFullPath(payloadRoot);
            var recovery = Path.Combine(payloadRoot, "bin", "startup-diagnostic-recovery.exe");
            using var recoveryLock = new FileStream(recovery, FileMode.Open, FileAccess.Read, FileShare.Read);
            var actual = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(recoveryLock));
            if (actual != RecoveryHash) throw new IOException(UiText.Get("TheRecoveryToolFailedVerificationInstallationWasSkipped"));
            var root = Path.Combine(ProductPaths.LocalDataDirectory, "StartupRepair1");
            Directory.CreateDirectory(root);
            var output = Path.Combine(root, "prepare-" + Guid.NewGuid().ToString("N") + ".json");
            var command = new ProcessStartInfo(recovery)
            {
                UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden
            };
            foreach (var item in new[] { "--prepare", gameRoot, diagnosticState ?? StateRoot, output })
                command.ArgumentList.Add(item);
            using var process = Process.Start(command) ?? throw new IOException(UiText.Get("TheRecoveryToolWasNotCreated"));
            process.WaitForExit();
            using var document = JsonDocument.Parse(File.ReadAllText(output));
            if (process.ExitCode != 0 || !document.RootElement.GetProperty("Success").GetBoolean())
                return new(false, document.RootElement.GetProperty("Message").GetString() ?? UiText.Get("RecoveryDidNotComplete"));
            var result = new InstallService().InstallOrRepair(gameRoot, payloadRoot);
            return result.Success
                ? result with { Message = UiText.Get("CompatibilityRepairInstalledExistingSettingsArePreservedCloseOldDiagnostics") + result.Message }
                : result;
        }
        catch (Exception error) { return new(false, UiText.Get("CompatibilityRepairFailed") + error.Message); }
    }
}
#endif
