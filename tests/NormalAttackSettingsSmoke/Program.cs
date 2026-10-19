using OnimushaPS5Adapter.Core;

static void Require(bool okay, string message)
{
    if (!okay) throw new InvalidOperationException(message);
}
var fixtureRoot = Path.Combine(Path.GetTempPath(), "OnimushaNormalAttackSettings-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(fixtureRoot);
var fixture = Path.Combine(fixtureRoot, "settings.json");
Require(typeof(ProductSettings).GetProperty("NormalAttackHapticEnabled") is null,
    "Normal attack must no longer be a user preference");
Require(new ProductSettings().SchemaVersion == ProductSettings.CurrentSchemaVersion && SettingsStore.Load(fixture).SchemaVersion == ProductSettings.CurrentSchemaVersion,
    "Fresh and missing settings must use the current schema");
SettingsStore.Save(new ProductSettings(), fixture);
Require(!File.ReadAllText(fixture).Contains("NormalAttackHapticEnabled"), "Fresh settings must omit the obsolete toggle");
foreach (var schema in new[] { 9, 10 })
foreach (var legacyEnabled in new[] { "true", "false" })
{
    File.WriteAllText(fixture, $$"""
    {"SchemaVersion":{{schema}},"NormalAttackHapticEnabled":{{legacyEnabled}},"RainEnabled":false,"WindEnabled":false,"HapticIntensityPercent":135,"GuardHapticEnabled":false,"ParryHapticIntensityPercent":200,"LeftAdaptiveTriggerEnabled":false,"GauntletVoiceLanguage":"ja-JP","GauntletVoiceVolumePercent":120}
    """);
    var migrated = SettingsStore.Load(fixture);
    Require(migrated.SchemaVersion == ProductSettings.CurrentSchemaVersion, "Older schema must upgrade");
    Require(!migrated.RainEnabled && !migrated.WindEnabled && migrated.HapticIntensityPercent == 135 &&
        !migrated.GuardHapticEnabled && migrated.ParryHapticIntensityPercent == 200 &&
        !migrated.LeftAdaptiveTriggerEnabled && migrated.GauntletVoiceLanguage == "ja-JP" &&
        migrated.GauntletVoiceVolumePercent == 200, "Unrelated user preferences survive; legacy voice volume rebases to the new 200% ceiling");
    Require(!File.ReadAllText(fixture).Contains("NormalAttackHapticEnabled"), "Migration must remove either legacy toggle value");
    SettingsStore.Save(migrated, fixture);
    Require(SettingsStore.Load(fixture).HapticIntensityPercent == 135 &&
        !File.ReadAllText(fixture).Contains("NormalAttackHapticEnabled"), "Save/reload must not restore the toggle");
}
File.WriteAllText(fixture, "{bad json");
Require(SettingsStore.Load(fixture).SchemaVersion == ProductSettings.CurrentSchemaVersion, "Invalid settings must fall back to current defaults");
Console.WriteLine("Managed normal attack toggle removal, schema9/10-to-current migration/preservation, legacy on/off cleanup, invalid JSON tests passed; isolated fixtures: " + fixtureRoot);
