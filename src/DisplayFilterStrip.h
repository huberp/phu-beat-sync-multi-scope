#pragma once

#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

/**
 * DisplayFilterStrip — a reusable HP/LP filter control strip.
 *
 * Each strip contains:
 *   - A toggle button (enable/disable the filter), APVTS-attached
 *   - A rotary knob (cutoff frequency), APVTS-attached
 *   - An editable label showing frequency in Hz or as a musical note name
 *
 * The label accepts user input in both numeric Hz form ("80 Hz" or "80")
 * and note-name form ("A2", "C#3", "Bb4"). Values are parsed and pushed
 * back to the APVTS parameter.
 *
 * onFreqChanged is called (on the message thread) whenever the frequency
 * changes, giving the editor a chance to enforce the HP < LP constraint.
 */
class DisplayFilterStrip : public juce::Component {
  public:
    DisplayFilterStrip(const juce::String& toggleText,
                       juce::AudioProcessorValueTreeState& apvts,
                       const juce::String& enabledParamID,
                       const juce::String& freqParamID);
    ~DisplayFilterStrip() override;

    void resized() override;

    /** Called after the frequency parameter changes (for constraint enforcement). */
    std::function<void(float)> onFreqChanged;

    /** Returns the current frequency value from the knob. */
    float getFrequency() const;

    /**
     * Programmatically set the frequency (e.g., for constraint clamping).
     * If notify is true, the APVTS parameter is updated via setValueNotifyingHost.
     */
    void setFrequency(float freq, bool notify = true);

    /** Convert a frequency in Hz to a display string: "A2 / 110 Hz" or "80 Hz". */
    static juce::String freqToDisplayString(float freq);

    /** Parse a user-entered string to Hz. Returns -1 on failure. */
    static float parseFreqInput(const juce::String& input);

  private:
    juce::AudioProcessorValueTreeState& m_apvts;
    juce::String m_freqParamID;

    juce::ToggleButton m_enableButton;
    juce::Slider m_freqKnob;
    juce::Label m_freqLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> m_enableAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> m_freqAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DisplayFilterStrip)
};
