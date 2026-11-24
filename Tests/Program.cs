using System.Text.Json.Nodes;
using VpnEcho;

string root = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../"));
string assets = Path.Combine(root, "Assets", "servers.json");
string output = Path.Combine(root, "Build", "configs"); Directory.CreateDirectory(output);
var profiles = ServerProfile.Load(assets);
if (profiles.Count != 3) throw new Exception("Exactly three server profiles expected.");
for (int i = 0; i < profiles.Count; i++)
{
    var p = profiles[i];
    if (p.Name != $"server-{i + 1}" || p.Port != 443 || p.Uuid != "c5edc003-f144-45ae-a0ee-07ea312c3757" || p.Path != "/pixelated") throw new Exception("VMess profile fields mismatch.");
    var tun = VpnConfigFactory.CreateTun(p, "203.0.113.10", Path.Combine(output, $"tun-{i + 1}.log"));
    var vm = tun["outbounds"]![0]!.AsObject();
    if (vm["type"]!.GetValue<string>() != "vmess" || vm["server"]!.GetValue<string>() != "203.0.113.10" || vm["server_port"]!.GetValue<int>() != 443) throw new Exception("VMess outbound mismatch.");
    if (vm["tls"]!["insecure"]!.GetValue<bool>() || vm["tls"]!["server_name"]!.GetValue<string>() != p.Sni) throw new Exception("TLS must verify the endpoint name.");
    if (vm["transport"]!["path"]!.GetValue<string>() != "/pixelated" || vm["transport"]!["headers"]!["Host"]!.GetValue<string>() != p.Address) throw new Exception("WebSocket settings mismatch.");
    if (tun["inbounds"]![0]!["type"]!.GetValue<string>() != "tun" || !tun["inbounds"]![0]!["strict_route"]!.GetValue<bool>() || !tun["inbounds"]![0]!["auto_route"]!.GetValue<bool>()) throw new Exception("TUN routing settings missing.");
    if (tun["route"]!["final"]!.GetValue<string>() != "proxy" || tun["dns"]!["servers"]![0]!["detour"]!.GetValue<string>() != "proxy") throw new Exception("Default traffic and DNS must use the VMess proxy.");
    File.WriteAllText(Path.Combine(output, $"tun-server-{i + 1}.json"), VpnConfigFactory.Serialize(tun));
    var probe = VpnConfigFactory.CreateProbe(p, "203.0.113.10", 41000 + i, Path.Combine(output, $"probe-{i + 1}.log"));
    if (probe["inbounds"]![0]!["listen"]!.GetValue<string>() != "127.0.0.1" || probe["route"]!["final"]!.GetValue<string>() != "proxy") throw new Exception("Health checks must be loopback-only and proxied.");
    File.WriteAllText(Path.Combine(output, $"probe-server-{i + 1}.json"), VpnConfigFactory.Serialize(probe));
    Console.WriteLine($"PASS {p.Name}: {p.Address}:{p.Port}, VMess AEAD, WS {p.Path}, TLS verified");
}
Console.WriteLine("3 profiles; 6 config variants generated; static assertions passed.");
