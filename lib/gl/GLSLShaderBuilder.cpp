#include "GLSLShaderBuilder.h"

using namespace juce::gl;

// ============================================================================
// GLSL Version Detection
// ============================================================================

std::optional<double> GLSLShaderBuilder::getGLSLVersion() {
    const double version = juce::OpenGLShaderProgram::getLanguageVersion();
    return version > 0.0 ? std::optional<double>(version) : std::nullopt;
}

bool GLSLShaderBuilder::isGLSLVersionSupported(double requiredVersion) {
    const auto version = getGLSLVersion();
    return version.has_value() && version.value() >= requiredVersion;
}

juce::String GLSLShaderBuilder::getVersionDirective() {
    const auto version = getGLSLVersion();
    if (!version.has_value())
        return "#version 130\n";  // Fallback to minimum supported

    if (version.value() >= 3.30)
        return "#version 330\n";
    else if (version.value() >= 1.50)
        return "#version 150\n";
    else
        return "#version 130\n";
}

bool GLSLShaderBuilder::useModernFragmentOutput() {
    const auto version = getGLSLVersion();
    return version.has_value() && version.value() >= 3.30;
}

bool GLSLShaderBuilder::validateGLSLVersion(const juce::String& debugName,
                                             double minVersion) {
    juce::ignoreUnused(debugName);

    const auto version = getGLSLVersion();

    if (!version.has_value()) {
        DBG(debugName + ": Failed to query GLSL version");
        return false;
    }

    if (version.value() < minVersion) {
        DBG(debugName + ": GLSL " + juce::String(version.value())
            + " is too old (need " + juce::String(minVersion) + "+)");
        return false;
    }

    return true;
}

// ============================================================================
// Fragment Shader Assembly
// ============================================================================

juce::String GLSLShaderBuilder::buildFragmentShader(
    const juce::String& uniformDeclarations,
    const juce::String& mainBody) {
    const auto versionDir = getVersionDirective();
    const bool useModern = useModernFragmentOutput();

    // Build the output declaration string
    juce::String outputDecl;
    juce::String outputAssignment;

    if (useModern) {
        outputDecl = "out vec4 fragColor;\n";
        outputAssignment = mainBody.replace("fragColor", "fragColor")
                           .replace("gl_FragColor", "fragColor");
    } else {
        outputDecl = "";  // Legacy: use implicit gl_FragColor
        outputAssignment = mainBody.replace("fragColor", "gl_FragColor");
    }

    return versionDir
        + outputDecl
        + uniformDeclarations
        + "void main() {\n"
        + outputAssignment
        + "}\n";
}

// ============================================================================
// Shader Compilation & Linking
// ============================================================================

std::unique_ptr<juce::OpenGLShaderProgram> GLSLShaderBuilder::compileShaderProgram(
    juce::OpenGLContext& ctx,
    const juce::String& vertSrc,
    const juce::String& fragSrc,
    const juce::String& debugName) {
    juce::ignoreUnused(debugName);

    auto shader = std::make_unique<juce::OpenGLShaderProgram>(ctx);

    if (!shader->addVertexShader(vertSrc)) {
        DBG(debugName + ": vertex shader failed: " + shader->getLastError());
        return nullptr;
    }

    if (!shader->addFragmentShader(fragSrc)) {
        DBG(debugName + ": fragment shader failed: " + shader->getLastError());
        return nullptr;
    }

    if (!shader->link()) {
        DBG(debugName + ": shader link failed: " + shader->getLastError());
        return nullptr;
    }

    return shader;
}
