using System.Net;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace VpnEcho;

public static class VpnConfigFactory
{
    private static JsonObject Vmess(ServerProfile p, string resolvedIp) => new()
    {
        ["type"] = "vmess", ["tag"] = "proxy", ["server"] = resolvedIp,
        ["server_port"] = p.Port, ["uuid"] = p.Uuid, ["security"] = p.Security,
        ["alter_id"] = p.AlterId,
        ["transport"] = new JsonObject { ["type"] = "ws", ["path"] = p.Path, ["headers"] = new JsonObject { ["Host"] = p.WsHost } },
        ["tls"] = new JsonObject { ["enabled"] = true, ["server_name"] = p.Sni, ["insecure"] = false,
            ["utls"] = new JsonObject { ["enabled"] = true, ["fingerprint"] = p.Fingerprint } }
    };

    private static JsonObject Dns() => new()
    {
        ["servers"] = new JsonArray(new JsonObject { ["type"] = "udp", ["tag"] = "remote", ["server"] = "1.1.1.1", ["server_port"] = 53, ["detour"] = "proxy" }),
        ["final"] = "remote", ["strategy"] = "ipv4_only"
    };

    private static JsonObject Log(string path) => new() { ["level"] = "info", ["timestamp"] = true, ["output"] = path };

    public static JsonObject CreateProbe(ServerProfile p, string resolvedIp, int socksPort, string logPath) => new()
    {
        ["log"] = Log(logPath), ["dns"] = Dns(),
        ["inbounds"] = new JsonArray(new JsonObject { ["type"] = "socks", ["tag"] = "probe", ["listen"] = "127.0.0.1", ["listen_port"] = socksPort }),
        ["outbounds"] = new JsonArray(Vmess(p, resolvedIp), new JsonObject { ["type"] = "direct", ["tag"] = "direct" }),
        ["route"] = new JsonObject
        {
            ["auto_detect_interface"] = true,
            ["rules"] = new JsonArray(new JsonObject { ["protocol"] = "dns", ["action"] = "hijack-dns" }),
            ["final"] = "proxy"
        }
    };

    public static JsonObject CreateTun(ServerProfile p, string resolvedIp, string logPath) => new()
    {
        ["log"] = Log(logPath), ["dns"] = Dns(),
        ["inbounds"] = new JsonArray(new JsonObject
        {
            ["type"] = "tun", ["tag"] = "tun-in", ["interface_name"] = "VPN Echo",
            ["address"] = new JsonArray("172.19.0.1/30", "fdfe:dcba:9876::1/126"), ["mtu"] = 1500,
            ["stack"] = "gvisor", ["auto_route"] = true, ["strict_route"] = true, ["dns_mode"] = "hijack",
            ["route_address"] = new JsonArray("0.0.0.0/1", "128.0.0.0/1", "::/1", "8000::/1"),
            ["route_exclude_address"] = new JsonArray("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "169.254.0.0/16", "fc00::/7", "fe80::/10", IpCidr(resolvedIp))
        }),
        ["outbounds"] = new JsonArray(Vmess(p, resolvedIp), new JsonObject { ["type"] = "direct", ["tag"] = "direct" }),
        ["route"] = new JsonObject
        {
            ["auto_detect_interface"] = true,
            ["rules"] = new JsonArray(
                new JsonObject { ["protocol"] = "dns", ["action"] = "hijack-dns" },
                new JsonObject { ["ip_is_private"] = true, ["action"] = "route", ["outbound"] = "direct" }),
            ["final"] = "proxy"
        }
    };

    private static string IpCidr(string ip) => IPAddress.TryParse(ip, out var parsed) && parsed.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork ? $"{ip}/32" : $"{ip}/128";

    public static string Serialize(JsonObject config) => config.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
}
