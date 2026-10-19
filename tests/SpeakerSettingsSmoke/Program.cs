using OnimushaPS5Adapter.Core;
using OnimushaPS5Adapter.Manager;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}
if (args.Length == 3 && args[0] == "--preview")
{
    var status = await SpeakerPreviewService.PlayAsync(int.Parse(args[1]), byte.Parse(args[2]), 100);
    Console.WriteLine($"Actual helper preview cue={args[2]} status={status}; queue acknowledgement only.");
    Require(status == 1, "Actual helper did not accept the preview");
    return;
}
if (args.Length == 2 && args[0] == "--preview-all")
{
    var previewSettings = SettingsStore.Load();
    (byte cue, int volume)[] previews = [
        (14, previewSettings.GauntletVoiceVolumePercent), (2, previewSettings.SoulSpeakerVolumePercent),
        (4, previewSettings.ClashSpeakerVolumePercent), (5, previewSettings.GuardSpeakerVolumePercent),
        (12, previewSettings.ParrySpeakerVolumePercent), (13, previewSettings.ParrySpeakerVolumePercent),
        (6, previewSettings.DeflectSpeakerVolumePercent), (7, previewSettings.ExecutionSpeakerVolumePercent),
        (11, previewSettings.IssenSpeakerVolumePercent)];
    foreach (var (cue, volume) in previews)
    {
        var status = await SpeakerPreviewService.PlayAsync(int.Parse(args[1]), cue, volume);
        Console.WriteLine($"Actual helper preview cue={cue} volume={volume} status={status}; queue acknowledgement only.");
        Require(status == 1, "Actual helper did not accept the preview");
        await Task.Delay(3000);
    }
    return;
}
var folder = Path.Combine(Path.GetTempPath(), "OnimushaSpeakerSettings-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(folder);
var fixture = Path.Combine(folder, "settings.json");
File.WriteAllText(fixture, """
{"SchemaVersion":11,"DeflectSpeakerVolumePercent":165,"HapticIntensityPercent":135,"ParryHapticIntensityPercent":200,"WindEnabled":false,"ControllerSpeakerEnabled":false,"GauntletVoiceLanguage":"ja-JP"}
""");
var settings = SettingsStore.Load(fixture);
Require(settings.SchemaVersion == 13 && settings.ParrySpeakerVolumePercent == 200 && settings.DeflectSpeakerVolumePercent == 165,
    "Legacy Repel inherits then rebases to the new ceiling; Deflect stays unchanged");
Require(settings.HapticIntensityPercent == 135 && settings.ParryHapticIntensityPercent == 200 &&
    !settings.WindEnabled && !settings.ControllerSpeakerEnabled && settings.GauntletVoiceLanguage == "ja-JP",
    "Migration must preserve unrelated user preferences");
settings.ParrySpeakerVolumePercent = 0;
SettingsStore.Save(settings, fixture);
Require(SettingsStore.Load(fixture).ParrySpeakerVolumePercent == 0 && SettingsStore.Load(fixture).DeflectSpeakerVolumePercent == 165,
    "Repel mute must not change Deflect volume");
settings.ParrySpeakerVolumePercent = 200;
SettingsStore.Save(settings, fixture);
Require(SettingsStore.Load(fixture).ParrySpeakerVolumePercent == 200, "Repel boost must survive save/reload");
settings.ParrySpeakerVolumePercent = 900;
settings.Normalize();
Require(settings.ParrySpeakerVolumePercent == 200, "Repel upper bound");
settings.ParrySpeakerVolumePercent = -1;
settings.Normalize();
Require(settings.ParrySpeakerVolumePercent == 0, "Repel lower bound");
File.WriteAllText(fixture, """{"SchemaVersion":11,"DeflectSpeakerVolumePercent":165,"ParrySpeakerVolumePercent":45}""");
Require(SettingsStore.Load(fixture).ParrySpeakerVolumePercent == 180, "Explicit independent legacy Repel volume must rebase from 45 to 180");
File.WriteAllText(fixture, """{"SchemaVersion":12,"GauntletVoiceVolumePercent":55,"SoulSpeakerVolumePercent":60,"ParrySpeakerVolumePercent":25,"DeflectSpeakerVolumePercent":100}""");
var preset = SettingsStore.Load(fixture);
Require(preset.SchemaVersion == 13 && preset.GauntletVoiceVolumePercent == 100 &&
    preset.SoulSpeakerVolumePercent == 100 && preset.ParrySpeakerVolumePercent == 100 &&
    preset.DeflectSpeakerVolumePercent == 100, "User calibrated screenshot must become all 100% without changing its output levels");
Require(SettingsStore.Load(fixture).ParrySpeakerVolumePercent == 100, "Calibration migration must only happen once");
Console.WriteLine("Speaker settings migration, independent Repel mute/boost and preservation passed. Fixtures: " + folder);
