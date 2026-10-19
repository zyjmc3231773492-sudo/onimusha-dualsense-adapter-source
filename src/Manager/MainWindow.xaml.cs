using Microsoft.Win32;
using OnimushaPS5Adapter.Core;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Application = System.Windows.Application;
using Button = System.Windows.Controls.Button;
using Brush = System.Windows.Media.Brush;
using Brushes = System.Windows.Media.Brushes;
using Drawing = System.Drawing;
using Forms = System.Windows.Forms;
using MessageBox = System.Windows.MessageBox;

namespace OnimushaPS5Adapter.Manager;

public partial class MainWindow : Window
{
    private readonly DispatcherTimer _refreshTimer;
    private readonly Forms.NotifyIcon _trayIcon;
    private readonly Drawing.Icon? _ownedTrayIcon;
    private readonly bool _startHidden;
    private ProductSettings _settings = new();
    private bool _loadingSettings;
    private bool _loadingStartup;
    private bool _allowClose;
    private bool _trayDisposed;
    private bool _previewBusy;
#if ONIMUSHA_STARTUP_REPAIR
    private readonly ExternalHelperRuntime _externalRuntime = new();
#endif
    private Button[] PreviewButtons => [GauntletVoicePreviewButton, SoulSpeakerPreviewButton,
        ClashSpeakerPreviewButton, GuardSpeakerPreviewButton, ParryBladePreviewButton,
        ParryProjectilePreviewButton, DeflectSpeakerPreviewButton, ExecutionSpeakerPreviewButton,
        IssenSpeakerPreviewButton];

    public MainWindow()
        : this(startHidden: false)
    {
    }

    public MainWindow(bool startHidden)
    {
        if (System.Windows.Application.Current?.TryFindResource("AppTitle") is null) UiText.Initialize();
        InitializeComponent();
        LanguageCombo.SelectedIndex = UiText.Language == "en-US" ? 1 : 0;
        UiText.LanguageChanged += RefreshLanguage;
        ProductVersionText.Text = UiText.Get("VersionDisplay", ProductConstants.ProductVersion, ProductConstants.BridgeBaselineVersion);
#if ONIMUSHA_BLUETOOTH_ENABLED
        ConnectionModeText.Text = "USB / BLUETOOTH";
        DeviceStatusDetailText.Text = UiText.Get("CheckingUsbBluetoothHidDevices");
        foreach (var button in PreviewButtons) button.Visibility = Visibility.Visible;
        SpeakerPreviewStatusText.Visibility = Visibility.Visible;
#endif
#if ONIMUSHA_STARTUP_REPAIR
        Title += UiText.Get("CompatibilityEdition");
        ProductVersionText.Text = UiText.Get("VersionDisplay", ProductConstants.ProductVersion, ProductConstants.BridgeBaselineVersion) + UiText.Get("CompatibilityVersionSuffix");
        AboutBehaviorText.Text = UiText.Get("ThisCompatibilityEditionStartsAHelperInTheBackgroundClosing");
#endif
        _startHidden = startHidden;
        if (_startHidden)
        {
            ShowActivated = false;
            ShowInTaskbar = false;
            WindowState = WindowState.Minimized;
        }
        _refreshTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _refreshTimer.Tick += (_, _) => RefreshStatus();
        (_trayIcon, _ownedTrayIcon) = CreateTrayIcon();
        Loaded += OnLoaded;
        StateChanged += OnWindowStateChanged;
        Closing += OnClosing;
        Application.Current!.SessionEnding += OnSessionEnding;
        Application.Current.Exit += OnApplicationExit;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        if (App.PreviewMode)
        {
            _trayIcon.Visible = false;
            GamePathText.Text = @"C:\Games\OnimushaWotS";
            LoadSettings();
            LoadDonationImages();
            DeviceStatusText.Text = UiText.Get("PreviewDeviceConnected");
            DeviceStatusDetailText.Text = "DualSense · USB";
            DeviceStatusDot.Background = Brushes.LightGreen;
            InstallStatusText.Text = UiText.Get("PreviewBridgeReady");
            InstallStatusText.Foreground = Brushes.LightGreen;
            StartupStatusText.Text = UiText.Get("StartupDisabled");
            FooterText.Text = UiText.Get("Ready");
            return;
        }
#if ONIMUSHA_STARTUP_REPAIR
        _externalRuntime.Start();
#endif
        var installed = InstallService.LoadManifest();
        GamePathText.Text = installed?.GameRoot ??
#if ONIMUSHA_STARTUP_REPAIR
            StartupRepairMaintenance.LocateGames().FirstOrDefault() ??
#else
            SteamLocator.FindGameDirectories().FirstOrDefault() ??
#endif
            string.Empty;
        LoadSettings();
        LoadStartupSetting();
        LoadDonationImages();
        RefreshStatus();
        _refreshTimer.Start();
        if (Environment.GetCommandLineArgs().Contains("--speaker-debug", StringComparer.OrdinalIgnoreCase))
        {
            MainTabs.SelectedIndex = 1;
            Dispatcher.BeginInvoke(() => SettingsScrollViewer.ScrollToEnd(), DispatcherPriority.Loaded);
        }
        if (_startHidden)
            Dispatcher.BeginInvoke(() => HideToTray(showNotification: false));
    }

