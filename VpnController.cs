using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Net;
using System.Net.Http;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Windows.Threading;

namespace VpnEcho;

public sealed class VpnController
{
    private static readonly HttpClient DirectClient = new(new SocketsHttpHandler { UseProxy = false, AllowAutoRedirect = false, ConnectTimeout = TimeSpan.FromSeconds(3) }) { Timeout = TimeSpan.FromSeconds(4) };
    private readonly Dispatcher _dispatcher;
    private readonly SemaphoreSlim _coreGate = new(1, 1);
    private readonly ServerTester _tester = new();
    private CancellationTokenSource? _operation;
    private SingBoxProcess? _core;
    private ServerEntry? _activeEntry;
    private DateTimeOffset _nextHealth = DateTimeOffset.MaxValue;
    private int _healthFailures;
    private int _completed;
    private int _revision;
    private int _tickRunning;
    private DateTimeOffset _lastStatsAt;
    private long _lastReceived, _lastSent;
    private long _downloadRate, _uploadRate;
    private string _state = "Disconnected", _message = "";
    private string _logPath = "";

    public ObservableCollection<ServerEntry> Entries { get; }
    public event EventHandler? Changed;
    public string State => _state;
    public string Message => _message;
    public int Completed => _completed;
    public int Revision => _revision;
    public int Total => Entries.Count;
    public int Available => Entries.Count(x => x.State == ProbeState.Ok);
    public long DownloadRate => Interlocked.Read(ref _downloadRate);
    public long UploadRate => Interlocked.Read(ref _uploadRate);
    public int ActiveId => _activeEntry?.Id ?? -1;
    public string ActiveName => _activeEntry?.Name ?? "Automatic";
    public string LogPath => _logPath;

    public VpnController(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher;
        DataPaths.Ensure();
        string profilesPath = Path.Combine(AppContext.BaseDirectory, "servers.json");
        Entries = new ObservableCollection<ServerEntry>(ServerProfile.Load(profilesPath).Select(p => new ServerEntry(p)));
    }

    private void OnUi(Action action)
    {
        if (_dispatcher.CheckAccess()) { action(); Changed?.Invoke(this, EventArgs.Empty); }
        else _dispatcher.Invoke(() => { action(); Changed?.Invoke(this, EventArgs.Empty); });
    }
    private Task OnUiAsync(Action action)
    {
        if (_dispatcher.CheckAccess()) { action(); Changed?.Invoke(this, EventArgs.Empty); return Task.CompletedTask; }
        return _dispatcher.InvokeAsync(() => { action(); Changed?.Invoke(this, EventArgs.Empty); }).Task;
    }
    private void SetState(string state, string message)
    {
        OnUi(() => { _state = state; _message = message; _revision++; });
    }

    public IReadOnlyList<ServerEntry> Ranked() => Entries
        .OrderBy(x => x.State == ProbeState.Ok ? 0 : 1)
        .ThenBy(x => x.State == ProbeState.Ok ? x.Score : double.MaxValue)
        .ThenBy(x => x.Id).ToArray();

    public async Task ToggleAsync()
    {
        if (!State.Equals("Disconnected", StringComparison.Ordinal)) await DisconnectAsync();
        else await ConnectAsync();
    }

    public async Task ConnectAsync()
    {
        var cts = new CancellationTokenSource();
        lock (this)
        {
            if (!_state.Equals("Disconnected", StringComparison.Ordinal)) { cts.Dispose(); return; }
            _operation = cts;
        }
        try
        {
            await OnUiAsync(() =>
            {
                _state = "Checking"; _message = "Testing all three servers before connecting…"; _completed = 0;
                foreach (var entry in Entries) { entry.IsActive = false; entry.SetPending(); entry.SetChecking(); }
                _revision++;
            });
            using var round = CancellationTokenSource.CreateLinkedTokenSource(cts.Token);
            round.CancelAfter(TimeSpan.FromSeconds(20));
            var checks = Entries.Select(entry => CheckOneAsync(entry, round.Token, cts.Token)).ToArray();
            await Task.WhenAll(checks);
            if (cts.IsCancellationRequested) { SetState("Disconnected", "Server checks cancelled."); return; }
            if (round.IsCancellationRequested)
                await OnUiAsync(() => { foreach (var e in Entries.Where(e => e.State == ProbeState.Checking)) e.SetFailure("Server check timed out."); _completed = Total; _revision++; });
            var candidates = Ranked().Where(e => e.State == ProbeState.Ok).ToArray();
            if (candidates.Length == 0)
            {
                SetState("Disconnected", "None of the three servers returned a verified HTTPS response. Check your network and try again.");
                return;
            }
            string lastError = "All verified servers failed during VPN startup.";
            foreach (var candidate in candidates)
            {
                cts.Token.ThrowIfCancellationRequested();
                if (await TryConnectCandidateAsync(candidate, cts.Token)) return;
                lastError = "Connection verification failed for " + candidate.Name + ". Trying the next verified server…";
            }
            if (!cts.IsCancellationRequested) SetState("Disconnected", lastError.Replace("Trying the next verified server…", "No verified server connected. Tap Connect to test again."));
        }
        catch (OperationCanceledException)
        {
            if (!State.Equals("Disconnecting", StringComparison.Ordinal)) SetState("Disconnected", "Connection cancelled.");
        }
        catch (Exception ex)
        {
            SetState("Disconnected", "Could not start the VPN. " + SafeMessage(ex));
        }
        finally
        {
            lock (this) { if (ReferenceEquals(_operation, cts)) _operation = null; }
            cts.Dispose();
        }
    }

