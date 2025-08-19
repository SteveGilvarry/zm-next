m#!/bin/bash
# ZM-Next Build Script
# This script automatically sets up vcpkg environment and builds the project

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
        echo -e "${RED}Error: VCPKG_ROOT not set and $HOME/vcpkg not found${NC}"
        echo "Please install vcpkg or set VCPKG_ROOT environment variable"
        exit 1
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
echo -e "${GREEN}Run './zm-core pipelines/rtsp_multi_to_webrtc.json' to start${NC}"
