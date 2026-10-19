using Microsoft.Win32;
using System.Text.RegularExpressions;

namespace OnimushaPS5Adapter.Core;

public static partial class SteamLocator
{
    public static IReadOnlyList<string> FindGameDirectories()
        => FindGameDirectories(FindSteamRoots());

    public static IReadOnlyList<string> FindGameDirectories(IEnumerable<string> steamRoots)
    {
        ArgumentNullException.ThrowIfNull(steamRoots);
        var scannedRoots = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var libraries = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var candidate in steamRoots)
        {
            var root = NormalizeDirectory(candidate);
            if (root is null || !Directory.Exists(root) || !scannedRoots.Add(root))
                continue;
            libraries.Add(root);
            var vdf = ReadOptionalFile(Path.Combine(root, "steamapps", "libraryfolders.vdf"));
            if (vdf is null)
                continue;
            foreach (Match match in VdfPathRegex().Matches(vdf))
            {
                var library = NormalizeDirectory(match.Groups[1].Value.Replace("\\\\", "\\"));
                if (library is not null && Directory.Exists(library))
                    libraries.Add(library);
            }
        }

        var gameDirectories = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var library in libraries)
        {
            var common = Path.Combine(library, "steamapps", "common");
            var manifest = ReadOptionalFile(Path.Combine(library, "steamapps",
                $"appmanifest_{ProductConstants.SteamAppId}.acf"));
            var installedDirectory = manifest is null ? null : InstallDirectoryRegex().Match(manifest);
            var installedName = installedDirectory?.Success == true
                ? installedDirectory.Groups[1].Value.Replace("\\\\", "\\") : null;
            var gameRoot = FindGameWithinCommon(common, installedName)
                ?? FindGameWithinCommon(common, "OnimushaWotS");
            if (gameRoot is not null)
                gameDirectories.Add(gameRoot);
        }
        return gameDirectories.ToArray();
    }

    private static IEnumerable<string> FindSteamRoots()
    {
        var candidates = new List<string?>();
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
            candidates.Add(key?.GetValue("SteamPath")?.ToString());
            candidates.Add(key?.GetValue("SteamExe")?.ToString() is { } exe ? Path.GetDirectoryName(exe) : null);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or System.Security.SecurityException) { }
        foreach (var registryPath in new[] { @"SOFTWARE\Valve\Steam", @"SOFTWARE\WOW6432Node\Valve\Steam" })
        {
            try
            {
                using var key = Registry.LocalMachine.OpenSubKey(registryPath);
                candidates.Add(key?.GetValue("InstallPath")?.ToString());
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or System.Security.SecurityException) { }
        }

        return candidates.Where(path => !string.IsNullOrWhiteSpace(path)).Select(path => path!);
    }

    private static string? NormalizeDirectory(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return null;
        try
        {
            return Path.TrimEndingDirectorySeparator(Path.GetFullPath(
                path.Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar)));
        }
        catch (Exception exception) when (exception is ArgumentException or IOException or NotSupportedException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static string? ReadOptionalFile(string path)
    {
        try { return File.ReadAllText(path); }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException) { return null; }
    }

    private static string? FindGameWithinCommon(string common, string? installedName)
    {
        if (string.IsNullOrWhiteSpace(installedName) || Path.IsPathRooted(installedName))
            return null;
        var gameRoot = NormalizeDirectory(Path.Combine(common, installedName));
        if (gameRoot is null || !gameRoot.StartsWith(common + Path.DirectorySeparatorChar,
                StringComparison.OrdinalIgnoreCase))
            return null;
        return File.Exists(Path.Combine(gameRoot, "OnimushaWotS.exe")) ? gameRoot : null;
    }

    [GeneratedRegex("\\\"path\\\"\\s+\\\"([^\\\"]+)\\\"", RegexOptions.IgnoreCase)]
    private static partial Regex VdfPathRegex();

    [GeneratedRegex("\\\"installdir\\\"\\s+\\\"([^\\\"]+)\\\"", RegexOptions.IgnoreCase)]
    private static partial Regex InstallDirectoryRegex();
}

