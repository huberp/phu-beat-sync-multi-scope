#include "GridPlayheadGLRenderer.h"

using namespace juce::gl;

// ============================================================================
// Lifecycle (GL thread)
// ============================================================================

bool GridPlayheadGLRenderer::create(juce::OpenGLContext& ctx) {
    m_ctx = &ctx;

    if (!GLSLShaderBuilder::validateGLSLVersion("GridPlayheadGLRenderer")) {
        return false;
    }

    // ---- Vertex shader: vec2 pass-through ----
    const juce::String vertSrc = GLSLShaderBuilder::getVersionDirective() + R"(
        in vec2 aPosition;
        void main() {
            gl_Position = vec4(aPosition, 0.0, 1.0);
        }
    )";

    const juce::String fragSrc = GLSLShaderBuilder::buildFragmentShader(
        "uniform vec4 uColour;\n",
        "    fragColor = uColour;\n");

    if (!compileShader(vertSrc, fragSrc, "GridPlayheadGLRenderer")) {
        return false;
    }

    m_aPositionLoc = ctx.extensions.glGetAttribLocation(m_shader->getProgramID(), "aPosition");
    m_uColourLoc   = m_shader->getUniformIDFromName("uColour");

    // Allocate grid VBO (size determined on first draw when range is known)
    ctx.extensions.glGenBuffers(1, &m_gridVbo);

    // Allocate playhead VBO: 2 vertices (4 floats: x1,y1, x2,y2)
    ctx.extensions.glGenBuffers(1, &m_playheadVbo);
    ctx.extensions.glBindBuffer(GL_ARRAY_BUFFER, m_playheadVbo);
    ctx.extensions.glBufferData(GL_ARRAY_BUFFER,
                                static_cast<GLsizeiptr>(4 * sizeof(float)),
                                nullptr, GL_DYNAMIC_DRAW);
    ctx.extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void GridPlayheadGLRenderer::release() {
    if (m_ctx != nullptr) {
        if (m_gridVbo     != 0) { m_ctx->extensions.glDeleteBuffers(1, &m_gridVbo);     m_gridVbo     = 0; }
        if (m_playheadVbo != 0) { m_ctx->extensions.glDeleteBuffers(1, &m_playheadVbo); m_playheadVbo = 0; }
    }

    m_aPositionLoc = m_uColourLoc = -1;
    m_lastGridRangeBeats = -1.0;
    m_gridVertexCount    = 0;
    clearPendingSnapshot();
    GLRendererBase::release();
}

// ============================================================================
// Data Upload (UI thread)
// ============================================================================

void GridPlayheadGLRenderer::setData(double displayRangeBeats,
                                      double currentPpq,
                                      bool   broadcastOnly) {
    Snapshot snapshot;
    snapshot.displayRangeBeats = displayRangeBeats;
    snapshot.currentPpq        = currentPpq;
    snapshot.broadcastOnly     = broadcastOnly;
    setSnapshot(snapshot);
}

// ============================================================================
// Drawing (GL thread)
// ============================================================================

void GridPlayheadGLRenderer::draw() {
    if (!isReady()) return;

    swapSnapshot();

    m_shader->use();
    drawGrid();

    if (!m_render.broadcastOnly)
        drawPlayhead();
}

void GridPlayheadGLRenderer::drawGrid() {
    if (m_aPositionLoc < 0) return;

    // Rebuild grid VBO only when the display range changes
    if (m_lastGridRangeBeats != m_render.displayRangeBeats) {
        m_lastGridRangeBeats = m_render.displayRangeBeats;
        m_gridStaging.clear();

        // Horizontal grid lines at amplitude levels [-1, -0.5, 0, 0.5, 1]
        const float levels[] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
        for (float lv : levels) {
            m_gridStaging.push_back(-1.0f); m_gridStaging.push_back(lv);
            m_gridStaging.push_back( 1.0f); m_gridStaging.push_back(lv);
        }

        // Vertical grid lines at beat divisions
        if (m_render.displayRangeBeats > 0.0) {
            const int numBeats = juce::jmax(1, static_cast<int>(m_render.displayRangeBeats));
            for (int i = 0; i <= numBeats; ++i) {
                const float x = -1.0f + 2.0f * static_cast<float>(i)
                                / static_cast<float>(numBeats);
                m_gridStaging.push_back(x); m_gridStaging.push_back(-1.0f);
                m_gridStaging.push_back(x); m_gridStaging.push_back( 1.0f);
            }
        }

        m_gridVertexCount = static_cast<int>(m_gridStaging.size()) / 2;

        m_ctx->extensions.glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
        m_ctx->extensions.glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(m_gridStaging.size() * sizeof(float)),
            m_gridStaging.data(), GL_STATIC_DRAW);
    } else {
        m_ctx->extensions.glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
    }

    if (m_gridVertexCount <= 0) {
        m_ctx->extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
        return;
    }

    // Grid colour: 0xFF333355
    glUniform4f(m_uColourLoc,
                0x33 / 255.0f, 0x33 / 255.0f, 0x55 / 255.0f, 1.0f);

    m_ctx->extensions.glEnableVertexAttribArray(static_cast<GLuint>(m_aPositionLoc));
    m_ctx->extensions.glVertexAttribPointer(
        static_cast<GLuint>(m_aPositionLoc),
        2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_LINES, 0, m_gridVertexCount);

    m_ctx->extensions.glDisableVertexAttribArray(static_cast<GLuint>(m_aPositionLoc));
    m_ctx->extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GridPlayheadGLRenderer::drawPlayhead() {
    if (m_render.displayRangeBeats <= 0.0 || m_aPositionLoc < 0) return;

    double normPos = std::fmod(m_render.currentPpq, m_render.displayRangeBeats)
                     / m_render.displayRangeBeats;
    if (normPos < 0.0) normPos += 1.0;

    const float x = -1.0f + 2.0f * static_cast<float>(normPos);

    float vertices[] = { x, -1.0f, x, 1.0f };

    m_ctx->extensions.glBindBuffer(GL_ARRAY_BUFFER, m_playheadVbo);
    m_ctx->extensions.glBufferSubData(GL_ARRAY_BUFFER, 0,
                                       static_cast<GLsizeiptr>(sizeof(vertices)),
                                       vertices);

    // Playhead colour: white with some transparency (0xAAFFFFFF)
    glUniform4f(m_uColourLoc, 1.0f, 1.0f, 1.0f, 0xAA / 255.0f);

    m_ctx->extensions.glEnableVertexAttribArray(static_cast<GLuint>(m_aPositionLoc));
    m_ctx->extensions.glVertexAttribPointer(
        static_cast<GLuint>(m_aPositionLoc),
        2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_LINES, 0, 2);

    m_ctx->extensions.glDisableVertexAttribArray(static_cast<GLuint>(m_aPositionLoc));
    m_ctx->extensions.glBindBuffer(GL_ARRAY_BUFFER, 0);
}
