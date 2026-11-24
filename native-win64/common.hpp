#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace vpnecho {

inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

inline std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline std::string ToUpperAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

inline bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (ca != cb) return false;
    }
    return true;
}

inline bool ContainsIgnoreCase(const std::string& haystack, const std::string& needle) {
    return ToLowerAscii(haystack).find(ToLowerAscii(needle)) != std::string::npos;
}

inline std::string Base64Decode(std::string in) {
    std::string cleaned;
    cleaned.reserve(in.size() + 4);
    for (char c : in) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        cleaned.push_back(c);
    }
    while (cleaned.size() % 4 != 0) cleaned.push_back('=');

    static const int8_t lut[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    std::string out;
    out.reserve((cleaned.size() / 4) * 3);
    for (size_t i = 0; i < cleaned.size(); i += 4) {
        int a = lut[static_cast<unsigned char>(cleaned[i])];
        int b = lut[static_cast<unsigned char>(cleaned[i + 1])];
        int c = lut[static_cast<unsigned char>(cleaned[i + 2])];
        int d = lut[static_cast<unsigned char>(cleaned[i + 3])];
        if (a < 0 || b < 0 || c == -1 || d == -1) {
            throw std::runtime_error("Invalid base64 payload in VMess URI.");
        }
        uint32_t triple = (static_cast<uint32_t>(a) << 18) | (static_cast<uint32_t>(b) << 12) |
                          (static_cast<uint32_t>(c < 0 ? 0 : c) << 6) | static_cast<uint32_t>(d < 0 ? 0 : d);
        out.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (c != -2) out.push_back(static_cast<char>((triple >> 8) & 0xFF));
        if (d != -2) out.push_back(static_cast<char>(triple & 0xFF));
    }
    return out;
}

inline std::string JsonEscape(const std::string& s) {
    std::string out = "\"";
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

// Lightweight JSON DOM parser for servers.json & VMess payloads
struct JsonVal {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool bval = false;
    double nval = 0.0;
    std::string sval;
    std::vector<JsonVal> arr;
    std::map<std::string, JsonVal> obj;

    bool Has(const std::string& k) const { return type == Object && obj.find(k) != obj.end(); }
    const JsonVal& At(const std::string& k) const {
        auto it = obj.find(k);
        if (it == obj.end()) throw std::runtime_error("Missing JSON property: " + k);
        return it->second;
    }
    std::string ToScalarString(const std::string& fallback = "") const {
        if (type == String) return sval;
        if (type == Number) {
            long long iv = static_cast<long long>(nval);
            if (static_cast<double>(iv) == nval) return std::to_string(iv);
            return std::to_string(nval);
        }
        if (type == Bool) return bval ? "true" : "false";
        return fallback;
    }
};

class JsonParser {
    const std::string& s_;
    size_t pos_ = 0;

    void SkipWs() {
        // Skip UTF-8 BOM if at start
        if (pos_ == 0 && s_.size() >= 3 &&
            static_cast<unsigned char>(s_[0]) == 0xEF &&
            static_cast<unsigned char>(s_[1]) == 0xBB &&
            static_cast<unsigned char>(s_[2]) == 0xBF) {
            pos_ = 3;
        }
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    char Peek() {
        SkipWs();
        if (pos_ >= s_.size()) throw std::runtime_error("Unexpected end of JSON.");
        return s_[pos_];
    }

    std::string ParseStr() {
        SkipWs();
        if (pos_ >= s_.size() || s_[pos_] != '"') throw std::runtime_error("Expected JSON string.");
        ++pos_;
        std::string out;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= s_.size()) throw std::runtime_error("Unterminated escape.");
                char e = s_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) throw std::runtime_error("Invalid \\u escape.");
                        unsigned cp = static_cast<unsigned>(std::stoul(s_.substr(pos_, 4), nullptr, 16));
                        pos_ += 4;
                        if (cp < 0x80) out.push_back(static_cast<char>(cp));
                        else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        throw std::runtime_error("Unterminated JSON string.");
    }

public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    JsonVal ParseValue() {
        char c = Peek();
        if (c == '"') {
            JsonVal v; v.type = JsonVal::String; v.sval = ParseStr(); return v;
        }
        if (c == '{') {
            ++pos_;
            JsonVal v; v.type = JsonVal::Object;
            if (Peek() == '}') { ++pos_; return v; }
            while (true) {
                std::string k = ParseStr();
                if (Peek() != ':') throw std::runtime_error("Expected ':' in JSON object.");
                ++pos_;
                v.obj[k] = ParseValue();
                char sep = Peek();
                if (sep == '}') { ++pos_; break; }
                if (sep != ',') throw std::runtime_error("Expected ',' in JSON object.");
                ++pos_;
            }
            return v;
        }
        if (c == '[') {
            ++pos_;
            JsonVal v; v.type = JsonVal::Array;
            if (Peek() == ']') { ++pos_; return v; }
            while (true) {
                v.arr.push_back(ParseValue());
                char sep = Peek();
                if (sep == ']') { ++pos_; break; }
                if (sep != ',') throw std::runtime_error("Expected ',' in JSON array.");
                ++pos_;
            }
            return v;
        }
        if (s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4; JsonVal v; v.type = JsonVal::Bool; v.bval = true; return v;
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5; JsonVal v; v.type = JsonVal::Bool; v.bval = false; return v;
        }
        if (s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4; JsonVal v; v.type = JsonVal::Null; return v;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = pos_;
            if (s_[pos_] == '-') ++pos_;
            while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) {
                ++pos_;
            }
            JsonVal v; v.type = JsonVal::Number;
            v.nval = std::stod(s_.substr(start, pos_ - start));
            return v;
        }
        throw std::runtime_error("Invalid JSON token.");
    }
};

