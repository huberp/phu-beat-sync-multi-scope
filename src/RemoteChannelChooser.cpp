#include "RemoteChannelChooser.h"

// ============================================================================
// Construction
// ============================================================================

RemoteChannelChooserComponent::RemoteChannelChooserComponent(uint8_t initialMask)
    : m_mask(initialMask)
{
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        const bool enabled = (m_mask >> ch) & 1;
        m_checkboxes[ch].setButtonText("Ch " + juce::String(ch + 1));
        m_checkboxes[ch].setToggleState(enabled, juce::dontSendNotification);
        m_checkboxes[ch].onClick = [this, ch]() {
            const bool on = m_checkboxes[ch].getToggleState();
            if (on)
                m_mask = static_cast<uint8_t>(m_mask | (1u << ch));
            else
                m_mask = static_cast<uint8_t>(m_mask & ~(1u << ch));
            notifyMaskChanged();
        };
        addAndMakeVisible(m_checkboxes[ch]);
    }

    m_allButton.onClick = [this]() {
        setMask(0xFF);
    };
    addAndMakeVisible(m_allButton);

    m_noneButton.onClick = [this]() {
        setMask(0x00);
    };
    addAndMakeVisible(m_noneButton);
}

// ============================================================================
// Layout
// ============================================================================

void RemoteChannelChooserComponent::resized()
{
    auto area = getLocalBounds().reduced(6, 6);

    // "All" / "None" buttons row at the top
    auto btnRow = area.removeFromTop(BUTTON_ROW_H);
    m_allButton.setBounds(btnRow.removeFromLeft(60));
    btnRow.removeFromLeft(6);
    m_noneButton.setBounds(btnRow.removeFromLeft(60));

    area.removeFromTop(4);

    // One row per channel
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        auto row = area.removeFromTop(ROW_HEIGHT);
        // Checkbox + label text (the ToggleButton text is "Ch N")
        m_checkboxes[ch].setBounds(row.removeFromLeft(80));
        // remaining row is the colour swatch + label — drawn in paint()
    }
}

// ============================================================================
// Paint (colour swatches + remote labels)
// ============================================================================

void RemoteChannelChooserComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xFF1E2A3A));

    auto area = getLocalBounds().reduced(6, 6);
    area.removeFromTop(BUTTON_ROW_H + 4); // skip button row + gap

    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        auto row = area.removeFromTop(ROW_HEIGHT);
        row.removeFromLeft(80); // skip checkbox area

        // Colour swatch (20×16)
        auto swatchArea = row.removeFromLeft(20).reduced(0, 4);
        g.setColour(m_channelInfos[ch].colour);
        g.fillRect(swatchArea);
        g.setColour(juce::Colours::white.withAlpha(0.3f));
        g.drawRect(swatchArea, 1);

        row.removeFromLeft(4);

        // Remote label text
        g.setColour(juce::Colours::lightgrey);
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawText(m_channelInfos[ch].label, row, juce::Justification::centredLeft, true);
    }
}

// ============================================================================
// Public API
// ============================================================================

void RemoteChannelChooserComponent::setMask(uint8_t mask)
{
    if (m_mask == mask) return;
    m_mask = mask;
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        const bool enabled = (m_mask >> ch) & 1;
        m_checkboxes[ch].setToggleState(enabled, juce::dontSendNotification);
    }
    notifyMaskChanged();
}

void RemoteChannelChooserComponent::updateRemoteInfos(
    const std::vector<phu::network::RemoteInstanceInfo>& infos)
{
    // For each channel [0..7], pick the most recently seen online instance
    // advertising instanceIndex == ch+1.
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        m_channelInfos[ch].label    = "\xe2\x80\x94"; // em dash: "—"
        m_channelInfos[ch].colour   = juce::Colour(0xFF888888);
        m_channelLastSeenMs[ch]     = 0;
    }

    for (const auto& info : infos) {
        if (!info.isOnline) continue;
        const int ch = static_cast<int>(info.instanceIndex) - 1;
        if (ch < 0 || ch >= NUM_CHANNELS) continue;

        // Pick the most recently seen instance for this channel
        if (m_channelLastSeenMs[ch] <= info.lastSeenMs) {
            m_channelLastSeenMs[ch] = info.lastSeenMs;
            const char* lbl = info.channelLabel;
            m_channelInfos[ch].label = (lbl[0] != '\0')
                                           ? juce::String::fromUTF8(lbl)
                                           : juce::String("\xe2\x80\x94");
            m_channelInfos[ch].colour = juce::Colour(
                info.colourRGBA[0], info.colourRGBA[1],
                info.colourRGBA[2], info.colourRGBA[3]);
        }
    }

    repaint();
}

// ============================================================================
// Private helpers
// ============================================================================

void RemoteChannelChooserComponent::notifyMaskChanged()
{
    if (onMaskChanged)
        onMaskChanged(m_mask);
}
