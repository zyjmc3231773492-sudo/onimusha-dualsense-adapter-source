using OnimushaPS5Adapter.Core;
using System.IO;
using System.Windows;
using Application = System.Windows.Application;
using MessageBox = System.Windows.MessageBox;

namespace OnimushaPS5Adapter.Manager;

public partial class App : Application
{
    internal static bool PreviewMode { get; private set; }
#if ONIMUSHA_STARTUP_REPAIR
    private Mutex? _instance;
    private bool _ownsInstance;
#endif
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        PreviewMode = e.Args.Contains("--render-ui", StringComparer.OrdinalIgnoreCase);
        UiText.Initialize(e.Args, persistRequested: !PreviewMode);
        var arguments = UiText.RemoveLanguageArguments(e.Args);
        if (PreviewMode)
        {
            RenderPreview(arguments);
            return;
        }
        if (arguments.Length > 0)
        {
            var exitCode = RunMaintenanceCommand(arguments);
            if (exitCode.HasValue)
            {
                Shutdown(exitCode.Value);
                return;
            }
        }

        var startHidden = e.Args.Any(argument =>
            argument.Equals("--startup", StringComparison.OrdinalIgnoreCase));
#if ONIMUSHA_STARTUP_REPAIR
        if (e.Args.Length == 2 && e.Args[0] == "--startup-repair-ui-preview")
        {
            PreviewMode = true;
            var preview = new MainWindow { Opacity = 0, ShowInTaskbar = false };
            MainWindow = preview;
            preview.Show();
            preview.UpdateLayout();
            var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap(
                (int)preview.ActualWidth, (int)preview.ActualHeight, 96, 96,
                System.Windows.Media.PixelFormats.Pbgra32);
            bitmap.Render((UIElement)preview.Content);
            var encoder = new System.Windows.Media.Imaging.PngBitmapEncoder();
            encoder.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bitmap));
            using (var stream = File.Create(Path.GetFullPath(e.Args[1]))) encoder.Save(stream);
            Shutdown();
            return;
        }
        _instance = new Mutex(true, @"Local\OnimushaPS5AdapterStartupRepairManager", out _ownsInstance);
        if (!_ownsInstance)
        {
            if (!startHidden) MessageBox.Show(UiText.Get("TheAdapterIsAlreadyRunningDoubleClickItsSystemTray"),
                UiText.Get("AppTitle"), MessageBoxButton.OK, MessageBoxImage.Information);
            Shutdown();
            return;
        }