    private async Task CheckOneAsync(ServerEntry entry, CancellationToken roundToken, CancellationToken userToken)
    {
        ProbeResult result;
        try { result = await _tester.TestAsync(entry.Profile, roundToken); }
        catch (Exception ex) { result = ProbeResult.Fail(SafeMessage(ex)); }
        await OnUiAsync(() =>
        {
            if (result.Success) entry.SetSuccess(result.AverageMs, result.Samples, result.Score, result.ResolvedIp);
            else if (userToken.IsCancellationRequested) entry.SetFailure("Check cancelled.");
            else entry.SetFailure(result.Error);
            _completed++; _revision++;
            if (_state == "Checking") _message = $"Checking servers · {_completed}/{Total} complete…";
        });
    }

    private async Task<bool> TryConnectCandidateAsync(ServerEntry entry, CancellationToken token)
    {
        await OnUiAsync(() => { _state = "Connecting"; _message = $"Automatic · {entry.Name} · verifying VPN connection…"; _revision++; });
        await _coreGate.WaitAsync(token);
        try
        {
            await StopCoreLockedAsync();
            token.ThrowIfCancellationRequested();
            string ip = entry.ResolvedIp;
            if (string.IsNullOrWhiteSpace(ip)) ip = await ServerTester.ResolveIpv4(entry.Address, token);
            _logPath = DataPaths.UniqueLog("tun", entry.Id);
            string configPath = DataPaths.UniqueConfig("tun", entry.Id);
            File.WriteAllText(configPath, VpnConfigFactory.Serialize(VpnConfigFactory.CreateTun(entry.Profile, ip, _logPath)), new UTF8Encoding(false));
            _core = SingBoxProcess.Start(DataPaths.CoreExe, configPath);
            bool passed = await VerifySystemTunnelAsync(_core, token);
            if (!passed)
            {
                string detail = TailLog(_logPath);
                await StopCoreLockedAsync();
                await OnUiAsync(() => { entry.SetFailure(detail.Length > 0 ? "Engine or tunnel check failed · tap for info" : "VPN verification failed · tap for info"); entry.IsActive = false; _revision++; });
                return false;
            }
            _activeEntry = entry; _healthFailures = 0; _nextHealth = DateTimeOffset.UtcNow.AddSeconds(25);
            _lastStatsAt = default; _lastReceived = _lastSent = 0; Interlocked.Exchange(ref _downloadRate, 0); Interlocked.Exchange(ref _uploadRate, 0);
            await OnUiAsync(() => { foreach (var e in Entries) e.IsActive = e.Id == entry.Id; _state = "Connected"; _message = ""; _revision++; });
            return true;
        }
        catch (OperationCanceledException) { await StopCoreLockedAsync(); throw; }
        catch (Exception ex)
        {
            await StopCoreLockedAsync();
            await OnUiAsync(() => { entry.SetFailure(SafeMessage(ex)); entry.IsActive = false; _revision++; });
            return false;
        }
        finally { _coreGate.Release(); }
    }

