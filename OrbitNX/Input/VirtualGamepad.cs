using Nefarius.ViGEm.Client;
using Nefarius.ViGEm.Client.Targets;
using Nefarius.ViGEm.Client.Targets.Xbox360;

namespace OrbitNX.Input;

/// <summary>
/// Owns up to 4 virtual Xbox 360 controllers via ViGEmBus (Nefarius.ViGEm.Client) - an
/// independent, actively maintained driver (also used by Sunshine, DS4Windows). Mirrors
/// main.js's handleControllerInput mapping exactly (button swap, boolean-threshold triggers,
/// dpad combination logic). Deliberately does not touch the real mouse/keyboard - all input is
/// gamepad-only. The client only ever sees "a standard Xbox 360 controller" through XInput.
/// </summary>
public sealed class VirtualGamepadHub : IDisposable
{
    /// <summary>True (default) swaps A/B and X/Y to match the client's physical Nintendo Switch
    /// controller layout - the Switch's own button positions are diagonally mirrored relative to
    /// an Xbox controller's, so without this, prompts/muscle memory from an Xbox-style game would
    /// point at the physically wrong button. Settable at runtime from the main window's checkbox
    /// (see MainForm) - takes effect on the very next packet, no reconnect needed.</summary>
    public bool SwapAbxy { get; set; } = true;

    private readonly ViGEmClient _client = new();
    private readonly IXbox360Controller?[] _controllers = new IXbox360Controller?[5]; // 1-based, 0 unused

    // Set by FullReplug while a controller is deliberately being held disconnected - see its doc
    // comment for why Apply must not just immediately plug a fresh one back in during that window.
    private DateTime _suppressReplugUntilUtc = DateTime.MinValue;

    public VirtualGamepadHub()
    {
        GameBarSuppressor.Disable();
    }

    public void Apply(HidPacket packet)
    {
        var count = Math.Clamp((int)packet.ControllerCount, 0, 4);
        var suppressReplug = DateTime.UtcNow < _suppressReplugUntilUtc;

        for (var i = 0; i < 4; i++)
        {
            var id = i + 1;
            if (i < count)
            {
                if (_controllers[id] is null)
                {
                    if (suppressReplug) continue; // mid FullReplug gap - its own delayed task will reconnect
                    _controllers[id] = PlugIn(id);
                }
                ApplyState(_controllers[id]!, packet.Controllers[i], SwapAbxy);
            }
            else if (_controllers[id] is not null)
            {
                UnplugAt(id);
            }
        }
    }

    private IXbox360Controller PlugIn(int id)
    {
        var pad = _client.CreateXbox360Controller();
        pad.Connect();
        Logger.Log("VirtualGamepad", $"controller {id} connected via ViGEmBus");
        return pad;
    }

    private void UnplugAt(int id)
    {
        try { _controllers[id]?.Disconnect(); }
        catch (Exception ex) { Logger.Log("VirtualGamepad", $"controller {id} disconnect failed (already gone?): {ex.Message}"); }
        _controllers[id] = null;
        Logger.Log("VirtualGamepad", $"controller {id} disconnected");
    }

