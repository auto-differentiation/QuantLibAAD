#!/bin/bash
##############################################################################
#
#  Local Forge Benchmark Build & Run Script
#
#  This script replicates the GitHub workflow for building and running
#  the Forge benchmark locally in the forge-private-workspace.
#
#  Uses Docker container to match GitHub CI environment (same Boost version).
#
#  Prerequisites:
#    - Docker installed and running
#    - forge-private-workspace with submodules: QuantLib, xad, forge, xad-forge
#
#  Usage:
#    ./runForgeBenchmark.sh [--configure] [--build] [--run] [--diagnose] [--all]
#    ./runForgeBenchmark.sh --quick       # Just run (assumes already built)
#    ./runForgeBenchmark.sh --rebuild     # Clean, configure, build, run
#    ./runForgeBenchmark.sh --no-docker   # Run without Docker (requires matching Boost)
#
##############################################################################

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Get script directory and workspace root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Docker image (same as GitHub CI)
DOCKER_IMAGE="ghcr.io/lballabio/quantlib-devenv:rolling"

# Paths (relative to workspace for Docker mounting)
QUANTLIB_DIR="QuantLib"
XAD_DIR="xad"
XAD_FORGE_DIR="xad-forge"
QLRISKS_DIR="QuantLib-Risks-Cpp-Forge"
FORGE_DIR="forge"
# INSTALL_DIR and BUILD_DIR set later based on Docker vs local

# Default actions
DO_CONFIGURE=false
DO_BUILD=false
DO_RUN=false
DO_DIAGNOSE=false
DO_CLEAN=false
DO_BUILD_FORGE_API=false
USE_DOCKER=true
BENCHMARK_ARGS=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --configure)
            DO_CONFIGURE=true
            shift
            ;;
        --build)
            DO_BUILD=true
            shift
            ;;
        --run)
            DO_RUN=true
            shift
            ;;
        --diagnose)
            DO_DIAGNOSE=true
            shift
            ;;
        --all)
            DO_CONFIGURE=true
            DO_BUILD=true
            DO_RUN=true
            shift
            ;;
        --quick)
            DO_RUN=true
            shift
            ;;
        --rebuild)
            DO_CLEAN=true
            DO_CONFIGURE=true
            DO_BUILD=true
            DO_RUN=true
            shift
            ;;
        --clean)
            DO_CLEAN=true
            shift
            ;;
        --build-forge-api)
            DO_BUILD_FORGE_API=true
            shift
            ;;
        --no-docker)
            USE_DOCKER=false
            shift
            ;;
        --lite|--lite-extended|--production)
            BENCHMARK_ARGS="$BENCHMARK_ARGS $1"
            shift
            ;;
        --fast)
            BENCHMARK_ARGS="$BENCHMARK_ARGS --max-paths=10000"
            shift
            ;;
        --max-paths=*)
            BENCHMARK_ARGS="$BENCHMARK_ARGS $1"
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --configure       Configure CMake"
            echo "  --build           Build benchmark_aad target"
            echo "  --run             Run the benchmark"
            echo "  --diagnose        Run XAD-Split vs JIT diagnostic"
            echo "  --all             Configure + Build + Run"
            echo "  --quick           Just run (assumes already built)"
            echo "  --rebuild         Clean + Configure + Build + Run"
            echo "  --clean           Remove build directory"
            echo "  --build-forge-api Build Forge C API first (needed once)"
            echo "  --no-docker       Run without Docker (requires matching Boost version)"
            echo ""
            echo "Benchmark options (passed to benchmark_aad):"
            echo "  --lite             Run lite benchmark only"
            echo "  --lite-extended    Run lite-extended benchmark only"
            echo "  --production       Run production benchmark only"
            echo "  --fast             Limit to 10K paths max (skip 100K)"
            echo "  --max-paths=N      Limit max path count"
            echo ""
            echo "Examples:"
            echo "  $0 --build-forge-api --all   # First time: build Forge API + full build"
            echo "  $0 --all                     # Full build and run"
            echo "  $0 --quick                   # Just run (if already built)"
            echo "  $0 --quick --lite            # Run lite benchmark only"
            echo "  $0 --diagnose                # Run diagnostic comparison"
            echo "  $0 --rebuild --production    # Rebuild and run production only"
            echo "  $0 --no-docker --all         # Build without Docker (local Boost)"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Default to --all if no action specified
