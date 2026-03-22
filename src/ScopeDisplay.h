#pragma once

#include "../lib/audio/BeatSyncBuffer.h"
#include "../lib/network/SampleBroadcaster.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

using phu::audio::BeatSyncBuffer;
using phu::network::SampleBroadcaster;

/**
 * ScopeDisplay — beat-synced oscilloscope display component.
 *
 * Renders local BeatSyncBuffer data as a waveform trace indexed by musical position.
 * Also renders remote waveform data received via SampleBroadcaster, with an option
 * to show/hide remote traces (following the pattern from phu-splitter).
 *
 * The display shows dB-scale waveform data mapped to pixel coordinates:
 * - X axis: musical position [0, 1) normalized within display range
 * - Y axis: dB level [-60, 0] mapped to component height
 * - Playhead marker showing current PPQ position
 */
class ScopeDisplay : public juce::Component {
  public:
    ScopeDisplay();
    ~ScopeDisplay() override = default;

    void paint(juce::Graphics& g) override;

    /** Set local beat-synced waveform data for rendering (from BeatSyncBuffer). */
    void setLocalData(const float* data, int numBins);

    /** Set remote waveform data received from other instances. */
    void setRemoteData(const std::vector<SampleBroadcaster::RemoteSampleData>& remoteData);

    /** Enable or disable rendering of remote data (toggle). */
    void setRemoteDisplayEnabled(bool enabled) { m_showRemote = enabled; }
    bool isRemoteDisplayEnabled() const { return m_showRemote; }

    /** Set current PPQ position for playhead marker. */
    void setCurrentPpq(double ppq) { m_currentPpq = ppq; }

    /** Set display range in beats. */
    void setDisplayRangeBeats(double beats) { m_displayRangeBeats = beats; }

  private:
    // Local waveform data (copied from BeatSyncBuffer on UI thread)
    std::vector<float> m_localData;

    // Remote waveform data from other instances
    std::vector<SampleBroadcaster::RemoteSampleData> m_remoteData;

    // Display state
    bool m_showRemote = true;
    double m_currentPpq = 0.0;
    double m_displayRangeBeats = 4.0;

    // Drawing helpers
    void drawGrid(juce::Graphics& g, juce::Rectangle<float> area);
    void drawWaveform(juce::Graphics& g, juce::Rectangle<float> area,
                      const float* data, int numBins, juce::Colour colour, float alpha = 1.0f);
    void drawPlayhead(juce::Graphics& g, juce::Rectangle<float> area);

    // Map dB value to Y coordinate
    static float dbToY(float db, float top, float height);

    // Colour palette for remote instances
    static juce::Colour getRemoteColour(int index);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScopeDisplay)
};