    private static void ApplyState(IXbox360Controller pad, ControllerState s, bool swapAbxy)
    {
        pad.SetAxisValue(Xbox360Axis.LeftThumbX, ToShortAxis(ConvertAnalog(s.LJoyX)));
        pad.SetAxisValue(Xbox360Axis.LeftThumbY, ToShortAxis(ConvertAnalog(s.LJoyY)));
        pad.SetAxisValue(Xbox360Axis.RightThumbX, ToShortAxis(ConvertAnalog(s.RJoyX)));
        pad.SetAxisValue(Xbox360Axis.RightThumbY, ToShortAxis(ConvertAnalog(s.RJoyY)));

        if (swapAbxy)
        {
            pad.SetButtonState(Xbox360Button.B, s.A);
            pad.SetButtonState(Xbox360Button.A, s.B);
            pad.SetButtonState(Xbox360Button.X, s.Y);
            pad.SetButtonState(Xbox360Button.Y, s.X);
        }
        else
        {
            pad.SetButtonState(Xbox360Button.B, s.B);
            pad.SetButtonState(Xbox360Button.A, s.A);
            pad.SetButtonState(Xbox360Button.X, s.X);
            pad.SetButtonState(Xbox360Button.Y, s.Y);
        }

        pad.SetButtonState(Xbox360Button.Back, s.Minus);
        pad.SetButtonState(Xbox360Button.Start, s.Plus);
        pad.SetButtonState(Xbox360Button.LeftShoulder, s.L);
        pad.SetButtonState(Xbox360Button.RightShoulder, s.R);
        pad.SetButtonState(Xbox360Button.LeftThumb, s.LS);
        pad.SetButtonState(Xbox360Button.RightThumb, s.RS);
        pad.SetButtonState(Xbox360Button.Up, s.Up);
        pad.SetButtonState(Xbox360Button.Down, s.Down);
        pad.SetButtonState(Xbox360Button.Left, s.Left);
        pad.SetButtonState(Xbox360Button.Right, s.Right);

        // ZL/ZR are boolean in the wire format, so triggers are all-or-nothing (matches main.js).
        pad.SetSliderValue(Xbox360Slider.LeftTrigger, (byte)(s.ZL ? 255 : 0));
        pad.SetSliderValue(Xbox360Slider.RightTrigger, (byte)(s.ZR ? 255 : 0));
    }

    // Mirrors main.js's convertAnalog: normalizes the raw wire int32 into [-1, 1].
    private static double ConvertAnalog(int axis)
    {
        var na = axis / 32767.5;
        if (na > 1)
        {
            na = 2 - na;
            na = -na;
        }
        return na;
    }

    private static short ToShortAxis(double normalized) =>
        (short)Math.Clamp(normalized * 32766, short.MinValue, short.MaxValue);

    public void DisconnectAll()
    {
        for (var id = 1; id <= 4; id++)
        {
            if (_controllers[id] is not null) UnplugAt(id);
        }
    }

    // How long a controller stays actually gone before FullReplug brings it back - long enough that
    // Steam has to genuinely notice the device disappearing and reappearing as two separate events,
    // not just a same-tick pair it could coalesce away.
    private const int FullReplugGapMs = 750;

    /// <summary>Disconnects every currently-connected virtual controller, then reconnects them all
    /// after a real gap (not immediately) - a fresh hotplug cycle, in case Steam's controller
    /// enumeration needs actual elapsed time with the device absent to notice it left and treat the
    /// reconnect as a genuinely new device, rather than a same-tick unplug+replug pair it might
    /// coalesce into a no-op. This forces Steam Input to re-evaluate a controller it may still be
    /// holding under a closed game's own binding profile instead of handing it back to the
    /// desktop/Big Picture profile.</summary>
    public void FullReplug()
    {
        var connectedIds = new List<int>();
        for (var id = 1; id <= 4; id++)
        {
            if (_controllers[id] is not null)
            {
                connectedIds.Add(id);
                UnplugAt(id);
            }
        }
        if (connectedIds.Count == 0) return;

        _suppressReplugUntilUtc = DateTime.UtcNow.AddMilliseconds(FullReplugGapMs);
        Logger.Log("VirtualGamepad", $"full replug: disconnected [{string.Join(',', connectedIds)}], reconnecting in {FullReplugGapMs}ms");

        _ = Task.Delay(FullReplugGapMs).ContinueWith(_ =>
        {
            foreach (var id in connectedIds)
            {
                try
                {
                    if (_controllers[id] is null) _controllers[id] = PlugIn(id);
                }
                catch (Exception ex)
                {
                    Logger.Log("VirtualGamepad", $"replug of controller {id} failed: {ex}");
                }
            }
        });
    }

    public void Dispose()
    {
        DisconnectAll();
        _client.Dispose();
    }
}