    private void OnWindowStateChanged(object? sender, EventArgs e)
    {
        if (WindowState != WindowState.Minimized)
            return;

        Dispatcher.BeginInvoke(() => HideToTray(showNotification: true));
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (!_allowClose)
        {
            e.Cancel = true;
            HideToTray(showNotification: true);
            return;
        }

        UiText.LanguageChanged -= RefreshLanguage;
        _refreshTimer.Stop();
#if ONIMUSHA_STARTUP_REPAIR
        _externalRuntime.Dispose();
#endif
        DisposeTrayIcon();
    }

    private void OnSessionEnding(object sender, SessionEndingCancelEventArgs e) => _allowClose = true;

    private void OnApplicationExit(object sender, ExitEventArgs e)
    {
#if ONIMUSHA_STARTUP_REPAIR
        _externalRuntime.Dispose();
#endif
        DisposeTrayIcon();
    }

    private (Forms.NotifyIcon TrayIcon, Drawing.Icon? OwnedIcon) CreateTrayIcon()
    {
        var contextMenu = new Forms.ContextMenuStrip
        {
            BackColor = Drawing.Color.FromArgb(18, 18, 15),
            ForeColor = Drawing.Color.FromArgb(242, 239, 231),
            ShowImageMargin = false,
            Renderer = new Forms.ToolStripProfessionalRenderer(new TrayMenuColorTable())
        };

        var openItem = new Forms.ToolStripMenuItem(UiText.Get("TrayOpen"));
        var minimizeItem = new Forms.ToolStripMenuItem(UiText.Get("TrayMinimize"));
        var launchItem = new Forms.ToolStripMenuItem(UiText.Get("TrayLaunch"));
        var exitItem = new Forms.ToolStripMenuItem(UiText.Get("TrayExit"))
        {
            ForeColor = Drawing.Color.FromArgb(225, 194, 123)
        };

        openItem.Click += (_, _) => Dispatcher.BeginInvoke(RestoreFromTray);
        minimizeItem.Click += (_, _) => Dispatcher.BeginInvoke(() => HideToTray(showNotification: false));
        launchItem.Click += (_, _) => Dispatcher.BeginInvoke(LaunchGame);
        exitItem.Click += (_, _) => Dispatcher.BeginInvoke(ExitApplication);
        contextMenu.Items.Add(openItem);
        contextMenu.Items.Add(minimizeItem);
        contextMenu.Items.Add(new Forms.ToolStripSeparator());
        contextMenu.Items.Add(launchItem);
        contextMenu.Items.Add(new Forms.ToolStripSeparator());
        contextMenu.Items.Add(exitItem);
        contextMenu.Opening += (_, _) =>
        {
            openItem.Enabled = !IsVisible || WindowState == WindowState.Minimized;
            minimizeItem.Enabled = IsVisible && WindowState != WindowState.Minimized;
        };

        Drawing.Icon? icon = null;
        var executablePath = Environment.ProcessPath;
        try
        {
            if (!string.IsNullOrWhiteSpace(executablePath) && File.Exists(executablePath))
                icon = Drawing.Icon.ExtractAssociatedIcon(executablePath);
        }
        catch
        {
            // The application can still use the Windows fallback icon.
        }

        var trayIcon = new Forms.NotifyIcon
        {
            ContextMenuStrip = contextMenu,
            Icon = icon ?? Drawing.SystemIcons.Application,
            Text = UiText.Get("TrayTitle"),
            Visible = !App.PreviewMode
        };
        trayIcon.DoubleClick += (_, _) => Dispatcher.BeginInvoke(RestoreFromTray);
        return (trayIcon, icon);
    }

