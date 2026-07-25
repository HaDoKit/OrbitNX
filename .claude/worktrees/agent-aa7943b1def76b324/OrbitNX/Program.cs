using System.Runtime.InteropServices;

namespace OrbitNX;

internal static class Program
{
    private const int SwRestore = 9;

    [STAThread]
    private static void Main(string[] args)
    {
        // Windows invokes this exe again (with this argument) when it resolves the ms-gamebar/
        // ms-gamebarservices protocol stubs registered by GameBarSuppressor - do absolutely nothing
        // and exit immediately, no window, no tray icon, nothing for the first user to notice.
        if (Array.IndexOf(args, "--protocol-stub") >= 0) return;

        // Only one instance may run at a time - a second copy would fight the first over the same
        // ffmpeg/virtual-gamepad/tray-icon resources. The mutex name is process-wide but session-local
        // (no "Global\" prefix), matching how this app is actually used: one user, one desktop session.
        using var singleInstanceMutex = new Mutex(true, "Local\\OrbitNX-SingleInstance", out var isFirstInstance);
        if (!isFirstInstance)
        {
            ActivateExistingInstance();
            return;
        }

        ApplicationConfiguration.Initialize();

        var config = Config.Load();
        var orchestrator = new AppOrchestrator(config);

        using var appIcon = Icon.ExtractAssociatedIcon(Application.ExecutablePath) ?? SystemIcons.Application;

        using var trayIcon = new NotifyIcon
        {
            Icon = appIcon,
            Text = "OrbitNX",
            Visible = true,
        };

        var mainForm = new MainForm(config, orchestrator, trayIcon);

        var menu = new ContextMenuStrip();
        menu.Items.Add("Открыть", null, (_, _) =>
        {
            mainForm.Show();
            mainForm.WindowState = FormWindowState.Normal;
            mainForm.Activate();
        });
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Выход", null, (_, _) => Application.Exit());
        trayIcon.ContextMenuStrip = menu;
        trayIcon.DoubleClick += (_, _) =>
        {
            mainForm.Show();
            mainForm.WindowState = FormWindowState.Normal;
            mainForm.Activate();
        };

        Application.ApplicationExit += (_, _) => orchestrator.Dispose();
        Application.Run(mainForm);
    }

    /// <summary>Best-effort: brings the already-running instance's window to the front by title.
    /// Works even while that window is hidden in the tray (FindWindow isn't limited to visible
    /// windows). SetForegroundWindow can be silently ignored by Windows' foreground-lock rules for
    /// a background process stealing focus - if that happens the existing instance simply stays
    /// wherever it was (tray), which is still correct: there's still exactly one process running.</summary>
    private static void ActivateExistingInstance()
    {
        var hwnd = FindWindow(null, "OrbitNX");
        if (hwnd == IntPtr.Zero) return;

        ShowWindow(hwnd, SwRestore);
        SetForegroundWindow(hwnd);
    }

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern IntPtr FindWindow(string? lpClassName, string lpWindowName);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);
}
