#pragma once

#include "Event.h"

namespace phu {
namespace events {

struct BPMEvent : public Event {
    struct Values {
        double bpm = 0.0;
        double msecPerBeat = 0.0;
        double samplesPerBeat = 0.0;
    };

    Values oldValues;
    Values newValues;
};

struct IsPlayingEvent : public Event {
    bool oldValue = false;
    bool newValue = false;
};

struct SampleRateEvent : public Event {
    double oldRate = 0.0;
    double newRate = 0.0;
};

class GlobalsEventListener {
  public:
    virtual ~GlobalsEventListener() = default;

    virtual void onBPMChanged(const BPMEvent& event) { (void)event; }
    virtual void onIsPlayingChanged(const IsPlayingEvent& event) { (void)event; }
    virtual void onSampleRateChanged(const SampleRateEvent& event) { (void)event; }
};

} // namespace events
} // namespace phu
