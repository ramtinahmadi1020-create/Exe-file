using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Text;

namespace VpnEcho;

public static class DataPaths
{
    public static string Root => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "VPN Echo");
    public static string Configs => Path.Combine(Root, "configs");
    public static string Logs => Path.Combine(Root, "logs");
    public static string CoreExe => Path.Combine(AppContext.BaseDirectory, "sing-box.exe");
    public static void Ensure()
    {
        Directory.CreateDirectory(Root); Directory.CreateDirectory(Configs); Directory.CreateDirectory(Logs);
    }
    public static string UniqueConfig(string mode, int id) => Path.Combine(Configs, $"{mode}-{id + 1}-{DateTime.UtcNow:yyyyMMdd-HHmmss-fff}.json");
    public static string UniqueLog(string mode, int id) => Path.Combine(Logs, $"{mode}-{id + 1}-{DateTime.UtcNow:yyyyMMdd-HHmmss-fff}.log");
}

public sealed class ServerTester
{
    private static readonly (string Host, string Path)[] Checks = [("www.gstatic.com", "/generate_204"), ("cp.cloudflare.com", "/generate_204")];
    private readonly string _core = DataPaths.CoreExe;

    public async Task<ProbeResult> TestAsync(ServerProfile profile, CancellationToken cancellationToken)
    {
        string ip = "";
        try
        {
            DataPaths.Ensure();
            if (!File.Exists(_core)) return ProbeResult.Fail("The bundled VPN engine is missing.");
            ip = await ResolveIpv4(profile.Address, cancellationToken);
            int port = GetFreePort();
            string configPath = DataPaths.UniqueConfig("probe", profile.Id), logPath = DataPaths.UniqueLog("probe", profile.Id);
            File.WriteAllText(configPath, VpnConfigFactory.Serialize(VpnConfigFactory.CreateProbe(profile, ip, port, logPath)), new UTF8Encoding(false));
            using var limit = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            limit.CancelAfter(TimeSpan.FromSeconds(14));
            await using var core = SingBoxProcess.Start(_core, configPath);
            try
            {
                if (!await WaitForSocksAsync(core, port, TimeSpan.FromSeconds(6), limit.Token))
                    return ProbeResult.Fail(core.HasExited ? "VPN engine rejected this profile. Tap for details." : "No verified response before timeout.", ip);
                var sw = Stopwatch.StartNew();
                string workingHost;
                double first;
                try
                {
                    first = await HttpsThroughSocksAsync(port, Checks[0].Host, Checks[0].Path, TimeSpan.FromSeconds(5), limit.Token);
                    workingHost = Checks[0].Host;
                }
                catch
                {
                    first = await HttpsThroughSocksAsync(port, Checks[1].Host, Checks[1].Path, TimeSpan.FromSeconds(5), limit.Token);
                    workingHost = Checks[1].Host;
                }
                sw.Stop();
                double sample1 = first;
                int samples = 1;
                double average = sample1;
                double score = sample1 + 300;
                try
                {
                    double sample2 = await HttpsThroughSocksAsync(port, workingHost, "/generate_204", TimeSpan.FromSeconds(3.5), limit.Token);
                    samples = 2;
                    average = (sample1 + sample2) / 2.0;
                    score = average + Math.Abs(sample1 - sample2) * 0.2;
                }
                catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested) { }
                catch { }
                return new ProbeResult(true, average, score, samples, ip, "");
            }
            finally
            {
                await core.StopAsync();
                try { File.Delete(configPath); } catch { }
            }
        }
        catch (OperationCanceledException)
        {
            return ProbeResult.Fail(cancellationToken.IsCancellationRequested ? "Check cancelled." : "Server check timed out.", ip);
        }
        catch (Exception ex)
        {
            return ProbeResult.Fail(SafeError(ex), ip);
        }
    }

    public static async Task<string> ResolveIpv4(string host, CancellationToken token)
    {
        IPAddress[] addresses = await Dns.GetHostAddressesAsync(host, token);
        var ip = addresses.FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork);
        if (ip is null) throw new IOException("The server name could not be resolved to IPv4.");
        return ip.ToString();
    }

    private static int GetFreePort()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start(); return ((IPEndPoint)listener.LocalEndpoint).Port;
    }

    private static async Task<bool> WaitForSocksAsync(SingBoxProcess core, int port, TimeSpan timeout, CancellationToken token)
    {
        var until = Stopwatch.StartNew();
        while (until.Elapsed < timeout)
        {
            token.ThrowIfCancellationRequested();
            if (core.HasExited) return false;
            try
            {
                using var client = new TcpClient(); using var attempt = CancellationTokenSource.CreateLinkedTokenSource(token);
                attempt.CancelAfter(250); await client.ConnectAsync(IPAddress.Loopback, port, attempt.Token); return true;
            }
            catch (OperationCanceledException) when (!token.IsCancellationRequested) { }
            catch (SocketException) { }
            await Task.Delay(120, token);
        }
        return false;
    }

    private static async Task<double> HttpsThroughSocksAsync(int port, string targetHost, string path, TimeSpan timeout, CancellationToken outer)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(outer); deadline.CancelAfter(timeout);
        var watch = Stopwatch.StartNew();
        using var client = new TcpClient();
        await client.ConnectAsync(IPAddress.Loopback, port, deadline.Token);
        await using var network = client.GetStream();
        await network.WriteAsync(new byte[] { 5, 1, 0 }, deadline.Token);
        byte[] hello = new byte[2]; await network.ReadExactlyAsync(hello, deadline.Token);
        if (hello[0] != 5 || hello[1] != 0) throw new IOException("The local SOCKS proxy rejected the check.");
        byte[] host = Encoding.ASCII.GetBytes(targetHost);
        if (host.Length > 255) throw new IOException("Probe host is too long.");
        byte[] request = new byte[7 + host.Length];
        request[0] = 5; request[1] = 1; request[2] = 0; request[3] = 3; request[4] = (byte)host.Length;
        Buffer.BlockCopy(host, 0, request, 5, host.Length); request[^2] = 1; request[^1] = 187; // TCP 443
        await network.WriteAsync(request, deadline.Token);
        byte[] reply = new byte[4]; await network.ReadExactlyAsync(reply, deadline.Token);
        if (reply[0] != 5 || reply[1] != 0) throw new IOException("The remote VMess proxy could not open the HTTPS check.");
        int rest = reply[3] switch
        {
            1 => 6,
            4 => 18,
            3 => await ReadSocksDomainLengthAsync(network, deadline.Token) + 2,
            _ => throw new IOException("Invalid SOCKS response.")
        };
        byte[] tail = new byte[rest]; await network.ReadExactlyAsync(tail, deadline.Token);
        await using var tls = new SslStream(network, leaveInnerStreamOpen: true);
        await tls.AuthenticateAsClientAsync(new SslClientAuthenticationOptions
        {
            TargetHost = targetHost,
            EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13
        }, deadline.Token);
        string requestText = $"GET {path} HTTP/1.1\r\nHost: {targetHost}\r\nUser-Agent: VPN Echo/1.1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n";
        await tls.WriteAsync(Encoding.ASCII.GetBytes(requestText), deadline.Token); await tls.FlushAsync(deadline.Token);
        string status = await ReadHttpStatusAsync(tls, deadline.Token);
        if (!status.Contains(" 204 ", StringComparison.Ordinal) && !status.EndsWith(" 204", StringComparison.Ordinal))
            throw new IOException("The HTTPS test endpoint did not return 204.");
        watch.Stop(); return watch.Elapsed.TotalMilliseconds;
    }

    private static async Task<int> ReadSocksDomainLengthAsync(Stream stream, CancellationToken token)
    {
        byte[] b = new byte[1]; await stream.ReadExactlyAsync(b, token); return b[0];
    }

    private static async Task<string> ReadHttpStatusAsync(Stream stream, CancellationToken token)
    {
        var bytes = new List<byte>(512); byte[] one = new byte[1];
        while (bytes.Count < 8192)
        {
            int n = await stream.ReadAsync(one.AsMemory(0, 1), token);
            if (n == 0) break;
            bytes.Add(one[0]); int l = bytes.Count;
            if (l >= 4 && bytes[l - 4] == 13 && bytes[l - 3] == 10 && bytes[l - 2] == 13 && bytes[l - 1] == 10) break;
        }
        string header = Encoding.ASCII.GetString(bytes.ToArray());
        return header.Split("\r\n", 2, StringSplitOptions.None)[0];
    }

    private static string SafeError(Exception ex) => ex switch
    {
        SocketException => "The check endpoint or server is unreachable.",
        AuthenticationException => "The TLS certificate check failed.",
        IOException when ex.Message.Contains("resolved", StringComparison.OrdinalIgnoreCase) => ex.Message,
        OperationCanceledException => "Server check timed out.",
        _ => "No verified response (offline, blocked endpoint, or timeout)."
    };
}
