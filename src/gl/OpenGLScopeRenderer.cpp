#include "OpenGLScopeRenderer.h"

using namespace juce::gl;

// ============================================================================
// Construction / Destruction
// ============================================================================

OpenGLScopeRenderer::OpenGLScopeRenderer() {
    m_gridStaging.reserve(128);
}

OpenGLScopeRenderer::~OpenGLScopeRenderer() {
    detach();
}

// ============================================================================
// Lifecycle
// ============================================================================

void OpenGLScopeRenderer::attachTo(juce::Component& component) {
    m_targetComponent = &component;
    m_glContext.setRenderer(this);
    // Let JUCE's normal paint() still work for overlays (RMS, cancellation, labels).
    // The OpenGL renderer draws only the waveform lines, grid, and playhead.
    m_glContext.setComponentPaintingEnabled(true);
    m_glContext.setContinuousRepainting(false);
    m_glContext.setMultisamplingEnabled(true);
    m_glContext.attachTo(component);
}

void OpenGLScopeRenderer::detach() {
    m_glContext.detach();  // Blocks until GL thread completes
    m_available.store(false, std::memory_order_release);
    m_targetComponent = nullptr;
}

// ============================================================================
// Data Upload (UI thread)
// ============================================================================

void OpenGLScopeRenderer::setFrameData(
    const std::array<WaveformInstance, MAX_INSTANCES>& instances,
    float amplitudeScale,
    double displayRangeBeats,
    double currentPpq,
    bool showLocal,
    bool showRemote,
    bool broadcastOnlyOverlay,
    int localSlotIndex,
    const float* rmsValues,
    int numRmsBars,
    bool showRms)
{
    // Capture viewport dimensions on UI thread (component is only safe here)
    int vpW = 0, vpH = 0;
    if (m_targetComponent != nullptr) {
        vpW = m_targetComponent->getWidth();
        vpH = m_targetComponent->getHeight();
    }

    const juce::SpinLock::ScopedLockType lock(m_dataLock);

    for (int i = 0; i < MAX_INSTANCES; ++i) {
        auto& dst = m_pendingData.instances[static_cast<size_t>(i)];
        const auto& src = instances[static_cast<size_t>(i)];
        dst.active  = src.active;
        dst.isLocal = src.isLocal;
        dst.r = src.colour.getFloatRed();
        dst.g = src.colour.getFloatGreen();
        dst.b = src.colour.getFloatBlue();
        dst.a = src.alpha;
        if (src.active && src.bins != nullptr)
            std::memcpy(dst.bins, src.bins, sizeof(float) * DISPLAY_BINS);
    }

    m_pendingData.amplitudeScale    = amplitudeScale;
    m_pendingData.displayRangeBeats = displayRangeBeats;
    m_pendingData.currentPpq        = currentPpq;
    m_pendingData.showLocal         = showLocal;
    m_pendingData.showRemote        = showRemote;
    m_pendingData.broadcastOnly     = broadcastOnlyOverlay;
    m_pendingData.localSlotIndex    = localSlotIndex;
    m_pendingData.viewportWidth     = vpW;
    m_pendingData.viewportHeight    = vpH;

    // RMS overlay
    m_pendingData.showRms    = showRms;
    m_pendingData.numRmsBars = 0;
    if (showRms && rmsValues != nullptr && numRmsBars > 0) {
        const int bars = juce::jmin(numRmsBars, RmsOverlayRenderer::MAX_RMS_BARS);
        std::memcpy(m_pendingData.rmsValues, rmsValues, static_cast<size_t>(bars) * sizeof(float));
        m_pendingData.numRmsBars = bars;
    }

    m_newDataAvailable = true;
}

// ============================================================================
// OpenGLRenderer — Context Created
// ============================================================================