inline JsonVal ParseJson(const std::string& text) {
    JsonParser p(text);
    return p.ParseValue();
}

inline bool IsHexChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool NormalizeUuid(const std::string& raw, std::string& outUuid) {
    std::string s = ToLowerAscii(Trim(raw));
    if (s.size() == 32 && std::all_of(s.begin(), s.end(), IsHexChar)) {
        outUuid = s.substr(0, 8) + "-" + s.substr(8, 4) + "-" + s.substr(12, 4) + "-" + s.substr(16, 4) + "-" + s.substr(20, 12);
        return true;
    }
    if (s.size() == 36 && s[8] == '-' && s[13] == '-' && s[18] == '-' && s[23] == '-') {
        for (size_t i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) continue;
            if (!IsHexChar(s[i])) return false;
        }
        outUuid = s;
        return true;
    }
    return false;
}

struct ServerProfile {
    int id = 0;
    std::string name;
    std::string uri;
    std::string address;
    int port = 443;
    std::string uuid;
    std::string security;
    int alterId = 0;
    std::string sni;
    std::string wsHost;
    std::string path;
    std::string fingerprint;

    static std::vector<ServerProfile> LoadFromJsonText(const std::string& jsonText) {
        JsonVal root = ParseJson(jsonText);
        if (root.type != JsonVal::Array || root.arr.size() != 3) {
            throw std::runtime_error("The Windows edition must contain exactly the three supplied VMess profiles.");
        }
        std::vector<ServerProfile> result;
        for (const auto& item : root.arr) {
            if (item.type != JsonVal::Object) throw std::runtime_error("Invalid server profile object.");
            int id = static_cast<int>(item.At("id").nval);
            std::string name = item.At("name").sval;
            std::string uri = item.At("uri").sval;
            if (uri.size() < 8 || !EqualsIgnoreCase(uri.substr(0, 8), "vmess://")) {
                throw std::runtime_error("Only VMess profiles are supported.");
            }
            std::string decoded = Base64Decode(uri.substr(8));
            JsonVal p = ParseJson(decoded);
            auto V = [&](const std::string& key, const std::string& fallback = "") -> std::string {
                return p.Has(key) ? p.At(key).ToScalarString(fallback) : fallback;
            };
            std::string address = V("add");
            int port = 0;
            try { port = std::stoi(V("port")); } catch (...) { port = 0; }
            if (address.empty() || port < 1 || port > 65535) {
                throw std::runtime_error("Invalid VMess endpoint.");
            }
            std::string uuid;
            if (!NormalizeUuid(V("id"), uuid)) {
                throw std::runtime_error("Invalid VMess UUID.");
            }
            int aid = -1;
            try { aid = std::stoi(V("aid", "0")); } catch (...) { aid = -1; }
            if (aid < 0) throw std::runtime_error("Invalid alterId.");
            if (!EqualsIgnoreCase(V("net", "tcp"), "ws") || !EqualsIgnoreCase(V("tls"), "tls")) {
                throw std::runtime_error("This release expects VMess over WebSocket and verified TLS.");
            }
            std::string sni = V("sni", V("host", address));
            if (sni.empty()) sni = address;
            std::string wsHost = V("host", address);
            if (wsHost.empty()) wsHost = address;
            std::string fp = V("fp", "chrome");
            if (fp.empty()) fp = "chrome";
            std::string scy = V("scy", "auto");
            if (scy.empty()) scy = "auto";
            std::string path = V("path", "/");
            if (path.empty()) path = "/";

            ServerProfile sp;
            sp.id = id;
            sp.name = name;
            sp.uri = uri;
            sp.address = address;
            sp.port = port;
            sp.uuid = uuid;
            sp.security = scy;
            sp.alterId = aid;
            sp.sni = sni;
            sp.wsHost = wsHost;
            sp.path = path;
            sp.fingerprint = fp;
            result.push_back(sp);
        }

        std::sort(result.begin(), result.end(), [](const ServerProfile& a, const ServerProfile& b) {
            return a.id < b.id;
        });

        const char* expected[3] = {
            "http2-rel1.pixelated4.ir",
            "http2-rel1.pixelated5.ir",
            "http2-rel1.pixelatedthree.ir"
        };
        for (int i = 0; i < 3; ++i) {
            std::string expName = "server-" + std::to_string(i + 1);
            if (result[i].id != i || result[i].name != expName || result[i].address != expected[i]) {
                throw std::runtime_error("The bundled server list does not match the approved three profiles.");
            }
        }
        return result;
    }
};

