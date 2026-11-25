using System.Collections.ObjectModel;
using System.Diagnostics;
using Icon = System.Drawing.Icon;
using SystemIcons = System.Drawing.SystemIcons;
using MessageBox = System.Windows.MessageBox;
using KeyEventArgs = System.Windows.Input.KeyEventArgs;
using TextChangedEventArgs = System.Windows.Controls.TextChangedEventArgs;
using TextBox = System.Windows.Controls.TextBox;
using Color = System.Windows.Media.Color;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using Forms = System.Windows.Forms;

namespace VpnEcho;

public partial class MainWindow : Window
{
    private readonly VpnController _controller;
    private readonly ObservableCollection<ServerEntry> _visibleEntries = new();
    private readonly DispatcherTimer _renderTimer = new() { Interval = TimeSpan.FromMilliseconds(400) };
    private readonly DispatcherTimer _trafficTimer = new() { Interval = TimeSpan.FromSeconds(1) };
    private readonly Forms.NotifyIcon _tray;
    private readonly Forms.ToolStripMenuItem _trayDisconnect;
    private int _lastRevision = -1;
    private bool _allowClose;

    public MainWindow()
    {
        InitializeComponent();
        try { _controller = new VpnController(Dispatcher); }
        catch (Exception ex)
        {
            MessageBox.Show("VPN Echo could not load its three bundled VMess profiles. Repair or reinstall the app.\n\n" + ex.Message, "VPN Echo", MessageBoxButton.OK, MessageBoxImage.Error);
            throw;
        }
        _controller.Changed += Controller_Changed;
        ServerList.ItemsSource = _visibleEntries;
        ActivationView.Visibility = LicenseStore.IsActivated() ? Visibility.Collapsed : Visibility.Visible;
        MainView.Visibility = LicenseStore.IsActivated() ? Visibility.Visible : Visibility.Collapsed;

        _tray = new Forms.NotifyIcon
        {
            Icon = LoadTrayIcon(),
            Visible = true,
            Text = "VPN Echo · Disconnected"
        };
        var menu = new Forms.ContextMenuStrip();
        var open = new Forms.ToolStripMenuItem("Open VPN Echo");
        open.Click += (_, _) => ShowWindow();
        _trayDisconnect = new Forms.ToolStripMenuItem("Disconnect");
        _trayDisconnect.Click += async (_, _) => await _controller.DisconnectAsync();
        var exit = new Forms.ToolStripMenuItem("Exit");
        exit.Click += Exit_Click;
        menu.Items.Add(open); menu.Items.Add(_trayDisconnect); menu.Items.Add(new Forms.ToolStripSeparator()); menu.Items.Add(exit);
        _tray.ContextMenuStrip = menu;
        _tray.DoubleClick += (_, _) => ShowWindow();
        _renderTimer.Tick += (_, _) => Render();
        _trafficTimer.Tick += async (_, _) => await _controller.TickAsync();
        _renderTimer.Start(); _trafficTimer.Start();
        Render();
    }

    private static Icon LoadTrayIcon()
    {
        try { return new Icon(Path.Combine(AppContext.BaseDirectory, "echo.ico")); }
        catch { return SystemIcons.Application; }
    }

    private void Window_Loaded(object sender, RoutedEventArgs e)
    {
        if (LicenseStore.IsActivated()) { MainView.Visibility = Visibility.Visible; ActivationView.Visibility = Visibility.Collapsed; }
        else { ActivationView.Visibility = Visibility.Visible; MainView.Visibility = Visibility.Collapsed; LicenseInput.Focus(); }
    }

