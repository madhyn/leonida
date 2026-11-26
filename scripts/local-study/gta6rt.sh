#!/bin/sh
# Starts the Windows build of the loader with Wine. WINE picks another one, e.g. a Proton build's bin/wine.
here=$(dirname "$(readlink -f "$0")")
if [ "$#" -eq 1 ] && { [ "$1" = --dry-run ] || [ "$1" = --init ] || [ "$1" = --run ]; }; then
    game=$(cat "$here/game_path.txt" && printf .) || exit 1
    cd "${game%.}" || exit 1
    image=game_scarlett_master.exe
    if [ ! -f "./$image" ]; then
        printf 'Missing game executable: %s/%s\n' "$PWD" "$image" >&2
        exit 1
    fi
    # Wine maps the Unix filesystem root to Z:. The game reads from its package and writes to the port's overlay.
    export LEONIDA_D3D12="${LEONIDA_D3D12:-vulkan}"
    set -- "$1" "./$image" --overlay "Z:$here/overlay"
fi
exec "${WINE:-wine}" "$here/gta6rt.exe" "$@"