void OpenGLScopeRenderer::newOpenGLContextCreated() {
    if (!createShaders()) {
        // Shader compilation failed — mark unavailable so ScopeDisplay falls back
        m_available.store(false, std::memory_order_release);
        return;
    }

    createVBOs();
    m_rmsRenderer.create(m_glContext);  // Non-fatal: RMS overlay just won't draw on failure
    m_available.store(true, std::memory_order_release);
}

// ============================================================================
// OpenGLRenderer — Context Closing
// ============================================================================

void OpenGLScopeRenderer::openGLContextClosing() {
    m_wave_aYValueLoc   = -1;
    m_wave_uColourLoc   = -1;
    m_wave_uNumBinsLoc  = -1;
    m_wave_uAmpScaleLoc = -1;
    m_waveShader.reset();

    m_util_aPositionLoc = -1;
    m_util_uColourLoc   = -1;
    m_utilShader.reset();

    m_rmsRenderer.release();

    deleteVBOs();
    m_lastGridRangeBeats = -1.0;
    m_available.store(false, std::memory_order_release);
}

// ============================================================================
// OpenGLRenderer — Render
// ============================================================================

void OpenGLScopeRenderer::renderOpenGL() {
    if (!m_waveShader || !m_utilShader) return;

    // Swap in the latest frame data
    {
        const juce::SpinLock::ScopedLockType lock(m_dataLock);
        if (m_newDataAvailable) {
            m_renderData = m_pendingData;
            m_newDataAvailable = false;
        }
    }

    const auto scale = static_cast<float>(m_glContext.getRenderingScale());
    const int w = juce::roundToInt(scale * static_cast<float>(m_renderData.viewportWidth));
    const int h = juce::roundToInt(scale * static_cast<float>(m_renderData.viewportHeight));

    if (w <= 0 || h <= 0) return;

    // Clear to match the ScopeDisplay background colour (0xFF1A1A2E)
    juce::OpenGLHelpers::clear(juce::Colour(0xFF1A1A2E));
    glViewport(0, 0, w, h);

    // Enable blending for alpha
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // ---- Grid and playhead (utility shader) ----
    m_utilShader->use();
    drawGrid(m_renderData.displayRangeBeats);
    if (!m_renderData.broadcastOnly)
        drawPlayhead(m_renderData.displayRangeBeats, m_renderData.currentPpq);

    // ---- Waveforms (waveform shader — Y-only VBO + gl_VertexID) ----
    m_waveShader->use();
    const int localSlot = m_renderData.localSlotIndex;

    // Remote waveforms first (underneath)
    if (m_renderData.showRemote && !m_renderData.broadcastOnly) {
        for (int i = 0; i < MAX_INSTANCES; ++i) {
            const auto& inst = m_renderData.instances[static_cast<size_t>(i)];
            if (inst.isLocal || !inst.active) continue;
            uploadWaveform(i, inst.bins, DISPLAY_BINS);
            drawWaveform(i, DISPLAY_BINS, m_renderData.amplitudeScale,
                         inst.r, inst.g, inst.b, inst.a);
        }
    }

    // Local waveform on top
    if (m_renderData.showLocal && !m_renderData.broadcastOnly) {
        const auto& localInst = m_renderData.instances[static_cast<size_t>(localSlot)];
        if (localInst.active) {
            uploadWaveform(localSlot, localInst.bins, DISPLAY_BINS);
            drawWaveform(localSlot, DISPLAY_BINS, m_renderData.amplitudeScale,
                         localInst.r, localInst.g, localInst.b, localInst.a);
        }
    }

    // ---- RMS overlay (RmsOverlayRenderer) ----
    if (m_renderData.showRms && m_rmsRenderer.isReady() && m_renderData.numRmsBars > 0) {
        m_rmsRenderer.draw(m_renderData.numRmsBars,
                           m_renderData.rmsValues,
                           m_renderData.amplitudeScale,
                           h);
    }

    glDisable(GL_BLEND);
}

// ============================================================================
// Shader Creation
// ============================================================================

