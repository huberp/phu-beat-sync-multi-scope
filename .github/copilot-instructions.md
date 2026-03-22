# Instructions for GitHub Copilot

## Building This Project

This is a JUCE-based audio plugin project that requires specific dependencies on Linux.

### Before Building on Linux

1. **Install dependencies first**: Run `sudo bash scripts/install-linux-deps.sh`
2. **Initialize submodules**: Ensure JUCE submodule is initialized with `git submodule update --init --recursive`
3. **Use the Linux preset**: Build with `cmake --preset linux-release && cmake --build --preset linux-build`

### Build Timeouts

If builds timeout:
- Use fewer parallel jobs: `cmake --build --preset linux-build -j2`
- Ensure all dependencies are installed before attempting to build
- The JUCE submodule must be fully initialized

### Documentation

- Main README: [README.md](./README.md)

---

## Architecture — Beat-Synced Multi Oscilloscope

This plugin is a beat-synced multi-instance oscilloscope that displays waveform data aligned to musical position (PPQ). Multiple plugin instances can share their waveform data over the local network.

### Core Components

1. **BeatSyncBuffer** (`lib/audio/BeatSyncBuffer.h`): Position-indexed display buffer that maps audio samples to musical time positions [0, 1). Each bin corresponds to a fractional beat position. Audio-thread writes, UI-thread reads.

2. **AudioSampleFifo** (`lib/audio/AudioSampleFifo.h`): Lock-free FIFO for transferring audio samples from the audio thread to the UI thread. Uses JUCE AbstractFifo for single-writer/single-reader indexing.

3. **SyncGlobals** (`lib/events/SyncGlobals.h`): DAW synchronization state tracking (BPM, PPQ, transport). Audio-thread updates, UI-thread reads via atomic PPQ.

4. **SampleBroadcaster** (`lib/network/SampleBroadcaster.h/.cpp`): UDP multicast broadcaster/receiver for sharing beat-synced waveform data between plugin instances. Extends the MulticastBroadcasterBase pattern from phu-splitter. Packets include PPQ reference for beat-sync alignment.

5. **MulticastBroadcasterBase** (`lib/network/MulticastBroadcasterBase.h/.cpp`): Abstract base class providing UDP multicast infrastructure (sockets, receiver thread, platform abstraction).

6. **ScopeDisplay** (`src/ScopeDisplay.h/.cpp`): Beat-synced oscilloscope display component. Renders local and remote waveform data with dB-scale Y axis and beat-position X axis.

### Data Flow

```
Audio Thread (processBlock)
  ├─ Update DAW globals (BPM, PPQ, transport)
  ├─ Push samples to input FIFO (for UI)
  └─ Write BeatSyncBuffer (PPQ → normalized position → bin)

Processor Timer (~10 Hz)
  └─ If broadcast enabled: send BeatSyncBuffer data via SampleBroadcaster

UI Timer (60 Hz)
  ├─ Read local BeatSyncBuffer → ScopeDisplay
  ├─ Read remote data from SampleBroadcaster → ScopeDisplay (if toggle on)
  └─ Repaint oscilloscope
```

### Network Protocol

- **Multicast Group**: 239.255.42.1 (administratively scoped, local network)
- **Port**: 49423 (separate from phu-splitter spectrum port 49421)
- **Packet**: SamplePacket with PPQ position, BPM, display range, and dB-quantized waveform data
- **Rate**: ~30 Hz (33ms throttle)
- **Staleness**: Remote instances silent > 3s are automatically pruned

### Hard Rules

- **NEVER perform network I/O on the audio thread.** Broadcasting is driven by a timer.
- **Network send/receive happens on the UI thread** (timer callbacks and receiver thread reads are consumed on UI thread via getReceivedSamples()).
- **BeatSyncBuffer writes are audio-thread safe** (single float stores are naturally atomic on x86/x64).
- **All PPQ calculations must use the DAW-provided position** from getPlayHead()->getPosition().