struct VpnConfigFactory {
    static std::string IpCidr(const std::string& ip) {
        if (ip.find(':') != std::string::npos) return ip + "/128";
        return ip + "/32";
    }

    static std::string VmessJson(const ServerProfile& p, const std::string& resolvedIp) {
        std::ostringstream o;
        o << "    {\n"
          << "      \"type\": \"vmess\",\n"
          << "      \"tag\": \"proxy\",\n"
          << "      \"server\": " << JsonEscape(resolvedIp) << ",\n"
          << "      \"server_port\": " << p.port << ",\n"
          << "      \"uuid\": " << JsonEscape(p.uuid) << ",\n"
          << "      \"security\": " << JsonEscape(p.security) << ",\n"
          << "      \"alter_id\": " << p.alterId << ",\n"
          << "      \"transport\": {\n"
          << "        \"type\": \"ws\",\n"
          << "        \"path\": " << JsonEscape(p.path) << ",\n"
          << "        \"headers\": {\n"
          << "          \"Host\": " << JsonEscape(p.wsHost) << "\n"
          << "        }\n"
          << "      },\n"
          << "      \"tls\": {\n"
          << "        \"enabled\": true,\n"
          << "        \"server_name\": " << JsonEscape(p.sni) << ",\n"
          << "        \"insecure\": false,\n"
          << "        \"utls\": {\n"
          << "          \"enabled\": true,\n"
          << "          \"fingerprint\": " << JsonEscape(p.fingerprint) << "\n"
          << "        }\n"
          << "      }\n"
          << "    }";
        return o.str();
    }

    static std::string CreateProbe(const ServerProfile& p, const std::string& resolvedIp, int socksPort, const std::string& logPath) {
        std::ostringstream o;
        o << "{\n"
          << "  \"log\": {\n"
          << "    \"level\": \"info\",\n"
          << "    \"timestamp\": true,\n"
          << "    \"output\": " << JsonEscape(logPath) << "\n"
          << "  },\n"
          << "  \"dns\": {\n"
          << "    \"servers\": [\n"
          << "      {\n"
          << "        \"type\": \"udp\",\n"
          << "        \"tag\": \"remote\",\n"
          << "        \"server\": \"1.1.1.1\",\n"
          << "        \"server_port\": 53,\n"
          << "        \"detour\": \"proxy\"\n"
          << "      }\n"
          << "    ],\n"
          << "    \"final\": \"remote\",\n"
          << "    \"strategy\": \"ipv4_only\"\n"
          << "  },\n"
          << "  \"inbounds\": [\n"
          << "    {\n"
          << "      \"type\": \"socks\",\n"
          << "      \"tag\": \"probe\",\n"
          << "      \"listen\": \"127.0.0.1\",\n"
          << "      \"listen_port\": " << socksPort << "\n"
          << "    }\n"
          << "  ],\n"
          << "  \"outbounds\": [\n"
          << VmessJson(p, resolvedIp) << ",\n"
          << "    {\n"
          << "      \"type\": \"direct\",\n"
          << "      \"tag\": \"direct\"\n"
          << "    }\n"
          << "  ],\n"
          << "  \"route\": {\n"
          << "    \"auto_detect_interface\": true,\n"
          << "    \"rules\": [\n"
          << "      {\n"
          << "        \"protocol\": \"dns\",\n"
          << "        \"action\": \"hijack-dns\"\n"
          << "      }\n"
          << "    ],\n"
          << "    \"final\": \"proxy\"\n"
          << "  }\n"
          << "}\n";
        return o.str();
    }

