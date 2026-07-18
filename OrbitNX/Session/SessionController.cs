using System.Net.Sockets;
using OrbitNX.Input;

namespace OrbitNX.Session;

/// <summary>
/// Mirrors the original client's connectHID() loop: repeatedly dial the client's HID port,
/// and treat a successful connection as "session started" / its closure as "session stopped".
/// This is the single component driving the rest of the app's lifecycle.
/// </summary>
public sealed class SessionController : IAsyncDisposable
{
    private readonly Config _config;
    private readonly HidFrameReader _frameReader = new();

    private CancellationTokenSource? _runCts;
    private Task? _runTask;
    private TcpClient? _currentClient;

    public event Action<HidPacket>? SessionStarted;
    public event Action? SessionStopped;
    public event Action<HidPacket>? PacketReceived;

    public SessionController(Config config)
    {
        _config = config;
    }

    /// <summary>Forcibly drops the current client connection (if any), without disarming the
    /// whole controller - RunLoopAsync's outer loop keeps running and goes back to listening for
    /// a fresh reconnect, exactly as if the client itself had disconnected. Used by AppOrchestrator
    /// when Big Picture's window disappears unexpectedly (closed by the first user, or crashed) -
    /// the second person's session shouldn't be able to keep "controlling" a Big Picture that no
    /// longer exists.</summary>
    public void ForceDisconnect()
    {
        try { _currentClient?.Close(); } catch { /* best-effort */ }
    }

    public void Start()
    {
        Stop();
        _runCts = new CancellationTokenSource();
        // Task.Run, not a direct call: Start() runs on the WinForms UI thread before
        // Application.Run() begins pumping messages, so a direct call here would capture that
        // thread's SynchronizationContext for every downstream await in this whole session/capture
        // chain (OnSessionStarted, SteamLauncher, ...). If the UI thread is
        // ever busy (e.g. a modal dialog), every one of those continuations queues up and the whole
        // chain silently stalls with no exception. Starting from a thread-pool thread instead means
        // nothing in this chain ever needs the UI thread's message loop to make progress.
        _runTask = Task.Run(() => RunLoopAsync(_runCts.Token));
    }

    public void Stop()
    {
        _runCts?.Cancel();
        _runCts = null;
    }

    private async Task RunLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            if (!_config.IsValid())
            {
                await Task.Delay(1000, ct).ContinueWith(_ => { }, TaskScheduler.Default);
                continue;
            }

            using var client = new TcpClient();
            try
            {
                await client.ConnectAsync(_config.ClientIp, _config.HidPort, ct);
                client.NoDelay = true;
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                Logger.Log("SessionController", $"HID connect to {_config.ClientIp}:{_config.HidPort} failed: {ex.Message}");
                try { await Task.Delay(1000, ct); } catch (TaskCanceledException) { }
                continue;
            }

            Logger.Log("SessionController", $"HID connected to {_config.ClientIp}:{_config.HidPort}");
            _currentClient = client;

            // A bare TCP accept doesn't confirm the real OrbitNX client is on the other end - some
            // other device on the network can end up holding this IP (DHCP lease reassignment,
            // typo'd address, etc.) and accept-then-immediately-close the connection, which used
            // to be enough to fire SessionStarted (and re-open Big Picture) on every such blip.
            // Only treat this as a genuine session once at least one real HID packet has actually
            // been parsed off the wire - see ReadLoopAsync's sawFirstPacket callback below.
            var sessionStarted = false;
            try
            {
                await ReadLoopAsync(client, ct, firstPacket =>
                {
                    sessionStarted = true;
                    Logger.Log("SessionController", "first HID packet received - session started");
                    SessionStarted?.Invoke(firstPacket);
                });
                Logger.Log("SessionController", "HID stream ended (remote closed)");
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                // fall through to SessionStopped + retry below
                Logger.Log("SessionController", $"HID read loop ended with exception: {ex}");
            }
            finally
            {
                _currentClient = null;
                if (sessionStarted)
                {
                    Logger.Log("SessionController", "session stopped");
                    SessionStopped?.Invoke();
                }
            }

            try { await Task.Delay(1000, ct); } catch (TaskCanceledException) { }
        }
    }

    private async Task ReadLoopAsync(TcpClient client, CancellationToken ct, Action<HidPacket> onFirstPacket)
    {
        var stream = client.GetStream();
        var buffer = new byte[4096];
        var firedStart = false;

        while (!ct.IsCancellationRequested)
        {
            var read = await stream.ReadAsync(buffer, ct);
            if (read == 0) break; // remote closed

            foreach (var packet in _frameReader.Feed(buffer, read))
            {
                if (!firedStart)
                {
                    firedStart = true;
                    onFirstPacket(packet);
                }
                PacketReceived?.Invoke(packet);
            }
        }
    }

    public async ValueTask DisposeAsync()
    {
        Stop();
        if (_runTask is not null)
        {
            try { await _runTask; } catch { /* cancellation */ }
        }
    }
}
