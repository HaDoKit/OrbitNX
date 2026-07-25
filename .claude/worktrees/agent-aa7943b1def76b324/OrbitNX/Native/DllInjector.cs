using System.Runtime.InteropServices;
using System.Text;

namespace OrbitNX.Native;

/// <summary>
/// Classic CreateRemoteThread+LoadLibraryW DLL injection, used to load FocusHook.dll into
/// steam.exe so Big Picture's internal focus check always sees itself as foreground - see
/// FocusHook/focushook.cpp for what the injected DLL actually does once loaded.
/// </summary>
public static class DllInjector
{
    private const uint ProcessCreateThread = 0x0002;
    private const uint ProcessQueryInformation = 0x0400;
    private const uint ProcessVmOperation = 0x0008;
    private const uint ProcessVmWrite = 0x0020;
    private const uint ProcessVmRead = 0x0010;
    private const uint MemCommit = 0x1000;
    private const uint MemReserve = 0x2000;
    private const uint PageReadWrite = 0x04;

    public static bool Inject(int processId, string dllPath)
    {
        var access = ProcessCreateThread | ProcessQueryInformation | ProcessVmOperation | ProcessVmWrite | ProcessVmRead;
        var hProcess = OpenProcess(access, false, (uint)processId);
        if (hProcess == IntPtr.Zero)
        {
            Logger.Log("DllInjector", $"OpenProcess(pid={processId}) failed, err={Marshal.GetLastWin32Error()}");
            return false;
        }

        try
        {
            var pathBytes = Encoding.Unicode.GetBytes(dllPath + "\0");
            var remoteMem = VirtualAllocEx(hProcess, IntPtr.Zero, (uint)pathBytes.Length, MemCommit | MemReserve, PageReadWrite);
            if (remoteMem == IntPtr.Zero)
            {
                Logger.Log("DllInjector", $"VirtualAllocEx failed, err={Marshal.GetLastWin32Error()}");
                return false;
            }

            if (!WriteProcessMemory(hProcess, remoteMem, pathBytes, (uint)pathBytes.Length, out _))
            {
                Logger.Log("DllInjector", $"WriteProcessMemory failed, err={Marshal.GetLastWin32Error()}");
                return false;
            }

            var hKernel32 = GetModuleHandle("kernel32.dll");
            var loadLibraryAddr = GetProcAddress(hKernel32, "LoadLibraryW");
            if (loadLibraryAddr == IntPtr.Zero)
            {
                Logger.Log("DllInjector", "GetProcAddress(LoadLibraryW) failed");
                return false;
            }

            var hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0, loadLibraryAddr, remoteMem, 0, out _);
            if (hThread == IntPtr.Zero)
            {
                Logger.Log("DllInjector", $"CreateRemoteThread failed, err={Marshal.GetLastWin32Error()}");
                return false;
            }

            try
            {
                WaitForSingleObject(hThread, 5000);
                GetExitCodeThread(hThread, out var remoteModuleBase);
                Logger.Log("DllInjector", $"injected {dllPath} into pid={processId}, remote module base=0x{remoteModuleBase:X}");
                return remoteModuleBase != 0;
            }
            finally
            {
                CloseHandle(hThread);
            }
        }
        finally
        {
            CloseHandle(hProcess);
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, uint nSize, out IntPtr lpNumberOfBytesWritten);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern IntPtr GetModuleHandle(string lpModuleName);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes, uint dwStackSize,
        IntPtr lpStartAddress, IntPtr lpParameter, uint dwCreationFlags, out uint lpThreadId);

    [DllImport("kernel32.dll")]
    private static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeThread(IntPtr hThread, out uint lpExitCode);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr hObject);
}
