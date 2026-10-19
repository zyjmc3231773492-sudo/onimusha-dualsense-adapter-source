#if ONIMUSHA_STARTUP_REPAIR
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;
using OnimushaPS5Adapter.Core;

namespace OnimushaPS5Adapter.Manager;

// The same DLL, helper and broker that passed the customer's R3 test.
internal sealed class ExternalHelperRuntime : IDisposable
{
    internal const string PluginHash = "92AB6F89571166EA94A92DD79529E82EEC7B64D3EB795CA55A794AA726F35482";
    internal const string HelperHash = "50A2267BE54E9F8248B7141CC983E4FB593D9216C93F253ED9C86B8D163F348C";
    internal const string BrokerHash = "41AE882C34DCB047A8F6CD3A72231A0582C48C9A68EAE4A112BCA8F913EBD242";
    private readonly string _payload;
    private readonly string _stateRoot;
    private readonly Func<string?> _gameRoot;
    private readonly string _expectedHelper;
    private readonly CancellationTokenSource _stop = new();
    private readonly HashSet<string> _attempted = new(StringComparer.Ordinal);
    private readonly Queue<string> _attemptOrder = new();
    private readonly List<Attempt> _history = [];
    private Process? _monitor;
    private FileStream? _helperLock;
    private FileStream? _brokerLock;
    private Attempt? _current;
    private Task? _worker;
    private int _disposed;
    private string _status = UiText.Get("CompatibilityRepairIsReadyAndWaitingForTheGame");
    private volatile bool _hasActiveGame;

    internal sealed class Attempt
    {
        public int GamePid { get; set; }
        public long GameStartedTicks { get; set; }
        public string StartedUtc { get; set; } = "";
        public string? Error { get; set; }
        public bool MonitorStoppedByTool { get; set; }
        public JsonElement? State { get; set; }
        [System.Text.Json.Serialization.JsonIgnore]
        public string StatePath { get; set; } = "";
    }

    internal ExternalHelperRuntime()
        : this(Path.Combine(AppContext.BaseDirectory, "payload"),
            Path.Combine(ProductPaths.LocalDataDirectory, "StartupRepair1"),
            () => InstallService.LoadManifest()?.GameRoot, HelperHash) { }

    // Tests use their own native fixture; the product only calls the fixed-hash constructor.
    internal ExternalHelperRuntime(string payload, string stateRoot, Func<string?> gameRoot, string helperHash)
    {
        _payload = Path.GetFullPath(payload);
        _stateRoot = Path.GetFullPath(stateRoot);
        _gameRoot = gameRoot;
        _expectedHelper = helperHash;
    }

    internal string Status => Volatile.Read(ref _status);
    internal bool HasActiveGame => _hasActiveGame;
    internal void Start() => _worker ??= Task.Run(Run);

    private async Task Run()
    {
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                try { Poll(); }
                catch (Exception error) { _status = UiText.Get("BackgroundAdaptationFailed") + error.Message; }
                await Task.Delay(750, _stop.Token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
        finally { StopMonitor(); }
    }

    private void Poll()
    {
        if (_monitor is not null && _monitor.HasExited)
        {
            if (_monitor is not null) ReadState();
            if (_current?.State is JsonElement state)
            {
                var failed = state.TryGetProperty("Error", out var error) && error.GetInt32() != 0
                    || state.TryGetProperty("Stage", out var stage)
                        && new[] { "launch-error", "game-rejected", "monitor-error" }.Contains(stage.GetString())
                    || state.TryGetProperty("ExitCode", out var code)
                        && code.ValueKind == JsonValueKind.String && code.GetString() != "0x0";
                _status = failed
                    ? UiText.Get("TheHelperFailedToStartOrRunItWillNot")
                    : UiText.Get("TheHelperExitedNormallyAndIsWaitingForTheNext");
            }
            else _status = UiText.Get("TheHelperExitedWithAnUnconfirmedStatusKeepTheSession");
            ReleaseMonitor();
        }
        var root = _gameRoot();
        if (string.IsNullOrWhiteSpace(root)) { _hasActiveGame = false; return; }
        root = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar);
        var games = FindGames(root);
        _hasActiveGame = games.Count != 0;
        if (games.Count != 1 || _monitor is not null)
        {
            if (games.Count > 1) _status = UiText.Get("RunOnlyOneGameInstanceNoExtraHelperWasStarted");
            if (_monitor is not null) ReadState();
            return;
        }
        var game = games[0];
        var key = game.Pid + ":" + game.StartedTicks;
        if (_attempted.Contains(key)) return;
        if (DiagnosticWindowOpen())
        {
            _status = UiText.Get("CloseOldR1R2R3DiagnosticWindowsSoTheCompatibility");
            return;
        }
        _attempted.Add(key);
        _attemptOrder.Enqueue(key);
        if (_attemptOrder.Count > 128) _attempted.Remove(_attemptOrder.Dequeue());
        _current = new Attempt
        {
            GamePid = game.Pid, GameStartedTicks = game.StartedTicks,
            StartedUtc = DateTime.UtcNow.ToString("o")
        };
        _history.Add(_current);
        if (_history.Count > 32) _history.RemoveAt(0);
        try
        {
            var pluginDirectory = Path.Combine(root, "reframework", "plugins");
            if (Hash(Path.Combine(pluginDirectory, "codex_dualsense_bridge.dll")) != PluginHash)
                throw new IOException(UiText.Get("TheGameComponentsDoNotMatchTheCompatibilityEditionExit"));
            var helper = Path.Combine(pluginDirectory, "codex_dualsense_hid_helper.exe");
            var broker = Path.Combine(_payload, "bin", "external-helper-host.exe");
            _helperLock = LockVerified(helper, _expectedHelper);
            _brokerLock = LockVerified(broker, BrokerHash);
            Directory.CreateDirectory(_stateRoot);
            _current.StatePath = Path.Combine(_stateRoot,
                "game-" + game.Pid + "-" + Guid.NewGuid().ToString("N") + ".json");
            var command = new ProcessStartInfo(broker)
            {
                UseShellExecute = false, CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden, WorkingDirectory = _stateRoot
            };
            foreach (var value in new[] { "--serve", helper, root, game.Pid.ToString(), _current.StatePath })
                command.ArgumentList.Add(value);
            _monitor = Process.Start(command) ?? throw new IOException(UiText.Get("TheHelperLauncherWasNotCreated"));
            _status = UiText.Get("TheBackgroundHelperStartedClosingTheMainWindowKeepsThe");
        }
        catch (Exception error)
        {
            _current.Error = error.GetType().Name + ": " + error.Message;
            _status = UiText.Get("TheHelperDidNotStart") + error.Message;
            ReleaseMonitor();
        }
        SaveHistory();
    }