bool OpenGLScopeRenderer::createShaders() {
    // Check GLSL version — gl_VertexID requires GLSL 1.30+ (OpenGL 3.0+)
    const double glslVersion = juce::OpenGLShaderProgram::getLanguageVersion();
    if (glslVersion < 1.30) {
        DBG("OpenGLScopeRenderer: GLSL version " + juce::String(glslVersion)
            + " too old (need 1.30+ for gl_VertexID)");
        return false;
    }

    // Select the appropriate GLSL version directive.
    // macOS Core Profile requires >= 150; other platforms accept 130+.
    juce::String versionLine;
    bool useModernOutput = false;  // Whether to use `out vec4` vs `gl_FragColor`

    if (glslVersion >= 3.30) {
        versionLine    = "#version 330\n";
        useModernOutput = true;
    } else if (glslVersion >= 1.50) {
        versionLine = "#version 150\n";
    } else {
        versionLine = "#version 130\n";
    }

    // ---- Waveform shader: Y-only input, X computed from gl_VertexID ----
    // The displayBins array is uploaded directly as a float-per-vertex VBO.
    // The shader maps each bin index to clip-space X via gl_VertexID.
    const juce::String waveVertSrc = versionLine + R"(
        in float yValue;
        uniform int   uNumBins;
        uniform float uAmpScale;
        void main() {
            float x = float(gl_VertexID) / float(max(uNumBins - 1, 1)) * 2.0 - 1.0;
            float y = clamp(yValue * uAmpScale, -1.0, 1.0);
            gl_Position = vec4(x, y, 0.0, 1.0);
        }
    )";

    // Fragment shader shared by both waveform and utility shaders (flat colour output)
    const juce::String commonFragSrc = versionLine
        + (useModernOutput ? "out vec4 fragColor;\n" : "")
        + R"(
        uniform vec4 uColour;
        void main() {
        )" + (useModernOutput ? "    fragColor = uColour;\n" : "    gl_FragColor = uColour;\n")
        + "}\n";

    m_waveShader = std::make_unique<juce::OpenGLShaderProgram>(m_glContext);

    if (!m_waveShader->addVertexShader(waveVertSrc)) {
        DBG("OpenGLScopeRenderer: wave vertex shader failed: " + m_waveShader->getLastError());
        m_waveShader.reset();
        return false;
    }
    if (!m_waveShader->addFragmentShader(commonFragSrc)) {
        DBG("OpenGLScopeRenderer: wave fragment shader failed: " + m_waveShader->getLastError());
        m_waveShader.reset();
        return false;
    }
    if (!m_waveShader->link()) {
        DBG("OpenGLScopeRenderer: wave shader link failed: " + m_waveShader->getLastError());
        m_waveShader.reset();
        return false;
    }

    // Cache waveform shader locations
    m_wave_aYValueLoc   = m_glContext.extensions.glGetAttribLocation(
        m_waveShader->getProgramID(), "yValue");
    m_wave_uColourLoc   = m_waveShader->getUniformIDFromName("uColour");
    m_wave_uNumBinsLoc  = m_waveShader->getUniformIDFromName("uNumBins");
    m_wave_uAmpScaleLoc = m_waveShader->getUniformIDFromName("uAmpScale");

    // ---- Utility shader: vec2 pass-through for grid and playhead ----
    const juce::String utilVertSrc = versionLine + R"(
        in vec2 aPosition;
        void main() {
            gl_Position = vec4(aPosition, 0.0, 1.0);
        }
    )";

    const juce::String utilFragSrc = commonFragSrc;  // Same flat-colour fragment shader

    m_utilShader = std::make_unique<juce::OpenGLShaderProgram>(m_glContext);

    if (!m_utilShader->addVertexShader(utilVertSrc)) {
        DBG("OpenGLScopeRenderer: util vertex shader failed: " + m_utilShader->getLastError());
        m_utilShader.reset();
        m_waveShader.reset();
        return false;
    }
    if (!m_utilShader->addFragmentShader(utilFragSrc)) {
        DBG("OpenGLScopeRenderer: util fragment shader failed: " + m_utilShader->getLastError());
        m_utilShader.reset();
        m_waveShader.reset();
        return false;
    }
    if (!m_utilShader->link()) {
        DBG("OpenGLScopeRenderer: util shader link failed: " + m_utilShader->getLastError());
        m_utilShader.reset();
        m_waveShader.reset();
        return false;
    }

    // Cache utility shader locations
    m_util_aPositionLoc = m_glContext.extensions.glGetAttribLocation(
        m_utilShader->getProgramID(), "aPosition");
    m_util_uColourLoc   = m_utilShader->getUniformIDFromName("uColour");

    return true;
}

