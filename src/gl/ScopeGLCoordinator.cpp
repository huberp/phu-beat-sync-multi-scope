#include "ScopeGLCoordinator.h"

using namespace juce::gl;

// ============================================================================
// Lifecycle
// ============================================================================

ScopeGLCoordinator::~ScopeGLCoordinator() {
    detach();
}

void ScopeGLCoordinator::attachTo(juce::Component& component) {
    m_targetComponent = &component;
    m_glContext.setRenderer(this);
    // Let JUCE's normal paint() still run for overlays (labels, broadcast text, etc.)
    m_glContext.setComponentPaintingEnabled(true);
    m_glContext.setContinuousRepainting(false);
    m_glContext.setMultisamplingEnabled(true);
    m_glContext.attachTo(component);
}

void ScopeGLCoordinator::detach() {
    m_glContext.detach();  // Blocks until the GL thread completes
    m_available.store(false, std::memory_order_release);
    m_targetComponent = nullptr;
}

// ============================================================================
// Typed data setters (UI thread)
// ============================================================================

void ScopeGLCoordinator::captureViewport() {
    if (m_targetComponent == nullptr) return;
    const auto w = static_cast<uint32_t>(m_targetComponent->getWidth());
    const auto h = static_cast<uint32_t>(m_targetComponent->getHeight());
    m_vpSize.store((static_cast<uint64_t>(w) << 32) | h, std::memory_order_release);
}

void ScopeGLCoordinator::setWaveformData(
    const std::array<WaveformInstanceData, WaveformGLRenderer::MAX_INSTANCES>& instances,
    float ampScale,
    bool  showLocal,
    bool  showRemote,
    bool  broadcastOnly,
    int   localSlotIndex)
{
    captureViewport();
    m_waveform.setData(instances, ampScale, showLocal, showRemote, broadcastOnly, localSlotIndex);
}

void ScopeGLCoordinator::setGridPlayheadData(double displayRangeBeats,
                                              double currentPpq,
                                              bool   broadcastOnly) {
    m_gridPlayhead.setData(displayRangeBeats, currentPpq, broadcastOnly);
}

void ScopeGLCoordinator::setRmsData(const float* values, int numBars,
                                     float ampScale, bool show) {
    m_rms.setData(values, numBars, ampScale, show);
}

// ============================================================================
// juce::OpenGLRenderer — Context Created (GL thread)
// ============================================================================

void ScopeGLCoordinator::newOpenGLContextCreated() {
    if (!m_waveform.create(m_glContext)) {
        m_available.store(false, std::memory_order_release);
        return;
    }

    if (!m_gridPlayhead.create(m_glContext)) {
        m_waveform.release();
        m_available.store(false, std::memory_order_release);
        return;
    }

    // RMS overlay is non-fatal: if its shader fails the waveform still works
    m_rms.create(m_glContext);

    m_available.store(true, std::memory_order_release);
}

// ============================================================================
// juce::OpenGLRenderer — Context Closing (GL thread)
// ============================================================================

void ScopeGLCoordinator::openGLContextClosing() {
    m_waveform.release();
    m_gridPlayhead.release();
    m_rms.release();
    m_available.store(false, std::memory_order_release);
}

// ============================================================================
// juce::OpenGLRenderer — Render (GL thread)
// ============================================================================

void ScopeGLCoordinator::renderOpenGL() {
    if (!m_waveform.isReady()) return;

    // Unpack viewport dimensions written by the UI thread
    const uint64_t vp      = m_vpSize.load(std::memory_order_acquire);
    const int logicalW     = static_cast<int>(vp >> 32);
    const int logicalH     = static_cast<int>(vp & 0xFFFFFFFFull);

    const auto scale = static_cast<float>(m_glContext.getRenderingScale());
    const int  w     = juce::roundToInt(scale * static_cast<float>(logicalW));
    const int  h     = juce::roundToInt(scale * static_cast<float>(logicalH));

    if (w <= 0 || h <= 0) return;

    // Clear to match the ScopeDisplay background colour (0xFF1A1A2E)
    juce::OpenGLHelpers::clear(juce::Colour(0xFF1A1A2E));
    glViewport(0, 0, w, h);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_gridPlayhead.draw();  // Grid + playhead (back)
    m_waveform.draw();      // Waveform lines
    m_rms.draw(h);          // RMS overlay (front)

    glDisable(GL_BLEND);
}
