# Native interop conventions

## CsWin32 vs manual `[DllImport]`

Two coexisting styles in this codebase — both are correct, use the right one for
the situation:

- **CsWin32-generated** (`NativeMethods.txt` at the project root drives the
  `Microsoft.Windows.CsWin32` source generator): ~60 APIs covering audio
  loopback/WASAPI, DXGI/D3D11 screen capture, and window enumeration. Consumed as
  generated `Windows.Win32.PInvoke.*` partial methods. **If an API is already
  listed in `NativeMethods.txt`, use the generated method — do not write a manual
  `[DllImport]` that duplicates it.**
- **Hand-written `[DllImport]` static extern**, grouped at the bottom of the
  owning class, used only for APIs CsWin32 doesn't cover or where the injection
  primitives are needed directly (e.g. `Program.cs` for
  `FindWindow`/`ShowWindow`/`SetForegroundWindow`; `Native/DllInjector.cs` for the
  full `CreateRemoteThread` injection chain).

Before adding a new P/Invoke signature: check `NativeMethods.txt` first. Adding
to it is the preferred path if CsWin32 can generate the binding; only fall back
to a manual `[DllImport]` if it can't.

## Handle lifetime

No `SafeHandle` wrappers exist anywhere in this project — handles are raw
`IntPtr`, closed manually in `finally` blocks. This is consistent, deliberate
practice throughout the codebase, not an oversight. Don't introduce a
`SafeHandle` wrapper in one isolated spot; it would be inconsistent with every
other handle in the project. If handle management needs to change project-wide,
that's a larger, explicit refactor, not something to slip into an unrelated
change.

## DLL injection as a deliberate pattern, not a last resort

`Native/DllInjector.cs` implements classic `OpenProcess` →`VirtualAllocEx` →
`WriteProcessMemory` → `CreateRemoteThread` → `LoadLibraryW` injection, used to
load `focushook.dll` into Steam's processes (see
`references/steam-bigpicture.md` for the full reasoning). This is not a
workaround to be "cleaned up" — less invasive approaches (window messages) were
tried first and confirmed not to work for this specific problem (spoofing a
foreground-focus check that polls `GetForegroundWindow()` directly). Treat this
file as the reference implementation if a similar injection need arises
elsewhere, rather than re-deriving the injection sequence from scratch.

## `unsafe` / `AllowUnsafeBlocks`

The project has `AllowUnsafeBlocks=true` set project-wide. When writing unsafe
code (pointer arithmetic, fixed buffers), document memory ownership and lifetime
explicitly in a comment — who allocates, who frees, how long the pointer stays
valid — consistent with this project's "engineering diary" comment style
elsewhere. Don't add `unsafe` code without a clear reason it's needed over a safe
alternative.