#endif
        var window = new MainWindow(startHidden);
        MainWindow = window;
        window.Show();
    }

    private void RenderPreview(string[] arguments)
    {
        var index = Array.FindIndex(arguments, value => value.Equals("--render-ui", StringComparison.OrdinalIgnoreCase));
        if (index < 0 || index + 1 >= arguments.Length) { Shutdown(2); return; }
        try
        {
            var path = Path.GetFullPath(arguments[index + 1]);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var preview = new MainWindow
            {
                Opacity = 0, ShowInTaskbar = false,
                WindowStyle = WindowStyle.None, ResizeMode = ResizeMode.NoResize
            };
            MainWindow = preview;
            preview.Show();
            var tabIndex = Array.FindIndex(arguments, value => value.Equals("--tab", StringComparison.OrdinalIgnoreCase));
            if (tabIndex >= 0 && tabIndex + 1 < arguments.Length && int.TryParse(arguments[tabIndex + 1], out var tab))
                ((System.Windows.Controls.TabControl)preview.FindName("MainTabs")).SelectedIndex = Math.Clamp(tab, 0, 3);
            preview.UpdateLayout();
            if (arguments.Contains("--scroll-to-end", StringComparer.OrdinalIgnoreCase))
            {
                ((System.Windows.Controls.ScrollViewer)preview.FindName("SettingsScrollViewer")).ScrollToEnd();
                preview.UpdateLayout();
            }
            preview.Dispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.Render);
            var content = (FrameworkElement)preview.Content;
            var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap(
                (int)Math.Ceiling(content.ActualWidth), (int)Math.Ceiling(content.ActualHeight), 96, 96,
                System.Windows.Media.PixelFormats.Pbgra32);
            bitmap.Render(content);
            var encoder = new System.Windows.Media.Imaging.PngBitmapEncoder();
            encoder.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bitmap));
            using (var stream = File.Create(path)) encoder.Save(stream);
            Shutdown(0);
        }
        catch (Exception exception)
        {
            System.Diagnostics.Debug.WriteLine(exception);
            Shutdown(1);
        }
    }

    private static int? RunMaintenanceCommand(string[] args)
    {
        try
        {
            if (args[0].Equals("--repair", StringComparison.OrdinalIgnoreCase) && args.Length >= 3 && args.Length <= 4)
            {
#if ONIMUSHA_STARTUP_REPAIR
                var result = StartupRepairMaintenance.Install(args[1], args[2], args.Length == 4 ? args[3] : null);
#else
                var result = new InstallService().InstallOrRepair(args[1], args[2]);
#endif
                MessageBox.Show(ServiceText.Translate(result.Message), UiText.Get("AppTitle"),
                    MessageBoxButton.OK, result.Success ? MessageBoxImage.Information : MessageBoxImage.Error);
                return result.Success ? 0 : 1;
            }
            if (args[0].Equals("--auto-install", StringComparison.OrdinalIgnoreCase))
            {
                string[] detectedRoots = [];
#if ONIMUSHA_STARTUP_REPAIR
                var roots = StartupRepairMaintenance.LocateGames();
                detectedRoots = roots.ToArray();
                var gameRoot = roots.Count == 1 ? roots[0] : null;
#else
                var previousRoot = InstallService.LoadManifest()?.GameRoot;
                var gameRoot = !string.IsNullOrWhiteSpace(previousRoot) &&
                    File.Exists(Path.Combine(previousRoot, "OnimushaWotS.exe")) ? previousRoot : null;
                if (gameRoot is null)
                {
                    detectedRoots = SteamLocator.FindGameDirectories().ToArray();
                    if (detectedRoots.Length == 1) gameRoot = detectedRoots[0];
                }
#endif
                if (gameRoot is null)
                {
                    MessageBox.Show(
                        detectedRoots.Length > 1
                            ? UiText.Get("MultipleGameFoldersDetected", string.Join(Environment.NewLine, detectedRoots))
                            : UiText.Get("NoGameFolderDetected"),
                        UiText.Get("AppTitle"),
                        MessageBoxButton.OK,
                        MessageBoxImage.Warning);
                    return 1;
                }
                var payloadRoot = Path.Combine(AppContext.BaseDirectory, "payload");
#if ONIMUSHA_STARTUP_REPAIR
                var result = StartupRepairMaintenance.Install(gameRoot, payloadRoot);
#else
                var result = new InstallService().InstallOrRepair(gameRoot, payloadRoot);
#endif
                MessageBox.Show(ServiceText.Translate(result.Message), UiText.Get("AppTitle"),
                    MessageBoxButton.OK, result.Success ? MessageBoxImage.Information : MessageBoxImage.Error);
                return result.Success ? 0 : 1;
            }
            if (args[0].Equals("--uninstall-bridge", StringComparison.OrdinalIgnoreCase))
            {
                var result = new InstallService().UninstallBridge();
                MessageBox.Show(ServiceText.Translate(result.Message), UiText.Get("AppTitle"),
                    MessageBoxButton.OK, result.Success ? MessageBoxImage.Information : MessageBoxImage.Error);
                return result.Success ? 0 : 1;
            }
            if (args[0].Equals("--uninstall-bridge-quiet", StringComparison.OrdinalIgnoreCase))
            {
                var result = new InstallService().UninstallBridge();
                try
                {
                    StartupRegistration.SetEnabled(Environment.ProcessPath ?? string.Empty, enabled: false);
                }
                catch
                {
                    // A stale startup entry must not prevent the bridge from being uninstalled.
                }
                return result.Success ? 0 : 1;
            }
            if (args[0].Equals("--can-uninstall", StringComparison.OrdinalIgnoreCase))
                return System.Diagnostics.Process.GetProcessesByName("OnimushaWotS").Length == 0
                    ? 0
                    : 1;
#if ONIMUSHA_STARTUP_REPAIR
            if (args[0].Equals("--enable-compatibility-startup", StringComparison.OrdinalIgnoreCase))
            {
                StartupRegistration.SetEnabled(Environment.ProcessPath ?? string.Empty, enabled: true);
                return 0;
            }
#endif
        }
        catch (Exception exception)
        {
            MessageBox.Show(exception.Message, UiText.Get("AppTitle"),
                MessageBoxButton.OK, MessageBoxImage.Error);
            return 1;
        }
        return null;
    }
#if ONIMUSHA_STARTUP_REPAIR
    protected override void OnExit(ExitEventArgs e)
    {
        if (_ownsInstance) { _instance?.ReleaseMutex(); _ownsInstance = false; }
        _instance?.Dispose();
        base.OnExit(e);
    }
#endif
}