    private void HideToTray(bool showNotification)
    {
#if ONIMUSHA_STARTUP_REPAIR
        if (App.PreviewMode) showNotification = false;
#endif
        if (_allowClose)
            return;

        ShowInTaskbar = false;
        Hide();
        if (showNotification)
        {
            _trayIcon.ShowBalloonTip(
                3500,
                UiText.Get("ManagerIsStillRunning"),
                UiText.Get("TheManagerIsRunningInTheSystemTrayDoubleClick"),
                Forms.ToolTipIcon.Info);
        }
    }

    private void RestoreFromTray()
    {
        if (_allowClose)
            return;

        ShowInTaskbar = true;
        Show();
        WindowState = WindowState.Normal;
        Activate();
        Topmost = true;
        Topmost = false;
        Focus();
    }

    private static void LaunchGame()
    {
        Process.Start(new ProcessStartInfo($"steam://run/{ProductConstants.SteamAppId}")
        {
            UseShellExecute = true
        });
    }

    private void ExitApplication()
    {
#if ONIMUSHA_STARTUP_REPAIR
        if (_externalRuntime.HasActiveGame)
        {
            MessageBox.Show(UiText.Get("ExitTheGameBeforeQuittingTheAdapterFromTheTray"),
                UiText.Get("AppTitle"), MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }
#endif
        _allowClose = true;
        Close();
        Application.Current.Shutdown();
    }

    private void DisposeTrayIcon()
    {
        if (_trayDisposed)
            return;

        _trayDisposed = true;
        _trayIcon.Visible = false;
        _trayIcon.ContextMenuStrip?.Dispose();
        _trayIcon.Dispose();
        _ownedTrayIcon?.Dispose();
    }

    private void RefreshStatus()
    {
        var device = DeviceConnectionService.GetSnapshot();
        DeviceStatusText.Text = device.Status;
        DeviceStatusDetailText.Text = device.Detail;
        var statusBrushKey = device.IsConnected
            ? "GoldBrightBrush"
            : device.DetectionAvailable ? "CrimsonBrush" : "MutedTextBrush";
        var statusBrush = Application.Current.TryFindResource(statusBrushKey) as Brush ?? Brushes.Gray;
        DeviceStatusDot.Background = statusBrush;
        DeviceStatusText.Foreground = statusBrush;

        var manifest = InstallService.LoadManifest();
        if (manifest is null)
        {
            InstallStatusText.Foreground = Brushes.Gray;
            InstallStatusText.Text = UiText.Get("GameBridgeIsNotInstalled");
        }
        else
        {
            InstallStatusText.Text =
                UiText.Get("InstalledVersion0CoreV1", manifest.ProductVersion, manifest.BridgeBaselineVersion) +
                manifest.GameRoot;
            try
            {
                var bridgeVerification = InstallService.VerifyBridgeSynchronization(
                    manifest.GameRoot,
                    FindPayloadRoot());
                InstallStatusText.Foreground = bridgeVerification.IsSynchronized
                    ? Brushes.LightGreen
                    : Brushes.IndianRed;
                InstallStatusText.Text += "\n" + ServiceText.Translate(bridgeVerification.Message);
#if ONIMUSHA_STARTUP_REPAIR
                if (bridgeVerification.IsSynchronized) InstallStatusText.Text += "\n" + _externalRuntime.Status;
#endif
            }
            catch (Exception exception)
            {
                InstallStatusText.Foreground = Brushes.IndianRed;
                InstallStatusText.Text += UiText.Get("CouldNotVerifyBridgeSynchronization") + exception.Message;
            }
        }
        FooterText.Text = UiText.Get("Ready");
    }

    private void LoadSettings()
    {
        _loadingSettings = true;
        _settings = App.PreviewMode ? new ProductSettings() : SettingsStore.Load();
        RainCheck.IsChecked = _settings.RainEnabled;
        WindCheck.IsChecked = _settings.WindEnabled;
        HapticIntensitySlider.Value = _settings.HapticIntensityPercent;
        GuardHapticCheck.IsChecked = _settings.GuardHapticEnabled;
        GuardHapticIntensitySlider.Value = _settings.GuardHapticIntensityPercent;
        ParryHapticCheck.IsChecked = _settings.ParryHapticEnabled;
        ParryHapticIntensitySlider.Value = _settings.ParryHapticIntensityPercent;
        DeflectHapticCheck.IsChecked = _settings.DeflectHapticEnabled;
        DeflectHapticIntensitySlider.Value = _settings.DeflectHapticIntensityPercent;
        IssenHapticCheck.IsChecked = _settings.IssenHapticEnabled;
        IssenHapticIntensitySlider.Value = _settings.IssenHapticIntensityPercent;
        LeftAdaptiveTriggerCheck.IsChecked = _settings.LeftAdaptiveTriggerEnabled;
        RightAdaptiveTriggerCheck.IsChecked = _settings.RightAdaptiveTriggerEnabled;
        ControllerSpeakerCheck.IsChecked = _settings.ControllerSpeakerEnabled;
        JapaneseVoiceRadio.IsChecked = string.Equals(
            _settings.GauntletVoiceLanguage,
            ProductSettings.JapaneseGauntletVoice,
            StringComparison.OrdinalIgnoreCase);
        ChineseVoiceRadio.IsChecked = JapaneseVoiceRadio.IsChecked != true;
        GauntletVoiceVolumeSlider.Value = _settings.GauntletVoiceVolumePercent;
        SoulSpeakerVolumeSlider.Value = _settings.SoulSpeakerVolumePercent;
        ClashSpeakerVolumeSlider.Value = _settings.ClashSpeakerVolumePercent;
        GuardSpeakerVolumeSlider.Value = _settings.GuardSpeakerVolumePercent;
        ParrySpeakerVolumeSlider.Value = _settings.ParrySpeakerVolumePercent;
        DeflectSpeakerVolumeSlider.Value = _settings.DeflectSpeakerVolumePercent;
        ExecutionSpeakerVolumeSlider.Value = _settings.ExecutionSpeakerVolumePercent;
        IssenSpeakerVolumeSlider.Value = _settings.IssenSpeakerVolumePercent;
        UpdateSpeakerVolumeLabels();
        _loadingSettings = false;
    }

    private void LoadStartupSetting()
    {
        _loadingStartup = true;
        try
        {
            var executablePath = Environment.ProcessPath ?? string.Empty;
            var enabled = StartupRegistration.IsEnabled(executablePath);
            if (enabled)
                StartupRegistration.SetEnabled(executablePath, enabled: true);
            StartupCheck.IsChecked = enabled;
            StartupStatusText.Text = enabled
                ? UiText.Get("StartupEnabled")
                : UiText.Get("StartupDisabled");
        }
        catch (Exception exception)
        {
            StartupCheck.IsChecked = false;
            StartupStatusText.Text = UiText.Get("CouldNotReadStartupSetting") + exception.Message;
        }
        finally
        {
            _loadingStartup = false;
        }
    }

    private void StartupChanged(object sender, RoutedEventArgs e)
    {
        if (!IsLoaded || _loadingStartup)
            return;

        var enabled = StartupCheck.IsChecked == true;
        try
        {
            StartupRegistration.SetEnabled(Environment.ProcessPath ?? string.Empty, enabled);
            StartupStatusText.Text = enabled
                ? UiText.Get("StartupEnabled")
                : UiText.Get("StartWithWindowsIsOff");
        }
        catch (Exception exception)
        {
            _loadingStartup = true;
            StartupCheck.IsChecked = !enabled;
            _loadingStartup = false;
            StartupStatusText.Text = UiText.Get("CouldNotChangeStartupSetting") + exception.Message;
        }
    }

    private void SettingChanged(object sender, RoutedEventArgs e)
    {
        if (!IsLoaded || _loadingSettings)
            return;
        UpdateSpeakerVolumeLabels();
        _settings.RainEnabled = RainCheck.IsChecked == true;
        _settings.WindEnabled = WindCheck.IsChecked == true;
        _settings.HapticIntensityPercent = SliderPercent(HapticIntensitySlider);
        _settings.GuardHapticEnabled = GuardHapticCheck.IsChecked == true;
        _settings.GuardHapticIntensityPercent = SliderPercent(GuardHapticIntensitySlider);
        _settings.ParryHapticEnabled = ParryHapticCheck.IsChecked == true;
        _settings.ParryHapticIntensityPercent = SliderPercent(ParryHapticIntensitySlider);
        _settings.DeflectHapticEnabled = DeflectHapticCheck.IsChecked == true;
        _settings.DeflectHapticIntensityPercent = SliderPercent(DeflectHapticIntensitySlider);
        _settings.IssenHapticEnabled = IssenHapticCheck.IsChecked == true;
        _settings.IssenHapticIntensityPercent = SliderPercent(IssenHapticIntensitySlider);
        _settings.LeftAdaptiveTriggerEnabled = LeftAdaptiveTriggerCheck.IsChecked == true;
        _settings.RightAdaptiveTriggerEnabled = RightAdaptiveTriggerCheck.IsChecked == true;
        _settings.ControllerSpeakerEnabled = ControllerSpeakerCheck.IsChecked == true;
        _settings.GauntletVoiceLanguage = JapaneseVoiceRadio.IsChecked == true
            ? ProductSettings.JapaneseGauntletVoice
            : ProductSettings.ChineseGauntletVoice;
        _settings.GauntletVoiceVolumePercent = SliderPercent(GauntletVoiceVolumeSlider);
        _settings.SoulSpeakerVolumePercent = SliderPercent(SoulSpeakerVolumeSlider);
        _settings.ClashSpeakerVolumePercent = SliderPercent(ClashSpeakerVolumeSlider);
        _settings.GuardSpeakerVolumePercent = SliderPercent(GuardSpeakerVolumeSlider);
        _settings.ParrySpeakerVolumePercent = SliderPercent(ParrySpeakerVolumeSlider);
        _settings.DeflectSpeakerVolumePercent = SliderPercent(DeflectSpeakerVolumeSlider);
        _settings.ExecutionSpeakerVolumePercent = SliderPercent(ExecutionSpeakerVolumeSlider);
        _settings.IssenSpeakerVolumePercent = SliderPercent(IssenSpeakerVolumeSlider);
        try
        {
            SettingsStore.Save(_settings);
            SettingsStatusText.Text = UiText.Get("SettingsSavedTheyWillApplyWithinOneSecond");
        }
        catch (Exception exception)
        {
            SettingsStatusText.Text = UiText.Get("CouldNotSave") + exception.Message;
        }
    }

    private void UpdateSpeakerVolumeLabels()
    {
        HapticIntensityText.Text = $"{SliderPercent(HapticIntensitySlider)}%";
        GuardHapticIntensityText.Text = $"{SliderPercent(GuardHapticIntensitySlider)}%";
        ParryHapticIntensityText.Text = $"{SliderPercent(ParryHapticIntensitySlider)}%";
        DeflectHapticIntensityText.Text = $"{SliderPercent(DeflectHapticIntensitySlider)}%";
        IssenHapticIntensityText.Text = $"{SliderPercent(IssenHapticIntensitySlider)}%";
        GauntletVoiceVolumeText.Text = $"{SliderPercent(GauntletVoiceVolumeSlider)}%";
        SoulSpeakerVolumeText.Text = $"{SliderPercent(SoulSpeakerVolumeSlider)}%";
        ClashSpeakerVolumeText.Text = $"{SliderPercent(ClashSpeakerVolumeSlider)}%";
        GuardSpeakerVolumeText.Text = $"{SliderPercent(GuardSpeakerVolumeSlider)}%";
        ParrySpeakerVolumeText.Text = $"{SliderPercent(ParrySpeakerVolumeSlider)}%";
        DeflectSpeakerVolumeText.Text = $"{SliderPercent(DeflectSpeakerVolumeSlider)}%";
        ExecutionSpeakerVolumeText.Text = $"{SliderPercent(ExecutionSpeakerVolumeSlider)}%";
        IssenSpeakerVolumeText.Text = $"{SliderPercent(IssenSpeakerVolumeSlider)}%";
        UpdatePreviewButtons();
    }

    private void UpdatePreviewButtons()
    {
        foreach (var button in PreviewButtons)
            button.IsEnabled = !_previewBusy && ControllerSpeakerCheck.IsChecked == true;
    }

    private async void SoundPreview_Click(object sender, RoutedEventArgs e)
    {
        if (_previewBusy || sender is not Button button || !byte.TryParse(button.Tag?.ToString(), out var cue)) return;
        var (slider, label) = cue switch
        {
            2 => (SoulSpeakerVolumeSlider, UiText.Get("SoulAbsorption2")),
            4 => (ClashSpeakerVolumeSlider, UiText.Get("SwordClash")),
            5 => (GuardSpeakerVolumeSlider, UiText.Get("Guard")),
            6 => (DeflectSpeakerVolumeSlider, UiText.Get("Deflect")),
            7 => (ExecutionSpeakerVolumeSlider, UiText.Get("Execution")),
            11 => (IssenSpeakerVolumeSlider, UiText.Get("Issen")),
            12 => (ParrySpeakerVolumeSlider, UiText.Get("BladeParry")),
            13 => (ParrySpeakerVolumeSlider, UiText.Get("ProjectileParry")),
            14 => (GauntletVoiceVolumeSlider, UiText.Get("GauntletVoice")),
            _ => throw new InvalidOperationException(UiText.Get("UnknownAudioCategory"))
        };
        var games = Process.GetProcessesByName("OnimushaWotS");
        _previewBusy = true;
        UpdatePreviewButtons();
        SpeakerPreviewStatusText.Text = UiText.Get("Requesting0Preview", label);
        try
        {
            if (games.Length == 0)
            {
                SpeakerPreviewStatusText.Text = UiText.Get("StartTheGameAndConnectAControllerBeforePreviewingAudio");
                return;
            }
            var volume = SliderPercent(slider);
            var status = await SpeakerPreviewService.PlayAsync(games[0].Id, cue, volume);
            SpeakerPreviewStatusText.Text = status switch
            {
                1 => volume == 0 ? UiText.Get("0PreviewAcceptedCurrentVolumeIsMuted0", label) : UiText.Get("0PreviewAccepted1CheckTheControllerSActualOutput", label, volume),
                2 => UiText.Get("TurnOnTheControllerSpeakerFirst"),
                3 => UiText.Get("NoControllerIsConnectedConnectByUsbOrBluetoothAnd"),
                4 => UiText.Get("AudioServiceIsUnavailableCheckThatTheLatestBridge"),
                5 => UiText.Get("ControllerAudioOrPreviewMaterialIsNotReadyTryAgain"),
                _ => UiText.Get("PreviewRequestWasRejectedCheckThatTheManagerAndBridge")
            };
        }
        catch (OperationCanceledException)
        {
            SpeakerPreviewStatusText.Text = UiText.Get("PreviewTimedOutCheckThatTheGameIsRunningWith");
        }
        catch (Exception exception)
        {
            SpeakerPreviewStatusText.Text = UiText.Get("PreviewFailed") + exception.Message;
        }
        finally
        {
            foreach (var game in games) game.Dispose();
            _previewBusy = false;
            UpdatePreviewButtons();
        }
    }

    private static int SliderPercent(Slider slider) =>
        (int)Math.Round(slider.Value, MidpointRounding.AwayFromZero);

    private void LoadDonationImages()
    {
        DonationImages.LoadThumbnail("alipay", AlipayDonationImage, AlipayMissingText);
        DonationImages.LoadThumbnail("wechat", WechatDonationImage, WechatMissingText);
    }

    private void LanguageChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!IsLoaded || LanguageCombo.SelectedItem is not ComboBoxItem { Tag: string language }) return;
        UiText.SetLanguage(language);
    }

    private void RefreshLanguage()
    {
        RefreshTrayLanguage();
        ProductVersionText.Text = UiText.Get("VersionDisplay", ProductConstants.ProductVersion, ProductConstants.BridgeBaselineVersion);
#if ONIMUSHA_STARTUP_REPAIR
        ProductVersionText.Text += UiText.Get("CompatibilityVersionSuffix");
#endif
        StartupStatusText.Text = UiText.Get(StartupCheck.IsChecked == true ? "StartupEnabled" : "StartupDisabled");
        SettingsStatusText.Text = string.Empty;
        SpeakerPreviewStatusText.Text = UiText.Get("AudioPreviewInstructions");
        RefreshStatus();
    }

    private void RefreshTrayLanguage()
    {
        _trayIcon.Text = UiText.Get("TrayTitle");
        var menu = _trayIcon.ContextMenuStrip;
        if (menu is null) return;
        menu.Items[0].Text = UiText.Get("TrayOpen");
        menu.Items[1].Text = UiText.Get("TrayMinimize");
        menu.Items[3].Text = UiText.Get("TrayLaunch");
        menu.Items[5].Text = UiText.Get("TrayExit");
    }

    private void ShowContactQr_Click(object sender, RoutedEventArgs e)
    {
        var window = new ContactQrWindow { Owner = this };
        window.ShowDialog();
    }

    private void BrowseGame_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog
        {
            Title = UiText.Get("SelectTheGameFolderContainingOnimushawotsExe"),
            Multiselect = false
        };
        if (dialog.ShowDialog(this) == true)
            GamePathText.Text = dialog.FolderName;
    }

    private void Repair_Click(object sender, RoutedEventArgs e)
    {
        var gameRoot = GamePathText.Text.Trim();
        if (!File.Exists(Path.Combine(gameRoot, "OnimushaWotS.exe")))
        {
            MessageBox.Show(UiText.Get("SelectAValidGameFolder"), UiText.Get("AppTitle"),
                MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        var payload = FindPayloadRoot();
#if ONIMUSHA_STARTUP_REPAIR
        RunElevated("--repair", gameRoot, payload, StartupRepairMaintenance.StateRoot);
#else
        RunElevated("--repair", gameRoot, payload);
#endif
        RefreshStatus();
    }

    private void Uninstall_Click(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show(
                UiText.Get("ThisRemovesTheBridgeAndToolSettingsAndRestoresThe"),
                UiText.Get("AppTitle"),
                MessageBoxButton.YesNo,
                MessageBoxImage.Warning) != MessageBoxResult.Yes)
            return;
        RunElevated("--uninstall-bridge");
    }

    private void LaunchGame_Click(object sender, RoutedEventArgs e)
    {
        LaunchGame();
    }

    private void Refresh_Click(object sender, RoutedEventArgs e) => RefreshStatus();

    private static string FindPayloadRoot()
    {
        var direct = Path.Combine(AppContext.BaseDirectory, "payload");
        if (Directory.Exists(direct))
            return direct;
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            var candidate = Path.Combine(directory.FullName, "payload");
            if (Directory.Exists(candidate))
                return candidate;
            directory = directory.Parent;
        }
        throw new DirectoryNotFoundException(UiText.Get("CannotFindTheProductPayloadFolder"));
    }

    private static void RunElevated(params string[] arguments)
    {
        var executable = Environment.ProcessPath ?? throw new InvalidOperationException(UiText.Get("CannotLocateTheManagerExecutable"));
        var process = Process.Start(new ProcessStartInfo
        {
            FileName = executable,
            Arguments = string.Join(' ', arguments.Append("--language=" + UiText.Language).Select(QuoteArgument)),
            UseShellExecute = true,
            Verb = IsAdministrator() ? string.Empty : "runas"
        });
        process?.WaitForExit();
    }

    private static bool IsAdministrator()
    {
        using var identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    private static string QuoteArgument(string value) => "\"" + value.Replace("\"", "\\\"") + "\"";

    private sealed class TrayMenuColorTable : Forms.ProfessionalColorTable
    {
        public TrayMenuColorTable() => UseSystemColors = false;

        public override Drawing.Color ToolStripDropDownBackground => Drawing.Color.FromArgb(18, 18, 15);
        public override Drawing.Color ImageMarginGradientBegin => Drawing.Color.FromArgb(18, 18, 15);
        public override Drawing.Color ImageMarginGradientMiddle => Drawing.Color.FromArgb(18, 18, 15);
        public override Drawing.Color ImageMarginGradientEnd => Drawing.Color.FromArgb(18, 18, 15);
        public override Drawing.Color MenuBorder => Drawing.Color.FromArgb(112, 89, 47);
        public override Drawing.Color MenuItemBorder => Drawing.Color.FromArgb(185, 151, 84);
        public override Drawing.Color MenuItemSelected => Drawing.Color.FromArgb(45, 39, 25);
        public override Drawing.Color SeparatorDark => Drawing.Color.FromArgb(82, 68, 39);
        public override Drawing.Color SeparatorLight => Drawing.Color.FromArgb(82, 68, 39);
    }
}