    static std::string CreateTun(const ServerProfile& p, const std::string& resolvedIp, const std::string& logPath) {
        std::ostringstream o;
        o << "{\n"
          << "  \"log\": {\n"
          << "    \"level\": \"info\",\n"
          << "    \"timestamp\": true,\n"
          << "    \"output\": " << JsonEscape(logPath) << "\n"
          << "  },\n"
          << "  \"dns\": {\n"
          << "    \"servers\": [\n"
          << "      {\n"
          << "        \"type\": \"udp\",\n"
          << "        \"tag\": \"remote\",\n"
          << "        \"server\": \"1.1.1.1\",\n"
          << "        \"server_port\": 53,\n"
          << "        \"detour\": \"proxy\"\n"
          << "      }\n"
          << "    ],\n"
          << "    \"final\": \"remote\",\n"
          << "    \"strategy\": \"ipv4_only\"\n"
          << "  },\n"
          << "  \"inbounds\": [\n"
          << "    {\n"
          << "      \"type\": \"tun\",\n"
          << "      \"tag\": \"tun-in\",\n"
          << "      \"interface_name\": \"VPN Echo\",\n"
          << "      \"address\": [\"172.19.0.1/30\", \"fdfe:dcba:9876::1/126\"],\n"
          << "      \"mtu\": 1500,\n"
          << "      \"stack\": \"gvisor\",\n"
          << "      \"auto_route\": true,\n"
          << "      \"strict_route\": true,\n"
          << "      \"dns_mode\": \"hijack\",\n"
          << "      \"route_address\": [\"0.0.0.0/1\", \"128.0.0.0/1\", \"::/1\", \"8000::/1\"],\n"
          << "      \"route_exclude_address\": [\"10.0.0.0/8\", \"172.16.0.0/12\", \"192.168.0.0/16\", \"169.254.0.0/16\", \"fc00::/7\", \"fe80::/10\", " << JsonEscape(IpCidr(resolvedIp)) << "]\n"
          << "    }\n"
          << "  ],\n"
          << "  \"outbounds\": [\n"
          << VmessJson(p, resolvedIp) << ",\n"
          << "    {\n"
          << "      \"type\": \"direct\",\n"
          << "      \"tag\": \"direct\"\n"
          << "    }\n"
          << "  ],\n"
          << "  \"route\": {\n"
          << "    \"auto_detect_interface\": true,\n"
          << "    \"rules\": [\n"
          << "      {\n"
          << "        \"protocol\": \"dns\",\n"
          << "        \"action\": \"hijack-dns\"\n"
          << "      },\n"
          << "      {\n"
          << "        \"ip_is_private\": true,\n"
          << "        \"action\": \"route\",\n"
          << "        \"outbound\": \"direct\"\n"
          << "      }\n"
          << "    ],\n"
          << "    \"final\": \"proxy\"\n"
          << "  }\n"
          << "}\n";
        return o.str();
    }

    static std::string FormatRate(long long bytesPerSec) {
        char buf[64];
        if (bytesPerSec < 1024) {
            std::snprintf(buf, sizeof(buf), "%lld B/s", bytesPerSec);
        } else if (bytesPerSec < 1024 * 1024) {
            std::snprintf(buf, sizeof(buf), "%.1f KB/s", static_cast<double>(bytesPerSec) / 1024.0);
        } else {
            std::snprintf(buf, sizeof(buf), "%.2f MB/s", static_cast<double>(bytesPerSec) / (1024.0 * 1024.0));
        }
        return buf;
    }
};

} // namespace vpnecho