if ! $DO_CONFIGURE && ! $DO_BUILD && ! $DO_RUN && ! $DO_DIAGNOSE && ! $DO_CLEAN && ! $DO_BUILD_FORGE_API; then
    DO_CONFIGURE=true
    DO_BUILD=true
    DO_RUN=true
fi

# Set build directory based on Docker vs local (avoid cache conflicts)
if $USE_DOCKER; then
    BUILD_SUBDIR="build-forge-docker"
    INSTALL_DIR="install-docker"
else
    BUILD_SUBDIR="build-forge-local"
    INSTALL_DIR="install"
fi
BUILD_DIR="QuantLib/$BUILD_SUBDIR"

# Print configuration
echo -e "${BLUE}============================================================${NC}"
echo -e "${BLUE}  Forge Benchmark Build Script${NC}"
echo -e "${BLUE}============================================================${NC}"
echo ""
echo -e "Workspace:    ${GREEN}$WORKSPACE_ROOT${NC}"
echo -e "Docker:       ${GREEN}$USE_DOCKER${NC}"
if $USE_DOCKER; then
    echo -e "Image:        ${GREEN}$DOCKER_IMAGE${NC}"
fi
echo -e "Build dir:    ${GREEN}$BUILD_DIR${NC}"
echo ""

# Check Docker if needed
if $USE_DOCKER; then
    if ! command -v docker &> /dev/null; then
        echo -e "${RED}ERROR: Docker not found. Install Docker or use --no-docker${NC}"
        exit 1
    fi

    echo -e "${YELLOW}Pulling Docker image (if needed)...${NC}"
    docker pull "$DOCKER_IMAGE" || true
    echo ""
fi

# Function to run command (in Docker or locally)
run_cmd() {
    if $USE_DOCKER; then
        docker run --rm \
            -v "$WORKSPACE_ROOT:/workspace" \
            -w /workspace \
            -e HOME=/workspace \
            "$DOCKER_IMAGE" \
            bash -c "apt-get update -qq && apt-get install -y -qq ninja-build > /dev/null 2>&1; $1"
    else
        cd "$WORKSPACE_ROOT"
        bash -c "$1"
    fi
}

# Check prerequisites
echo -e "${YELLOW}Checking prerequisites...${NC}"

if [[ ! -d "$WORKSPACE_ROOT/$QUANTLIB_DIR" ]]; then
    echo -e "${RED}ERROR: QuantLib not found at $WORKSPACE_ROOT/$QUANTLIB_DIR${NC}"
    exit 1
fi

if [[ ! -d "$WORKSPACE_ROOT/$XAD_DIR" ]]; then
    echo -e "${RED}ERROR: XAD not found at $WORKSPACE_ROOT/$XAD_DIR${NC}"
    exit 1
fi

if [[ ! -d "$WORKSPACE_ROOT/$XAD_FORGE_DIR" ]]; then
    echo -e "${RED}ERROR: xad-forge not found at $WORKSPACE_ROOT/$XAD_FORGE_DIR${NC}"
    exit 1
fi

echo -e "${GREEN}All prerequisites found.${NC}"
echo ""

# Build Forge C API if requested
if $DO_BUILD_FORGE_API; then
    echo -e "${YELLOW}Building Forge C API...${NC}"
    echo ""

    # Use separate build directory for Docker to avoid cache conflicts
    FORGE_BUILD_DIR="build-docker"
    DEPS_CACHE_DIR=".deps-cache-docker"
    if ! $USE_DOCKER; then
        FORGE_BUILD_DIR="build"
        DEPS_CACHE_DIR=".deps-cache"
    fi

    # Note: Deps cache is managed by Docker - no need to clean it

    run_cmd "
        cd $FORGE_DIR && \
        cmake -B $FORGE_BUILD_DIR -S api/c -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DFORGE_CAPI_BUILD_TESTS=OFF \
            -DFETCHCONTENT_BASE_DIR=/workspace/$DEPS_CACHE_DIR \
            -DCMAKE_INSTALL_PREFIX=/workspace/$INSTALL_DIR && \
        cmake --build $FORGE_BUILD_DIR && \
        cmake --install $FORGE_BUILD_DIR
    "

    echo ""
    echo -e "${GREEN}Forge C API build complete.${NC}"
    echo ""
fi

# Check for Forge C API
if [[ ! -f "$WORKSPACE_ROOT/$INSTALL_DIR/lib/libforge_capi.so" ]]; then
    echo -e "${RED}ERROR: Forge C API not found at $WORKSPACE_ROOT/$INSTALL_DIR/lib/libforge_capi.so${NC}"
    echo -e "${YELLOW}Run with --build-forge-api first:${NC}"
    echo "  $0 --build-forge-api --all"
    exit 1
