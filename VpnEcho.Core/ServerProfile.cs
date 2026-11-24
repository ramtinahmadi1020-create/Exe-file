using System.Text.Json;

namespace VpnEcho;

public sealed record ServerProfile(int Id, string Name, string Uri, string Address, int Port, string Uuid, string Security, int AlterId, string Sni, string WsHost, string Path, string Fingerprint)
{
    public static IReadOnlyList<ServerProfile> Load(string path)
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        if (doc.RootElement.ValueKind != JsonValueKind.Array || doc.RootElement.GetArrayLength() != 3)
            throw new InvalidDataException("The Windows edition must contain exactly the three supplied VMess profiles.");
        var result = new List<ServerProfile>();
        foreach (var item in doc.RootElement.EnumerateArray())
        {
            int id = item.GetProperty("id").GetInt32();
            string name = item.GetProperty("name").GetString() ?? "";
            string uri = item.GetProperty("uri").GetString() ?? "";
            if (!uri.StartsWith("vmess://", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Only VMess profiles are supported.");
            string raw = uri[8..].Trim().Replace('-', '+').Replace('_', '/');
            raw = raw.PadRight(raw.Length + (4 - raw.Length % 4) % 4, '=');
            using var pdoc = JsonDocument.Parse(Convert.FromBase64String(raw));
            var p = pdoc.RootElement;
            string V(string key, string fallback = "") => p.TryGetProperty(key, out var v) ? v.ToString() : fallback;
            string address = V("add");
            if (string.IsNullOrWhiteSpace(address) || !int.TryParse(V("port"), out int port) || port is < 1 or > 65535)
                throw new InvalidDataException("Invalid VMess endpoint.");
            string compact = V("id").Trim().ToLowerInvariant();
            if (compact.Length == 32 && compact.All(System.Uri.IsHexDigit)) compact = $"{compact[..8]}-{compact[8..12]}-{compact[12..16]}-{compact[16..20]}-{compact[20..]}";
            if (!Guid.TryParse(compact, out Guid uuid)) throw new InvalidDataException("Invalid VMess UUID.");
            if (!int.TryParse(V("aid", "0"), out int aid) || aid < 0) throw new InvalidDataException("Invalid alterId.");
            if (!V("net", "tcp").Equals("ws", StringComparison.OrdinalIgnoreCase) || !V("tls").Equals("tls", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("This release expects VMess over WebSocket and verified TLS.");
            string sni = V("sni", V("host", address)); if (string.IsNullOrWhiteSpace(sni)) sni = address;
            string wsHost = V("host", address); if (string.IsNullOrWhiteSpace(wsHost)) wsHost = address;
            string fingerprint = V("fp", "chrome"); if (string.IsNullOrWhiteSpace(fingerprint)) fingerprint = "chrome";
            result.Add(new ServerProfile(id, name, uri, address, port, uuid.ToString(), V("scy", "auto"), aid, sni, wsHost, V("path", "/"), fingerprint));
        }
        string[] expected = ["http2-rel1.pixelated4.ir", "http2-rel1.pixelated5.ir", "http2-rel1.pixelatedthree.ir"];
        var ordered = result.OrderBy(x => x.Id).ToArray();
        for (int i = 0; i < ordered.Length; i++)
            if (ordered[i].Id != i || ordered[i].Name != $"server-{i + 1}" || ordered[i].Address != expected[i])
                throw new InvalidDataException("The bundled server list does not match the approved three profiles.");
        return ordered;
    }
}
