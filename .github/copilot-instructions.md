# Instructions for GitHub Copilot

## Building This Project

This is a JUCE-based audio plugin project that requires specific dependencies on Linux.

### Before Building on Linux

1. **Install dependencies first**: Run `sudo bash scripts/install-linux-deps.sh`
2. **Dependencies are fetched by CMake**: JUCE and phu-audio-lib are downloaded into `JUCE/` and `phu-audio-lib/` during configure (see `cmake/PhuDependencies.cmake`); no submodule init is needed
3. **Use the Linux preset**: Build with `cmake --preset linux-x64-vst3-release && cmake --build --preset linux-x64-vst3-release-build`

### Build Timeouts

If builds timeout:
- Use fewer parallel jobs: `cmake --build --preset linux-x64-vst3-release-build -j2`
- Ensure all dependencies are installed before attempting to build
- The JUCE and phu-audio-lib fetch must complete (network access to GitHub is required on first configure)

### Tests

Project unit tests live in `tests/` (`test_display_range.cpp` tests the JUCE-free `src/DisplayRangeLimits.h`). Build the `test_display_range` target and run `ctest --test-dir build/<preset> -C Release --output-on-failure`. Tests for the shared `PpqAddressedRingBuffer`/`BucketSet` and other phu-audio-lib headers live in the phu-audio-lib repo (https://github.com/huberp/phu-audio-lib, `tests/` folder); changes to those classes must be tested there.

### CI and releases

`build.yml` (push/PR) and `release.yml` call per-platform reusable workflows (`build-windows-vst3.yml`, `build-linux.yml`, `build-macos.yml`) that build via the CMake presets; `release.yml` also calls `generate-pdf.yml`. A release is triggered by pushing a tag `v[0-9]*` or by running `release.yml` manually with the required `tag` input (the tag is then created on the selected commit). Details in CONTRIBUTING.md.

### Documentation

- User manual: [README.md](./README.md) — keep it in sync with user-visible behaviour (it is shipped as `README.pdf` in every release)
- Build instructions, project layout, architecture, network protocol, CI: [CONTRIBUTING.md](./CONTRIBUTING.md)

---

## Architecture — Beat-Synced Multi Oscilloscope

This plugin is a beat-synced multi-instance oscilloscope. It displays waveforms aligned to musical position (PPQ). Up to 8 plugin instances on the same machine share their waveforms over UDP multicast. The audio passes through unchanged. See [CONTRIBUTING.md](./CONTRIBUTING.md#architecture) for the full description.

### Core Components

Shared building blocks come from `phu-audio-lib/` (fetched by CMake, git-ignored — change them in the phu-audio-lib repository, not here):

1. **PpqAddressedRingBuffer** (`phu-audio-lib/audio/PpqAddressedRingBuffer.h`): Position-addressed overwrite ring buffer. A sample at absolute PPQ `p` is written to index `fmod(p, range) / range × N`.
2. **BucketSet** (`phu-audio-lib/audio/BucketSet.h`): Dirty-tracked bucket partitioning of a buffer. Used for the RMS (1/16 beat) and cancellation (~4 ms, max 256) overlays.
3. **LinkwitzRileyFilter** (`phu-audio-lib/audio/LinkwitzRileyFilter.h`): The 48 dB/oct HP/LP display filters.
4. **SyncGlobals** (`phu-audio-lib/events/SyncGlobals.h`): DAW state (BPM, PPQ, transport, sample rate). Audio-thread writes, UI-thread reads via atomic PPQ.
5. **MulticastBroadcasterBase / SampleBroadcaster / CtrlBroadcaster** (`phu-audio-lib/network/`): UDP multicast sockets and the two protocols (samples on port 49422, control on port 49423).
6. **GLRendererBase / GLSnapshotRenderer / GLSLShaderBuilder** (`phu-audio-lib/gl/`): OpenGL helpers.

Project code in `src/`:

7. **PluginProcessor** (`src/PluginProcessor.h/.cpp`): `processBlock`, ping-pong broadcast slots, SPSC local ring, APVTS parameters, state, and a 30 Hz timer for CTRL heartbeats and peer commands.
8. **PluginEditor** (`src/PluginEditor.h/.cpp`): Fixed-size UI and the 60 Hz timer that feeds the display.
9. **ScopeDisplay** (`src/ScopeDisplay.h/.cpp`): Up to 8 instance slots (slot = channel number − 1), filtering, overlays, software fallback painting.
10. **ScopeGLCoordinator and sub-renderers** (`src/gl/`): OpenGL drawing of waveforms, grid, playhead, RMS and cancellation bar.

### Data Flow

```
Audio Thread (processBlock)
  ├─ Update DAW globals (BPM, PPQ, transport)
  ├─ Mix to mono; push (sample, absolute PPQ) pairs to the local SPSC ring
  └─ If broadcasting: fill the ping-pong slot; when full (~33 ms)
       → SampleBroadcaster::broadcastRawSamples() (loopback sendto)

Processor Timer (30 Hz, also without editor)
  └─ CTRL heartbeat, peer commands, channel index sync

UI Timer (60 Hz)
  ├─ Drain local ring → HP/LP filter → PpqAddressedRingBuffer[local]
  ├─ SampleBroadcaster::getReceivedPackets() → HP/LP filter → PpqAddressedRingBuffer[channel-1]
  ├─ ScopeDisplay::computeFrame() (dirty RMS / cancellation buckets, display bins)
  └─ Hand off to ScopeGLCoordinator and repaint
```

### Network Protocol

- **Multicast Group**: 239.255.42.1, TTL 1 — instances on the same machine only
- **Sample port**: 49422 (`RawSamplesPacket`: raw mono floats + PPQ of first sample + BPM + sequence number)
- **Control port**: 49423 (`CtrlPacket`: channel index, label, colour, sample rate; `Announce`, `LabelChange`, `RangeChange`, `Goodbye`, `PeersBroadcastOnly`)
- **Rate**: one sample packet per ~33 ms; CTRL heartbeat adaptive 5–12 s
- **Staleness**: sample data silent > 3 s is pruned; CTRL info > 30 s (or on `Goodbye`)
- **Not used**: port 49421 belongs to the spectrum broadcaster of phu-splitter

### Hard Rules

- Follow `.github/instructions/audio-thread-safety.instructions.md`: no allocation, locks, file/console I/O or exceptions on the audio thread.
- **The only network call on the audio thread is `SampleBroadcaster::broadcastRawSamples()`** (lock-free loopback `sendto()` from pre-allocated slots). Everything else — socket setup, receiving, CTRL packets — runs on the message thread or the receiver threads. Never add a receive call or another send path to the audio thread.
- **All PPQ calculations must use the DAW-provided position** from `getPlayHead()->getPosition()`.
- **MUST work without assumptions about BPM and sample rate**: 44.1 kHz up to 192 kHz, tempos from 40 BPM upwards.
- **Channel numbers are slot indices**: instances talking to each other need unique channels; a remote packet claiming the local channel is ignored.
- **Display filters affect only the display path**, never the audio.
