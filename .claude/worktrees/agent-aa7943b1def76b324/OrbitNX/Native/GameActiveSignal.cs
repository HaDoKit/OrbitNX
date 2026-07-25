namespace OrbitNX.Native;

/// <summary>
/// Named event that tells every focushook.dll instance currently injected into a Steam-family
/// process whether a launched game (as opposed to Big Picture itself) is the active content window
/// - see Native/FocusHook/focushook.cpp's IsGameActive doc comment for why this exists. Signaled
/// (Set) while a game is active, non-signaled (Reset) the rest of the time - a plain named
/// ManualResetEvent is enough since only one writer (this process) ever calls Set/Reset and every
/// reader only ever polls the current state rather than waiting on a transition.
/// </summary>
public sealed class GameActiveSignal : IDisposable
{
    private readonly EventWaitHandle _event = new(initialState: false, EventResetMode.ManualReset, "OrbitNX_GameActive");

    public void SetGameActive(bool active)
    {
        if (active) _event.Set();
        else _event.Reset();
    }

    public void Dispose() => _event.Dispose();
}