// ============================================================================
// VBO Management
// ============================================================================

void OpenGLScopeRenderer::createVBOs() {
    // One VBO per waveform instance — each holds DISPLAY_BINS floats (Y-only)
    m_glContext.extensions.glGenBuffers(MAX_INSTANCES, m_vbos);

    const auto bufferBytes = static_cast<GLsizeiptr>(DISPLAY_BINS * sizeof(float));
    for (int i = 0; i < MAX_INSTANCES; ++i) {
        m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_vbos[i]);
        m_glContext.extensions.glBufferData(GL_ARRAY_BUFFER, bufferBytes,
                                            nullptr, GL_DYNAMIC_DRAW);
    }

    // Grid VBO (interleaved x, y pairs)
    m_glContext.extensions.glGenBuffers(1, &m_gridVbo);

    // Playhead VBO — 2 vertices (4 floats: x1,y1, x2,y2)
    m_glContext.extensions.glGenBuffers(1, &m_playheadVbo);
    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_playheadVbo);
    m_glContext.extensions.glBufferData(GL_ARRAY_BUFFER,
                                        static_cast<GLsizeiptr>(4 * sizeof(float)),
                                        nullptr, GL_DYNAMIC_DRAW);

    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_vbosCreated = true;
}

void OpenGLScopeRenderer::deleteVBOs() {
    if (!m_vbosCreated) return;

    m_glContext.extensions.glDeleteBuffers(MAX_INSTANCES, m_vbos);
    std::memset(m_vbos, 0, sizeof(m_vbos));

    m_glContext.extensions.glDeleteBuffers(1, &m_gridVbo);
    m_gridVbo = 0;

    m_glContext.extensions.glDeleteBuffers(1, &m_playheadVbo);
    m_playheadVbo = 0;

    m_vbosCreated = false;
}

// ============================================================================
// Waveform Upload (GL thread)
// ============================================================================

void OpenGLScopeRenderer::uploadWaveform(int index, const float* bins, int numBins) {
    if (index < 0 || index >= MAX_INSTANCES || bins == nullptr || numBins < 2)
        return;

    // Upload the raw Y-value array directly — no CPU-side transformation needed.
    // The vertex shader computes X from gl_VertexID and applies amplitude scaling.
    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_vbos[index]);
    m_glContext.extensions.glBufferSubData(
        GL_ARRAY_BUFFER, 0,
        static_cast<GLsizeiptr>(numBins * sizeof(float)),
        bins);
    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ============================================================================
// Drawing Helpers (GL thread)
// ============================================================================

