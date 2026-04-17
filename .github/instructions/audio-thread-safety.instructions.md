---
description: "Use when: writing or editing C++ audio code, processBlock, audio callbacks, real-time DSP, lock-free buffers, JUCE AudioProcessor. Enforces real-time audio thread safety constraints."
applyTo: ["src/**/*.{h,cpp}", "lib/**/*.{h,cpp}"]
---

# Audio Thread Safety Rules

Code in `processBlock`, `renderNextBlock`, timer callbacks called from the audio thread, or any function reachable from the audio callback **must** follow these rules.

## Absolute Prohibitions on the Audio Thread

1. **No memory allocation** — `new`, `delete`, `malloc`, `free`, `std::vector::push_back`, `std::vector::resize`, `std::string` construction/concatenation, `juce::String` construction, `juce::Array` mutation, `std::make_shared`, `std::make_unique` in hot path.
2. **No locks** — `std::mutex`, `std::lock_guard`, `std::unique_lock`, `juce::CriticalSection`, `juce::SpinLock`, `juce::ScopedLock`. If you need shared state, use `std::atomic` or a lock-free FIFO.
3. **No system calls** — File I/O (`fopen`, `std::fstream`), logging (`DBG`, `std::cout`, `printf`), network calls, `std::this_thread::sleep_for`.
4. **No virtual dispatch in tight loops** — Avoid calling virtual methods per-sample. Resolve to concrete types before the sample loop.
5. **No exceptions** — Do not throw or rely on try/catch in audio processing code.

## Required Practices

- **Atomic parameter access**: Cache `getRawParameterValue()` pointers in `prepareToPlay`. Load with `param->load()` once per block (or once per sample if modulation), never call `getRawParameterValue()` per-sample.
- **Denormal protection**: Call `juce::ScopedNoDenormals noDenormals;` at the top of `processBlock`.
- **prepareToPlay initialization**: All DSP state (filters, buffers, delays, FFT objects) must be allocated and initialized here, never lazily in `processBlock`.
- **Buffer bounds**: Always use `buffer.getNumSamples()` for loop bounds, never hardcoded sizes. Use `juce::jmin` when combining with internal buffer sizes.
- **Channel safety**: Handle mono, stereo, and arbitrary channel counts. Use `buffer.getNumChannels()` and loop, don't assume 2 channels.

## Lock-Free Communication Patterns

- **Audio → UI**: Use `AudioSampleFifo` (single-writer on audio thread, single-reader on UI thread) or `std::atomic` stores.
- **UI → Audio**: Use `std::atomic<float>` parameters via APVTS, or a lock-free FIFO with UI as writer, audio as reader.
- **Never share raw pointers** to UI components on the audio thread — UI components can be deleted at any time.

## Code Smells to Flag

| Smell | Why it's dangerous |
|-------|-------------------|
| `std::vector` as member used in processBlock | Might reallocate |
| `juce::String::formatted()` in audio code | Allocates |
| `DBG()` in processBlock | I/O + allocation |
| `listeners` or `callbacks` from audio thread | May allocate or lock |
| `sendChangeMessage()` from audio thread | Posts to message thread, may allocate |
| `getStateInformation` accessing audio state without atomic | Race condition |
