#!/bin/bash
# Inicia o Leonida via Wine com isolamento de rede, NVMe em /mnt/docs/leonida-fast,
# frame pacing real e ajustes mínimos para 8 GiB de VRAM (RTX 3060 Ti).
set -eu

here=$(dirname "$(readlink -f "$0")")
FAST_DIR="/mnt/docs/leonida-fast"

if [ -f "$FAST_DIR/game/game_scarlett_master.exe" ]; then
    game="$FAST_DIR/game"
else
    game=$(cat "$here/game_path.txt")
fi

if [ ! -f "$game/game_scarlett_master.exe" ]; then
    printf 'Executavel do jogo nao encontrado em: %s\n' "$game" >&2
    exit 1
fi

if [ -x "$FAST_DIR/out/gta6rt.exe" ] && [ -d "$FAST_DIR/shaders/spirv" ]; then
    RT_EXE="$FAST_DIR/out/gta6rt.exe"
    DEFAULT_SHADERS="Z:$FAST_DIR/shaders/spirv"
    DEFAULT_OVERLAY="Z:$FAST_DIR/overlay"
    TRACE_HOST_PATH="$FAST_DIR/overlay/startup.jsonl"
    mkdir -p "$FAST_DIR/overlay"
else
    RT_EXE="$here/gta6rt.exe"
    DEFAULT_SHADERS="Z:/mnt/jogos2/Games/leonida-work/shaders/spirv"
    DEFAULT_OVERLAY="Z:$here/overlay"
    TRACE_HOST_PATH="$here/overlay/startup.jsonl"
    mkdir -p "$here/overlay"
fi

mkdir -p "$HOME/.local/share/leonida-wine" "$HOME/.cache/leonida-pso" "$HOME/.cache/leonida-nv"

export WINEPREFIX="${WINEPREFIX:-$HOME/.local/share/leonida-wine}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=;winedbg.exe=d;winemenubuilder.exe=d}"

export LEONIDA_D3D12="${LEONIDA_D3D12:-vulkan}"
export LEONIDA_DEVICE_TYPE="${LEONIDA_DEVICE_TYPE:-6}"
export LEONIDA_TITLE_WATCHDOG="${LEONIDA_TITLE_WATCHDOG:-0}"
export LEONIDA_TITLE_WATCHDOG_KICK="${LEONIDA_TITLE_WATCHDOG_KICK:-2215c35,71b6331}"
export LEONIDA_SKIP_INT3="${LEONIDA_SKIP_INT3:-1}"
export LEONIDA_PIPELINE_COMPILE="${LEONIDA_PIPELINE_COMPILE:-on-demand}"
export LEONIDA_COMPILE_THREADS="${LEONIDA_COMPILE_THREADS:-4}"
export LEONIDA_COMPUTE_WARMUP="${LEONIDA_COMPUTE_WARMUP:-0}"
export LEONIDA_FRAME_PACING="${LEONIDA_FRAME_PACING:-1}"
export LEONIDA_GPU_LABELS="${LEONIDA_GPU_LABELS:-1}"
export LEONIDA_GRAPHICS_STAGE_LAYOUT="${LEONIDA_GRAPHICS_STAGE_LAYOUT:-1}"
export LEONIDA_MAP_READBACK="${LEONIDA_MAP_READBACK:-1}"
export LEONIDA_CONSOLE_RELEASE_FILTER="${LEONIDA_CONSOLE_RELEASE_FILTER:-1}"
export LEONIDA_GEOMETRY_HOST="${LEONIDA_GEOMETRY_HOST:-1}"
export LEONIDA_SHADERS="${LEONIDA_SHADERS:-$DEFAULT_SHADERS}"
export LEONIDA_ARENA_MB="${LEONIDA_ARENA_MB:-2048}"
export PULSE_LATENCY_MSEC="${PULSE_LATENCY_MSEC:-60}"
export LEONIDA_PIPELINE_CACHE_DIR="${LEONIDA_PIPELINE_CACHE_DIR:-Z:$HOME/.cache/leonida-pso}"
export LEONIDA_PIPELINE_CACHE_BYTES="${LEONIDA_PIPELINE_CACHE_BYTES:-4294967296}"
export LEONIDA_STARTUP_CACHE_CHECKPOINTS="${LEONIDA_STARTUP_CACHE_CHECKPOINTS:-1}"
export LEONIDA_KEYBOARD_PAD="${LEONIDA_KEYBOARD_PAD:-always}"

export __GL_SHADER_DISK_CACHE=1
export __GL_SHADER_DISK_CACHE_PATH="${__GL_SHADER_DISK_CACHE_PATH:-$HOME/.cache/leonida-nv}"
export __GL_SHADER_DISK_CACHE_SIZE="${__GL_SHADER_DISK_CACHE_SIZE:-21474836480}"
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1
export __GL_ThreadedOptimization=1

TRACE_ARGS=()
if [ "${LEONIDA_TRACE:-0}" = "1" ]; then
    rm -f "$TRACE_HOST_PATH"
    TRACE_ARGS=(--trace "Z:$TRACE_HOST_PATH")
fi

if pgrep -f 'gta6rt\.exe' >/dev/null 2>&1; then
    printf 'Aviso: ja existe outra instancia de gta6rt.exe ativa (pode disputar VRAM na RTX 3060 Ti 8 GB). Para limpar: pkill -9 -f gta6rt.exe\n' >&2
fi

cd "$game"
exec unshare -rn "${WINE:-wine}" "$RT_EXE" \
    --run ./game_scarlett_master.exe \
    --game-dir "Z:$game" \
    --overlay "$DEFAULT_OVERLAY" \
    "${TRACE_ARGS[@]}" \
    "$@"
