# Architecture conventions

## Composition root: `AppOrchestrator`

`Program.cs` is a thin bootstrap only: single-instance mutex, tray icon setup,
`Application.Run`. All real wiring happens in `AppOrchestrator.cs`, which
constructs and owns the service instances (`VirtualGamepadHub`, `VideoEncoder`,
`AudioCaptureSender`, `SessionController`, `SteamLauncher`, etc.) and connects
them to each other.

`MainForm`/`SettingsForm` are UI-only — they hold a reference to the
orchestrator and call into it, but do not implement business logic themselves.

**To add new functionality**: create a new service class, construct and wire it
in `AppOrchestrator`, and have any Form call into it through the orchestrator.
Do not add logic directly to a Form's event handlers beyond translating a UI
action into a call on the orchestrator/service.

## `IDisposable` / teardown pattern

Services that own unmanaged or long-lived resources are `sealed class ... :
IDisposable`. `Dispose()` does **best-effort** teardown:

```csharp
public void Dispose()
{
    try
    {
        DisconnectAll();
        _client.Dispose();
    }
    catch
    {
        // best-effort teardown; failures here are logged, not fatal — the
        // process is shutting down regardless.
    }
}
```

- Never throw from `Dispose()`.
- Log failures via `Logger.Log`, don't rethrow.
- `Dispose()` should be safe to call even if the object was never fully
  initialized (e.g. construction failed partway through).

Example from `Input/TouchInjector.cs`: closes stdin, then `WaitForExit(500)`
before disposing the underlying `Process` — a bounded wait, not an indefinite
block, since this runs during shutdown where hanging is worse than a slightly
unclean exit.

## Logging

`Logger` is a minimal `internal static class`:

```csharp
Logger.Log("ComponentName", $"description of what happened, {someValue}");
```

- Writes timestamped lines to `OrbitNX.log` in `AppContext.BaseDirectory`.
- Truncated once per process start.
- All file I/O is wrapped in a swallowing `try/catch` — logging must never be a
  source of crashes; treat it as strictly best-effort.
- Never use `Console.WriteLine` or `Debug.WriteLine` in this codebase — always
  `Logger.Log`.

## Fire-and-forget

The established pattern for fire-and-forget work in this codebase is:

```csharp
_ = Task.Delay(delay).ContinueWith(_ => DoSomething());
```

rather than an `async void` method or an unobserved `async Task`. `async`/
`await` keywords appear relatively rarely in this codebase — this is a
deliberate stylistic choice, not an oversight. When adding new fire-and-forget
work, match this existing pattern rather than introducing `async void` (which
can't have its exceptions observed/caught) or a bare unawaited `async Task` call
(which silently drops exceptions differently than the `ContinueWith` form does).

## Comment style — "engineering diary"

XML `<summary>` comments and inline comments in this codebase are frequently
long-form, explaining *why* rather than *what*: what was tried before, what
specific error or symptom was observed, what the root cause turned out to be.
Examples appear throughout `Capture/VideoEncoder.cs`, `BigPicture/SteamLauncher.cs`,
and `Native/DllInjector.cs`.

**Preserve this style.** When refactoring or moving code, keep these
explanatory comments intact rather than trimming them to a one-line description
of current behavior — the historical context (what was rejected and why) is
exactly what prevents someone from re-trying a dead end later.

## Language conventions

- Russian strings for user-facing UI text (e.g. tray menu items like
  "Открыть"/"Выход").
- English for all code, identifiers, and comments.
- Keep this split consistent — don't localize comments, don't leave UI strings
  in English.
