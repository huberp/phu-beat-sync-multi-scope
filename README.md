# PHU Beat Sync Multi Scope

A JUCE-based VST3 audio plugin that provides a beat-synced multi-instance oscilloscope. Multiple plugin instances running in the same DAW project (or on the same network) can share their waveform data in real time, allowing you to compare signals across different tracks aligned to musical time.

## Features

- **Beat-synced waveform display**: Audio samples are mapped to musical position (PPQ), so the oscilloscope trace aligns with beats and bars
- **Multi-instance networking**: Waveform data is shared between plugin instances via UDP multicast on the local network
- **Remote data toggle**: Show or hide remote instance waveforms with a single click (following phu-splitter UI pattern)
- **Broadcast toggle**: Enable or disable sending your waveform data to other instances
- **Configurable display range**: Choose from 1/4 beat to 8 beats display window
- **Playhead marker**: Shows current PPQ position on the oscilloscope
- **dB-scale display**: Waveform amplitude shown in decibels with grid lines

## Architecture

This plugin combines patterns from two reference projects:

- **[phu-compressor](https://github.com/huberp/phu-compressor)**: Beat-synced audio buffer (`BeatSyncBuffer`), audio sample FIFO (`AudioSampleFifo`), DAW sync globals (`SyncGlobals`)
- **[phu-splitter](https://github.com/huberp/phu-splitter)** (branch `feature/spectrum-multicast-broadcast`): Multicast networking (`MulticastBroadcasterBase`), remote data display toggle pattern

The `SampleBroadcaster` extends the `SpectrumBroadcaster` pattern from phu-splitter, adapted to send beat-synced waveform data (dB-quantized `BeatSyncBuffer` contents) instead of FFT spectrum data. Each network packet includes a PPQ position reference for beat-sync alignment.

## Building

### Prerequisites

- CMake 3.15+
- C++17 compiler
- JUCE framework (included as git submodule)

### Linux

```bash
git submodule update --init --recursive
sudo bash scripts/install-linux-deps.sh
cmake --preset linux-release
cmake --build --preset linux-build
```

### Windows

```bash
git submodule update --init --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Project Structure

```
phu-beat-sync-multi-scope/
├── CMakeLists.txt              # Root CMake config
├── CMakePresets.json           # Build presets (VS2026, Linux)
├── JUCE/                       # JUCE framework (submodule)
├── src/                        # Plugin implementation
│   ├── PluginProcessor.h/cpp   # Audio processing, beat-sync, broadcast
│   ├── PluginEditor.h/cpp      # UI layout, controls, timer
│   ├── ScopeDisplay.h/cpp      # Beat-synced oscilloscope display
│   └── CMakeLists.txt
├── lib/                        # Reusable library components
│   ├── audio/
│   │   ├── AudioSampleFifo.h   # Lock-free sample FIFO
│   │   └── BeatSyncBuffer.h    # Position-indexed display buffer
│   ├── events/
│   │   ├── Event.h             # Base event struct
│   │   ├── EventSource.h       # Template listener pattern
│   │   ├── SyncGlobals.h       # BPM/PPQ/transport state
│   │   └── SyncGlobalsListener.h
│   ├── network/
│   │   ├── MulticastBroadcasterBase.h/cpp  # UDP multicast infrastructure
│   │   └── SampleBroadcaster.h/cpp         # Beat-synced waveform multicast
│   ├── debug/
│   │   └── EditorLogger.h/cpp  # Debug log viewer
│   └── CMakeLists.txt
├── .github/
│   ├── copilot-instructions.md
│   └── workflows/
│       ├── build.yml           # CI: Windows + Linux builds + VST3 validation
│       └── release.yml         # Release: cross-platform packaging
└── scripts/
    └── install-linux-deps.sh   # Linux dependency installer
```

## License

See [LICENSE](LICENSE) for details.
