#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace phu {
namespace events {

/**
 * Base Event class - mirrors the Lua event table structure
 * All events carry a source pointer and context information
 */
struct Event {
    virtual ~Event() = default;

    // Source that fired this event
    const void* source = nullptr;

    // Context from DAW (position, samples, etc.)
    struct Context {
        const juce::AudioBuffer<float>* buffer = nullptr;
        int numberOfSamplesInFrame = 0;
        const juce::MidiBuffer* midiBuffer = nullptr;
        const juce::Optional<juce::AudioPlayHead::PositionInfo>* positionInfo = nullptr;
        int epoch = 0;
    } context;

  protected:
    Event() = default;
};

} // namespace events
} // namespace phu
