# Changelog

All notable changes to ytr-music are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/).

## [4.2.1] - 2026-10-01

### Fixed
- The Smooth Audio button showed OFF at startup even when Smooth Audio was on, so the first click looked like it did nothing. The EQ button label had the same startup issue.

## [4.2.0] - 2026-10-01

### Performance
- The WebView is marked hidden when the window is in the tray or minimized, so the page stops rendering while audio keeps playing (hidden + playing: 24.0 % → 6.8 % of a core).
- WebView2 low-memory mode when the window is hidden and playback is paused (after a short debounce); normal mode as soon as the window is shown or playback starts.
- Removed the periodic `EmptyWorkingSet` timer, which only caused page-fault churn.
- Page script is event driven: no polling intervals; ads are detected from player class changes, state is sent only on change and once per second while playing.
- Web Audio graph is created on first play with `latencyHint: 'playback'`, suspends ~3 s after pause, and bypasses the EQ filters when the EQ is off or flat (hidden + paused: 10.3 % → 0.8 % of a core).
- Miniplayer runs no timers while hidden, animates only during transitions, caches GDI+ resources, interpolates progress locally and fetches artwork off the UI thread.
- Tray tooltip and taskbar buttons are only updated when their content changes.
- Release build uses `-O2`, LTO and section garbage collection; the page script is embedded as UTF-8; unused libraries dropped. The exe went from 1.56 MB to 0.68 MB.

### Security
- The DevTools remote-debugging port is only opened with `--debug` (it used to be open in every build with any origin allowed).
- `WebView2Loader.dll` is loaded by full path only, and the extracted copy is refreshed when it differs from the embedded one.
- Pop-up windows open in the default browser (http/https only).

### Fixed
- Tray icon is restored after Explorer restarts; taskbar thumbnail buttons are re-added.
- WebView2 renderer or browser crashes are recovered automatically.
- Ad skip button could never be clicked; seek feedback loop during ads; CSS rules that could hide menus, the sidebar section or playlists named "upgrade".
- Play/pause pressed during a fade is no longer lost; holding a media key no longer toggles repeatedly.
- Page messages with escaped characters no longer truncate titles and artists.
- Miniplayer volume hit zone matches the drawn slider; seek is applied on release; DPI scaling.
- COM callback handlers are released; shutdown runs before COM is uninitialized.
- A ReferenceError in the audio unlock handler that fired on every click and key press.

### Changed
- Equalizer presets renamed to `Spatial`, `Studio` and `Cinema`; saved selections migrate automatically.
- The "Trim Memory" menu entries were removed (memory is managed automatically).
- The retired Electron client was removed from the repository (last revision tagged `electron-final`).
- `build.bat` takes `release` (default) or `debug`; the version is defined once in `native/res/version.h`.

## [4.1.0] - 2026-09-23

### Added
- 10-band Web Audio equalizer with presets, next to Smooth Audio in the top bar.
- Web Audio gain-ramped fades on pause, resume and track changes.
- Miniplayer volume slider, mute toggle, wheel volume and seek scrubber.
- WebView2 loader embedded in the exe for a single-file portable build.

## [4.0.0] - 2026-09-23

### Added
- Native C++ Win32 + WebView2 client with ad blocking, desktop miniplayer, tray and taskbar controls, and global media hotkeys.

[4.2.1]: https://github.com/iAlturki/ytr-music/compare/v4.2.0...v4.2.1
[4.2.0]: https://github.com/iAlturki/ytr-music/compare/v4.1.0...v4.2.0
[4.1.0]: https://github.com/iAlturki/ytr-music/compare/v4.0.0...v4.1.0
[4.0.0]: https://github.com/iAlturki/ytr-music/releases/tag/v4.0.0
