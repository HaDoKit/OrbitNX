using System.Diagnostics;
using System.Runtime.Versioning;
using OrbitNX.Audio;
using OrbitNX.BigPicture;
using OrbitNX.Capture;
using OrbitNX.Input;
using OrbitNX.Native;
using OrbitNX.Session;
using OrbitNX.ShadPs4;

namespace OrbitNX;

/// <summary>
/// Wires SessionController's start/stop signals to the capture/encode/input pipelines.
///
/// Steam and any games it launches run on the normal interactive desktop and are captured via
/// ffmpeg's own gfxcapture input, scoped to the real primary monitor. Big Picture/games are
/// visible on the real screen (the first user sees the same thing the second person does), so
/// Big Picture is exited (not just left running) whenever the client disconnects, giving the
/// first user their normal desktop back between sessions.
/// </summary>
[SupportedOSPlatform("windows8.0")]
public sealed class AppOrchestrator : IDisposable
{
    private readonly Config _config;
    private readonly SessionController _session;
    private readonly SteamLauncher _launcher = new();
    private readonly ShadPs4Launcher _shadLauncher = new();
    private readonly VirtualGamepadHub _gamepads = new();
    private readonly TouchInjector _touch = new();
    private readonly GameActiveSignal _gameActiveSignal = new();

    private VideoEncoder? _videoEncoder;
    private AudioCaptureSender? _audioEncoder;
    private AppTarget? _activeTarget;
    private bool _pipelineActive;
    private System.Threading.Timer? _contentWindowWatcher;

    // Rapid client reconnects fire SessionStarted (and SessionStopped) repeatedly - confirmed
    // live: a client that connects/disconnects several times in under a second caused 5 Steam
    // relaunches back to back (fixed by gating the Steam-launch part), and separately caused the
    // whole capture/encode pipeline to be torn down and rebuilt from scratch on every single
    // reconnect, with no serialization against OnSessionStopped running concurrently on another
    // thread. This one gate now serializes both handlers against each other, and _pipelineActive
    // skips rebuilding a pipeline that's already up.
    private readonly SemaphoreSlim _pipelineGate = new(1, 1);

    public AppOrchestrator(Config config)
    {
        _config = config;
        _gamepads.SwapAbxy = config.SwitchButtonLayout;
        _session = new SessionController(config);
        _session.SessionStarted += OnSessionStarted;
        _session.SessionStopped += OnSessionStopped;
        _session.PacketReceived += OnPacketReceived;
    }

    /// <summary>Whether the app is currently armed (listening for a client) - driven by the main
    /// window's toggle button, replacing the old "always start on launch" behavior.</summary>
    public bool IsArmed { get; private set; }

    /// <summary>Whether a client is currently connected and the capture/encode pipeline is
    /// active. Raises <see cref="ConnectionChanged"/> whenever it changes.</summary>
    public bool IsConnected => _pipelineActive;

    /// <summary>Fired on the thread pool (not the UI thread) whenever <see cref="IsConnected"/>
    /// changes - subscribers touching WinForms controls must marshal back via Invoke/BeginInvoke.</summary>
    public event Action? ConnectionChanged;

    /// <summary>Which app is currently launched/focused (null if neither has been launched yet
    /// this run) - lets MainForm show the user which mode is actually active. Set right before
    /// <see cref="ConnectionChanged"/> fires in OnSessionStarted, so it's always current by the
    /// time subscribers read it from that event.</summary>
    public AppTarget? ActiveTarget => _activeTarget;

    /// <summary>Fired on the thread pool when the Switch asked for shadPS4 but
    /// <see cref="Config.ShadPs4ExePath"/> is empty or points at a missing file - MainForm
    /// subscribes to prompt the user to configure it (and optionally open the download page),
    /// since AppOrchestrator itself has no UI to show one.</summary>
    public event Action? ShadPs4NotConfigured;

