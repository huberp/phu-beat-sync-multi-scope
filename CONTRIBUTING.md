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
