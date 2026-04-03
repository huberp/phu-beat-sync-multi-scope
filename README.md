# PHU Beat Sync Multi Scope

[![Build](https://github.com/huberp/phu-beat-sync-multi-scope/actions/workflows/build.yml/badge.svg)](https://github.com/huberp/phu-beat-sync-multi-scope/actions/workflows/build.yml)
[![Release](https://github.com/huberp/phu-beat-sync-multi-scope/actions/workflows/release.yml/badge.svg)](https://github.com/huberp/phu-beat-sync-multi-scope/actions/workflows/release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue.svg)](#building)
[![Format](https://img.shields.io/badge/format-VST3-purple.svg)](#building)
[![JUCE](https://img.shields.io/badge/JUCE-8.0.12-orange.svg)](https://juce.com)
[![Coffee](https://img.shields.io/badge/By%20me%20a%20Coffee-purple.svg)](https://ko-fi.com/phuplugins)

A VST3 oscilloscope that loads on multiple DAW tracks simultaneously. All instances stay beat-aligned and share waveforms over the local network in real time — useful for comparing phase, level and transient alignment across tracks.

![Overview — RMS and Cancellation overlays active](doc/image-scope-rms-cancelation.png)

---

## Contents

- [User Guide](#user-guide)
- [Building](#building)
- [Architecture](#architecture)
- [Cancellation Detector](#cancellation-detector)
- [Contributing](#contributing)
- [License](#license)

---

## User Guide

### Installation

1. Download the latest release from [Releases](https://github.com/huberp/phu-beat-sync-multi-scope/releases)
2. Copy the `.vst3` bundle to your DAW's VST3 folder:
   - Windows: `C:\Program Files\Common Files\VST3\`
   - Linux: `~/.vst3/` or `/usr/lib/vst3/`
3. Rescan plugins in your DAW
4. Load **PHU BEAT SYNC MULTI SCOPE** on any tracks you want to compare

No external dependencies — the binary is self-contained.

### Setup

```
Track 1  →  [plugin]  Broadcast ✔  Show Remote ✔  Ch 1  "Kick"
Track 2  →  [plugin]  Broadcast ✔  Show Remote ✔  Ch 2  "Snare"
Track 3  →  [plugin]  Broadcast ✔  Show Remote ✔  Ch 3  "Bass"
```

Every instance that has **Broadcast** enabled sends its waveform via UDP multicast (`239.255.42.1:49423`) at ~30 Hz. All instances see each other automatically — no pairing required. Broadcasting continues even when the plugin window is closed.

### Controls

**Network group**

| Control | Function |
|---|---|
| Show Local | Hide/show the local waveform |
| Show Remote | Hide/show all received remote waveforms |
| Broadcast | Enable/disable sending this instance's waveform |
| B/Cast on/off | Broadcast-only mode — UI display disabled, CPU freed. If this instance is only meant for sending it's data to it' peers |
| Peers B/Cast Only | Sends a command to all peers to enter broadcast-only mode. If this instance is the single instance used for viewing |

**Identity group**

Each instance gets a channel number (Ch 1–8), a free-text label, and a colour. These are transmitted with every packet; all other instances use them to label and colour their overlaid waveforms.

![Channel selection](doc/image-select-channel.png)

**Range**

Selects the beat window shown — 1, 2, 4 or 8 beats. At high BPM, wider ranges are automatically greyed out when the buffer would be too large to be reliable.

**Display Filters**

HP and LP filters applied only to the display signal — the audio path is unaffected. Useful for isolating a frequency band without changing the mix.

**Analysis**

- 〰 **RMS Envelope** — step lines at every 1/16-note showing RMS level of the combined (local + all visible remote) signals
- 🟩 **Cancellation** — colour bar at the bottom of the scope indicating phase cancellation between instances: green = in-phase, yellow = partial, red = heavy cancellation

![Broadcast-only mode](doc/image-broadcast-mode.png)

*In broadcast-only mode the scope display is replaced by a status overlay. All display computations stop, reducing CPU impact for instances that only need to feed data to others.*

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

### Project Layout

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

## Cancellation Detector

The cancellation detector measures deviation of the combined signal from the incoherent sum of individual levels — a direct indicator of phase cancellation across tracks.

### Formula

For each time window of ~4 ms, the raw cancellation index is:

$$CI = 1 - \frac{\text{RMS}(L + R_1 + R_2 + \cdots)}{\text{RMS}(L) + \text{RMS}(R_1) + \text{RMS}(R_2) + \cdots}$$

The denominator is the maximum possible RMS if every instance were perfectly in-phase (triangle inequality). $CI \in [0, 1]$: 0 = no cancellation, 1 = total cancellation.

### Level Weighting

$CI$ values near the noise floor are suppressed to avoid false readings on near-silent signals:

$$CI_w = CI \cdot \sqrt{\min\!\left(1,\;\frac{D}{D_{\text{ref}}}\right)}$$

where $D = \text{RMS}(L) + \sum \text{RMS}(R_i)$ and $D_{\text{ref}} = 0.1$ (≈ −20 dBFS).

| $D$ | Weight | Effect |
|---|---|---|
| 0.01 (−40 dBFS) | 0.32 | Strongly suppressed |
| 0.03 (−30 dBFS) | 0.55 | Moderately suppressed |
| ≥ 0.10 (−20 dBFS) | 1.00 | Full CI displayed |

A hard gate (`sumIndividualRms > 0.01`) skips computation entirely below −40 dBFS.

### Colour Mapping

| $CI_w$ | Colour | Meaning |
|---|---|---|
| 0.0 | `#00BB55` green | In-phase |
| 0.4 | `#FFCC00` yellow | Partial cancellation |
| 1.0 | `#FF3300` red | Total cancellation |

### Resolution

256 windows span the full display range:

$$\Delta t \approx \frac{60}{\text{BPM}} \cdot \frac{R}{256} \text{ s}$$

At 120 BPM, 4-beat range: Δt ≈ 62 ms. At 1-beat range: Δt ≈ 15 ms.

---

## Contributing

Contributions are welcome.

1. Fork and branch from `main`
2. Follow existing C++17/JUCE code style
3. Keep to the hard rules in [`.github/copilot-instructions.md`](.github/copilot-instructions.md) — in particular: **no network I/O on the audio thread**
4. Verify the project builds and passes pluginval before opening a PR

**Bug reports** — please include DAW name/version, OS, and reproduction steps in a [GitHub Issue](https://github.com/huberp/phu-beat-sync-multi-scope/issues).

---

## License

[MIT](LICENSE)