fi

# Clean if requested
if $DO_CLEAN; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "$WORKSPACE_ROOT/$BUILD_DIR"
    echo -e "${GREEN}Done.${NC}"
    echo ""
fi

# Configure
if $DO_CONFIGURE; then
    echo -e "${YELLOW}Configuring CMake...${NC}"
    echo ""

    run_cmd "
        cd $QUANTLIB_DIR && \
        cmake -B $BUILD_SUBDIR -G Ninja \
            -DCMAKE_CXX_STANDARD=17 \
            -DCMAKE_BUILD_TYPE=Release \
            -DXAD_WARNINGS_PARANOID=OFF \
            -DXAD_ENABLE_JIT=ON \
            -DCMAKE_PREFIX_PATH=/workspace/$INSTALL_DIR \
            -DQL_EXTERNAL_SUBDIRECTORIES='/workspace/$XAD_DIR;/workspace/$XAD_FORGE_DIR;/workspace/$QLRISKS_DIR' \
            -DQL_EXTRA_LINK_LIBRARIES=QuantLib-Risks \
            -DQL_NULL_AS_FUNCTIONS=ON \
            -DQL_BUILD_TEST_SUITE=OFF \
            -DQL_BUILD_EXAMPLES=OFF \
            -DQL_BUILD_BENCHMARK=OFF \
            -DQLRISKS_DISABLE_AAD=OFF \
            -DQLRISKS_ENABLE_FORGE=ON \
            -DQLRISKS_USE_FORGE_CAPI=ON \
            -DXAD_FORGE_USE_CAPI=ON \
            -DQLRISKS_BUILD_BENCHMARK_AAD=ON \
            -DQLRISKS_BUILD_BENCHMARK_AAD_LOCAL=ON
    "

    echo ""
    echo -e "${GREEN}Configuration complete.${NC}"
    echo ""
fi

# Build
if $DO_BUILD; then
    echo -e "${YELLOW}Building benchmark_aad...${NC}"
    echo ""

    run_cmd "cmake --build $BUILD_DIR --target benchmark_aad_local_script -- -j\$(nproc)"

    echo ""
    echo -e "${GREEN}Build complete.${NC}"
    echo ""
fi

# Find the benchmark executable (only check if we're running or diagnosing)
BENCHMARK_EXE="$WORKSPACE_ROOT/$BUILD_DIR/$QLRISKS_DIR/test-suite/benchmark-aad-local"

if $DO_RUN || $DO_DIAGNOSE; then
    if [[ ! -f "$BENCHMARK_EXE" ]]; then
        echo -e "${RED}ERROR: Benchmark executable not found at $BENCHMARK_EXE${NC}"
        echo -e "${YELLOW}Try running with --build first${NC}"
        exit 1
    fi
fi

# Run benchmark
if $DO_RUN; then
    echo -e "${BLUE}============================================================${NC}"
    echo -e "${BLUE}  Running Forge Benchmark${NC}"
    echo -e "${BLUE}============================================================${NC}"
    echo ""

    if [[ -z "$BENCHMARK_ARGS" ]]; then
        BENCHMARK_ARGS="--all"
    fi

    echo -e "${YELLOW}Command: benchmark-aad-local $BENCHMARK_ARGS${NC}"
    echo ""

    run_cmd "
        export LD_LIBRARY_PATH=/workspace/$INSTALL_DIR/lib:\$LD_LIBRARY_PATH && \
        $BUILD_DIR/$QLRISKS_DIR/test-suite/benchmark-aad-local $BENCHMARK_ARGS
    "

    echo ""
    echo -e "${GREEN}Benchmark complete.${NC}"
fi

# Run diagnostic
if $DO_DIAGNOSE; then
    echo -e "${BLUE}============================================================${NC}"
    echo -e "${BLUE}  Running XAD-Split vs JIT Diagnostic${NC}"
    echo -e "${BLUE}============================================================${NC}"
    echo ""

    run_cmd "
        export LD_LIBRARY_PATH=/workspace/$INSTALL_DIR/lib:\$LD_LIBRARY_PATH && \
        $BUILD_DIR/$QLRISKS_DIR/test-suite/benchmark-aad-local --diagnose --diagnose-paths=100
    "

    echo ""
    echo -e "${GREEN}Diagnostic complete.${NC}"
fi

echo ""
echo -e "${GREEN}Done!${NC}"
