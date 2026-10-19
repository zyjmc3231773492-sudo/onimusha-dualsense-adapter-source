using System.Text.Json;
using System.Text.Json.Serialization;

namespace OnimushaPS5Adapter.Core;

public sealed class ProductSettings
{
    public const string ChineseGauntletVoice = "zh-CN";
    public const string JapaneseGauntletVoice = "ja-JP";
    public const int DefaultSpeakerVolumePercent = 100;
    public const int MinimumSpeakerVolumePercent = 0;
    public const int MaximumSpeakerVolumePercent = 200;
    public const int DefaultHapticIntensityPercent = 100;
    public const int GauntletSpeakerBaselinePercent = 55;
    public const int SoulSpeakerBaselinePercent = 60;
    public const int ParrySpeakerBaselinePercent = 25;

    public const int CurrentSchemaVersion = 13;

    public int SchemaVersion { get; set; } = CurrentSchemaVersion;
    public bool RainEnabled { get; set; } = true;
    public bool WindEnabled { get; set; }
    public int HapticIntensityPercent { get; set; } = DefaultHapticIntensityPercent;
    public bool GuardHapticEnabled { get; set; } = true;
    public int GuardHapticIntensityPercent { get; set; } = DefaultHapticIntensityPercent;
    public bool ParryHapticEnabled { get; set; } = true;
    public int ParryHapticIntensityPercent { get; set; } = DefaultHapticIntensityPercent;
    public bool DeflectHapticEnabled { get; set; } = true;
    public int DeflectHapticIntensityPercent { get; set; } = DefaultHapticIntensityPercent;
    public bool IssenHapticEnabled { get; set; } = true;
    public int IssenHapticIntensityPercent { get; set; } = DefaultHapticIntensityPercent;
    public bool LeftAdaptiveTriggerEnabled { get; set; } = true;
    public bool RightAdaptiveTriggerEnabled { get; set; } = true;
    public bool ControllerSpeakerEnabled { get; set; } = true;
    public string GauntletVoiceLanguage { get; set; } = ChineseGauntletVoice;
    public int GauntletVoiceVolumePercent { get; set; } = DefaultSpeakerVolumePercent;
    public int SoulSpeakerVolumePercent { get; set; } = DefaultSpeakerVolumePercent;
    public int ClashSpeakerVolumePercent { get; set; } = DefaultSpeakerVolumePercent;
    public int GuardSpeakerVolumePercent { get; set; } = DefaultSpeakerVolumePercent;
    public int ParrySpeakerVolumePercent { get; set; } = DefaultSpeakerVolumePercent;
    public int DeflectSpeakerVolumePercent { get; set; } = DefaultSpeakerVolumePercent;
    public int ExecutionSpeakerVolumePercent { get; set; } = DefaultSpeakerVolumePercent;
    public int IssenSpeakerVolumePercent { get; set; } = DefaultSpeakerVolumePercent;

    public void Normalize()
    {
        if (!string.Equals(
                GauntletVoiceLanguage,
                JapaneseGauntletVoice,
                StringComparison.OrdinalIgnoreCase))
        {
            GauntletVoiceLanguage = ChineseGauntletVoice;
        }
        else
        {
            GauntletVoiceLanguage = JapaneseGauntletVoice;
        }

        GauntletVoiceVolumePercent = ClampSpeakerVolume(GauntletVoiceVolumePercent);
        SoulSpeakerVolumePercent = ClampSpeakerVolume(SoulSpeakerVolumePercent);
        ClashSpeakerVolumePercent = ClampSpeakerVolume(ClashSpeakerVolumePercent);
        GuardSpeakerVolumePercent = ClampSpeakerVolume(GuardSpeakerVolumePercent);
        ParrySpeakerVolumePercent = ClampSpeakerVolume(ParrySpeakerVolumePercent);
        DeflectSpeakerVolumePercent = ClampSpeakerVolume(DeflectSpeakerVolumePercent);
        ExecutionSpeakerVolumePercent = ClampSpeakerVolume(ExecutionSpeakerVolumePercent);
        IssenSpeakerVolumePercent = ClampSpeakerVolume(IssenSpeakerVolumePercent);
        HapticIntensityPercent = ClampSpeakerVolume(HapticIntensityPercent);
        GuardHapticIntensityPercent = ClampSpeakerVolume(GuardHapticIntensityPercent);
        ParryHapticIntensityPercent = ClampSpeakerVolume(ParryHapticIntensityPercent);
        DeflectHapticIntensityPercent = ClampSpeakerVolume(DeflectHapticIntensityPercent);
        IssenHapticIntensityPercent = ClampSpeakerVolume(IssenHapticIntensityPercent);
    }

    private static int ClampSpeakerVolume(int value) => Math.Clamp(
        value,
        MinimumSpeakerVolumePercent,
        MaximumSpeakerVolumePercent);

    public static int RebaseLegacySpeakerVolume(int value, int baselinePercent) =>
        ClampSpeakerVolume((int)Math.Round(
            ClampSpeakerVolume(value) * 100.0 / baselinePercent,
            MidpointRounding.AwayFromZero));
}

[JsonSerializable(typeof(ProductSettings))]
internal partial class SettingsJsonContext : JsonSerializerContext;

public static class SettingsStore
{
    public static ProductSettings Load()
        => Load(ProductPaths.SettingsFile);