    private static async Task<bool> VerifySystemTunnelAsync(SingBoxProcess core, CancellationToken token)
    {
        var started = Stopwatch.StartNew();
        string[] urls = ["https://www.gstatic.com/generate_204", "https://cp.cloudflare.com/generate_204"];
        await Task.Delay(750, token);
        while (started.Elapsed < TimeSpan.FromSeconds(14))
        {
            token.ThrowIfCancellationRequested();
            if (core.HasExited) return false;
            foreach (var url in urls)
            {
                try
                {
                    using var one = CancellationTokenSource.CreateLinkedTokenSource(token); one.CancelAfter(TimeSpan.FromSeconds(3));
                    using var response = await DirectClient.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, one.Token);
                    if ((int)response.StatusCode == 204) return true;
                }
                catch (OperationCanceledException) when (!token.IsCancellationRequested) { }
                catch { }
            }
            await Task.Delay(850, token);
        }
        return false;
    }

    public async Task DisconnectAsync()
    {
        CancellationTokenSource? operation;
        lock (this) { operation = _operation; _operation = null; }
        try { operation?.Cancel(); } catch (ObjectDisposedException) { }
        if (State == "Disconnected" && _core is null) return;
        await OnUiAsync(() => { _state = "Disconnecting"; _message = "Stopping the VPN tunnel…"; _revision++; });
        await _coreGate.WaitAsync();
        try
        {
            await StopCoreLockedAsync();
            _activeEntry = null; _nextHealth = DateTimeOffset.MaxValue; _healthFailures = 0;
            Interlocked.Exchange(ref _downloadRate, 0); Interlocked.Exchange(ref _uploadRate, 0);
            await OnUiAsync(() => { foreach (var e in Entries) e.IsActive = false; _state = "Disconnected"; _message = "Disconnected."; _revision++; });
        }
        finally { _coreGate.Release(); }
    }

    private async Task StopCoreLockedAsync()
    {
        var core = _core; _core = null;
        if (core is not null) await core.StopAsync();
    }

    public async Task TickAsync()
    {
        if (Interlocked.Exchange(ref _tickRunning, 1) != 0) return;
        try
        {
            SampleTraffic();
            if ((State == "Connected" || State == "Reconnecting") && DateTimeOffset.UtcNow >= _nextHealth)
            {
                bool healthy = false;
                try
                {
                    using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(8));
                    if (_core is not null && !_core.HasExited)
                        healthy = await VerifySystemTunnelAsync(_core, cts.Token);
                }
                catch { healthy = false; }
                if (healthy)
                {
                    _healthFailures = 0; _nextHealth = DateTimeOffset.UtcNow.AddSeconds(25);
                    if (State == "Reconnecting") SetState("Connected", "");
                }
                else if (++_healthFailures == 1)
                {
                    _nextHealth = DateTimeOffset.UtcNow.AddSeconds(8);
                    SetState("Reconnecting", "Connection interrupted · checking again…");
                }
                else
                {
                    _nextHealth = DateTimeOffset.UtcNow.AddSeconds(25);
                    if (_activeEntry is not null) await FailoverAsync(_activeEntry);
                }
            }
        }
        finally { Interlocked.Exchange(ref _tickRunning, 0); }
    }

    private async Task FailoverAsync(ServerEntry failed)
    {
        await OnUiAsync(() => { failed.SetFailure("Connection was lost."); failed.IsActive = false; _state = "Reconnecting"; _message = "Automatic · trying another verified server…"; _revision++; });
        var candidates = Ranked().Where(e => e.State == ProbeState.Ok && e.Id != failed.Id).ToArray();
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(25));
        foreach (var candidate in candidates)
        {
            if (await TryConnectCandidateAsync(candidate, cts.Token)) return;
        }
        await DisconnectAsync();
        SetState("Disconnected", "Connection lost. No other checked server could connect. Tap Connect to run fresh checks.");
    }

    private void SampleTraffic()
    {
        if (State != "Connected" && State != "Reconnecting")
        {
            _lastStatsAt = default; return;
        }
        try
        {
            var nic = NetworkInterface.GetAllNetworkInterfaces().FirstOrDefault(n =>
                n.Name.Contains("VPN Echo", StringComparison.OrdinalIgnoreCase) ||
                n.Description.Contains("VPN Echo", StringComparison.OrdinalIgnoreCase));
            if (nic is null) return;
            var s = nic.GetIPStatistics(); long received = s.BytesReceived, sent = s.BytesSent; var now = DateTimeOffset.UtcNow;
            if (_lastStatsAt != default)
            {
                double seconds = Math.Max(0.1, (now - _lastStatsAt).TotalSeconds);
                Interlocked.Exchange(ref _downloadRate, (long)Math.Max(0, (received - _lastReceived) / seconds));
                Interlocked.Exchange(ref _uploadRate, (long)Math.Max(0, (sent - _lastSent) / seconds));
            }
            _lastStatsAt = now; _lastReceived = received; _lastSent = sent;
        }
        catch { }
    }

    public static string Rate(long value)
    {
        if (value < 1024) return value + " B/s";
        if (value < 1024 * 1024) return (value / 1024d).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) + " KB/s";
        return (value / (1024d * 1024d)).ToString("0.00", System.Globalization.CultureInfo.InvariantCulture) + " MB/s";
    }

    private static string TailLog(string path)
    {
        try
        {
            if (!File.Exists(path)) return "";
            return string.Join("\n", File.ReadAllLines(path).TakeLast(8)).Trim();
        }
        catch { return ""; }
    }
    private static string SafeMessage(Exception ex) => ex switch
    {
        UnauthorizedAccessException => "Administrator permission is needed for the Windows VPN adapter.",
        FileNotFoundException => "A required VPN engine file is missing. Repair or reinstall VPN Echo.",
        SocketException => "Network or DNS is unavailable.",
        System.ComponentModel.Win32Exception => "Windows could not start the VPN engine. Check Windows security or the install folder.",
        _ => ex.Message.Length > 180 ? ex.Message[..180] : ex.Message
    };
}