    /// <summary>Takes effect on the very next gamepad packet, no reconnect needed.</summary>
    public bool SwapAbxy
    {
        get => _gamepads.SwapAbxy;
        set => _gamepads.SwapAbxy = value;
    }

    /// <summary>Arms the app - starts listening for the client to connect. Actually launching Big
    /// Picture/starting the capture pipeline still only happens once a client really connects (see
    /// OnSessionStarted) - this alone doesn't touch Steam or anything else yet.</summary>
    public void Arm()
    {
        if (IsArmed) return;
        IsArmed = true;
        _session.Start();
    }

    /// <summary>Disarms the app - stops listening entirely. If a client was actually connected,
    /// this also triggers the normal disconnect cleanup (close the game, exit Big Picture, stop
    /// encoders - see OnSessionStopped) since SessionController's own loop observes the
    /// cancellation and fires SessionStopped on its way out.</summary>
    public void Disarm()
    {
        if (!IsArmed) return;
        IsArmed = false;
        _session.Stop();
    }

    [SupportedOSPlatform("windows8.0")]
    private async void OnSessionStarted(HidPacket firstPacket)
    {
        Logger.Log("AppOrchestrator", "OnSessionStarted: entered, waiting for pipeline gate");
        await _pipelineGate.WaitAsync();
        Logger.Log("AppOrchestrator", "OnSessionStarted: gate acquired");
        try
        {
            var target = firstPacket.AppTarget;
            IntPtr hwnd;

            // shadPS4 gets a virtual DualShock4 instead of Xbox360 - the only path that can carry
            // real gyro/accelerometer data through to the game (see VirtualGamepadHub's class doc
            // comment). Steam/Big Picture keeps Xbox360.
            //
            // History: (1) gamepad silently didn't respond in shadPS4 at all; (2) a standalone test
            // creating a ViGEm DS4 and reading it back via Windows.Gaming.Input.RawGameController
            // confirmed the raw report's button/axis/dpad encoding is correct at the Windows HID
            // level, ruling out a bit-layout bug; (3) with DS4 mode re-enabled, live testing showed
            // stutter/artifacts in the video feed, only in shadPS4 sessions, correlated with
            // ffmpeg's video mux thread dying on WSAENOBUFS (-10055, UDP send buffer exhaustion) -
            // i.e. the video breakage was a downstream symptom of system-wide contention, not
            // shadPS4-specific rendering behavior. Leading suspect: shadPS4 sending DS4 output
            // reports (rumble/lightbar color) that nothing was consuming, backing up the report
            // queue - VirtualGamepadHub.PlugInDs4/DrainDs4OutputReports now actively drains it via
            // AwaitRawOutputReport. If WSAENOBUFS/artifacts recur even with that fix, the next
            // suspect is genuine network congestion unrelated to controller mode, not this report
            // queue.
            _gamepads.Mode = target == AppTarget.BigPicture ? ControllerMode.Xbox360 : ControllerMode.DualShock4;

            if (target == AppTarget.BigPicture)
            {
                if (_activeTarget != AppTarget.BigPicture)
                {
                    Logger.Log("AppOrchestrator", "OnSessionStarted: launching Steam");
                    await _launcher.LaunchSteamAsync(CancellationToken.None);
                    Logger.Log("AppOrchestrator", "OnSessionStarted: LaunchSteamAsync returned");
                }
                else
                {
                    // Steam's still running from a previous session but Big Picture was exited on
                    // disconnect (see OnSessionStopped) - just re-open it instead of a full relaunch.
                    _launcher.OpenBigPicture();
                }
                _activeTarget = AppTarget.BigPicture;

                Logger.Log("AppOrchestrator", "OnSessionStarted: waiting for Big Picture window");
                hwnd = await _launcher.WaitForWindowAsync(TimeSpan.FromSeconds(30), CancellationToken.None);
            }
            else
            {
                if (_activeTarget != AppTarget.ShadPs4)
                {
                    // Launches straight into shadPS4's own --big-picture mode (its native,
                    // gamepad-navigable game list/UI) instead of a specific title picked via our own
                    // Switch-side picker screen - see ShadPs4Launcher.LaunchOrFocusAsync's doc
                    // comment. The titleId-driven direct-launch path (ShadPs4LibraryChannel/
                    // LibraryProtocol/library.c on the Switch) is deprecated/unwired but left intact
                    // in case it's worth reviving later - passing null here always takes the
                    // --big-picture fallback branch.
                    Logger.Log("AppOrchestrator", "OnSessionStarted: launching shadPS4 (--big-picture)");
                    var launched = await _shadLauncher.LaunchOrFocusAsync(_config.ShadPs4ExePath, titleId: null, CancellationToken.None);
                    if (!launched)
                    {
                        Logger.Log("AppOrchestrator", "OnSessionStarted: shadPS4 not configured/found");
                        ShadPs4NotConfigured?.Invoke();
                        return;
                    }
                }
                else
                {
                    _shadLauncher.Focus(_config.ShadPs4ExePath);
                }
                _activeTarget = AppTarget.ShadPs4;

                Logger.Log("AppOrchestrator", "OnSessionStarted: waiting for shadPS4 window");
                hwnd = await _shadLauncher.WaitForWindowAsync(_config.ShadPs4ExePath, TimeSpan.FromSeconds(30), CancellationToken.None);
            }

            Logger.Log("AppOrchestrator", $"OnSessionStarted: WaitForWindowAsync returned hwnd={hwnd}");

            if (hwnd == IntPtr.Zero) return; // window never appeared; nothing to capture

            if (_pipelineActive)
            {
                // A rapid reconnect fired SessionStarted again while the previous session's
                // pipeline is still up (its matching SessionStopped hasn't arrived yet, or already
                // came and went without us tearing anything down because it raced this call) -
                // nothing new to do, the existing capture/encoders are still perfectly valid.
                return;
            }

            // See TimerResolution's doc comment - requested for the whole session, not just while
            // audio is active, since video capture/encode competing for the same CPU quanta is part
            // of the same scheduling-jitter problem.
            TimerResolution.RequestHighResolution();

            // Audio capture/resample/send now all run as managed threads inside this process (see
            // AudioCaptureSender) rather than a separate ffmpeg process - MMCSS on those specific
            // threads (see Mmcss.Register) handles their own scheduling, but this process's overall
            // priority still affects how promptly the CLR schedules everything else they depend on
            // (GC, thread pool). A modest process-wide bump complements the per-thread MMCSS boost
            // rather than replacing it.
            try
            {
                Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.AboveNormal;
            }
            catch (Exception ex)
            {
                Logger.Log("AppOrchestrator", $"failed to raise process priority: {ex}");
            }

            _videoEncoder = new VideoEncoder(_config);
            _videoEncoder.Start();

            // Captures whatever is currently playing on the first user's real default audio
            // device directly via WASAPI loopback - see AudioCaptureSender.
            RestartAudioEncoder("session started");

            _pipelineActive = true;
            ConnectionChanged?.Invoke();

            // This watcher's whole job is working around Steam Input's own focus/rebinding
            // quirks (see the class doc comment and InjectFocusHookIntoNewContentWindows) - none
            // of that is known to apply to shadPS4's plain single-window process, so it's only
            // started for the Big Picture target. See the "biggest open risk" note in the plan:
            // whether shadPS4 keeps taking gamepad input while unfocused is unverified.
            if (target == AppTarget.BigPicture)
            {
                _contentWindowWatcher = new System.Threading.Timer(
                    _ => InjectFocusHookIntoNewContentWindows(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
            }
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"session start failed: {ex}");
        }
        finally
        {
            _pipelineGate.Release();
        }
    }

    /// <summary>Stops and recreates the audio capture/stream from scratch - a fresh WASAPI capture
    /// and a fresh TCP connection to the client, both of which the client already has to cope with
    /// on the very first connect, so this needs no special handling on that side. Caller must
    /// already hold <see cref="_pipelineGate"/>. Audio failing to (re)start must never take down
    /// video/gamepad, so failures are caught and logged here rather than propagated.</summary>
    private void RestartAudioEncoder(string reason)
    {
        try
        {
            _audioEncoder?.Stop();
            _audioEncoder = new AudioCaptureSender(_config);
            _audioEncoder.Start();
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"audio capture (re)start failed ({reason}): {ex}");
        }
    }

