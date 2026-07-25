using System.Buffers.Binary;

namespace OrbitNX.Input;

/// <summary>
/// One decoded controller's state from the OrbitNX HID packet.
/// </summary>
public readonly struct ControllerState
{
    public required ushort HeldKeys { get; init; }
    public required int LJoyX { get; init; }
    public required int LJoyY { get; init; }
    public required int RJoyX { get; init; }
    public required int RJoyY { get; init; }

    public bool A => IsSet(0);
    public bool B => IsSet(1);
    public bool X => IsSet(2);
    public bool Y => IsSet(3);
    public bool LS => IsSet(4);
    public bool RS => IsSet(5);
    public bool L => IsSet(6);
    public bool R => IsSet(7);
    public bool ZL => IsSet(8);
    public bool ZR => IsSet(9);
    public bool Plus => IsSet(10);
    public bool Minus => IsSet(11);
    public bool Left => IsSet(12);
    public bool Up => IsSet(13);
    public bool Right => IsSet(14);
    public bool Down => IsSet(15);

    private bool IsSet(int bit) => ((HeldKeys >> bit) & 1) != 0;
}

/// <summary>Which app the Switch client wants the PC host to launch/focus, chosen once via the
/// on-screen menu (menu.c) before the input thread starts and sent unchanged on every JoyPkg for
/// the rest of the process's life. Wire value carried in JoyPkg.appTarget (see network.h on the
/// client). Declared as a ulong-backed enum to mirror the wire field's uint64_t type exactly - see
/// network.h's comment on why that field is a uint64_t rather than uint32_t (alignment/padding).</summary>
public enum AppTarget : ulong
{
    BigPicture = 0,
    ShadPs4 = 1,
}

public sealed class HidPacket
{
    public const int PayloadSize = 136;

    public required ControllerState[] Controllers { get; init; }
    public required uint TouchX1 { get; init; }
    public required uint TouchY1 { get; init; }
    public required uint ControllerCount { get; init; }
    public required AppTarget AppTarget { get; init; }

    /// <summary>Real angular velocity (rad/s, per the client's libnx HidSixAxisSensorState -
    /// NOT an accumulated angle), from whichever single physical controller/handheld the client is
    /// currently reading motion from - there is only one motion sensor's worth of data on the wire
    /// regardless of ControllerCount, so this applies to controller index 0 only (see
    /// VirtualGamepadHub's DualShock4 path, the only consumer of this today).</summary>
    public required float GyroX { get; init; }
    public required float GyroY { get; init; }
    public required float GyroZ { get; init; }

    /// <summary>Real acceleration (g), same single-sensor caveat as GyroX/Y/Z above.</summary>
    public required float AccelX { get; init; }
    public required float AccelY { get; init; }
    public required float AccelZ { get; init; }

    /// <summary>
    /// Mirrors main.js parseInputStruct: 4x (HeldKeys u32, LJoyX i32, LJoyY i32, RJoyX i32, RJoyY i32),
    /// touchX1,Y1,X2,Y2 (u32), accelX,Y,Z (f32), gyroX,Y,Z (f32), frameRate (u32), controllerCount (u32),
    /// appTarget (u64).
    /// </summary>
    public static HidPacket Parse(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != PayloadSize)
            throw new ArgumentException($"Expected {PayloadSize} bytes, got {payload.Length}");

        var controllers = new ControllerState[4];
        var offset = 0;
        for (var i = 0; i < 4; i++)
        {
            var heldKeys = BinaryPrimitives.ReadUInt32LittleEndian(payload[offset..]);
            var lJoyX = BinaryPrimitives.ReadInt32LittleEndian(payload[(offset + 4)..]);
            var lJoyY = BinaryPrimitives.ReadInt32LittleEndian(payload[(offset + 8)..]);
            var rJoyX = BinaryPrimitives.ReadInt32LittleEndian(payload[(offset + 12)..]);
            var rJoyY = BinaryPrimitives.ReadInt32LittleEndian(payload[(offset + 16)..]);
            controllers[i] = new ControllerState
            {
                HeldKeys = (ushort)heldKeys,
                LJoyX = lJoyX,
                LJoyY = lJoyY,
                RJoyX = rJoyX,
                RJoyY = rJoyY,
            };
            offset += 20;
        }

