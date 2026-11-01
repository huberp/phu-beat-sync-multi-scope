# Contributing to PHU Beat Sync Multi Scope

Contributions are welcome.

1. Fork and branch from `main`
2. Follow existing C++20/JUCE code style
3. Keep to the [audio-thread rules](#audio-thread-rules) below and in [`.github/instructions/audio-thread-safety.instructions.md`](.github/instructions/audio-thread-safety.instructions.md)
4. Verify the project builds, the [unit tests](#tests) pass, and the VST3 passes pluginval before opening a PR
5. If you change user-visible behaviour, update [README.md](README.md) — it is the user manual and is shipped as `README.pdf` inside every release zip

**Bug reports** — please include DAW name/version, OS, and reproduction steps in a [GitHub Issue](https://github.com/huberp/phu-beat-sync-multi-scope/issues).

---

## Project Layout

```
phu-beat-sync-multi-scope/
├── CMakeLists.txt / CMakePresets.json
├── doc/                            Screenshots used by README.md
├── cmake/
│   ├── PhuDependencies.cmake       FetchContent helpers for JUCE and phu-audio-lib
│   ├── juce_config.cmake           JUCE feature flags and build options
│   └── compiler_flags.cmake        Optional warnings / sanitizers / coverage
├── src/
│   ├── PluginProcessor.h/cpp       processBlock, ping-pong broadcast slots, SPSC local ring,
│   │                               APVTS parameters, state save/restore,
│   │                               30 Hz timer (CTRL heartbeat, peer commands)
│   ├── PluginEditor.h/cpp          Fixed-size UI (800 × 612), 60 Hz refresh timer,
│   │                               control wiring, display-range constraints per BPM
│   ├── ScopeDisplay.h/cpp          InstanceSlot pipeline: filter + write, RMS and cancellation
│   │                               buckets, scatter → display bins, software fallback painting
│   ├── DisplayFilterStrip.h/cpp    HP/LP display filter UI strip
│   ├── DebugLogPanel.h/cpp         In-plugin log viewer (debug builds only)
│   ├── DisplayRangeLimits.h        JUCE-free BPM → max display range mapping (unit-tested)
│   ├── gl/                         OpenGL rendering pipeline (see "Rendering" below)
│   │   ├── ScopeGLCoordinator.h/cpp         Owns the OpenGLContext, delegates to sub-renderers
│   │   ├── WaveformGLRenderer.h/cpp         Waveform lines (gl_VertexID shader, displayBins VBO)
│   │   ├── GridPlayheadGLRenderer.h/cpp     Amplitude grid + beat playhead
│   │   ├── RmsOverlayRenderer.h/cpp         1/16-beat RMS step lines
│   │   ├── CancellationOverlayRenderer.h/cpp  Green → yellow → red cancellation bar
│   │   └── OpenGLScopeRenderer.h/cpp        Earlier monolithic renderer — not part of the build
│   └── CMakeLists.txt
├── tests/
│   ├── test_display_range.cpp      Unit tests for the BPM → max display range mapping
│   └── CMakeLists.txt
├── phu-audio-lib/                  Shared library, fetched by CMake (git-ignored, see below)
├── JUCE/                           Fetched by CMake (git-ignored)
├── .github/
│   ├── workflows/                  build.yml (CI), release.yml (tagged releases), reusable
│   │                               per-platform workflows (build-windows-vst3.yml, build-linux.yml,
│   │                               build-macos.yml) and generate-pdf.yml (README.md → README.pdf)
│   ├── instructions/               Audio thread-safety rules for Copilot
│   └── copilot-instructions.md     Project notes for Copilot
└── scripts/install-linux-deps.sh
```

### phu-audio-lib

The reusable building blocks live in the shared [huberp/phu-audio-lib](https://github.com/huberp/phu-audio-lib) repository. CMake clones it into `phu-audio-lib/` at configure time (branch `main`, see `cmake/PhuDependencies.cmake`); it is **not** a git submodule and the folder is git-ignored. This project uses:

| Path in `phu-audio-lib/` | Contents |
|---|---|
| `audio/PpqAddressedRingBuffer.h` | Position-addressed overwrite ring buffer |
| `audio/BucketSet.h` | Dirty-tracked bucket partitioning of a buffer |
| `audio/LinkwitzRileyFilter.h` | Linkwitz-Riley filters (LR4 / LR8), used as LR8 (48 dB/oct) for the display filters |
| `events/SyncGlobals.h`, `SyncGlobalsListener.h`, `Event.h`, `EventSource.h` | BPM / sample rate / PPQ / transport state and event infrastructure |
| `network/MulticastBroadcasterBase`, `StatefulBroadcaster.h` | UDP multicast sockets, receiver thread, per-instance state map |
| `network/SampleBroadcaster`, `CtrlBroadcaster` | The two protocols used by this plugin (see [Network Protocol](#network-protocol)) |
| `gl/GLRendererBase.h`, `GLSnapshotRenderer.h`, `GLSLShaderBuilder` | OpenGL helpers (`PhuGLUtils` target) |
| `debug/EditorLogger`, `DebugLogEventQueue.h`, `DebugLogSink.h` | Lock-free log queue for the debug panel (debug builds only) |
| `util/StringUtil.h` | String helpers (instance ID formatting) |

When a change belongs into the shared library, make it in that repository and push it before building here. To work against a local checkout of the library, configure with `-DFETCHCONTENT_SOURCE_DIR_PHU-AUDIO-LIB=<path>`; to stop CMake from touching the network on later configures, use `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`.

---

## Building

### Prerequisites

| Tool | Minimum version |
|---|---|
| CMake | 3.28 |
| C++ compiler | C++20 — MSVC 2022 or newer (Visual Studio 2026 generator in the Windows presets), GCC 11, or Clang 14 |
| JUCE | 8.0.15 (fetched by CMake into `JUCE/`) |
| phu-audio-lib | `main` (fetched by CMake into `phu-audio-lib/`) |

### Clone

JUCE and phu-audio-lib are downloaded automatically during the first CMake configure (network access to GitHub required). There are no submodules to initialise.

```bash
git clone https://github.com/huberp/phu-beat-sync-multi-scope.git
cd phu-beat-sync-multi-scope
```

### Windows

```bash
cmake --preset windows-x64-vst3-release
cmake --build --preset windows-x64-vst3-release-build
```

Output: `build/windows-x64-vst3-release/src/phu-beat-sync-multi-scope_artefacts/Release/VST3/`

Other presets: `windows-x64-vst3-debug` (with `-build`), and `windows-arm64-vst3-release` (ARM64 cross-compile, with `-build`).

### macOS Intel (x64)

```bash
cmake --preset macos-x64-release
cmake --build --preset macos-x64-release-build
```

Output:
- `build/macos-x64-release/src/phu-beat-sync-multi-scope_artefacts/Release/AU/` (AU)
- `build/macos-x64-release/src/phu-beat-sync-multi-scope_artefacts/Release/VST3/` (VST3)

### macOS Apple Silicon (arm64)

```bash
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release-build
```

Output:
- `build/macos-arm64-release/src/phu-beat-sync-multi-scope_artefacts/Release/AU/` (AU)
- `build/macos-arm64-release/src/phu-beat-sync-multi-scope_artefacts/Release/VST3/` (VST3)

Single-format builds: `macos-arm64-release-build-vst3` / `macos-arm64-release-build-au` (same configure preset). Debug presets: `macos-x64-debug`, `macos-arm64-debug`.

### Linux

```bash
sudo bash scripts/install-linux-deps.sh
cmake --preset linux-x64-vst3-release
cmake --build --preset linux-x64-vst3-release-build
```

If the build times out: `cmake --build --preset linux-x64-vst3-release-build -j2`

Output: `build/linux-x64-vst3-release/src/phu-beat-sync-multi-scope_artefacts/Release/VST3/`

An ARM64 variant is available as `linux-arm64-vst3-release`.

Presets follow `<os>-<arch>-<format>-<config>` (macOS omits the format and builds VST3 + AU); every configure preset has a matching `-build` preset. See `cmake --list-presets=all`.

### Debug builds

Debug builds (the `*-debug` presets) add a log panel below the scope (`DebugLogPanel`, fed by `EditorLogger`). It is compiled out with `NDEBUG`, so release builds never contain it.

### Optional diagnostics

| Option / preset | Purpose |
|---|---|
| `-DENABLE_STRICT_WARNINGS=ON` | Strict compiler warnings for this project's code (not for fetched dependencies) |
| `--preset sanitizers` / `--preset sanitizers-build` | Debug build with Address + Undefined Behavior sanitizers |
| `--preset coverage` / `--preset coverage-build` | Debug build with coverage instrumentation |

### Tests

`tests/test_display_range.cpp` is a dependency-free test executable (no JUCE) for the BPM → max display range mapping in `src/DisplayRangeLimits.h`. It is part of the normal build, and CI runs it on Windows, Linux and macOS Apple Silicon. `enable_testing()` is in the top-level `CMakeLists.txt`, so run CTest from the build directory of your preset:

```bash
cmake --build --preset windows-x64-vst3-release-build --target test_display_range
ctest --test-dir build/windows-x64-vst3-release -C Release --output-on-failure
```

Substitute the preset / build directory for your platform. Single-config generators (Linux, macOS) do not need `-C Release`. The test executable can also be run directly from `build/<preset>/tests/`.

The unit tests for the shared `PpqAddressedRingBuffer`, `BucketSet` and the other `phu-audio-lib` headers live in the [phu-audio-lib repository](https://github.com/huberp/phu-audio-lib) (`tests/` folder); changes to those classes must be tested there.

### Plugin validation

CI validates the Windows VST3 with [pluginval](https://github.com/Tracktion/pluginval) at `--strictness-level 5` (`build.yml` on every push and PR, `release.yml` before packaging). To check locally:

```powershell
pluginval.exe --strictness-level 5 --validate-in-process --validate "<path to PHU BEAT SYNC MULTI SCOPE.vst3>"
```

### CI and releases

| Workflow | Trigger | What it does |
|---|---|---|
| `build.yml` | push to `main`, pull requests, manual | Calls the per-platform reusable workflows: Windows x64 (with pluginval), Linux x64 and arm64, macOS Apple Silicon and macOS Intel (with pluginval); plus a `test` job that builds and runs the unit tests on Windows, Linux and macOS |
| `release.yml` | tag push `v[0-9]*` (e.g. `v0.2.0`), or manual run with a **required** `tag` input | A `prepare` job validates the tag (and, for manual runs, creates and pushes it on the selected commit). Then it calls the reusable workflows to build and package Windows x64 (with pluginval), Windows ARM64, Linux x64, macOS Apple Silicon and macOS Intel, calls `generate-pdf.yml` to render `README.md` to `README.pdf` (pandoc + xelatex), injects it into every zip, and publishes a GitHub Release. Tags that contain a `-` (e.g. `v0.2.0-beta`) are marked as pre-release. |
| `build-windows-vst3.yml`, `build-linux.yml`, `build-macos.yml` | called by `build.yml` and `release.yml` | Reusable per-platform workflows (inputs `arch`, `run-pluginval`, `upload-artifact`, `artifact-name`, `ref-name`; output `validation-status`): configure and build via the CMake presets, optional pluginval run, and — when `ref-name` is set — packaging into `PHU-BEAT-SYNC-MULTI-SCOPE-<os>-<arch>-<version>.zip` plus artifact upload |
| `generate-pdf.yml` | called by `release.yml`, or manual | Converts a Markdown file (input `in-md`) to PDF with pandoc + xelatex and uploads it as the `pdf` artifact |

Because `README.md` is converted to PDF, keep it to plain Markdown with relative image paths (`doc/…`).

---

## Architecture

### Overview

Every plugin instance does two independent things:

1. **Send** — the audio thread accumulates the mono-mixed input into 33 ms chunks tagged with their PPQ position and sends them over UDP multicast (loopback) to the other instances.
2. **Display** — the UI thread collects its own samples and the samples received from the other instances into one position-addressed ring buffer per instance, derives the overlays from them, and draws the result.

The audio itself is never modified. All instances run in the same process or at least on the same machine; the multicast group is `239.255.42.1` with a TTL of 1.

### Core Components

| Component | Location | Responsibility |
|---|---|---|
| `PpqAddressedRingBuffer` | `phu-audio-lib/audio/PpqAddressedRingBuffer.h` | Position-addressed overwrite ring buffer. Samples are written at `(int)(fmod(ppq, range)/range × N)`. Size N is computed from `displayBeats`, `bpm` and `sampleRate` in `prepare()`. |
| `BucketSet` | `phu-audio-lib/audio/BucketSet.h` | Partitions a buffer into dirty-tracked `Bucket` ranges. Used twice per instance: *RMS* buckets (16 per beat, max 128) and *Cancel* buckets (≈ 4 ms each, max 256). Only dirty buckets are recomputed per frame. |
| `LinkwitzRileyFilter` | `phu-audio-lib/audio/LinkwitzRileyFilter.h` | Linkwitz-Riley HP/LP filter. The scope uses the 8th-order (48 dB/oct, four cascaded biquads) variant. One independent HP and LP instance per active slot. |
| `SyncGlobals` | `phu-audio-lib/events/SyncGlobals.h` | Holds current BPM, sample rate and block-end PPQ. Written on the audio thread; `ppqEndOfBlock` is an `std::atomic<double>` for safe UI-thread reads. Fires `GlobalsEventListener` callbacks on BPM / sample-rate changes. |
| `MulticastBroadcasterBase` | `phu-audio-lib/network/` | Abstract UDP multicast foundation: socket creation, multicast group join/leave, WSA init (Windows, ref-counted), instance ID generation, and receiver-thread lifecycle. |
| `SampleBroadcaster` | `phu-audio-lib/network/SampleBroadcaster.h` | Sends `RawSamplesPacket` (raw mono float samples + `ppqOfFirstSample` + `bpm` + sequence number) **directly from the audio thread** via loopback `sendto()`. The receiver thread stores the latest packet per remote instance. Port **49422**. |
| `CtrlBroadcaster` | `phu-audio-lib/network/CtrlBroadcaster.h` | `CtrlPacket` for instance identity (channel index, label, colour, sample rate, display range) and commands. Event types: `Announce`, `LabelChange`, `RangeChange`, `Goodbye`, `PeersBroadcastOnly`. Port **49423**. |
| `PluginProcessor` | `src/PluginProcessor.h` | `AudioProcessor` + `juce::Timer`. Owns `SyncGlobals`, both broadcasters, the ping-pong broadcast slots, the local SPSC ring and the APVTS. The 30 Hz timer handles CTRL heartbeats, peer commands and channel changes only — sample sending happens on the audio thread, so a broadcasting instance works without an open editor. |
| `PluginEditor` | `src/PluginEditor.h` | Controls, 60 Hz timer that feeds `ScopeDisplay`, and the BPM-dependent limit of the display range (`getMaxDisplayBeatsForBpm`). |
| `ScopeDisplay` | `src/ScopeDisplay.h` | Holds up to 8 `InstanceSlot`s. Each owns a `PpqAddressedRingBuffer`, an RMS and a Cancel `BucketSet`, an HP and an LP filter, and a 4096-bin display array. **The slot index is the instance's channel number − 1**: the local instance occupies its own channel, and a remote packet that claims the local channel is ignored. Computes the RMS and cancellation overlays and hands the result to the GL pipeline (or paints in software). |
| `DisplayFilterStrip` | `src/DisplayFilterStrip.h` | UI control strip for one HP or LP display filter (enable toggle + frequency knob). |
| `ScopeGLCoordinator` | `src/gl/ScopeGLCoordinator.h` | The only `juce::OpenGLRenderer` on the `OpenGLContext`. Delegates to the four sub-renderers. |

### Data Flow

```
Audio Thread  (processBlock)
  ├─ Read play-head: extract BPM, block-start PPQ, isPlaying
  ├─ Update SyncGlobals (writes ppqEndOfBlock atomically)
  ├─ Mix stereo → mono ((L+R)/2); unless broadcast-only: push (monoSample, absolutePpq)
  │    pairs into the local SPSC ring  [capacity: 2 × BROADCAST_CHUNK_SAMPLES = 12700]
  └─ If Broadcast is on: accumulate mono samples into the ping-pong broadcast slot
       └─ slot full (~33 ms of samples, 128 … 6350 samples):
            sendto() loopback multicast → SampleBroadcaster (port 49422)
            flip to other slot

Network receive thread  (SampleBroadcaster, port 49422)
  └─ recvfrom() → parse RawSamplesPacket → store in map[instanceID]
                  (mutex-protected; UI thread snapshots via getReceivedPackets())

Network receive thread  (CtrlBroadcaster, port 49423)
  └─ recvfrom() → parse CtrlPacket → update RemoteInstanceInfo map
                  (mutex-protected; UI thread snapshots via getRemoteInfos())

Processor Timer  (30 Hz — runs headlessly when UI is closed)
  ├─ Apply PeersBroadcastOnly command from a peer
  ├─ Sync a changed channel index to both broadcasters
  └─ CTRL heartbeat: send Announce when the adaptive interval has elapsed

UI Timer  (60 Hz)
  ├─ Update prepare() parameters if BPM / range / sample rate changed
  ├─ Drain local SPSC ring
  │    ├─ Apply HP/LP Linkwitz-Riley filter (per-sample)
  │    └─ PpqAddressedRingBuffer[local].writeAt(ppq-mapped index)
  │         → mark dirty range in BucketSet(Rms) + BucketSet(Cancel)
  ├─ getReceivedPackets() → for each remote RawSamplesPacket:
  │    ├─ Apply HP/LP filter (per-instance filter state)
  │    └─ PpqAddressedRingBuffer[channel-1].writeAt(ppq-mapped index)
  │         → mark dirty buckets
  ├─ computeFrame():
  │    ├─ Recompute dirty Rms buckets  → rms[] per slot
  │    ├─ Recompute dirty Cancel buckets  → cancellationIndex[] per slot
  │    └─ Scatter PpqAddressedRingBuffer[*] → displayBins[4096] (last-write-wins)
  └─ Hand the frame to the GL coordinator and repaint ScopeDisplay
```

In broadcast-only mode the audio thread stops filling the local ring, the editor skips all of the UI-timer work, and the editor shows the "BROADCAST MODE" overlay.

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

The sender's sample rate is taken from its latest `Announce` packet. Because positions are absolute and the buffer size covers exactly one `displayRangeBeats` window, instances with different sample rates stay beat-grid aligned with no drift. All instances should use the same display range; the range is part of the CTRL metadata.

Display ranges are 1, 2, 4 or 8 beats. `PluginProcessor::getMaxDisplayBeatsForBpm()` limits the longest range at low tempos (< 80 BPM: 4 beats, < 60 BPM: 2 beats, < 40 BPM: 1 beat).

### Display Filter Pipeline

Each active `InstanceSlot` inside `ScopeDisplay` holds two independent Linkwitz-Riley filter chains (HP + LP, 48 dB/oct). Filters are reset at display-window boundaries (when `floor(ppq / displayRangeBeats)` changes) to prevent boundary transients from polluting the next window. The filters only touch the data that goes into the display buffers — never the audio in `processBlock`.

### Analysis Overlays

| Overlay | Buckets | Computation |
|---|---|---|
| RMS envelope | `rmsBuckets`: `min(128, 16 × range)` buckets, i.e. one per 1/16 beat | Per instance RMS per bucket. The drawn value is the sum over all active instances when remote display is on and remotes are present, otherwise the local RMS. |
| Cancellation | `cancelBuckets`: `ceil(sampleRate × 4 ms)` samples per bucket, max 256 buckets | `CI = 1 − RMS(Σ instances) / Σ RMS(instance)`, weighted by `sqrt(min(1, D / 0.1))`; buckets with `D ≤ 1e-4` are zeroed. Active only with ≥ 2 active instances. See the formula section in the [README](README.md#cancellation-detector). |

Both overlays recompute only the buckets that were marked dirty by new data.

### Rendering

`ScopeDisplay::parentHierarchyChanged()` attaches the `ScopeGLCoordinator` to the component. When the OpenGL context is available (`isOpenGLActive()`), the GL pipeline draws background, grid, waveforms, playhead and the RMS and cancellation overlays; `ScopeDisplay::paint()` then only adds the remote labels, the broadcast-only overlay and the border. Without OpenGL, `paint()` draws everything in software.

Each sub-renderer is built on the helpers from `phu-audio-lib/gl/`: `GLRendererBase` (create/release lifecycle), `GLSLShaderBuilder` (GLSL version handling, shader assembly and compile errors) and `GLSnapshotRenderer<T>` (double-buffered snapshot handed from the UI thread to the GL thread under a `SpinLock`). New renderers should follow the same pattern and register with `ScopeGLCoordinator`.

### Network Protocol

| Property | Value |
|---|---|
| Multicast group | `239.255.42.1`, TTL 1, loopback enabled (instances filter their own packets by `instanceID`) |
| Sample data port | `49422` (`SampleBroadcaster`, protocol version 4) |
| Control data port | `49423` (`CtrlBroadcaster`) |
| Sample send rate | Audio-thread driven, one packet per ≈ 33 ms slot |
| Sample encoding | Raw mono `float` (no quantization), up to 6350 samples per packet (≈ 25 KB) |
| Control heartbeat | Adaptive, 5 s … 12 s: backs off when many control packets arrive, recovers when it is quiet; staggered per-instance phase offset |
| Staleness | Sample data: 3 s without a packet removes the remote trace. Control info: 30 s (or immediately on `Goodbye`). |

Instances on the same machine only. The loopback assumption allows packets far larger than a network MTU; cross-machine use would require packet splitting.

### State and Parameters

| Item | Storage |
|---|---|
| `display_range`, `display_hp_enabled`, `display_hp_freq`, `display_lp_enabled`, `display_lp_freq`, `instance_channel` | APVTS parameters (automatable, saved with the project). HP frequency is limited to 18 kHz, LP to 20 kHz, and the editor keeps HP below LP by at least 10 Hz. |
| Broadcast enabled, receive enabled, broadcast-only mode, channel label, colour | `PluginState` child node of the APVTS state, written in `getStateInformation()` and read back (with a fallback for older flat states) in `setStateInformation()` |

---

## Audio-Thread Rules

Authoritative rules for the audio thread are in [`.github/instructions/audio-thread-safety.instructions.md`](.github/instructions/audio-thread-safety.instructions.md). In short: no allocation, no locks, no file or console I/O, no exceptions in `processBlock` or anything it calls.

Project-specific points:

- The **only** network call on the audio thread is `SampleBroadcaster::broadcastRawSamples()`: a lock-free `sendto()` on a loopback multicast socket, fed from pre-allocated ping-pong slots. Do not add other socket calls (and never a receive) to the audio thread. Socket setup, receiving, parsing and CTRL packets run on the message thread or the receiver threads.
- Data flows from the audio thread to the UI through the SPSC ring (`AbstractFifo`) and atomics only; the UI thread never writes into audio-thread state.
- All PPQ values must come from the DAW (`getPlayHead()->getPosition()`). Do not assume a fixed BPM or sample rate: the plugin must handle 44.1 kHz … 192 kHz and tempos from 40 BPM upwards.
- Buffers that depend on BPM, sample rate or display range are sized in `prepare()` (UI thread), never in `processBlock`.
