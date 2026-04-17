#include "OpenGLScopeRenderer.h"

using namespace juce::gl;

// ============================================================================
// Construction / Destruction
// ============================================================================

OpenGLScopeRenderer::OpenGLScopeRenderer() = default;

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
    m_glContext.attachTo(component);
}

void OpenGLScopeRenderer::detach() {
    m_glContext.detach();
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
    int localSlotIndex)
{
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
    m_available.store(true, std::memory_order_release);
}

// ============================================================================
// OpenGLRenderer — Context Closing
// ============================================================================

void OpenGLScopeRenderer::openGLContextClosing() {
    m_shader.reset();
    deleteVBOs();
    m_available.store(false, std::memory_order_release);
}

// ============================================================================
// OpenGLRenderer — Render
// ============================================================================

void OpenGLScopeRenderer::renderOpenGL() {
    if (!m_shader || m_targetComponent == nullptr) return;

    // Swap in the latest frame data
    {
        const juce::SpinLock::ScopedLockType lock(m_dataLock);
        if (m_newDataAvailable) {
            m_renderData = m_pendingData;
            m_newDataAvailable = false;
        }
    }

    const auto scale = static_cast<float>(m_glContext.getRenderingScale());
    const int w = juce::roundToInt(scale * static_cast<float>(m_targetComponent->getWidth()));
    const int h = juce::roundToInt(scale * static_cast<float>(m_targetComponent->getHeight()));

    if (w <= 0 || h <= 0) return;

    // Clear to match the ScopeDisplay background colour (0xFF1A1A2E)
    juce::OpenGLHelpers::clear(juce::Colour(0xFF1A1A2E));
    glViewport(0, 0, w, h);

    // Enable blending for alpha
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Enable line smoothing for anti-aliased lines
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    m_shader->use();

    // Draw grid
    drawGrid(m_renderData.displayRangeBeats);

    // Upload and draw waveforms
    const int localSlot = m_renderData.localSlotIndex;

    // Remote waveforms first (underneath)
    if (m_renderData.showRemote && !m_renderData.broadcastOnly) {
        for (int i = 0; i < MAX_INSTANCES; ++i) {
            const auto& inst = m_renderData.instances[static_cast<size_t>(i)];
            if (inst.isLocal || !inst.active) continue;
            uploadWaveform(i, inst.bins, DISPLAY_BINS, m_renderData.amplitudeScale);
            drawWaveform(i, DISPLAY_BINS, inst.r, inst.g, inst.b, inst.a);
        }
    }

    // Local waveform on top
    if (m_renderData.showLocal && !m_renderData.broadcastOnly) {
        const auto& localInst = m_renderData.instances[static_cast<size_t>(localSlot)];
        if (localInst.active) {
            uploadWaveform(localSlot, localInst.bins, DISPLAY_BINS, m_renderData.amplitudeScale);
            drawWaveform(localSlot, DISPLAY_BINS, localInst.r, localInst.g, localInst.b, localInst.a);
        }
    }

    // Playhead
    if (!m_renderData.broadcastOnly)
        drawPlayhead(m_renderData.displayRangeBeats, m_renderData.currentPpq);

    glDisable(GL_BLEND);
    glDisable(GL_LINE_SMOOTH);
}

// ============================================================================
// Shader Creation
// ============================================================================

