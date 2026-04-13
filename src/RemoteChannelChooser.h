#pragma once

#include "../lib/network/CtrlBroadcaster.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

/**
 * RemoteChannelChooserComponent
 *
 * A popup component displaying 8 rows (Ch 1..8), each with:
 *  - a toggle checkbox to enable/disable the channel
 *  - channel text ("Ch N")
 *  - a small colour swatch
 *  - a label text showing the remote's channelLabel (or "—" when offline)
 *
 * Maintains a uint8_t mask (bits 0..7 correspond to Ch 1..8).
 * Calls onMaskChanged whenever any toggle changes.
 */
class RemoteChannelChooserComponent : public juce::Component {
  public:
    /** Called whenever the enabled mask changes. */
    std::function<void(uint8_t newMask)> onMaskChanged;

    explicit RemoteChannelChooserComponent(uint8_t initialMask = 0xFF);
    ~RemoteChannelChooserComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Update the infos used for label + colour display (call every frame). */
    void updateRemoteInfos(const std::vector<phu::network::RemoteInstanceInfo>& infos);

    /** Read/write the current mask. */
    uint8_t getMask() const { return m_mask; }
    void    setMask(uint8_t mask);

    static constexpr int NUM_CHANNELS   = 8;
    static constexpr int ROW_HEIGHT     = 26;
    static constexpr int BUTTON_ROW_H   = 26;
    static constexpr int PREFERRED_W    = 280;
    static constexpr int PREFERRED_H    = NUM_CHANNELS * ROW_HEIGHT + BUTTON_ROW_H + 12;

  private:
    uint8_t m_mask = 0xFF;

    // Per-channel row widgets
    juce::ToggleButton m_checkboxes[NUM_CHANNELS];

    // "All" and "None" quick-toggle buttons
    juce::TextButton m_allButton  { "All"  };
    juce::TextButton m_noneButton { "None" };

    // Per-channel cached display data (label + colour), keyed by channel index [0..7]
    struct ChannelInfo {
        juce::String label  { "\xe2\x80\x94" }; // em dash UTF-8
        juce::Colour colour { 0xFF888888 };
    };
    ChannelInfo m_channelInfos[NUM_CHANNELS];
    int64_t     m_channelLastSeenMs[NUM_CHANNELS]{};

    void notifyMaskChanged();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RemoteChannelChooserComponent)
};
