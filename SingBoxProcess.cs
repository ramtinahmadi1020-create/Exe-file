using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace VpnEcho;

/// <summary>Starts sing-box in a hidden console process group so CTRL+BREAK can stop it gracefully and let TUN routes unwind.</summary>
public sealed class SingBoxProcess : IAsyncDisposable
{
    private readonly Process _process;
    private int _stopping;
    public int ProcessId => _process.Id;
    public bool HasExited { get { try { return _process.HasExited; } catch { return true; } } }

    private SingBoxProcess(Process process) => _process = process;

    public static SingBoxProcess Start(string executable, string configPath)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("The desktop tunnel is Windows-only.");
        if (!File.Exists(executable)) throw new FileNotFoundException("sing-box.exe is missing from the installation folder.", executable);
        var si = new Native.STARTUPINFO { cb = Marshal.SizeOf<Native.STARTUPINFO>(), dwFlags = Native.STARTF_USESHOWWINDOW, wShowWindow = Native.SW_HIDE };
        string command = $"\"{executable}\" run -c \"{configPath}\"";
        var mutable = new StringBuilder(command);
        if (!Native.CreateProcess(executable, mutable, IntPtr.Zero, IntPtr.Zero, false,
                Native.CREATE_NEW_CONSOLE | Native.CREATE_UNICODE_ENVIRONMENT,
                IntPtr.Zero, Path.GetDirectoryName(executable), ref si, out var pi))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Windows could not start the VPN engine.");
        try
        {
            Native.CloseHandle(pi.hThread);
            var process = Process.GetProcessById(unchecked((int)pi.dwProcessId));
            return new SingBoxProcess(process);
        }
        finally { Native.CloseHandle(pi.hProcess); }
    }

    public async Task StopAsync()
    {
        if (Interlocked.Exchange(ref _stopping, 1) != 0) return;
        await Task.Run(() =>
        {
            if (HasExited) { _process.Dispose(); return; }
            bool attached = false, ignored = false;
            try
            {
                attached = Native.AttachConsole((uint)_process.Id);
                if (attached)
                {
                    ignored = Native.SetConsoleCtrlHandler(IntPtr.Zero, true);
                    Native.GenerateConsoleCtrlEvent(Native.CTRL_BREAK_EVENT, (uint)_process.Id);
                }
            }
            catch { }
            finally
            {
                if (attached) Native.FreeConsole();
                if (ignored) Native.SetConsoleCtrlHandler(IntPtr.Zero, false);
            }
            try
            {
                if (!_process.WaitForExit(4500) && !_process.HasExited)
                {
                    _process.Kill(entireProcessTree: true);
                    _process.WaitForExit(3000);
                }
            }
            catch { try { if (!_process.HasExited) _process.Kill(entireProcessTree: true); } catch { } }
            _process.Dispose();
        });
    }

    public ValueTask DisposeAsync() => new(StopAsync());

    private static class Native
    {
        internal const uint CREATE_NEW_CONSOLE = 0x00000010, CREATE_UNICODE_ENVIRONMENT = 0x00000400;
        internal const uint CTRL_BREAK_EVENT = 1;
        internal const int STARTF_USESHOWWINDOW = 1, SW_HIDE = 0;
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct STARTUPINFO
        {
            public int cb;
            public string? lpReserved, lpDesktop, lpTitle;
            public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
            public short wShowWindow, cbReserved2;
            public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public uint dwProcessId, dwThreadId; }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        internal static extern bool CreateProcess(string lpApplicationName, StringBuilder lpCommandLine,
            IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles, uint creationFlags,
            IntPtr environment, string? currentDirectory, ref STARTUPINFO startupInfo, out PROCESS_INFORMATION processInformation);
        [DllImport("kernel32.dll", SetLastError = true)] internal static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)] internal static extern bool AttachConsole(uint processId);
        [DllImport("kernel32.dll", SetLastError = true)] internal static extern bool FreeConsole();
        [DllImport("kernel32.dll", SetLastError = true)] internal static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint processGroupId);
        [DllImport("kernel32.dll", SetLastError = true)] internal static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    }
}
