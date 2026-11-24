using System.Security.Cryptography;
using System.Text;

namespace VpnEcho;

public static class LicenseStore
{
    private const string ExpectedSha256 = "8848baf57c30e8a91a9ecd6b893a64a4564959eed47049fce10d0ebbe871ef3d";
    private static string MarkerPath => Path.Combine(DataPaths.Root, "activation-v1");
    private static readonly byte[] Expected = Convert.FromHexString(ExpectedSha256);

    public static bool IsActivated()
    {
        try { return CryptographicOperations.FixedTimeEquals(File.ReadAllBytes(MarkerPath), Expected); }
        catch { return false; }
    }

    public static bool Activate(string key)
    {
        byte[] digest = SHA256.HashData(Encoding.UTF8.GetBytes(key.Trim()));
        if (!CryptographicOperations.FixedTimeEquals(digest, Expected)) return false;
        DataPaths.Ensure(); string temp = MarkerPath + ".tmp";
        File.WriteAllBytes(temp, Expected);
        File.Move(temp, MarkerPath, true);
        return true;
    }
}
