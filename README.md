# PControl

Native Windows process control, built from an empty repository. Stage 1 provides a C++20 Win32 frontend and an independent background Core. It uses documented Windows APIs and does not replace the Windows scheduler.

## Build and run

Requires Windows x64, Visual Studio 2022 Build Tools with Desktop C++, Windows SDK, and CMake 3.24+. The first configure downloads nlohmann/json 3.11.3 from its upstream release and verifies its SHA-256. No WebView2 or .NET runtime is required. The MSVC C++ runtime is required on machines without Visual Studio.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
& .\scripts\smoke.ps1
& .\build\Release\PControl.exe
```

Keep `PControl.exe` and `PControl.Core.exe` together. The GUI connects to an existing Core or starts it on its background worker. Closing the GUI leaves Core and its rules running. **Connect / start** reconnects manually. Neither executable requests elevation automatically; process operations use the rights of the account running Core. Protected or higher privilege processes may reject operations.

The frontend shows processes, priority, affinity mask, architecture, access errors, and executable paths. Third-party/user processes appear first, followed by Windows processes; each group is ordered case-insensitively by executable name and then PID. Classification primarily uses the actual directory returned by `GetWindowsDirectoryW`, with a small fallback for obvious Windows processes whose path is unavailable. Automatic refresh preserves both the selected instance and the first visible instance by PID plus creation time, falling back to the previous bounded row position if the visible process exits. Select a process to populate the priority and logical CPU controls. **Apply** modifies that exact runtime instance. **Save ... rule** saves an executable-name rule and applies it to currently matching instances; **Remove ...** removes that rule without reverting previous process settings. Type an executable name to manage a rule even when the process is absent. Real Time priority requires an explicit GUI warning confirmation. At least one CPU must be selected.

## Architecture

* `src/gui.cpp`: native controls and a serialized worker for IPC. The GUI never enumerates processes or enforces persistent rules.
* `src/core.cpp`: background lifecycle, per-user/per-session single instance, clean IPC shutdown.
* `src/ipc.*`: local Named Pipe, framing, deadlines, size/depth limits, ACL and session checks.
* `src/process.*`: Tool Help enumeration, process details, priority and affinity actions.
* `src/engine.*`: monitoring thread, snapshot, configuration commands, runtime synchronization.
* `src/rules.*`: exact case-insensitive executable matching and per-instance application tracking.
* `src/config.*`: validated persistent configuration and replacement writes.
* `src/logger.*`: timestamped event log with bounded rotation.
* `src/shared.*`: shared JSON responses, strict conversions, paths, and RAII handles.

Core enumerates using `CreateToolhelp32Snapshot`, `Process32FirstW` and `Process32NextW`, normally every 1500 ms after the previous cycle. The interval is configurable from 250 to 60000 ms. It reads creation time with `GetProcessTimes`, image path with `QueryFullProcessImageNameW`, priority with `GetPriorityClass`, affinity with `GetProcessAffinityMask`, architecture with `IsWow64Process`, and session with `ProcessIdToSessionId`. Each accessible process is queried once per cycle; inaccessible entries remain visible with their error code.

Runtime identity is PID plus creation time, serialized as an unsigned decimal string. Every action opens the target once and verifies its creation time on that same handle before mutation. Unknown identity is never eligible for actions or rules. New/replaced and terminated identities are detected by snapshot comparison. For inaccessible processes whose creation time is unknown, lifecycle classification is approximate; they are never treated as safely identified targets.

Priority changes use `SetPriorityClass`; all six standard classes are supported. Affinity changes use `SetProcessAffinityMask`. Logical CPU selection is represented as `{ "group": 0, "cpus": [0, 1] }`, translated to a native mask only in the Windows layer. Multi-group affinity is explicitly rejected in Stage 1. This avoids applying a single mask to ambiguous group topology; see [Microsoft's processor group documentation](https://learn.microsoft.com/en-us/windows/win32/procthread/processor-groups).

Persistent rules apply immediately when saved, on newly observed matching instances, and again after configuration reload or Core restart. Application is attempted once per identity and rule revision, including failures, to avoid repeated denied operations. Rules do not continually undo later manual changes to an existing instance. Reload retries rules explicitly. Failed actions are logged and do not stop other targets.

## Configuration and logs

Default directory: `%LOCALAPPDATA%\PControl`. Tests override it with `PCONTROL_DATA_DIR`; they do not modify normal user configuration.

`config.json` is UTF-8 JSON, limited to 1 MiB:

```json
{
  "schemaVersion": 1,
  "core": { "updateIntervalMs": 1500 },
  "priorityRules": [
    { "matchType": "executableName", "name": "game.exe", "value": 16384 }
  ],
  "affinityRules": [
    { "matchType": "executableName", "name": "game.exe", "value": { "group": 0, "cpus": [0, 1] } }
  ]
}
```

Each category permits at most 1024 unique executable names. Paths, wildcards and regular expressions are reserved for future stages. Values and integer ranges are validated before writing. Saving writes `config.json.tmp`, checks byte count, calls `FlushFileBuffers`, then `MoveFileExW` with replacement and write-through. A failed load preserves the original file and blocks mutation until the file is repaired and reloaded; failed reload retains the previous in-memory rules. A missing file starts with defaults. Defaults are persisted on the first settings/rule change.

`Core.log` contains local timestamp with milliseconds, severity, category and JSON-escaped message. At 2 MiB it rotates to `Core.log.1`, keeping one backup. Entries cover startup/shutdown, IPC connections/disconnections/errors, malformed requests, config load/error, process arrival/termination, access failure and rule success/failure. Snapshot contents are not logged every cycle. Connections are short-lived, so connection events also occur during GUI refresh. Log write failures fall back to debugger output.

## IPC and security

See [protocol](docs/PROTOCOL.md) for commands, wire format and security boundaries. Same-user applications are inside the trust boundary. This is a desktop backend, not a system service or a privilege broker.

## Validation and limits

`PControl.Tests` covers parsing, configuration protection, runtime identity, direct Windows process actions, dispatch errors and corrupt configuration. `scripts/smoke.ps1` exercises real GUI/Core processes, pipes, rules, restarts, timeouts and handle counts using an isolated test directory and a dedicated test executable. It refuses to run while PControl is already running. Its temporary logs remain available as diagnostic evidence.

Stage 1 intentionally excludes services, drivers, ProBalance, watchdogs, CPU Sets, Efficiency Mode, power plans, automatic elevation and automatic startup at Windows logon. Core must be started again after logout/reboot. No full multi-group affinity support, installer, localization, dark theme or advanced filtering/sorting is included. The UI is a new native Win32 frontend, not a port of an existing application. Processes that start and exit between polling cycles can be missed. Access Denied is expected for some system/protected targets.

Manual validation still needed: visible UI at different DPI scales, keyboard navigation and screen readers, Real Time warning flow (without applying it to a critical process), restricted/PPL/anti-cheat targets, multiple users/sessions, Windows 10 and machines with multiple processor groups. Automated GUI lifecycle checks do not constitute visual acceptance testing.