    /// <summary>Kills and restarts the ffmpeg video process from scratch - see
    /// VideoEncoder.IsStalled's doc comment for why this exists (a hung Windows.Graphics.Capture
    /// source that nothing else notices or recovers from). Caller must already hold
    /// <see cref="_pipelineGate"/>. Mirrors RestartAudioEncoder: failures here must never take down
    /// audio/gamepad, so they're caught and logged rather than propagated.</summary>
    private void RestartVideoEncoder(string reason)
    {
        try
        {
            Logger.Log("AppOrchestrator", $"restarting video encoder ({reason})");
            _videoEncoder?.Stop();
            _videoEncoder = new VideoEncoder(_config);
            _videoEncoder.Start();
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"video encoder restart failed ({reason}): {ex}");
        }
    }

    /// <summary>Runs RestartAudioEncoder a few seconds from now instead of immediately - see the
    /// "game launched" call site for why. Re-acquires <see cref="_pipelineGate"/> itself (the
    /// delay means this can't stay under the caller's own acquisition) and bails out quietly if
    /// the session already ended during the wait.</summary>
    private void ScheduleDelayedAudioRestart(string reason)
    {
        _ = Task.Run(async () =>
        {
            await Task.Delay(TimeSpan.FromSeconds(3));
            await _pipelineGate.WaitAsync();
            try
            {
                if (!_pipelineActive) return;
                RestartAudioEncoder(reason);
            }
            finally
            {
                _pipelineGate.Release();
            }
        });
    }

    /// <summary>
    /// Fully closes and reopens Big Picture after a game closes, instead of trying to coax Steam
    /// Input into handing the controller binding back to Big Picture's own context on its own.
    /// Confirmed live (via Steam's own C:\steam\logs\controller_ui.txt) that it doesn't: after a
    /// non-Steam-game shortcut (e.g. "Luna Abyss") closes, Steam keeps reloading that game's own
    /// controller config seconds after correctly switching to Big Picture's - neither a forced
    /// gamepad replug (removed, confirmed live to make no difference) nor a synthetic Escape
    /// keypress (SendBackNavigation, kept, also confirmed live not to fully fix it alone) stopped
    /// this. A fresh Big Picture launch is the one thing already known to produce a working,
    /// responsive Big Picture - every session's very first connect goes through exactly this path
    /// (LaunchSteamAsync/OpenBigPicture + WaitForWindowAsync) - so this trades a visible reload
    /// flicker for gamepad control that actually works, rather than a subtler in-place fix that
    /// repeatedly hasn't. No artificial settle delay between close and reopen (see below) - the
    /// flicker's actual duration hasn't been measured live yet, don't assume it's long.
    ///
    /// Runs as a background task (mirrors ScheduleDelayedAudioRestart) since the content-window
    /// watcher tick that triggers this must not block on a multi-second close/reopen/wait sequence.
    /// Deliberately does not hold _pipelineGate for the whole operation - only _launcher state is
    /// touched, and OnSessionStopped's own ExitBigPicture/CloseActiveGame calls are idempotent-ish
    /// enough (matches WaitForWindowAsync's own periodic re-fire-OpenBigPicture pattern) that a
    /// benign race with a concurrent disconnect isn't worth serializing against.
    /// </summary>
    private void RestartBigPictureAfterGameClosed()
    {
        _restartingBigPicture = true;
        _ = Task.Run(async () =>
        {
            try
            {
                // No settle delay between ExitBigPicture and OpenBigPicture - that was an untested,
                // conservative guess, not a confirmed requirement (WaitForWindowAsync's own re-fire
                // pattern already tolerates OpenBigPicture landing before Steam's fully ready for
                // it). Measure actual close-to-reopen timing live before adding any delay back.
                _launcher.ExitBigPicture();

                if (!_pipelineActive || _activeTarget != AppTarget.BigPicture) return;
                _launcher.OpenBigPicture();

                var hwnd = await _launcher.WaitForWindowAsync(TimeSpan.FromSeconds(30), CancellationToken.None);
                Logger.Log("AppOrchestrator", $"RestartBigPictureAfterGameClosed: reopened, hwnd={hwnd}");
            }
            catch (Exception ex)
            {
                Logger.Log("AppOrchestrator", $"RestartBigPictureAfterGameClosed failed: {ex}");
            }
            finally
            {
                _restartingBigPicture = false;
            }
        });
    }

    // Set for the duration of RestartBigPictureAfterGameClosed - tells the null-content-window
    // handling below that Big Picture's window disappearing right now is our own deliberate doing,
    // not the first user closing it, so it must not count towards ForceDisconnect's tolerance.
    private volatile bool _restartingBigPicture;

    /// <summary>
    /// Launching a game from Big Picture opens a second, separate top-level window with its own
    /// "am I focused?" check - FocusHook must be injected into that process specifically, not just
    /// steam.exe/steamwebhelper.exe, or gamepad input stops the instant the first user's own real
    /// OS focus moves to a different window (likely, since the game shares the same visible
    /// desktop the first user actually works on). Capture itself never needs to change here -
    /// ffmpeg's gfxcapture reads the whole primary monitor's output regardless of which window is
    /// currently showing there.
    /// </summary>
    // Tracks whether the last watcher tick's active content window belonged to a game (true) or
    // Big Picture itself (false) - null means unknown/not yet observed. Used to detect the
    // game-just-closed transition below.
    private bool? _lastActiveWasGame;

    // See the isGame-is-null handling below: debounces a transient non-Steam-family window (a
    // third-party game launcher) covering Big Picture against being mistaken for it actually
    // closing.
    private const int NullContentWindowTicksBeforeDisconnect = 5;
    private int _consecutiveNullContentWindowTicks;
    private bool _contentWindowWasKnownBeforeNullStreak;

    private void InjectFocusHookIntoNewContentWindows()
    {
        if (!_pipelineGate.Wait(0)) return; // a start/stop is in progress this tick; try again next tick
        try
        {
            if (!_pipelineActive) return;

            // See VideoEncoder.IsStalled's doc comment - this is the only thing in the whole
            // pipeline that ever notices a hung capture and does something about it. 5s: long
            // enough that ffmpeg's own periodic stderr progress cadence never false-positives,
            // short enough to cut a real hang well before it's this noticeable again.
            if (_videoEncoder is { } video && video.IsStalled(TimeSpan.FromSeconds(5)))
            {
                RestartVideoEncoder("no encode progress for 5s");
            }

            SteamLauncher.InjectFocusHookIntoAllSteamProcesses();
            var isGame = _launcher.FindActiveContentWindowAndInjectFocusHook();

            // See GameActiveSignal/focushook.cpp's IsGameActive - lets the injected hook's focus
            // spoof step aside while a game (not Big Picture itself) is active, so Big Picture
            // correctly notices it lost real focus instead of both it and the game reacting to the
            // same gamepad input at once. Set every tick (not just on transition) so it stays
            // correct even if a tick is missed - isGame is null while no Steam-family content
            // window exists at all (see below), in which case there's no meaningful game/Big
            // Picture distinction to make, so leave the spoof on rather than guess.
            _gameActiveSignal.SetGameActive(isGame == true);

            // After closing a game launched from Big Picture, gamepad control can stay completely
            // dead in Big Picture itself even though the focus hook keeps correctly targeting its
            // window every tick. FullReplug (a forced ViGEm disconnect/reconnect cycle) used to run
            // here as an attempted fix - removed after live testing via Steam's own
            // controller_ui.txt proved it had no effect: Steam Input re-warmed and correctly
            // switched the active binding back to Big Picture's own appid (769) with or without a
            // replug, then independently reloaded the just-closed game's own config seconds later
            // regardless. That reload is Steam's own behavior, not a device-presence/binding-handoff
            // problem a replug could ever address - see SendBackNavigation's doc comment for the
            // current best lead (the game's own Steam Input config having no navigation-compatible
            // action set) and CloseActiveGame in SteamLauncher.cs's comment history for other things
            // already ruled out.
            if (_lastActiveWasGame == true && isGame == false)
            {
                // SendBackNavigation (a real Escape keypress) backs Big Picture off the just-closed
                // game's own detail/store page - confirmed live via Steam's own controller_ui.txt
                // that while that page is showing, Steam Input loads THAT game's controller config
                // instead of Big Picture's generic navigation config, which is a dead end for a
                // non-Steam-game shortcut with no configured Steam Input layout. Also confirmed live
                // NOT to fully fix the symptom by itself (Steam still reloads the game's config
                // moments later regardless) - kept because it's cheap and not shown to hurt, not
                // because it's a confirmed fix.
                Logger.Log("AppOrchestrator", "game closed, back in Big Picture - backing off the game's own page and restarting Big Picture");
                RestartAudioEncoder("game closed");
                SteamLauncher.SendBackNavigation();
                RestartBigPictureAfterGameClosed();
            }

            // A game briefly grabbing/reconfiguring the shared audio endpoint right as it starts can
            // leave audio crackling for the rest of the session if the capture restart lands in the
            // middle of that disturbance - delaying the automatic restart lets it settle first.
            //
            // ForceForegroundWindow is needed here because GameActiveSignal's spoof-suppression
            // alone is not enough: real OS foreground focus never actually moves to the game unless
            // something calls SetForegroundWindow (see ReassertFocus's doc comment for why that's
            // deliberately avoided as a per-tick call), and Steam Input's per-context controller
            // binding keys off real focus regardless of what FocusHook tells Big Picture's own
            // internal poll. Safe here specifically because it fires exactly once, at a confirmed
            // transition, targeting the exact just-detected game hwnd.
            if (_lastActiveWasGame != true && isGame == true)
            {
                Logger.Log("AppOrchestrator", "game launched - forcing real foreground focus and scheduling audio capture restart after a settle delay");
                SteamLauncher.ForceForegroundWindow(_launcher.LastActiveContentWindow);
                ScheduleDelayedAudioRestart("game launched");
            }

            // isGame is null when FindActiveContentWindowAndInjectFocusHook found no Steam-family
            // content window at all - neither Big Picture nor a game. Since this watcher only ever
            // runs while _pipelineActive is true, which only becomes true after Big Picture's
            // window was already found once (see OnSessionStarted), a null result here always
            // means it disappeared *during* an active session - the first user closed it (or it
            // crashed) rather than us tearing it down ourselves (which only happens after a
            // disconnect, at which point this watcher has already been disposed).
            //
            // Not necessarily true, though: a game launched through a third-party launcher (Ubisoft
            // Connect, EA app, the game's own separate launcher window, etc.) shows a window that
            // isn't Steam-family at all (it isn't steam.exe/steamwebhelper.exe and typically isn't
            // installed under the Steam directory this check relies on - see IsSteamFamilyProcess),
            // so it can briefly cover/replace Big Picture's window while the launcher does its own
            // thing before the actual game starts. A single null tick here used to force-disconnect
            // the client immediately on exactly that transient case. Requiring several consecutive
            // null ticks before concluding Big Picture is *really* gone tolerates a launcher window
            // sitting there for a few seconds without kicking the client, while still catching a
            // genuine close/crash (which stays null) shortly after.
            if (isGame is null)
            {
                if (_restartingBigPicture)
                {
                    // We're the ones who made Big Picture's window disappear right now (see
                    // RestartBigPictureAfterGameClosed) - don't let the transient-launcher-window
                    // tolerance below mistake our own deliberate close/reopen for the first user
                    // having closed Big Picture themselves.
                    _consecutiveNullContentWindowTicks = 0;
                }
                else
                {
                    if (_consecutiveNullContentWindowTicks == 0)
                    {
                        // Capture this before _lastActiveWasGame gets overwritten to null below - by
                        // the time the tick threshold is reached a few seconds from now,
                        // _lastActiveWasGame would already read null regardless of what it was when
                        // this streak of null ticks actually started.
                        _contentWindowWasKnownBeforeNullStreak = _lastActiveWasGame is not null;
                    }
                    _consecutiveNullContentWindowTicks++;
                    if (_consecutiveNullContentWindowTicks == NullContentWindowTicksBeforeDisconnect && _contentWindowWasKnownBeforeNullStreak)
                    {
                        Logger.Log("AppOrchestrator", "Big Picture window gone for several seconds - forcing client disconnect");
                        _session.ForceDisconnect();
                    }
                }
            }
            else
            {
                _consecutiveNullContentWindowTicks = 0;
            }
            _lastActiveWasGame = isGame;
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"content window focus-hook injection failed: {ex}");
        }
        finally
        {
            _pipelineGate.Release();
        }
    }

    private void OnSessionStopped()
    {
        _pipelineGate.Wait();
        try
        {
            _contentWindowWatcher?.Dispose();
            _contentWindowWatcher = null;

            _videoEncoder?.Stop();
            _audioEncoder?.Stop();
            _gamepads.DisconnectAll();
            TimerResolution.Release();
            try { Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.Normal; } catch { /* best-effort */ }

            _videoEncoder = null;
            _audioEncoder = null;
            _pipelineActive = false;
            _lastActiveWasGame = null;
            ConnectionChanged?.Invoke();

            // Nothing is hidden anymore - Big Picture/shadPS4 (or a game) sitting idle on the real
            // screen once nobody is connected would be a visible nuisance to the first user. Steam
            // itself stays running (see LaunchSteamAsync) so the next connect can jump straight
            // back in - shadPS4 has no equivalent persistent background process, so its window is
            // simply closed outright (see ShadPs4Launcher.Close).
            if (_activeTarget == AppTarget.BigPicture)
            {
                _launcher.CloseActiveGame();
                _launcher.ExitBigPicture();
            }
            else if (_activeTarget == AppTarget.ShadPs4)
            {
                _shadLauncher.Close(_config.ShadPs4ExePath);
                // Unlike Big Picture (Steam itself keeps running, so _activeTarget staying
                // BigPicture correctly means "just OpenBigPicture(), don't relaunch Steam" on the
                // next connect), shadPS4 has no persistent background process - it's now fully
                // closed. Leaving _activeTarget at ShadPs4 here made the next OnSessionStarted take
                // the "already active" branch and just Focus() a window that no longer exists,
                // which silently does nothing and left WaitForWindowAsync timing out 30s later
                // without ever actually relaunching shadPS4 - confirmed live via OrbitNX.log.
                _activeTarget = null;
            }
        }
        finally
        {
            _pipelineGate.Release();
        }
    }

    private void OnPacketReceived(HidPacket packet)
    {
        try
        {
            _gamepads.Apply(packet);
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"gamepad apply failed: {ex}");
        }

        try
        {
            _touch.Update(packet.TouchX1, packet.TouchY1);
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"touch inject failed: {ex}");
        }
    }

    public void Dispose()
    {
        IsArmed = false;
        _session.DisposeAsync().AsTask().GetAwaiter().GetResult();
        OnSessionStopped();
        _gamepads.Dispose();
        _touch.Dispose();
        _gameActiveSignal.Dispose();
        _pipelineGate.Dispose();
    }
}