    internal static List<(int Pid, long StartedTicks)> FindGames(string root)
    {
        var result = new List<(int, long)>();
        var expected = Path.GetFullPath(Path.Combine(root, "OnimushaWotS.exe"));
        foreach (var process in Process.GetProcessesByName("OnimushaWotS"))
        {
            using (process)
            {
                try
                {
                    if (!process.HasExited && string.Equals(process.MainModule?.FileName,
                            expected, StringComparison.OrdinalIgnoreCase))
                        result.Add((process.Id, process.StartTime.ToUniversalTime().Ticks));
                }
                catch (System.ComponentModel.Win32Exception) { }
                catch (InvalidOperationException) { }
            }
        }
        return result;
    }

    private static bool DiagnosticWindowOpen()
    {
        try { using var handle = Mutex.OpenExisting(@"Local\OnimushaStartupDiagnosticR1"); return true; }
        catch (WaitHandleCannotBeOpenedException) { return false; }
        catch (UnauthorizedAccessException) { return true; }
    }
    private static string Hash(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }
    private static FileStream LockVerified(string path, string expected)
    {
        var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        try
        {
            if (Convert.ToHexString(SHA256.HashData(stream)) != expected)
                throw new IOException(UiText.Get("ComponentVerificationFailed") + Path.GetFileName(path));
            return stream;
        }
        catch { stream.Dispose(); throw; }
    }

    private static readonly string[] StateKeys =
    [
        "Version", "UpdatedUtc", "GamePid", "HelperPid", "Stage", "Error", "ErrorStage",
        "Created", "LaunchMs", "Mitigation", "PipeAvailableObserved",
        "NaturalExit", "ExitCode", "ControlledStop", "CleanupConfirmed"
    ];
    private void ReadState()
    {
        if (_current is null || !File.Exists(_current.StatePath)) return;
        try
        {
            if (new FileInfo(_current.StatePath).Length > 65536) throw new IOException(UiText.Get("TheStartupStatusRecordIsTooLarge"));
            using var document = JsonDocument.Parse(File.ReadAllText(_current.StatePath));
            var source = document.RootElement;
            if (source.GetProperty("Version").GetString() != "ExternalHelperHostR3"
                || source.GetProperty("GamePid").GetInt32() != _current.GamePid)
                throw new IOException(UiText.Get("TheStartupStatusIdentityDoesNotMatch"));
            if (_current.State is JsonElement previous
                && previous.TryGetProperty("UpdatedUtc", out var previousTime)
                && source.TryGetProperty("UpdatedUtc", out var currentTime)
                && previousTime.GetString() == currentTime.GetString()) return;
            var allowed = new Dictionary<string, JsonElement>();
            foreach (var key in StateKeys)
                if (source.TryGetProperty(key, out var value)) allowed[key] = value.Clone();
            _current.State = JsonSerializer.SerializeToElement(allowed);
            SaveHistory();
        }
        catch (IOException) { } // The broker atomically replaces this file.
        catch (JsonException) { }
    }
    private void SaveHistory()
    {
        try
        {
            Directory.CreateDirectory(_stateRoot);
            var path = Path.Combine(_stateRoot, "last-session.json");
            var temporary = path + ".tmp";
            File.WriteAllText(temporary, JsonSerializer.Serialize(new
            {
                Mode = "startup-compatibility-repair1",
                UpdatedUtc = DateTime.UtcNow.ToString("o"), Attempts = _history
            }));
            File.Move(temporary, path, true);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
    private void StopMonitor()
    {
        try
        {
            if (_monitor is not null && !_monitor.WaitForExit(6000))
            {
                // The broker's private job contains only its own helper.
                _monitor.Kill();
                _monitor.WaitForExit(2000);
                if (_current is not null) _current.MonitorStoppedByTool = true;
            }
            ReadState();
            SaveHistory();
        }
        finally { ReleaseMonitor(); }
    }
    private void ReleaseMonitor()
    {
        _monitor?.Dispose(); _monitor = null;
        _helperLock?.Dispose(); _helperLock = null;
        _brokerLock?.Dispose(); _brokerLock = null;
    }
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _stop.Cancel();
        _worker?.GetAwaiter().GetResult();
        _stop.Dispose();
    }
}
#endif
