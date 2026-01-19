#!/usr/bin/env bash
set -Eeuo pipefail

DISABLE_WARNINGS=false
EXIT_ON_NO_PATIENTS=false
SKIP_BUILD=false
FAST_BUILD=true
NO_FAST=false
USE_REALTIME=false

print_help() {
    cat <<'EOF'

Project ER — run.sh helper & runner

Usage:
  ./run.sh [options]

Options:
  -h, -help, --help         Show this help and exit.
  -no_warns                 Disable compiler warnings during build.
  -exit_on_no_patients      Forward -exit_on_no_patients to master (master exits when no patients left).
  -no_build                 Skip the build step and run the last-built binaries.
  -fast                     Build with aggressive optimizations (default behavior).
  -no_fast                  Disable aggressive optimizations (use -O2, no LTO).
  -realtime                 Attempt to run master with real-time scheduling (chrt -f 99). Requires root.
  -realtime (with sudo)     Use with care — real-time threads can starve system tasks.

Examples:
  # Default (fast build, use all CPUs, taskset master to all cores)
  ./run.sh

  # Build with safe/reproducible flags (disable -Ofast/LTO)
  ./run.sh -no_fast

  # Skip build and run previously-built binaries
  ./run.sh -no_build

  # Request master to exit when no patients left
  ./run.sh -exit_on_no_patients

  # Run master with real-time priority (requires root)
  sudo ./run.sh -realtime

Tips & notes:
  * FAST_BUILD=true by default – this enables -Ofast, -march=native and LTO. If you see strange FP behavior,
    use -no_fast to fallback to -O2 and faster link times.
  * Ninja (if installed) will be used automatically for faster incremental builds.
  * The script raises ulimits (open files, user processes, stack) – these are best-effort and may fail without privileges.
  * Real-time scheduling (chrt -f 99) can make the simulation run with highest priority — but it can starve the host.
    Use only when you know what you're doing and preferably on a dedicated machine.
  * The script pins the master process to all CPUs using taskset. Child processes are not forcibly pinned and are
    scheduled by the kernel (they will still run on all cores normally).
  * LTO/IPO gives faster runtime but increases compile/link memory & time. If builds fail due to memory, use -no_fast.
  * If you want the fastest possible build on multi-core machines, install ninja and sufficient RAM (for LTO).

EOF
}

for arg in "$@"; do
    case "$arg" in
        -no_warns) DISABLE_WARNINGS=true ;;
        -exit_on_no_patients) EXIT_ON_NO_PATIENTS=true ;;
        -no_build) SKIP_BUILD=true ;;
        -fast) FAST_BUILD=true ;;
        -no_fast) FAST_BUILD=false; NO_FAST=true ;;
        -realtime) USE_REALTIME=true ;;
        -h|-help|--help) print_help; exit 0 ;;
        *) ;;
    esac
done

BUILD_DIR="build"
BIN_DIR="${BUILD_DIR}/bin"

WAITING_ROOM_SIZE=100
DOCTORS=10
PATIENTS=100000

log() { printf "[%s] %s\n" "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

if [[ "$(uname -s)" != "Linux" ]]; then
    die "This project must be run on Linux (or WSL)."
fi

log "Stopping leftover ER processes (if any)"
pkill -f "./master" || true
pkill -f "./registration" || true
pkill -f "./triage" || true
pkill -f "./doctor" || true
pkill -f "./patient" || true

log "Cleaning POSIX message queues and shared memory"
[[ -d /dev/mqueue ]] && rm -f /dev/mqueue/er_* || true
rm -f /dev/shm/er_* || true

log "Raising resource limits (open files, processes, stack)"
ulimit -n 32768 || log "Warning: failed to increase open file limit"
ulimit -u 65535 || log "Warning: failed to increase user process limit"
ulimit -s unlimited || log "Warning: failed to set stack size (unlimited)"

if [[ "$SKIP_BUILD" == "false" ]]; then
    log "Cleaning build directory"
    rm -rf "${BUILD_DIR}"
    mkdir -p "${BUILD_DIR}"

    GENERATOR=""
    if command -v ninja >/dev/null 2>&1; then
        log "Ninja detected -> using Ninja generator (faster builds)"
        GENERATOR="-G Ninja"
    fi

    log "Configuring CMake (FAST_BUILD=${FAST_BUILD})"
    cmake -S . -B "${BUILD_DIR}" ${GENERATOR} \
        -DCMAKE_BUILD_TYPE=Release \
        -DFAST_BUILD="${FAST_BUILD}" \
        -DDISABLE_WARNINGS="${DISABLE_WARNINGS}" \
        || die "CMake configuration failed"

    PAR_JOBS="$(nproc --all 2>/dev/null || nproc)"
    log "Building project (-j ${PAR_JOBS})"
    cmake --build "${BUILD_DIR}" -- -j"${PAR_JOBS}" || die "Build failed"
else
    log "Skipping build due to -no_build"
fi

if [[ ! -d "${BIN_DIR}" ]]; then
    die "Binary dir not found: ${BIN_DIR}"
fi

log "Running ER simulation"
cd "${BIN_DIR}"

MASTER_ARGS=("${WAITING_ROOM_SIZE}" "${DOCTORS}" "${PATIENTS}")
if [[ "${EXIT_ON_NO_PATIENTS}" == "true" ]]; then
    MASTER_ARGS+=("-exit_on_no_patients")
    log "run.sh: forwarding -exit_on_no_patients to master"
fi

NPROC="$(nproc --all 2>/dev/null || nproc)"
CPU_LIST="0-$((NPROC-1))"

MASTER_CMD="./master ${MASTER_ARGS[*]}"

if [[ "$(id -u)" -eq 0 && "${USE_REALTIME}" == "true" ]]; then
    log "Running master pinned to all CPUs with real-time priority (chrt -f 99). Requires root."
    exec chrt -f 99 taskset -c "${CPU_LIST}" ./master "${MASTER_ARGS[@]}"
else
    log "Running master pinned to all CPUs (taskset)."
    exec taskset -c "${CPU_LIST}" ./master "${MASTER_ARGS[@]}"
fi

log "Simulation finished"
