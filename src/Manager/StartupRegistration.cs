using Microsoft.Win32;
using System.IO;
using System.Runtime.InteropServices;

namespace OnimushaPS5Adapter.Manager;

internal static class StartupRegistration
{
    private const string LegacyRunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string LegacyApprovalKeyPath =
        @"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run";
    private const string StartupFolderApprovalKeyPath =
        @"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\StartupFolder";
    private const string LegacyValueName = "OnimushaPS5Adapter";
    private const string ShortcutFileName = "OnimushaPS5Adapter.lnk";
    private static readonly byte[] EnabledApproval =
        { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    public static bool IsEnabled(string executablePath)
    {
        if (File.Exists(GetShortcutPath()))
            return IsApprovalEnabled(StartupFolderApprovalKeyPath, ShortcutFileName);

        if (string.IsNullOrWhiteSpace(executablePath))
            return false;

        using var legacyKey = Registry.CurrentUser.OpenSubKey(LegacyRunKeyPath, writable: false);
        var legacyCommand = legacyKey?.GetValue(
            LegacyValueName,
            null,
            RegistryValueOptions.DoNotExpandEnvironmentNames) as string;
        return string.Equals(
                legacyCommand,
                BuildLegacyCommand(executablePath),
                StringComparison.OrdinalIgnoreCase) &&
            IsApprovalEnabled(LegacyApprovalKeyPath, LegacyValueName);
    }

    public static void SetEnabled(string executablePath, bool enabled)
    {
        if (!enabled)
        {
            DeleteShortcut();
            DeleteRegistryValue(StartupFolderApprovalKeyPath, ShortcutFileName);
            DeleteLegacyRegistration();
            return;
        }

        if (string.IsNullOrWhiteSpace(executablePath) || !File.Exists(executablePath))
            throw new InvalidOperationException(UiText.Get("CannotLocateTheManagerExecutable2"));

        CreateShortcut(Path.GetFullPath(executablePath));
        using (var approvalKey = Registry.CurrentUser.CreateSubKey(
                   StartupFolderApprovalKeyPath,
                   writable: true)
               ?? throw new InvalidOperationException(UiText.Get("CannotUpdateWindowsStartupSettings")))
        {
            approvalKey.SetValue(ShortcutFileName, EnabledApproval, RegistryValueKind.Binary);
        }

        DeleteLegacyRegistration();
    }

    private static void CreateShortcut(string executablePath)
    {
        var shellType = Type.GetTypeFromProgID("WScript.Shell")
            ?? throw new InvalidOperationException(UiText.Get("TheWindowsShortcutServiceIsUnavailable"));
        object? shell = null;
        object? shortcut = null;
        try
        {
            shell = Activator.CreateInstance(shellType)
                ?? throw new InvalidOperationException(UiText.Get("CannotCreateTheWindowsShortcutService"));
            shortcut = ((dynamic)shell).CreateShortcut(GetShortcutPath());
            dynamic link = shortcut;
            link.TargetPath = executablePath;
            link.Arguments = "--startup";
            link.WorkingDirectory = Path.GetDirectoryName(executablePath) ?? AppContext.BaseDirectory;
            link.Description = UiText.Get("OnimushaDualsenseAdapterWindowsStartup");
            link.IconLocation = executablePath + ",0";
            link.WindowStyle = 7;
            link.Save();
        }
        finally
        {
            if (shortcut is not null && Marshal.IsComObject(shortcut))
                Marshal.FinalReleaseComObject(shortcut);
            if (shell is not null && Marshal.IsComObject(shell))
                Marshal.FinalReleaseComObject(shell);
        }
    }

    private static bool IsApprovalEnabled(string keyPath, string valueName)
    {
        using var approvalKey = Registry.CurrentUser.OpenSubKey(keyPath, writable: false);
        var approval = approvalKey?.GetValue(valueName) as byte[];
        if (approval is null)
            return true;

        return approval.Length > 0 && approval[0] == EnabledApproval[0];
    }

    private static void DeleteShortcut()
    {
        var shortcutPath = GetShortcutPath();
        if (File.Exists(shortcutPath))
            File.Delete(shortcutPath);
    }

    private static void DeleteLegacyRegistration()
    {
        DeleteRegistryValue(LegacyRunKeyPath, LegacyValueName);
        DeleteRegistryValue(LegacyApprovalKeyPath, LegacyValueName);
    }

    private static void DeleteRegistryValue(string keyPath, string valueName)
    {
        using var key = Registry.CurrentUser.OpenSubKey(keyPath, writable: true);
        key?.DeleteValue(valueName, throwOnMissingValue: false);
    }

    private static string GetShortcutPath()
    {
        var startupFolder = Environment.GetFolderPath(Environment.SpecialFolder.Startup);
        if (string.IsNullOrWhiteSpace(startupFolder))
            throw new InvalidOperationException(UiText.Get("CannotLocateTheCurrentUserSWindowsStartupFolder"));

        Directory.CreateDirectory(startupFolder);
        return Path.Combine(startupFolder, ShortcutFileName);
    }

    private static string BuildLegacyCommand(string executablePath) =>
        $"\"{Path.GetFullPath(executablePath)}\" --startup";
}