void OpenGLScopeRenderer::drawWaveform(int index, int numBins, float ampScale,
                                        float r, float g, float b, float a) {
    if (index < 0 || index >= MAX_INSTANCES || numBins < 2 || m_wave_aYValueLoc < 0)
        return;

    // Set waveform-shader uniforms
    glUniform4f(m_wave_uColourLoc, r, g, b, a);
    glUniform1i(m_wave_uNumBinsLoc, numBins);
    glUniform1f(m_wave_uAmpScaleLoc, ampScale);

    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_vbos[index]);
    m_glContext.extensions.glEnableVertexAttribArray(
        static_cast<GLuint>(m_wave_aYValueLoc));
    m_glContext.extensions.glVertexAttribPointer(
        static_cast<GLuint>(m_wave_aYValueLoc),
        1, GL_FLOAT, GL_FALSE, 0, nullptr);  // 1 component per vertex (Y only)

    glDrawArrays(GL_LINE_STRIP, 0, numBins);

    m_glContext.extensions.glDisableVertexAttribArray(
        static_cast<GLuint>(m_wave_aYValueLoc));
    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void OpenGLScopeRenderer::drawGrid(double displayRangeBeats) {
    if (m_util_aPositionLoc < 0) return;

    // Only rebuild grid VBO when display range changes
    if (m_lastGridRangeBeats != displayRangeBeats) {
        m_lastGridRangeBeats = displayRangeBeats;

        m_gridStaging.clear();

        // Horizontal grid lines at amplitude levels [-1, -0.5, 0, 0.5, 1]
        const float levels[] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
        for (float lv : levels) {
            m_gridStaging.push_back(-1.0f); m_gridStaging.push_back(lv);
            m_gridStaging.push_back( 1.0f); m_gridStaging.push_back(lv);
        }

        // Vertical grid lines at beat divisions
        if (displayRangeBeats > 0.0) {
            const int numBeats = juce::jmax(1, static_cast<int>(displayRangeBeats));
            for (int i = 0; i <= numBeats; ++i) {
                const float x = -1.0f + 2.0f * static_cast<float>(i)
                                / static_cast<float>(numBeats);
                m_gridStaging.push_back(x); m_gridStaging.push_back(-1.0f);
                m_gridStaging.push_back(x); m_gridStaging.push_back( 1.0f);
            }
        }

        m_gridVertexCount = static_cast<int>(m_gridStaging.size()) / 2;

        m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
        m_glContext.extensions.glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(m_gridStaging.size() * sizeof(float)),
            m_gridStaging.data(), GL_STATIC_DRAW);
    } else {
        m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
    }

    if (m_gridVertexCount <= 0) {
        m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    // Grid colour: 0xFF333355
    glUniform4f(m_util_uColourLoc,
                0x33 / 255.0f, 0x33 / 255.0f, 0x55 / 255.0f, 1.0f);

    m_glContext.extensions.glEnableVertexAttribArray(
        static_cast<GLuint>(m_util_aPositionLoc));
    m_glContext.extensions.glVertexAttribPointer(
        static_cast<GLuint>(m_util_aPositionLoc),
        2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_LINES, 0, m_gridVertexCount);

    m_glContext.extensions.glDisableVertexAttribArray(
        static_cast<GLuint>(m_util_aPositionLoc));
    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void OpenGLScopeRenderer::drawPlayhead(double displayRangeBeats, double currentPpq) {
    if (displayRangeBeats <= 0.0 || m_util_aPositionLoc < 0) return;

    double normPos = std::fmod(currentPpq, displayRangeBeats) / displayRangeBeats;
    if (normPos < 0.0) normPos += 1.0;

    const float x = -1.0f + 2.0f * static_cast<float>(normPos);

    float vertices[] = { x, -1.0f, x, 1.0f };

    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_playheadVbo);
    m_glContext.extensions.glBufferSubData(
        GL_ARRAY_BUFFER, 0,
        static_cast<GLsizeiptr>(sizeof(vertices)),
        vertices);

    // Playhead colour: white with some transparency (0xAAFFFFFF)
    glUniform4f(m_util_uColourLoc, 1.0f, 1.0f, 1.0f, 0xAA / 255.0f);

    m_glContext.extensions.glEnableVertexAttribArray(
        static_cast<GLuint>(m_util_aPositionLoc));
    m_glContext.extensions.glVertexAttribPointer(
        static_cast<GLuint>(m_util_aPositionLoc),
        2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_LINES, 0, 2);

    m_glContext.extensions.glDisableVertexAttribArray(
        static_cast<GLuint>(m_util_aPositionLoc));
    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}
