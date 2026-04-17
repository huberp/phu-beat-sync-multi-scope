#include "CancellationOverlayRenderer.h"

using namespace juce::gl;

// =============================================================================
bool CancellationOverlayRenderer::create(juce::OpenGLContext& ctx)
{
    m_ctx = &ctx;

    const double glslVersion = juce::OpenGLShaderProgram::getLanguageVersion();
    if (glslVersion < 1.30)
    {
        DBG("CancellationOverlayRenderer: GLSL " + juce::String(glslVersion) + " too old");
        return false;
    }

    juce::String versionLine;
    bool useModernOutput = false;
    if      (glslVersion >= 3.30) { versionLine = "#version 330\n"; useModernOutput = true; }
    else if (glslVersion >= 1.50) { versionLine = "#version 150\n"; }
    else                          { versionLine = "#version 130\n"; }

    // -------------------------------------------------------------------------
    // Vertex shader — geometry built from gl_VertexID; no VBO required.
    // -------------------------------------------------------------------------
    const juce::String vertSrc = versionLine + R"(
        uniform float uCancelValues[256];
        uniform int   uNumSlots;
        uniform float uBarTopNDC;
        out vec3 vColour;

        void main()
        {
            int slotIndex = gl_VertexID / 6;
            int corner    = gl_VertexID % 6;

            // Two CCW triangles per slot:
            //   triangle 0: corners 0,1,2  (bottom-left, bottom-right, top-right)
            //   triangle 1: corners 3,4,5  (bottom-left, top-right,   top-left )
            bool isRight = (corner == 1 || corner == 3 || corner == 4);
            bool isTop   = (corner == 2 || corner == 3 || corner == 5);

            int n = max(uNumSlots, 1);
            float xLeft  = float(slotIndex)     / float(n) * 2.0 - 1.0;
            float xRight = float(slotIndex + 1) / float(n) * 2.0 - 1.0;

            float x = isRight ? xRight : xLeft;
            float y = isTop   ? uBarTopNDC : -1.0;
            gl_Position = vec4(x, y, 0.0, 1.0);

            // Colour ramp: green -> yellow -> red
            float ci      = clamp(uCancelValues[slotIndex], 0.0, 1.0);
            vec3 green    = vec3(0.0,   0.733, 0.333);
            vec3 yellow   = vec3(1.0,   0.800, 0.000);
            vec3 red      = vec3(1.0,   0.200, 0.000);
            float useSec  = step(0.4, ci);
            vec3 col1     = mix(green,  yellow, clamp(ci / 0.4,         0.0, 1.0));
            vec3 col2     = mix(yellow, red,    clamp((ci - 0.4) / 0.6, 0.0, 1.0));
            vColour       = mix(col1, col2, useSec);
        }
    )";

    // -------------------------------------------------------------------------
    // Fragment shader — output colour passed from vertex stage.
    // -------------------------------------------------------------------------
    const juce::String fragSrc = versionLine
        + (useModernOutput ? "out vec4 fragColor;\n" : "")
        + "in vec3 vColour;\n"
        + "void main() {\n"
        + (useModernOutput ? "    fragColor    = vec4(vColour, 0.85);\n"
                           : "    gl_FragColor = vec4(vColour, 0.85);\n")
        + "}\n";

    m_shader = std::make_unique<juce::OpenGLShaderProgram>(ctx);

    if (!m_shader->addVertexShader(vertSrc))
    {
        DBG("CancellationOverlayRenderer: vertex shader error: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }
    if (!m_shader->addFragmentShader(fragSrc))
    {
        DBG("CancellationOverlayRenderer: fragment shader error: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }
    if (!m_shader->link())
    {
        DBG("CancellationOverlayRenderer: link error: " + m_shader->getLastError());
        m_shader.reset();
        return false;
    }

    // Cache uniform locations — use "uCancelValues[0]" for array uniforms.
    const GLuint prog  = m_shader->getProgramID();
    m_uCancelValuesLoc = ctx.extensions.glGetUniformLocation(prog, "uCancelValues[0]");
    m_uNumSlotsLoc     = ctx.extensions.glGetUniformLocation(prog, "uNumSlots");
    m_uBarTopNDCLoc    = ctx.extensions.glGetUniformLocation(prog, "uBarTopNDC");
    return true;
}

// =============================================================================
void CancellationOverlayRenderer::release()
{
    m_shader.reset();
    m_uCancelValuesLoc = m_uNumSlotsLoc = m_uBarTopNDCLoc = -1;
    m_ctx = nullptr;
}

// =============================================================================
void CancellationOverlayRenderer::setData(const float* values, int numSlots, bool show)
{
    const juce::SpinLock::ScopedLockType lock(m_lock);

    m_pending.show     = show;
    m_pending.numSlots = 0;

    if (show && values != nullptr && numSlots > 0)
    {
        const int n = juce::jmin(numSlots, MAX_CANCEL_SLOTS);
        std::memcpy(m_pending.cancelValues, values, static_cast<size_t>(n) * sizeof(float));
        m_pending.numSlots = n;
    }

    m_newData = true;
}

// =============================================================================
void CancellationOverlayRenderer::draw(int vpHeightPx)
{
    if (!m_shader) return;

    {
        const juce::SpinLock::ScopedLockType lock(m_lock);
        if (m_newData) { m_render = m_pending; m_newData = false; }
    }

    if (!m_render.show || m_render.numSlots <= 0 || vpHeightPx <= 0) return;

    // Bar is 6 px tall, anchored to the bottom edge of the viewport.
    const float barTopNDC = -1.0f + 2.0f * 6.0f / static_cast<float>(vpHeightPx);

    m_shader->use();
    glUniform1fv(m_uCancelValuesLoc, m_render.numSlots, m_render.cancelValues);
    glUniform1i (m_uNumSlotsLoc,     m_render.numSlots);
    glUniform1f (m_uBarTopNDCLoc,    barTopNDC);

    glDrawArrays(GL_TRIANGLES, 0, m_render.numSlots * 6);
}
