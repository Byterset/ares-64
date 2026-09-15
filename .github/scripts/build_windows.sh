#!/usr/bin/env sh
set -euo pipefail


cmake --preset $TARGET_PRESET
pushd build
cmake --build . --config RelWithDebInfo

if [ "$CROSS_COMPILE" = true ] || [ "$ARES_PLATFORM_NAME" = "windows-arm64" ]; then
  cp ../.deps/ares-deps-windows-arm64/lib/*.pdb desktop-ui/rundir/
else
  cp ../.deps/ares-deps-windows-x64/lib/*.pdb desktop-ui/rundir/
fi

mkdir PDBs
mv desktop-ui/rundir/*.pdb PDBs/
popd
