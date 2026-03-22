#include "ScopeDisplay.h"

// ============================================================================
// Construction
// ============================================================================

ScopeDisplay::ScopeDisplay() {
    setOpaque(true);
}

// ============================================================================
// Data Updates (called from UI timer)
// ============================================================================

void ScopeDisplay::setLocalData(const float* data, int numBins) {
    m_localData.assign(data, data + numBins);
}

void ScopeDisplay::setRemoteData(
    const std::vector<SampleBroadcaster::RemoteSampleData>& remoteData) {
    m_remoteData = remoteData;
}

// ============================================================================
// Painting
// ============================================================================

void ScopeDisplay::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();

    // Background
    g.fillAll(juce::Colour(0xFF1A1A2E));

    // Grid lines
    drawGrid(g, bounds);

    // Remote waveforms (drawn first, underneath local)
    if (m_showRemote && !m_remoteData.empty()) {
        for (int i = 0; i < static_cast<int>(m_remoteData.size()); ++i) {
            const auto& remote = m_remoteData[static_cast<size_t>(i)];
            if (!remote.samples.empty()) {
                drawWaveform(g, bounds, remote.samples.data(),
                             static_cast<int>(remote.samples.size()),
                             getRemoteColour(i), 0.5f);
            }
        }
    }

    // Local waveform (on top)
    if (m_showLocal && !m_localData.empty()) {
        drawWaveform(g, bounds, m_localData.data(),
                     static_cast<int>(m_localData.size()),
                     juce::Colour(0xFF00FF88), 1.0f);
    }

    // Playhead
    drawPlayhead(g, bounds);

    // Border
    g.setColour(juce::Colour(0xFF333355));
    g.drawRect(bounds, 1.0f);
}

// ============================================================================
// Grid
// ============================================================================

void ScopeDisplay::drawGrid(juce::Graphics& g, juce::Rectangle<float> area) {
    g.setColour(juce::Colour(0xFF333355));

    // Horizontal grid lines at linear levels
    const float levels[] = {1.0f, 0.5f, 0.0f, -0.5f, -1.0f};
    for (float lv : levels) {
        float y = sampleToY(lv, area.getY(), area.getHeight());
        g.drawHorizontalLine(static_cast<int>(y), area.getX(), area.getRight());
    }

    // Vertical grid lines at beat divisions
    if (m_displayRangeBeats > 0.0) {
        int numBeats = static_cast<int>(m_displayRangeBeats);
        if (numBeats < 1) numBeats = 1;
        for (int i = 0; i <= numBeats; ++i) {
            float x = area.getX() +
                      (static_cast<float>(i) / static_cast<float>(numBeats)) * area.getWidth();
            g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
        }
    }

    // Linear labels
    g.setColour(juce::Colour(0xFF666688));
    g.setFont(10.0f);
    const float labelLevels[] = {1.0f, 0.5f, 0.0f, -0.5f, -1.0f};
    for (float lv : labelLevels) {
        float y = sampleToY(lv, area.getY(), area.getHeight());
        juce::String text = (lv == 0.0f) ? "0" : juce::String(lv, 1);
        g.drawText(text,
                   static_cast<int>(area.getX() + 2), static_cast<int>(y - 6), 30, 12,
                   juce::Justification::centredLeft);
    }
}

// ============================================================================
// Waveform Drawing
// ============================================================================

void ScopeDisplay::drawWaveform(juce::Graphics& g, juce::Rectangle<float> area,
                                const float* data, int numBins,
                                juce::Colour colour, float alpha) {
    if (numBins < 2) return;

    juce::Path path;
    bool started = false;

    for (int i = 0; i < numBins; ++i) {
        float x = area.getX() + (static_cast<float>(i) / static_cast<float>(numBins - 1)) *
                                     area.getWidth();
        float y = sampleToY(data[i], area.getY(), area.getHeight());

        if (!started) {
            path.startNewSubPath(x, y);
            started = true;
        } else {
            path.lineTo(x, y);
        }
    }

    g.setColour(colour.withAlpha(alpha));
    g.strokePath(path, juce::PathStrokeType(1.5f));
}

// ============================================================================
// Playhead
// ============================================================================

void ScopeDisplay::drawPlayhead(juce::Graphics& g, juce::Rectangle<float> area) {
    if (m_displayRangeBeats <= 0.0) return;

    double normPos = std::fmod(m_currentPpq, m_displayRangeBeats) / m_displayRangeBeats;
    if (normPos < 0.0) normPos += 1.0;

    float x = area.getX() + static_cast<float>(normPos) * area.getWidth();

    g.setColour(juce::Colour(0xAAFFFFFF));
    g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
}

// ============================================================================
// Helpers
// ============================================================================

float ScopeDisplay::sampleToY(float sample, float top, float height) {
    // Map [-1, +1] to [bottom, top]: +1 at top, -1 at bottom
    float normalized = (sample + 1.0f) * 0.5f; // [-1,+1] → [0,1]
    normalized = juce::jlimit(0.0f, 1.0f, normalized);
    return top + (1.0f - normalized) * height;
}

juce::Colour ScopeDisplay::getRemoteColour(int index) {
    // Colour palette for remote instances
    static const juce::Colour colours[] = {
        juce::Colour(0xFFFF6B6B), // Red
        juce::Colour(0xFF4ECDC4), // Teal
        juce::Colour(0xFFFFE66D), // Yellow
        juce::Colour(0xFFA8E6CF), // Mint
        juce::Colour(0xFFFF8C94), // Salmon
        juce::Colour(0xFF88D8B0), // Light green
        juce::Colour(0xFFB8A9C9), // Lavender
        juce::Colour(0xFFF6CD61), // Gold
    };
    static const int numColours = sizeof(colours) / sizeof(colours[0]);
    return colours[index % numColours];
}
