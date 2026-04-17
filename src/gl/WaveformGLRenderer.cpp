#include "WaveformGLRenderer.h"

using namespace juce::gl;

// ============================================================================
// Lifecycle (GL thread)
// ============================================================================

bool WaveformGLRenderer::create(juce::OpenGLContext& ctx) {
    m_ctx = &ctx;

    // gl_VertexID requires GLSL 1.30+ (OpenGL 3.0)
    const double glslVersion = juce::OpenGLShaderProgram::getLanguageVersion();
    if (glslVersion < 1.30) {
        DBG("WaveformGLRenderer: GLSL " + juce::String(glslVersion)
            + " too old (need 1.30+ for gl_VertexID)");
        return false;
    }

    // Select GLSL version directive
    juce::String versionLine;
    bool         useModernOutput = false;

    if (glslVersion >= 3.30)      { versionLine = "#version 330\n"; useModernOutput = true; }
    else if (glslVersion >= 1.50) { versionLine = "#version 150\n"; }
    else                          { versionLine = "#version 130\n"; }

    // ---- Vertex shader: Y-only input, X computed from gl_VertexID ----
    const juce::String vertSrc = versionLine + R"(
        in float yValue;
        uniform int   uNumBins;
        uniform float uAmpScale;
        void main() {
            float x = float(gl_VertexID) / float(max(uNumBins - 1, 1)) * 2.0 - 1.0;
            float y = clamp(yValue * uAmpScale, -1.0, 1.0);
            gl_Position = vec4(x, y, 0.0, 1.0);
        }
    )";

    // ---- Fragment shader: flat colour ----
    const juce::String fragSrc = versionLine
        + (useModernOutput ? "out vec4 fragColor;\n" : "")
        + R"(
        uniform vec4 uColour;
        void main() {
        )"
        + (useModernOutput ? "    fragColor = uColour;\n" : "    gl_FragColor = uColour;\n")
        + "}\n";

    m_shader = std::make_unique<juce::OpenGLShaderProgram>(ctx);

    if (!m_shader->addVertexShader(vertSrc)) {
        DBG("WaveformGLRenderer: vertex shader failed: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }
    if (!m_shader->addFragmentShader(fragSrc)) {
        DBG("WaveformGLRenderer: fragment shader failed: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }
    if (!m_shader->link()) {
        DBG("WaveformGLRenderer: shader link failed: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }

    m_aYValueLoc   = ctx.extensions.glGetAttribLocation(m_shader->getProgramID(), "yValue");
    m_uColourLoc   = m_shader->getUniformIDFromName("uColour");
    m_uNumBinsLoc  = m_shader->getUniformIDFromName("uNumBins");
    m_uAmpScaleLoc = m_shader->getUniformIDFromName("uAmpScale");

    createVBOs();
    return true;
}

void WaveformGLRenderer::release() {
    deleteVBOs();
    m_shader.reset();
    m_aYValueLoc = m_uColourLoc = m_uNumBinsLoc = m_uAmpScaleLoc = -1;
    m_ctx = nullptr;
}

// ============================================================================
// Data Upload (UI thread)
// ============================================================================

void WaveformGLRenderer::setData(const std::array<InstanceData, MAX_INSTANCES>& instances,
                                  float ampScale,
                                  bool  showLocal,
                                  bool  showRemote,
                                  bool  broadcastOnly,
                                  int   localSlotIndex) {
    const juce::SpinLock::ScopedLockType lock(m_lock);

    for (int i = 0; i < MAX_INSTANCES; ++i) {
        const auto& src = instances[static_cast<size_t>(i)];
        auto& dst = m_pending.instances[static_cast<size_t>(i)];

        dst.active  = src.active;
        dst.isLocal = src.isLocal;
        dst.r = src.colour.getFloatRed();
        dst.g = src.colour.getFloatGreen();
        dst.b = src.colour.getFloatBlue();
        dst.a = src.alpha;

        if (src.active && src.bins != nullptr)
            std::memcpy(dst.bins, src.bins, sizeof(float) * DISPLAY_BINS);
    }

    m_pending.ampScale       = ampScale;
    m_pending.showLocal      = showLocal;
    m_pending.showRemote     = showRemote;
    m_pending.broadcastOnly  = broadcastOnly;
    m_pending.localSlotIndex = localSlotIndex;
    m_newData = true;
}

// ============================================================================
// Drawing (GL thread)
// ============================================================================

void WaveformGLRenderer::draw() {
    if (!m_shader) return;

    // Swap in the latest snapshot
    {
        const juce::SpinLock::ScopedLockType lock(m_lock);
        if (m_newData) {
            m_render  = m_pending;
            m_newData = false;
        }
    }

    m_shader->use();
    const int localSlot = m_render.localSlotIndex;

    // Remote waveforms first (underneath)
    if (m_render.showRemote && !m_render.broadcastOnly) {
        for (int i = 0; i < MAX_INSTANCES; ++i) {
            const auto& inst = m_render.instances[static_cast<size_t>(i)];
            if (inst.isLocal || !inst.active) continue;
            uploadAndDraw(i, inst.bins, DISPLAY_BINS, m_render.ampScale,
                          inst.r, inst.g, inst.b, inst.a);
        }
    }

    // Local waveform on top
    if (m_render.showLocal && !m_render.broadcastOnly) {
        const auto& localInst = m_render.instances[static_cast<size_t>(localSlot)];
        if (localInst.active)
            uploadAndDraw(localSlot, localInst.bins, DISPLAY_BINS, m_render.ampScale,
                          localInst.r, localInst.g, localInst.b, localInst.a);
    }
}

// ============================================================================
// VBO Management (GL thread)
// ============================================================================

void WaveformGLRenderer::createVBOs() {
    m_ctx->extensions.glGenBuffers(MAX_INSTANCES, m_vbos);

    const auto bytes = static_cast<GLsizeiptr>(DISPLAY_BINS * sizeof(float));
    for (int i = 0; i < MAX_INSTANCES; ++i) {
        m_ctx->extensions.glBindBuffer(GL_ARRAY_BUFFER, m_vbos[i]);
        m_ctx->extensions.glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_DYNAMIC_DRAW);
    }

    m_ctx->extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_vbosCreated = true;
}

void WaveformGLRenderer::deleteVBOs() {
    if (!m_vbosCreated || m_ctx == nullptr) return;

    m_ctx->extensions.glDeleteBuffers(MAX_INSTANCES, m_vbos);
    std::memset(m_vbos, 0, sizeof(m_vbos));
    m_vbosCreated = false;
}

// ============================================================================
// Waveform Draw Helper (GL thread)
// ============================================================================

void WaveformGLRenderer::uploadAndDraw(int index, const float* bins, int numBins,
                                        float ampScale,
                                        float r, float g, float b, float a) {
    if (index < 0 || index >= MAX_INSTANCES || bins == nullptr
            || numBins < 2 || m_aYValueLoc < 0)
        return;

    // Upload Y-only array — vertex shader computes X from gl_VertexID
    m_ctx->extensions.glBindBuffer(GL_ARRAY_BUFFER, m_vbos[index]);
    m_ctx->extensions.glBufferSubData(GL_ARRAY_BUFFER, 0,
        static_cast<GLsizeiptr>(numBins * sizeof(float)), bins);

    glUniform4f(m_uColourLoc,  r, g, b, a);
    glUniform1i(m_uNumBinsLoc, numBins);
    glUniform1f(m_uAmpScaleLoc, ampScale);

    m_ctx->extensions.glEnableVertexAttribArray(static_cast<GLuint>(m_aYValueLoc));
    m_ctx->extensions.glVertexAttribPointer(
        static_cast<GLuint>(m_aYValueLoc),
        1, GL_FLOAT, GL_FALSE, 0, nullptr);  // 1 component per vertex (Y only)

    glDrawArrays(GL_LINE_STRIP, 0, numBins);

    m_ctx->extensions.glDisableVertexAttribArray(static_cast<GLuint>(m_aYValueLoc));
    m_ctx->extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}
