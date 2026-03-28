#include "DisplayFilterStrip.h"
#include <cmath>

// ============================================================================
// Musical note helpers
// ============================================================================

namespace {

// Note names indexed 0–11 (C, C#, D, …, B)
static const char* const NOTE_NAMES[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

} // anonymous namespace

juce::String DisplayFilterStrip::freqToDisplayString(float freq) {
    if (freq <= 0.0f)
        return juce::String(static_cast<int>(std::round(freq))) + " Hz";

    // Convert Hz → MIDI note number (float)
    const float midiFloat = 69.0f + 12.0f * std::log2(freq / 440.0f);
    const int midiNote = static_cast<int>(std::round(midiFloat));
    const float noteFreq = 440.0f * std::pow(2.0f, (midiNote - 69.0f) / 12.0f);

    // Show note name if the frequency is within 1% of an equal-temperament note
    if (std::abs(freq - noteFreq) / noteFreq < 0.01f) {
        const int octave = midiNote / 12 - 1;
        const int noteIdx = ((midiNote % 12) + 12) % 12;
        return juce::String(NOTE_NAMES[noteIdx]) + juce::String(octave) +
               " / " + juce::String(static_cast<int>(std::round(freq))) + " Hz";
    }

    return juce::String(static_cast<int>(std::round(freq))) + " Hz";
}

float DisplayFilterStrip::parseFreqInput(const juce::String& input) {
    juce::String trimmed = input.trim();
    if (trimmed.isEmpty())
        return -1.0f;

    // If the string starts with a digit, parse numeric Hz value
    if (trimmed[0] >= '0' && trimmed[0] <= '9') {
        // Strip trailing " Hz" or other non-numeric suffix
        const float val = trimmed.getFloatValue();
        return (val > 0.0f) ? val : -1.0f;
    }

    // Otherwise try note-name parsing (e.g. "A2", "C#3", "Bb4")
    juce::String upper = trimmed.toUpperCase();

    // Normalise flat notation to sharp: Bb→A#, Eb→D#, Ab→G#, Db→C#, Gb→F#
    upper = upper.replace("BB", "A#")
                 .replace("EB", "D#")
                 .replace("AB", "G#")
                 .replace("DB", "C#")
                 .replace("GB", "F#");

    // Iterate backwards so two-character names (like "C#") are tried before "C"
    for (int i = 11; i >= 0; --i) {
        const juce::String noteName(NOTE_NAMES[i]);
        if (upper.startsWith(noteName)) {
            const juce::String rest = upper.substring(noteName.length()).trim();
            if (rest.isNotEmpty() && (rest[0] == '-' || (rest[0] >= '0' && rest[0] <= '9'))) {
                const int octave = rest.getIntValue();
                const int midiNote = (octave + 1) * 12 + i;
                return 440.0f * std::pow(2.0f, (midiNote - 69.0f) / 12.0f);
            }
        }
    }

    return -1.0f; // parse failure
}

// ============================================================================
// Construction / Destruction
// ============================================================================

DisplayFilterStrip::DisplayFilterStrip(const juce::String& toggleText,
                                       juce::AudioProcessorValueTreeState& apvts,
                                       const juce::String& enabledParamID,
                                       const juce::String& freqParamID)
    : m_apvts(apvts), m_freqParamID(freqParamID) {
    // --- Toggle button ---
    m_enableButton.setButtonText(toggleText);
    addAndMakeVisible(m_enableButton);
    m_enableAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            apvts, enabledParamID, m_enableButton);

    // --- Frequency knob ---
    m_freqKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    m_freqKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    addAndMakeVisible(m_freqKnob);
    m_freqAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, freqParamID, m_freqKnob);

    // --- Frequency label (editable on double-click) ---
    m_freqLabel.setEditable(false, true, false);
    m_freqLabel.setJustificationType(juce::Justification::centred);
    m_freqLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xFF2A2A4E));
    m_freqLabel.setColour(juce::Label::outlineColourId, juce::Colour(0xFF555577));
    addAndMakeVisible(m_freqLabel);

    // Update label whenever the knob changes
    m_freqKnob.onValueChange = [this]() {
        const float freq = static_cast<float>(m_freqKnob.getValue());
        m_freqLabel.setText(freqToDisplayString(freq), juce::dontSendNotification);
        if (onFreqChanged)
            onFreqChanged(freq);
    };

    // Parse user-typed label text and push to APVTS
    m_freqLabel.onTextChange = [this]() {
        const float parsed = parseFreqInput(m_freqLabel.getText());
        if (parsed > 0.0f) {
            if (auto* param = m_apvts.getParameter(m_freqParamID))
                param->setValueNotifyingHost(param->convertTo0to1(parsed));
        }
        // Always refresh label to reflect actual parameter value
        m_freqLabel.setText(freqToDisplayString(static_cast<float>(m_freqKnob.getValue())),
                            juce::dontSendNotification);
    };

    // Initialise label text from current parameter value
    m_freqLabel.setText(freqToDisplayString(static_cast<float>(m_freqKnob.getValue())),
                        juce::dontSendNotification);
}

DisplayFilterStrip::~DisplayFilterStrip() = default;

// ============================================================================
// Layout
// ============================================================================

void DisplayFilterStrip::resized() {
    auto area = getLocalBounds();

    // Toggle button (fixed width)
    m_enableButton.setBounds(area.removeFromLeft(64));
    area.removeFromLeft(4);

    // Rotary knob (square, centred vertically)
    const int knobSize = juce::jmin(area.getHeight(), 36);
    m_freqKnob.setBounds(area.removeFromLeft(knobSize)
                              .withSizeKeepingCentre(knobSize, knobSize));
    area.removeFromLeft(4);

    // Frequency label fills remaining width
    m_freqLabel.setBounds(area);
}

// ============================================================================
// Public helpers
// ============================================================================

float DisplayFilterStrip::getFrequency() const {
    return static_cast<float>(m_freqKnob.getValue());
}

void DisplayFilterStrip::setFrequency(float freq, bool notify) {
    if (auto* param = m_apvts.getParameter(m_freqParamID)) {
        if (notify) {
            param->setValueNotifyingHost(param->convertTo0to1(freq));
        } else {
            m_freqKnob.setValue(freq, juce::dontSendNotification);
            m_freqLabel.setText(freqToDisplayString(freq), juce::dontSendNotification);
        }
    }
}
