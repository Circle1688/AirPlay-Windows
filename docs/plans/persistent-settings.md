# External FastXR settings

The external FastXR application owns `%LOCALAPPDATA%\FastXR\settings.ini`.
The receiver displays a full-width video stage with a toolbar and status bar;
there are no device or settings side panels.

## Lifecycle

1. Load the new file. If it does not exist, load the old
   `%APPDATA%\AirPlay-Windows\settings.ini` as a migration source.
2. If resolution is absent, detect it with `SDL_GetCurrentDisplayMode()`,
   falling back to 2560x1440.
3. Initialize the new file using exclusive creation, preserving an existing
   file even if the external application created it during startup.
4. Apply CLI overrides to the initial runtime values.
5. Poll the file from the main thread every 200 ms. Require the same contents
   on two consecutive polls before publishing a validated update.
6. Never save the runtime settings over the external file on shutdown.

`SettingsFile` owns the last accepted values and file contents. It compares
contents rather than timestamps to detect replacement even when timestamps
are preserved. Empty, locked, missing and invalid files keep the last accepted
values. Parsing updates a candidate snapshot first so an invalid known value
cannot publish only part of an update. Missing keys retain their previous
values; unknown keys are ignored. UTF-8 BOM, CRLF, surrounding whitespace and
whole-line `#` / `;` comments are supported.

External writers should write a complete temporary file beside `settings.ini`
and atomically replace the destination. Waiting for two equal snapshots reduces
ordinary truncate/write glitches but cannot distinguish a valid partial file
from an intentional update if a writer pauses for longer than the debounce.

## Fields

| Key | Range | Application |
|---|---|---|
| `mirror_width` | 320–7680 | Next AirPlay connection |
| `mirror_height` | 240–4320 | Next AirPlay connection |
| `hevc_enabled` | 0 or 1 | Next AirPlay connection |
| `max_fps` | 1–60 | Next AirPlay connection |
| `refresh_rate` | 1–60 | Next AirPlay connection |
| `mirror_hwaccel` | 0 or 1 | Next AirPlay stream setup |
| `vsync_enabled` | 0 or 1 | Live on the renderer thread |
| `fullscreen` | 0 or 1 | Live on the renderer thread |
| `chromeless` | 0 or 1 | Live on the renderer thread |

The renderer consumes atomic `LiveSettings` values and performs SDL changes
on its own thread. Window hotkeys update those atomics for the current session
without writing the file. An unchanged file does not undo a hotkey; the next
accepted external update becomes authoritative.

The default location uses `FOLDERID_LocalAppData` for the current account.
Default persistence remains Windows-only. Receiver identity remains in the
working directory as `identity.key`.

## Validation

`ctest --test-dir build -C Release --output-on-failure` runs the settings tests.
They cover initial creation, non-overwriting behavior, UTF-8/CRLF parsing,
all runtime fields, malformed and incomplete updates, debounce, locked files,
atomic replacement with preserved timestamps, deletion/recreation and unchanged
files preserving window hotkeys. The protocol checks in
`tools/test_pair_verify.py` validate the AirPlay handshake separately.
