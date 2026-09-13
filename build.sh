#!/bin/bash
# ZM-Next Build Script
# Configures and builds the project. vcpkg is optional: it is used when present
# (e.g. a Linux FFmpeg built with VAAPI); macOS builds need only Homebrew.

set -e

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== ZM-Next Build Script ===${NC}"

# Set VCPKG_ROOT if not already set
if [ -z "$VCPKG_ROOT" ]; then
    if [ -d "$HOME/vcpkg" ]; then
        export VCPKG_ROOT="$HOME/vcpkg"
        echo -e "${YELLOW}Auto-detected VCPKG_ROOT: $VCPKG_ROOT${NC}"
    else
        echo -e "${YELLOW}vcpkg not found; building with system/Homebrew packages only${NC}"
    fi
else
    echo -e "${GREEN}Using VCPKG_ROOT: $VCPKG_ROOT${NC}"
fi

# Clean build directory if requested
if [ "$1" = "clean" ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf build
fi

# Create build directory
mkdir -p build
cd build

# Configure with CMake
echo -e "${YELLOW}Configuring with CMake...${NC}"
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Build
echo -e "${YELLOW}Building...${NC}"
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Run tests if requested
if [ "$1" = "test" ] || [ "$2" = "test" ]; then
    echo -e "${YELLOW}Running tests...${NC}"
    ctest --output-on-failure
fi

echo -e "${GREEN}Build completed successfully!${NC}"
echo -e "${GREEN}Run 'cd build && ./zm-core --pipeline ../pipelines/<name>.json' to start (copy a *.template.json)${NC}"
