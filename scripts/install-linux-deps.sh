#!/bin/bash
# Install JUCE dependencies for Linux (Ubuntu/Debian)
# Run with: sudo ./scripts/install-linux-deps.sh

set -e

echo "Installing minimal JUCE build dependencies for Linux..."

# Update package list
apt-get update

# Compiler and build tools
apt-get install -y \
    build-essential \
    cmake \
    ninja-build

# JUCE audio dependencies
apt-get install -y \
    libasound2-dev \
    libjack-jackd2-dev

# JUCE graphics dependencies
apt-get install -y \
    libfreetype6-dev \
    libfontconfig1-dev

# JUCE GUI dependencies
apt-get install -y \
    libx11-dev \
    libxcomposite-dev \
    libxcursor-dev \
    libxext-dev \
    libxinerama-dev \
    libxrandr-dev \
    libxrender-dev

echo "✓ All required dependencies installed successfully!"
echo ""
echo "Note: This installs only the minimal dependencies required for this plugin."
echo "Optional dependencies (webkit, curl, ladspa) are disabled via CMake flags."
echo ""
echo "You can now build with:"
echo "  cmake --preset linux-release"
echo "  cmake --build --preset linux-build"