bool OpenGLScopeRenderer::createShaders() {
    // Vertex shader: takes position attribute vec2(x, y) in clip space [-1, 1]
    // and a uniform colour. This is a simple pass-through.
    const char* vertexShader = R"(
        attribute vec2 aPosition;
        void main() {
            gl_Position = vec4(aPosition, 0.0, 1.0);
        }
    )";

    // Fragment shader: flat colour from uniform
    const char* fragmentShader = R"(
        uniform vec4 uColour;
        void main() {
            gl_FragColor = uColour;
        }
    )";

    m_shader = std::make_unique<juce::OpenGLShaderProgram>(m_glContext);

    if (!m_shader->addVertexShader(vertexShader)) {
        DBG("OpenGLScopeRenderer: vertex shader failed: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }

    if (!m_shader->addFragmentShader(fragmentShader)) {
        DBG("OpenGLScopeRenderer: fragment shader failed: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }

    if (!m_shader->link()) {
        DBG("OpenGLScopeRenderer: shader link failed: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }

    return true;
}

// ============================================================================
// VBO Management
// ============================================================================

void OpenGLScopeRenderer::createVBOs() {
    // One VBO per waveform instance
    m_glContext.extensions.glGenBuffers(MAX_INSTANCES, m_vbos);

    // Pre-allocate each VBO with DISPLAY_BINS * 2 floats (x, y interleaved)
    const size_t bufferBytes = static_cast<size_t>(DISPLAY_BINS) * 2 * sizeof(float);
    for (int i = 0; i < MAX_INSTANCES; ++i) {
        m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_vbos[i]);
        m_glContext.extensions.glBufferData(GL_ARRAY_BUFFER,
                                            static_cast<GLsizeiptr>(bufferBytes),
                                            nullptr, GL_DYNAMIC_DRAW);
    }

    // Grid VBO
    m_glContext.extensions.glGenBuffers(1, &m_gridVbo);

    // Playhead VBO
    m_glContext.extensions.glGenBuffers(1, &m_playheadVbo);

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

void OpenGLScopeRenderer::uploadWaveform(int index, const float* bins, int numBins,
                                          float ampScale) {
    if (index < 0 || index >= MAX_INSTANCES || bins == nullptr || numBins < 2)
        return;

    // Build interleaved (x, y) vertex data
    // x: map bin index [0, numBins) to clip space [-1, 1]
    // y: sample * ampScale clamped to [-1, 1]
    std::vector<float> vertices(static_cast<size_t>(numBins) * 2);
    const float invN = 2.0f / static_cast<float>(numBins - 1);

    for (int i = 0; i < numBins; ++i) {
        const float x = -1.0f + static_cast<float>(i) * invN;
        float y = bins[i] * ampScale;
        y = juce::jlimit(-1.0f, 1.0f, y);
        vertices[static_cast<size_t>(i) * 2]     = x;
        vertices[static_cast<size_t>(i) * 2 + 1] = y;
    }

    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_vbos[index]);
    m_glContext.extensions.glBufferSubData(
        GL_ARRAY_BUFFER, 0,
        static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
        vertices.data());
    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ============================================================================
// Drawing Helpers (GL thread)
// ============================================================================

void OpenGLScopeRenderer::drawWaveform(int index, int numBins,
                                        float r, float g, float b, float a) {
    if (index < 0 || index >= MAX_INSTANCES || numBins <= 1) return;

    m_shader->setUniform("uColour", r, g, b, a);

    auto posAttr = juce::OpenGLShaderProgram::Attribute(*m_shader, "aPosition");
    if (posAttr.attributeID < 0) return;

    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_vbos[index]);
    m_glContext.extensions.glEnableVertexAttribArray(static_cast<GLuint>(posAttr.attributeID));
    m_glContext.extensions.glVertexAttribPointer(
        static_cast<GLuint>(posAttr.attributeID),
        2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_LINE_STRIP, 0, numBins);

    m_glContext.extensions.glDisableVertexAttribArray(static_cast<GLuint>(posAttr.attributeID));
    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void OpenGLScopeRenderer::drawGrid(double displayRangeBeats) {
    // Build grid lines in clip space
    std::vector<float> vertices;

    // Horizontal grid lines at amplitude levels [-1, -0.5, 0, 0.5, 1]
    const float levels[] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
    for (float lv : levels) {
        vertices.push_back(-1.0f); vertices.push_back(lv);
        vertices.push_back( 1.0f); vertices.push_back(lv);
    }

    // Vertical grid lines at beat divisions
    if (displayRangeBeats > 0.0) {
        const int numBeats = juce::jmax(1, static_cast<int>(displayRangeBeats));
        for (int i = 0; i <= numBeats; ++i) {
            const float x = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(numBeats);
            vertices.push_back(x); vertices.push_back(-1.0f);
            vertices.push_back(x); vertices.push_back( 1.0f);
        }
    }

    m_gridVertexCount = static_cast<int>(vertices.size()) / 2;

    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
    m_glContext.extensions.glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
        vertices.data(), GL_DYNAMIC_DRAW);

    // Grid colour: 0xFF333355 → normalised
    m_shader->setUniform("uColour",
                         0x33 / 255.0f, 0x33 / 255.0f, 0x55 / 255.0f, 1.0f);

    auto posAttr = juce::OpenGLShaderProgram::Attribute(*m_shader, "aPosition");
    if (posAttr.attributeID < 0) return;

    m_glContext.extensions.glEnableVertexAttribArray(static_cast<GLuint>(posAttr.attributeID));
    m_glContext.extensions.glVertexAttribPointer(
        static_cast<GLuint>(posAttr.attributeID),
        2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_LINES, 0, m_gridVertexCount);

    m_glContext.extensions.glDisableVertexAttribArray(static_cast<GLuint>(posAttr.attributeID));
    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void OpenGLScopeRenderer::drawPlayhead(double displayRangeBeats, double currentPpq) {
    if (displayRangeBeats <= 0.0) return;

    double normPos = std::fmod(currentPpq, displayRangeBeats) / displayRangeBeats;
    if (normPos < 0.0) normPos += 1.0;

    const float x = -1.0f + 2.0f * static_cast<float>(normPos);

    float vertices[] = { x, -1.0f, x, 1.0f };

    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_playheadVbo);
    m_glContext.extensions.glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(sizeof(vertices)),
        vertices, GL_DYNAMIC_DRAW);

    // Playhead colour: white with some transparency (0xAAFFFFFF)
    m_shader->setUniform("uColour", 1.0f, 1.0f, 1.0f, 0xAA / 255.0f);

    auto posAttr = juce::OpenGLShaderProgram::Attribute(*m_shader, "aPosition");
    if (posAttr.attributeID < 0) return;

    m_glContext.extensions.glEnableVertexAttribArray(static_cast<GLuint>(posAttr.attributeID));
    m_glContext.extensions.glVertexAttribPointer(
        static_cast<GLuint>(posAttr.attributeID),
        2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_LINES, 0, 2);

    m_glContext.extensions.glDisableVertexAttribArray(static_cast<GLuint>(posAttr.attributeID));
    m_glContext.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}
