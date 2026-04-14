# Contributing to PHU Beat Sync Multi Scope

Contributions are welcome.

1. Fork and branch from `main`
2. Follow existing C++17/JUCE code style
3. Keep to the hard rules in [`.github/copilot-instructions.md`](.github/copilot-instructions.md) — in particular: **no network I/O on the audio thread**
4. Verify the project builds and passes pluginval before opening a PR

**Bug reports** — please include DAW name/version, OS, and reproduction steps in a [GitHub Issue](https://github.com/huberp/phu-beat-sync-multi-scope/issues).

---

## Project Layout

```
phu-beat-sync-multi-scope/
├── CMakeLists.txt / CMakePresets.json
├── doc/                            Screenshots
├── JUCE/                           JUCE 8.0.12 (git submodule)
├── src/
│   ├── PluginProcessor.h/cpp       processBlock, ping-pong broadcast buffer,
│   │                               SPSC local ring, CTRL heartbeat timer (30 Hz)
│   ├── PluginEditor.h/cpp          UI layout, 60 Hz refresh timer, control wiring
│   ├── ScopeDisplay.h/cpp          InstanceSlot pipeline, RMS + cancellation overlays,
│   │                               display-filter application, scatter → display bins
│   ├── DisplayFilterStrip.h/cpp    HP/LP display filter UI strip
│   ├── DebugLogPanel.h/cpp         In-plugin log viewer (debug builds only)
│   └── CMakeLists.txt
├── lib/
│   ├── audio/
│   │   ├── RawSampleBuffer.h       Position-addressed overwrite ring buffer
│   │   └── BucketSet.h             Dirty-tracked bucket partitioning (Rms + Cancel kinds)
│   ├── events/
│   │   ├── SyncGlobals.h           BPM / PPQ / transport state; atomic PPQ
│   │   ├── SyncGlobalsListener.h   Event listener interface
│   │   ├── Event.h / EventSource.h Typed event infrastructure
│   ├── network/
│   │   ├── MulticastBroadcasterBase.h/cpp  UDP multicast socket + receiver thread
│   │   ├── SampleBroadcaster.h/cpp         Raw sample send/receive (port 49422)
│   │   └── CtrlBroadcaster.h/cpp           Control / identity events (port 49423)
│   ├── LinkwitzRileyFilter.h       4th-order Linkwitz-Riley HP/LP filter
│   ├── StringUtil.h                String helpers (instance ID formatting)
│   └── debug/
│       ├── EditorLogger.h/cpp      Logger producer (debug builds only)
│       ├── DebugLogEventQueue.h    MPSC lock-free log queue
│       └── DebugLogSink.h          Consumer interface for the log panel
├── .github/workflows/              CI build + pluginval + release workflows
└── scripts/install-linux-deps.sh
```

---

## Building

### Prerequisites

| Tool | Minimum version |
|---|---|
| CMake | 3.15 |
| C++ compiler | C++17 — MSVC 2022, GCC 11, or Clang 14 |
| JUCE | 8.0.12 (included as git submodule) |

### Clone

```bash
git clone https://github.com/huberp/phu-beat-sync-multi-scope.git
cd phu-beat-sync-multi-scope
git submodule update --init --recursive
```

### Windows

```bash
cmake --preset vs2026-x64
cmake --build --preset vs2026-build --config Release
```

Output: `build/vs2026-x64/src/phu-beat-sync-multi-scope_artefacts/Release/VST3/`

### macOS Intel (x86_64)

```bash
cmake --preset macos-x86_64-release
cmake --build --preset macos-x86_64-build
```

Output:
- `build/macos-x86_64-release/src/phu-beat-sync-multi-scope_artefacts/Release/AU/` (AU)
- `build/macos-x86_64-release/src/phu-beat-sync-multi-scope_artefacts/Release/VST3/` (VST3)

### macOS Apple Silicon (arm64)

```bash
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-build
```

Output:
- `build/macos-arm64-release/src/phu-beat-sync-multi-scope_artefacts/Release/AU/` (AU)
- `build/macos-arm64-release/src/phu-beat-sync-multi-scope_artefacts/Release/VST3/` (VST3)

### Linux

```bash
sudo bash scripts/install-linux-deps.sh
cmake --preset linux-release
cmake --build --preset linux-build
```

If the build times out: `cmake --build --preset linux-build -j2`

Output: `build/linux-release/src/phu-beat-sync-multi-scope_artefacts/VST3/`

---

## Architecture

### Core Components

| Component | Location | Responsibility |
|---|---|---|
| `RawSampleBuffer` | `lib/audio/RawSampleBuffer.h` | Position-addressed overwrite ring buffer. Samples are written at `(int)(fmod(ppq, range)/range × N)`. Size N is computed from `displayBeats`, `bpm` and `sampleRate` in `prepare()`. |
| `BucketSet` | `lib/audio/BucketSet.h` | Partitions a `RawSampleBuffer` into dirty-tracked `Bucket` ranges. Two kinds: `Rms` (≤128 buckets, one per 1/16-beat) and `Cancel` (≤256 buckets, ~4 ms each). Only dirty buckets are recomputed per frame. |
| `LinkwitzRileyFilter` | `lib/LinkwitzRileyFilter.h` | 4th-order (48 dB/oct) Linkwitz-Riley HP/LP filter. One independent instance per active ScopeDisplay slot for display-path filtering. |
| `SyncGlobals` | `lib/events/SyncGlobals.h` | Holds current BPM, sample rate and block-end PPQ. Written on the audio thread; `ppqEndOfBlock` is an `std::atomic<double>` for safe UI-thread reads. Fires `GlobalsEventListener` callbacks on BPM / sample-rate changes. |
| `MulticastBroadcasterBase` | `lib/network/MulticastBroadcasterBase.h` | Abstract UDP multicast foundation: socket creation, multicast group join/leave, WSA init (Windows, ref-counted), instance ID generation, and receiver-thread lifecycle. |
| `SampleBroadcaster` | `lib/network/SampleBroadcaster.h` | Extends `MulticastBroadcasterBase`. Sends `RawSamplesPacket` (raw float samples + `ppqOfFirstSample` + `bpm`) **directly from the audio thread** via loopback `sendto()`. Receiver thread stores the latest packet per remote instance (keyed by `instanceID`). Port **49422**. |
| `CtrlBroadcaster` | `lib/network/CtrlBroadcaster.h` | Extends `MulticastBroadcasterBase`. Sends/receives `CtrlPacket` for instance identity (channel index, label, colour, sample rate) and commands (`PeersBroadcastOnly`). Event types: `Announce`, `LabelChange`, `RangeChange`, `Goodbye`, `PeersBroadcastOnly`. Port **49423**. |
| `ScopeDisplay` | `src/ScopeDisplay.h` | Oscilloscope component. Maintains up to 8 `InstanceSlot`s (1 local + 7 remote), each owning a `RawSampleBuffer` + `BucketSet(Rms)` + `BucketSet(Cancel)` + two `LinkwitzRileyFilter`s. Scatters buffer data into 4096 display bins per frame. |
| `DisplayFilterStrip` | `src/DisplayFilterStrip.h` | UI control strip for one HP or LP display filter (enable toggle + frequency knob). |
| `PluginProcessor` | `src/PluginProcessor.h` | `AudioProcessor` + `juce::Timer`. Owns `SyncGlobals`, `SampleBroadcaster`, `CtrlBroadcaster`, the ping-pong broadcast buffer, and the local SPSC ring. The 30 Hz timer handles CTRL heartbeats only — sample sending happens on the audio thread. |

### Data Flow

```
Audio Thread  (processBlock)
  ├─ Read play-head: extract BPM, block-start PPQ, isPlaying
  ├─ Update SyncGlobals (writes ppqEndOfBlock atomically)
  ├─ Mix stereo → mono; push (monoSample, absolutePpq) pairs
  │    into local SPSC ring  [capacity: 2 × BROADCAST_CHUNK_SAMPLES = 12700]
  └─ Accumulate mono samples into ping-pong broadcast slot
       └─ slot full (~33 ms / BROADCAST_CHUNK_SAMPLES samples):
            sendto() loopback multicast → SampleBroadcaster (port 49422)
            flip to other slot

Network receive thread  (SampleBroadcaster, port 49422)
  └─ recvfrom() → parse RawSamplesPacket → store in map[instanceID]
                  (mutex-protected; UI thread snapshots via getReceivedPackets())

Network receive thread  (CtrlBroadcaster, port 49423)
  └─ recvfrom() → parse CtrlPacket → update RemoteInstanceInfo map
                  (mutex-protected; UI thread snapshots via getRemoteInfos())

Processor Timer  (30 Hz — runs headlessly when UI is closed)
  └─ CTRL heartbeat: send Announce if interval elapsed (staggered per-instance phase)

UI Timer  (60 Hz)
  ├─ Drain local SPSC ring
  │    ├─ Apply HP/LP Linkwitz-Riley filter (per-sample)
  │    └─ RawSampleBuffer[local].writeAt(ppq-mapped index)
  │         → mark dirty range in BucketSet(Rms) + BucketSet(Cancel)
  ├─ getReceivedPackets() → for each remote RawSamplesPacket:
  │    ├─ Apply HP/LP filter (per-instance filter state)
  │    └─ RawSampleBuffer[remote_i].writeAt(ppq-mapped index)
  │         → mark dirty buckets
  ├─ computeFrame():
  │    ├─ Recompute dirty Rms buckets  → rms[] per slot
  │    ├─ Recompute dirty Cancel buckets  → cancellationIndex[] per slot
  │    └─ Scatter RawSampleBuffer[*] → displayBins[4096] (last-write-wins)
  └─ repaint ScopeDisplay
```

### Beat Alignment

Every sample is stored at its absolute beat position using only the PPQ value already carried by the packet:

```
normPos  = fmod(absolutePpq, displayRangeBeats) / displayRangeBeats   // [0, 1)
writeIdx = (int)(normPos × N)                                          // N = buffer size
```

`RawSamplesPacket` carries `ppqOfFirstSample` and `bpm`. The receiver reconstructs per-sample PPQ as:

```
ppq_i = ppqOfFirstSample + i × (bpm / (60.0 × sampleRate))
```

Because positions are absolute and the buffer size covers exactly one `displayRangeBeats` window, instances with different display ranges remain beat-grid aligned with no drift.

### Display Filter Pipeline

Each active `InstanceSlot` inside `ScopeDisplay` holds two independent Linkwitz-Riley filter chains (HP + LP, 48 dB/oct). Filters are reset at display-window boundaries (when `floor(ppq / displayRangeBeats)` changes) to prevent boundary transients from polluting the next window.

### Network Protocol

| Property | Value |
|---|---|
| Multicast group | `239.255.42.1` (RFC 2365 administratively scoped — stays on LAN) |
| Sample data port | `49422` (`SampleBroadcaster`) |
| Control data port | `49423` (`CtrlBroadcaster`) |
| Sample send rate | Audio-thread driven (~30 Hz at default slot size) |
| Sample encoding | Raw `float` (no quantization — loopback only) |
| Control heartbeat | Every 5 s (staggered per-instance phase offset) |
| Staleness timeout | 3 s — remote instances not heard within this window are pruned |

Note: the loopback-only assumption allows large packets (~25 KB per slot at 192 kHz) without MTU constraints. Cross-machine use would require packet splitting.