        var touchX1 = BinaryPrimitives.ReadUInt32LittleEndian(payload[offset..]);
        var touchY1 = BinaryPrimitives.ReadUInt32LittleEndian(payload[(offset + 4)..]);
        // TouchX2/Y2 (bytes offset+8..offset+15) are on the wire but unused - see TouchInjector.
        offset += 16;

        // AccelX/Y/Z, GyroX/Y/Z (24 bytes, wire order matches JoyPkg in network.h exactly).
        var accelX = BinaryPrimitives.ReadSingleLittleEndian(payload[offset..]);
        var accelY = BinaryPrimitives.ReadSingleLittleEndian(payload[(offset + 4)..]);
        var accelZ = BinaryPrimitives.ReadSingleLittleEndian(payload[(offset + 8)..]);
        var gyroX = BinaryPrimitives.ReadSingleLittleEndian(payload[(offset + 12)..]);
        var gyroY = BinaryPrimitives.ReadSingleLittleEndian(payload[(offset + 16)..]);
        var gyroZ = BinaryPrimitives.ReadSingleLittleEndian(payload[(offset + 20)..]);
        offset += 24;

        // FrameRate is on the wire but unused.
        var controllerCount = BinaryPrimitives.ReadUInt32LittleEndian(payload[(offset + 4)..]);
        offset += 8;

        var appTargetRaw = BinaryPrimitives.ReadUInt64LittleEndian(payload[offset..]);
        // Defensive default: anything other than a known value (e.g. a mismatched/older client
        // build) falls back to Big Picture rather than an undefined enum value propagating further.
        var appTarget = appTargetRaw is (ulong)AppTarget.BigPicture or (ulong)AppTarget.ShadPs4
            ? (AppTarget)appTargetRaw
            : AppTarget.BigPicture;

        return new HidPacket
        {
            Controllers = controllers,
            TouchX1 = touchX1,
            TouchY1 = touchY1,
            ControllerCount = controllerCount,
            AppTarget = appTarget,
            AccelX = accelX,
            AccelY = accelY,
            AccelZ = accelZ,
            GyroX = gyroX,
            GyroY = gyroY,
            GyroZ = gyroZ,
        };
    }
}

/// <summary>
/// Extracts framed 128-byte HID payloads out of a raw byte stream.
/// The client wraps each frame with an 8x0xFF start marker followed by the fixed-size payload;
/// the original main.js end-marker is a substring match on a hex string, not byte-aligned, so instead
/// of reproducing that exact (ambiguous) pattern we rely on the unambiguous start marker + known
/// payload length and resync on the next start marker for whatever trailing bytes follow.
/// NOTE: verify this framing against a live capture from the real client before relying on it.
/// </summary>
public sealed class HidFrameReader
{
    private static readonly byte[] StartMarker = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    private readonly List<byte> _buffer = new();

    // Takes a plain array/length rather than Span<byte>: iterator methods (this uses yield)
    // cannot declare or use ref struct types like Span<T> anywhere in their body.
    public IEnumerable<HidPacket> Feed(byte[] chunk, int length)
    {
        for (var i = 0; i < length; i++) _buffer.Add(chunk[i]);

        while (true)
        {
            var startIdx = IndexOf(_buffer, StartMarker, 0);
            if (startIdx < 0)
            {
                // Keep only a start-marker's worth of tail in case it's split across chunks.
                if (_buffer.Count > StartMarker.Length)
                    _buffer.RemoveRange(0, _buffer.Count - StartMarker.Length);
                yield break;
            }

            var payloadStart = startIdx + StartMarker.Length;
            if (_buffer.Count < payloadStart + HidPacket.PayloadSize)
            {
                _buffer.RemoveRange(0, startIdx);
                yield break;
            }

            var payload = _buffer.GetRange(payloadStart, HidPacket.PayloadSize).ToArray();
            yield return HidPacket.Parse(payload);

            _buffer.RemoveRange(0, payloadStart + HidPacket.PayloadSize);
        }
    }

    private static int IndexOf(List<byte> haystack, byte[] needle, int from)
    {
        for (var i = from; i <= haystack.Count - needle.Length; i++)
        {
            var match = true;
            for (var j = 0; j < needle.Length; j++)
            {
                if (haystack[i + j] != needle[j]) { match = false; break; }
            }
            if (match) return i;
        }
        return -1;
    }
}