    private void Activate_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            if (!LicenseStore.Activate(LicenseInput.Text)) { LicenseError.Text = "Invalid license. Please try again."; return; }
            LicenseError.Text = ""; LicenseInput.Clear(); ActivationView.Visibility = Visibility.Collapsed; MainView.Visibility = Visibility.Visible;
            Render();
        }
        catch { LicenseError.Text = "Could not save activation. Please try again."; }
    }

    private void LicenseInput_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (sender is TextBox box && box.Text != box.Text.ToUpperInvariant())
        {
            int caret = box.CaretIndex; string upper = box.Text.ToUpperInvariant(); box.Text = upper; box.CaretIndex = Math.Min(caret, upper.Length);
        }
    }

    private void LicenseInput_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter) { Activate_Click(sender, new RoutedEventArgs()); e.Handled = true; }
    }

    private async void Power_Click(object sender, RoutedEventArgs e)
    {
        try { await _controller.ToggleAsync(); }
        catch (Exception ex) { MessageBox.Show("VPN operation failed.\n\n" + ex.Message, "VPN Echo", MessageBoxButton.OK, MessageBoxImage.Warning); }
    }

    private void Controller_Changed(object? sender, EventArgs e)
    {
        if (Dispatcher.CheckAccess()) Render(); else Dispatcher.BeginInvoke(new Action(Render));
    }

    private void Render()
    {
        if (!IsLoaded && !IsVisible) return;
        string state = _controller.State;
        PowerLabel.Text = state switch
        {
            "Connected" => "Connected", "Checking" => "Checking…", "Connecting" => "Connecting…",
            "Reconnecting" => "Reconnecting…", "Disconnecting" => "Disconnecting…", _ => "Connect"
        };
        bool connected = state == "Connected";
        PowerButton.Background = new SolidColorBrush(connected ? Color.FromRgb(32, 88, 62) : Color.FromRgb(7, 9, 9));
        PowerButton.BorderBrush = new SolidColorBrush(connected ? Color.FromRgb(102, 201, 149) : Color.FromRgb(48, 56, 59));
        PowerButton.IsEnabled = state != "Disconnecting";
        ScanProgress.Visibility = state == "Checking" ? Visibility.Visible : Visibility.Collapsed;
        if (state == "Checking") ScanLabel.Text = $"Checking {_controller.Completed}/{_controller.Total} · Automatic";
        else if (_controller.Completed == 0) ScanLabel.Text = "3 SERVERS · AUTOMATIC";
        else ScanLabel.Text = $"Last check · {_controller.Completed}/{_controller.Total} · {_controller.Available} available";

        if (!string.IsNullOrWhiteSpace(_controller.Message)) DetailText.Text = _controller.Message;
        else if (state == "Connected")
            DetailText.Text = $"{_controller.ActiveName} · Automatic\n↓ {VpnController.Rate(_controller.DownloadRate)}    ↑ {VpnController.Rate(_controller.UploadRate)}";
        else if (state == "Checking") DetailText.Text = "Fresh HTTPS checks through each VMess server\nBest verified server connects automatically.";
        else if (state == "Connecting" || state == "Reconnecting") DetailText.Text = $"{state} · Automatic server selection";
        else if (_controller.Completed == 0) DetailText.Text = "Tap Connect to test all 3 servers\nand connect to the best one.";
        else DetailText.Text = "Tap Connect to run fresh checks and connect automatically.";

        if (_lastRevision != _controller.Revision)
        {
            _lastRevision = _controller.Revision; _visibleEntries.Clear();
            foreach (var entry in _controller.Ranked()) _visibleEntries.Add(entry);
        }
        _trayDisconnect.Enabled = state != "Disconnected" && state != "Disconnecting";
        string tip = $"VPN Echo · {state} · {_controller.ActiveName} · ↓{VpnController.Rate(_controller.DownloadRate)} ↑{VpnController.Rate(_controller.UploadRate)}";
        _tray.Text = tip.Length > 63 ? tip[..63] : tip;
    }

    private void ServerRow_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (sender is not FrameworkElement element || element.DataContext is not ServerEntry entry) return;
        string message = entry.State switch
        {
            ProbeState.Failed => string.IsNullOrWhiteSpace(entry.Error) ? "No verified response. Check the internet connection and try Connect again." : entry.Error,
            ProbeState.Ok => $"HTTPS response latency: {entry.Ms:0} ms\nSuccessful samples: {entry.Samples}\n\nGreen < 1000 ms · Red ≥ 1000 ms or no response\n\nConnect checks all three profiles, then automatically uses the best verified one. Latency is not download bandwidth.",
            ProbeState.Checking => "Echo is testing this VMess profile now.",
            _ => "Tap Connect to test all three servers. Echo will display the results and choose the best verified server automatically."
        };
        MessageBox.Show(this, message, entry.Name, MessageBoxButton.OK, entry.State == ProbeState.Failed ? MessageBoxImage.Warning : MessageBoxImage.Information);
    }

    private void TopBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left) { try { DragMove(); } catch { } }
    }
    private void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void Close_Click(object sender, RoutedEventArgs e) => Close();
    private void ShowWindow() { Show(); WindowState = WindowState.Normal; Activate(); }

    private void Window_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        if (!_allowClose && _controller.State != "Disconnected") { e.Cancel = true; Hide(); return; }
        _renderTimer.Stop(); _trafficTimer.Stop(); _tray.Visible = false; _tray.Dispose();
    }

    private async void Exit_Click(object? sender, EventArgs e)
    {
        try { await _controller.DisconnectAsync(); }
        finally { _allowClose = true; _tray.Visible = false; Close(); }
    }

    private void ShowAbout_MouseRightButtonUp(object sender, MouseButtonEventArgs e)
    {
        MessageBox.Show(this,
            "VPN Echo 1.1.1.1 · developer: Ramtin Ahmadi\n\nThe Windows tunnel uses the unmodified sing-box 1.14.4 core. GPL-3.0 license and corresponding source: https://github.com/SagerNet/sing-box/tree/v1.14.4\n\nConnection checks verify HTTPS through each profile; availability depends on the server and your network.",
            "About VPN Echo", MessageBoxButton.OK, MessageBoxImage.Information);
    }
}
