using System.Threading;
using System.Windows;
using MessageBox = System.Windows.MessageBox;

namespace VpnEcho;

public partial class App : System.Windows.Application
{
    private Mutex? _singleInstance;
    protected override void OnStartup(StartupEventArgs e)
    {
        _singleInstance = new Mutex(true, "Local\\VPN-Echo-Windows-1.1", out bool created);
        if (!created)
        {
            MessageBox.Show("VPN Echo is already running. Check the notification area.", "VPN Echo", MessageBoxButton.OK, MessageBoxImage.Information);
            Shutdown();
            return;
        }
        base.OnStartup(e);
    }
    protected override void OnExit(ExitEventArgs e)
    {
        _singleInstance?.ReleaseMutex();
        _singleInstance?.Dispose();
        base.OnExit(e);
    }
}
