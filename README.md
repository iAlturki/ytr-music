# ytr-music

🎵 A blisteringly fast, 100% ad-free desktop music client for Windows built with a pure C++ Win32 engine, 10-band Dolby Equalizer, studio-grade Web Audio smooth transitions, and an interactive glass miniplayer with volume control. One self-contained client, zero clutter, no ads.

[![Download Latest Release](https://img.shields.io/github/v/release/iAlturki/ytr-music?label=Download&style=for-the-badge&color=brightgreen)](https://github.com/iAlturki/ytr-music/releases/latest/download/ytr-music.exe)

<img src="assets/demo.gif" width="760" alt="ytr-music client playing music with smooth audio, zero ads, and responsive controls">

| Clean Ad-Free Player | 10-Band Dolby Equalizer | Desktop Miniplayer | Native Speed |
|:-:|:-:|:-:|:-:|
| <img src="assets/player-clean.png" width="220"> | <img src="assets/equalizer.png" width="220"> | <img src="assets/miniplayer.png" width="150"> | <img src="assets/native-performance.png" width="180"> |

## What's new in 4.1

- **10-Band Dolby Studio Equalizer** – High-precision Web Audio graphic EQ (`32Hz` to `16kHz`) with tuned presets: `Dolby Atmos`, `Dolby Music`, `Dolby Movie`, `Bass Boost`, `Vocal`, `Rock`, `Gaming`, and `Flat`. Accessible right from the TopBar next to Smooth Audio.
- **Pure Transparent 64-Bit Audio DSP** – Zero artificial compression, zero makeup gain, zero volume tampering. Clean, bit-for-bit bypass when EQ is off or flat.
- **Hardware-Ramped Smooth Audio Fades** – Dedicated `fadeGainNode` eliminates glitchy volume jumps, audio clipping, and slider desync during play, pause, and track skips.
- **Interactive Miniplayer Volume Control** – Full volume slider track with vector speaker mute-toggle, drag-to-set volume, live percentage readout, precision 5% mouse-wheel steps, and seek scrubber.
- **100% Standalone Portable Executable** – Statically linked native Win32 binary with embedded runtime loader. Zero external MinGW runtime DLLs required.

## Features

- **Pure Native Speed** – Built with modern C++17 and the lightweight Windows WebView2 Evergreen runtime. Instant startup with background RAM working set compaction (<90MB).
- **100% Ad-Free Audio Engine** – In-player JSON payload pruner and network domain blocker. Zero audio ads, zero video ads, zero interruptions.
- **Dolby Audio Equalizer** – 10 adjustable frequency bands (`32Hz` to `16kHz`, -12dB to +12dB) with live dB readouts, instant preset switching, and master toggle.
- **Studio-Grade Audio Transitions** – Web Audio hardware gain ramps on pause, resume, and track skipping for seamless, pop-free listening.
- **Glass Acrylic Miniplayer** – Right-docked widget floating above the taskbar with album art, animated EQ bars, interactive seek scrubber, speaker mute toggle, volume slider, and smooth wheel adjustments.
- **Global Hotkeys & Media Keys**
  - `Ctrl + Alt + M` – Toggle Desktop Glass Miniplayer
  - `Ctrl + Alt + Space` / `Media Play/Pause` – Play / Pause
  - `Ctrl + Alt + Right` / `Media Next` – Next Track
  - `Ctrl + Alt + Left` / `Media Prev` – Previous Track
  - `Mouse Wheel` over Miniplayer – Adjust Volume (5% clean steps)
  - `Click or Drag Slider` on Miniplayer – Instant volume / seek positioning
- **System Tray Integration** – Seamless minimize-to-tray with live song info tooltips and instant wake.

## Quick Start

### Portable Run (No installation needed)
1. Download **[`ytr-music.exe`](https://github.com/iAlturki/ytr-music/releases/latest/download/ytr-music.exe)** from the latest release.
2. Run `ytr-music.exe` directly, or double-click **`Run-Native.bat`**.

### Building from Source
```powershell
# Clone the repository
git clone https://github.com/iAlturki/ytr-music.git
cd ytr-music

# Compile the native C++ client with MinGW-W64
cmd.exe /c native\build.bat
```

## Author & License

- **Creator & Sole Rights Holder**: **[iALTURKi](https://github.com/iALTURKi)**
- **Repository**: [github.com/iAlturki/ytr-music](https://github.com/iAlturki/ytr-music)
- **License**: MIT License (see [license](license)). All rights reserved.