    public static ProductSettings Load(string settingsFile)
    {
        try
        {
            if (!File.Exists(settingsFile))
                return new ProductSettings();

            var json = File.ReadAllText(settingsFile);
            var settings = JsonSerializer.Deserialize(json, SettingsJsonContext.Default.ProductSettings)
                ?? new ProductSettings();

            var migrated = false;
            if (settings.SchemaVersion < 2)
            {
                migrated = true;
            }
            if (settings.SchemaVersion < 3)
            {
                settings.GauntletVoiceLanguage = ProductSettings.ChineseGauntletVoice;
                migrated = true;
            }
            if (settings.SchemaVersion < 4)
            {
                settings.GauntletVoiceVolumePercent = ProductSettings.DefaultSpeakerVolumePercent;
                settings.SoulSpeakerVolumePercent = ProductSettings.DefaultSpeakerVolumePercent;
                settings.ClashSpeakerVolumePercent = ProductSettings.DefaultSpeakerVolumePercent;
                settings.GuardSpeakerVolumePercent = ProductSettings.DefaultSpeakerVolumePercent;
                settings.DeflectSpeakerVolumePercent = ProductSettings.DefaultSpeakerVolumePercent;
                settings.ExecutionSpeakerVolumePercent = ProductSettings.DefaultSpeakerVolumePercent;
                settings.IssenSpeakerVolumePercent = ProductSettings.DefaultSpeakerVolumePercent;
                migrated = true;
            }
            if (settings.SchemaVersion < 5)
            {
                settings.ControllerSpeakerEnabled = true;
                migrated = true;
            }
            if (settings.SchemaVersion < 6)
            {
                settings.HapticIntensityPercent = ProductSettings.DefaultHapticIntensityPercent;
                migrated = true;
            }
            if (settings.SchemaVersion < 7)
            {
                settings.LeftAdaptiveTriggerEnabled = true;
                settings.RightAdaptiveTriggerEnabled = true;
                migrated = true;
            }
            if (settings.SchemaVersion < 8)
            {
                settings.GuardHapticEnabled = true;
                settings.GuardHapticIntensityPercent =
                    ProductSettings.DefaultHapticIntensityPercent;
                settings.DeflectHapticEnabled = true;
                settings.DeflectHapticIntensityPercent =
                    ProductSettings.DefaultHapticIntensityPercent;
                settings.IssenHapticEnabled = true;
                settings.IssenHapticIntensityPercent =
                    ProductSettings.DefaultHapticIntensityPercent;
                migrated = true;
            }
            if (settings.SchemaVersion < 9)
            {
                // Version 8 exposed Parry and Just Guard as one “弹开 / 化解”
                // control. Preserve that user's switch and strength for both
                // newly separated categories.
                settings.ParryHapticEnabled = settings.DeflectHapticEnabled;
                settings.ParryHapticIntensityPercent =
                    settings.DeflectHapticIntensityPercent;
                migrated = true;
            }

            using var document = JsonDocument.Parse(json);
            if (!document.RootElement.TryGetProperty(nameof(ProductSettings.ParrySpeakerVolumePercent), out _))
            {
                // Repel previously used the Deflect volume. Preserve the old
                // sound level when introducing its independent control.
                settings.ParrySpeakerVolumePercent = settings.DeflectSpeakerVolumePercent;
                migrated = true;
            }

            if (settings.SchemaVersion < ProductSettings.CurrentSchemaVersion)
            {
                settings.GauntletVoiceVolumePercent = ProductSettings.RebaseLegacySpeakerVolume(
                    settings.GauntletVoiceVolumePercent, ProductSettings.GauntletSpeakerBaselinePercent);
                settings.SoulSpeakerVolumePercent = ProductSettings.RebaseLegacySpeakerVolume(
                    settings.SoulSpeakerVolumePercent, ProductSettings.SoulSpeakerBaselinePercent);
                settings.ParrySpeakerVolumePercent = ProductSettings.RebaseLegacySpeakerVolume(
                    settings.ParrySpeakerVolumePercent, ProductSettings.ParrySpeakerBaselinePercent);
                // Normal attack haptics are now an internal always-on layer.
                // Re-save older settings without the obsolete user toggle.
                migrated = true;
            }

            var languageBeforeNormalization = settings.GauntletVoiceLanguage;
            settings.Normalize();
            migrated |= !string.Equals(
                languageBeforeNormalization,
                settings.GauntletVoiceLanguage,
                StringComparison.Ordinal);
            settings.SchemaVersion = ProductSettings.CurrentSchemaVersion;
            if (migrated)
                Save(settings, settingsFile);

            return settings;
        }
        catch
        {
            return new ProductSettings();
        }
    }

    public static void Save(ProductSettings settings)
        => Save(settings, ProductPaths.SettingsFile);

    public static void Save(ProductSettings settings, string settingsFile)
    {
        settings.Normalize();
        settings.SchemaVersion = ProductSettings.CurrentSchemaVersion;
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(settingsFile))!);
        var temporary = settingsFile + ".tmp";
        var json = JsonSerializer.Serialize(
            settings,
            SettingsJsonContext.Default.ProductSettings);
        File.WriteAllText(temporary, json);
        File.Move(temporary, settingsFile, true);
    }
}
