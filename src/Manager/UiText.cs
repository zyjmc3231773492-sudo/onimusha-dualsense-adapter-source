using OnimushaPS5Adapter.Core;
using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Windows;

namespace OnimushaPS5Adapter.Manager;

[JsonSerializable(typeof(Dictionary<string, string>))]
internal partial class LocalizationJsonContext : JsonSerializerContext;

/// <summary>UI language preferences are independent of game and controller voice settings.</summary>
internal static class UiText
{
    private static ResourceDictionary? _resources;
    private static Dictionary<string, string> _strings = [];
    internal static string Language { get; private set; } = "zh-CN";
    internal static event Action? LanguageChanged;
    private static string PreferenceFile => Path.Combine(ProductPaths.LocalDataDirectory, "ui-language.txt");

    internal static void Initialize(string[]? arguments = null, bool persistRequested = true)
    {
        var requested = ParseLanguage(arguments ?? []);
        var saved = ReadPreference(PreferenceFile) ?? ReadPreference(Path.Combine(AppContext.BaseDirectory, "ui-language.txt"));
        SetLanguage(requested ?? saved ??
            (CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "zh" ? "zh-CN" : "en-US"),
            persist: requested is not null && persistRequested);
    }

    internal static string? ParseLanguage(string[] arguments)
    {
        for (var index = 0; index < arguments.Length; index++)
        {
            var argument = arguments[index];
            var value = argument.StartsWith("--language=", StringComparison.OrdinalIgnoreCase)
                ? argument[11..]
                : argument.Equals("--language", StringComparison.OrdinalIgnoreCase) && index + 1 < arguments.Length
                    ? arguments[++index] : null;
            if (value is not null && NormalizeLanguage(value) is { } language) return language;
        }
        return null;
    }

    internal static string[] RemoveLanguageArguments(string[] arguments)
    {
        var remaining = new List<string>();
        for (var index = 0; index < arguments.Length; index++)
        {
            if (arguments[index].StartsWith("--language=", StringComparison.OrdinalIgnoreCase)) continue;
            if (arguments[index].Equals("--language", StringComparison.OrdinalIgnoreCase))
            {
                if (index + 1 < arguments.Length) index++;
                continue;
            }
            remaining.Add(arguments[index]);
        }
        return [.. remaining];
    }

    internal static void SetLanguage(string language, bool persist = true)
    {
        Language = NormalizeLanguage(language) ?? "zh-CN";
        using var stream = typeof(UiText).Assembly.GetManifestResourceStream(
            $"OnimushaPS5Adapter.Manager.Localization.{Language}.json")
            ?? throw new InvalidOperationException($"Missing UI language resource: {Language}");
        _strings = JsonSerializer.Deserialize(stream, LocalizationJsonContext.Default.DictionaryStringString)
            ?? throw new InvalidOperationException("Invalid UI language resource.");
        var replacement = new ResourceDictionary();
        foreach (var entry in _strings) replacement[entry.Key] = entry.Value;
        var application = System.Windows.Application.Current;
        if (application is not null)
        {
            if (_resources is not null) application.Resources.MergedDictionaries.Remove(_resources);
            application.Resources.MergedDictionaries.Add(replacement);
        }
        _resources = replacement;
        if (persist)
        {
            try
            {
                Directory.CreateDirectory(ProductPaths.LocalDataDirectory);
                var temporary = PreferenceFile + ".tmp";
                File.WriteAllText(temporary, Language);
                File.Move(temporary, PreferenceFile, overwrite: true);
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            {
                // Read-only profiles can still switch the current session's language.
                System.Diagnostics.Debug.WriteLine(exception.Message);
            }
        }
        LanguageChanged?.Invoke();
    }

    internal static string Get(string key, params object?[] values)
    {
        if (_strings.Count == 0) Initialize();
        var format = _strings.TryGetValue(key, out var value) ? value : key;
        return values.Length == 0 ? format : string.Format(
            CultureInfo.GetCultureInfo(Language), format, values);
    }

    private static string? ReadPreference(string path)
    {
        try { return File.Exists(path) ? NormalizeLanguage(File.ReadAllText(path).Trim()) : null; }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException) { return null; }
    }

    private static string? NormalizeLanguage(string language) => language.ToLowerInvariant() switch
    {
        "zh" or "zh-cn" => "zh-CN",
        "en" or "en-us" => "en-US",
        _ => null
    };
}
