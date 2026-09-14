#!/bin/bash
set -e

# Paths and directories we've intentionally deleted.
# Conflicts in these paths are auto-resolved by accepting our deletion.
# Add new entries here when deleting more files from upstream.
DELETED=(
  hiro
  tools/genius
  tools/mame2bml
  scripts/update-arcade-rom-db.sh
  desktop-ui/tools/cheats.cpp
  desktop-ui/tools/graphics.cpp
  desktop-ui/tools/manifest.cpp
  desktop-ui/tools/memory.cpp
  desktop-ui/tools/properties.cpp
  desktop-ui/tools/streams.cpp
  desktop-ui/tools/tape.cpp
  desktop-ui/tools/tools.cpp
  desktop-ui/tools/tracer.cpp
  # hiro settings panels; ImGui draws all of these now (see desktop-ui/ui).
  # Only hotkeys.cpp, input.cpp, settings.cpp and settings.hpp are kept.
  desktop-ui/settings/audio.cpp
  desktop-ui/settings/cores.cpp
  desktop-ui/settings/developer.cpp
  desktop-ui/settings/emulators.cpp
  desktop-ui/settings/firmware.cpp
  desktop-ui/settings/home.cpp
  desktop-ui/settings/importexport.cpp
  desktop-ui/settings/options.cpp
  desktop-ui/settings/paths.cpp
  desktop-ui/settings/video.cpp
  ruby/video/glx.cpp
  ruby/video/wgl.cpp
  ruby/video/cgl.cpp
  ruby/video/direct3d9.cpp
  ruby/video/metal
  ruby/audio/alsa.cpp
  ruby/audio/pulseaudio.cpp
  ruby/audio/oss.cpp
  ruby/audio/openal.cpp
  ruby/audio/asio.cpp
  ruby/audio/wasapi.cpp
  ruby/audio/directsound.cpp
  ruby/audio/waveout.cpp
  ruby/audio/xaudio2.cpp
  ruby/input/shared
  ruby/input/keyboard
  ruby/input/mouse
  ruby/input/joypad
  ruby/input/xlib.cpp
  ruby/input/udev.cpp
  ruby/input/uhid.cpp
  ruby/input/carbon.cpp
  ruby/input/quartz.cpp
  ruby/input/rawinput.cpp
  ruby/input/directinput.cpp
  ruby/input/iokit.cpp
  ares/a26
  ares/cv
  ares/fc
  ares/gb
  ares/gba
  ares/md
  ares/ms
  ares/msx
  ares/myvision
  ares/ng
  ares/ngp
  ares/pce
  ares/ps1
  ares/saturn
  ares/sfc
  ares/sg
  ares/spec
  ares/ws
  ares/System/ColecoVision
  ares/System/Famicom
  "ares/System/Game Boy"
  "ares/System/Game Boy Advance"
  "ares/System/Game Boy Color"
  "ares/System/Game Boy Player"
  "ares/System/Game Gear"
  "ares/System/Master System"
  "ares/System/Mega Drive"
  ares/System/MSX
  ares/System/MSX2
  "ares/System/Neo Geo Pocket"
  "ares/System/Neo Geo Pocket Color"
  "ares/System/PC Engine"
  "ares/System/PC Engine Duo"
  ares/System/PlayStation
  "ares/System/Pocket Challenge V2"
  ares/System/SC-3000
  ares/System/SG-1000
  "ares/System/Super Famicom"
  ares/System/SuperGrafx
  ares/System/SwanCrystal
  ares/System/WonderSwan
  "ares/System/WonderSwan Color"
  desktop-ui/emulator/atari-2600.cpp
  desktop-ui/emulator/colecovision.cpp
  desktop-ui/emulator/dendy.cpp
  desktop-ui/emulator/famicom.cpp
  desktop-ui/emulator/famicom-disk-system.cpp
  desktop-ui/emulator/game-boy.cpp
  desktop-ui/emulator/game-boy-color.cpp
  desktop-ui/emulator/game-boy-advance.cpp
  desktop-ui/emulator/game-gear.cpp
  desktop-ui/emulator/master-system.cpp
  desktop-ui/emulator/mega-32x.cpp
  desktop-ui/emulator/mega-cd.cpp
  desktop-ui/emulator/mega-cd-32x.cpp
  desktop-ui/emulator/mega-drive.cpp
  desktop-ui/emulator/mega-ld.cpp
  desktop-ui/emulator/msx.cpp
  desktop-ui/emulator/msx2.cpp
  desktop-ui/emulator/myvision.cpp
  desktop-ui/emulator/neo-geo-aes.cpp
  desktop-ui/emulator/neo-geo-mvs.cpp
  desktop-ui/emulator/neo-geo-pocket.cpp
  desktop-ui/emulator/neo-geo-pocket-color.cpp
  desktop-ui/emulator/pc-engine.cpp
  desktop-ui/emulator/pc-engine-cd.cpp
  desktop-ui/emulator/pc-engine-ld.cpp
  desktop-ui/emulator/playstation.cpp
  desktop-ui/emulator/pocket-challenge-v2.cpp
  desktop-ui/emulator/saturn.cpp
  desktop-ui/emulator/sc-3000.cpp
  desktop-ui/emulator/sg-1000.cpp
  desktop-ui/emulator/super-famicom.cpp
  desktop-ui/emulator/supergrafx.cpp
  desktop-ui/emulator/supergrafx-cd.cpp
  desktop-ui/emulator/wonderswan.cpp
  desktop-ui/emulator/wonderswan-color.cpp
  desktop-ui/emulator/zx-spectrum.cpp
  desktop-ui/emulator/zx-spectrum-128.cpp
  desktop-ui/presentation/presentation.cpp
  ares/System
  ares/ares/resource
  ares/cmake
  ares/component
  cmake/finders/FindGTK.cmake
  cmake/finders/FindSDL.cmake
  cmake/finders/Findlibrashader.cmake
  desktop-ui/game-browser/game-browser.cpp
  desktop-ui/macos-fix-jit.cpp
  desktop-ui/resource/resource.bml
  mia/Database
  "mia/Firmware/Game Boy"
  "mia/Firmware/Game Boy Color"
  mia/Firmware/MSX2
  "mia/Firmware/Mega 32X"
  "mia/Firmware/Mega Drive"
  "mia/Firmware/Pocket Challenge V2"
  "mia/Firmware/Super Famicom"
  mia/Firmware/WonderSwan
  "mia/Firmware/WonderSwan Color"
  "mia/Firmware/ZX Spectrum"
  "mia/Firmware/ZX Spectrum 128"
  mia/medium/arcade.cpp
  mia/medium/atari-2600.cpp
  mia/medium/bs-memory.cpp
  mia/medium/colecovision.cpp
  mia/medium/famicom-disk-system.cpp
  mia/medium/famicom.cpp
  mia/medium/game-boy-advance.cpp
  mia/medium/game-boy-color.cpp
  mia/medium/game-boy.cpp
  mia/medium/game-gear.cpp
  mia/medium/mame.cpp
  mia/medium/master-system.cpp
  mia/medium/mega-32x.cpp
  mia/medium/mega-cd.cpp
  mia/medium/mega-drive.cpp
  mia/medium/mega-ld.cpp
  mia/medium/msx.cpp
  mia/medium/msx2.cpp
  mia/medium/myvision.cpp
  mia/medium/neo-geo-crypt.hpp
  mia/medium/neo-geo-pocket-color.cpp
  mia/medium/neo-geo-pocket.cpp
  mia/medium/neo-geo.cpp
  mia/medium/pc-engine-cd.cpp
  mia/medium/pc-engine-ld.cpp
  mia/medium/pc-engine.cpp
  mia/medium/playstation.cpp
  mia/medium/pocket-challenge-v2.cpp
  mia/medium/saturn.cpp
  mia/medium/sc-3000.cpp
  mia/medium/sg-1000.cpp
  mia/medium/sufami-turbo.cpp
  mia/medium/super-famicom.cpp
  mia/medium/supergrafx.cpp
  mia/medium/wonderswan-color.cpp
  mia/medium/wonderswan.cpp
  mia/medium/zx-spectrum.cpp
  mia/resource/resource.bml
  mia/system/arcade.cpp
  mia/system/atari-2600.cpp
  mia/system/colecovision.cpp
  mia/system/famicom.cpp
  mia/system/game-boy-advance.cpp
  mia/system/game-boy-color.cpp
  mia/system/game-boy.cpp
  mia/system/game-gear.cpp
  mia/system/master-system.cpp
  mia/system/mega-32x.cpp
  mia/system/mega-cd-32x.cpp
  mia/system/mega-cd.cpp
  mia/system/mega-drive.cpp
  mia/system/mega-ld.cpp
  mia/system/msx.cpp
  mia/system/msx2.cpp
  mia/system/myvision.cpp
  mia/system/neo-geo-aes.cpp
  mia/system/neo-geo-mvs.cpp
  mia/system/neo-geo-pocket-color.cpp
  mia/system/neo-geo-pocket.cpp
  mia/system/pc-engine-ld.cpp
  mia/system/pc-engine.cpp
  mia/system/playstation.cpp
  mia/system/pocket-challenge-v2.cpp
  mia/system/saturn.cpp
  mia/system/sc-3000.cpp
  mia/system/sg-1000.cpp
  mia/system/super-famicom.cpp
  mia/system/supergrafx.cpp
  mia/system/wonderswan-color.cpp
  mia/system/wonderswan.cpp
  mia/system/zx-spectrum-128.cpp
  mia/system/zx-spectrum.cpp
  nall/nall/any.hpp
  nall/nall/cipher
  nall/nall/database
  nall/nall/decode/mmi.hpp
  nall/nall/elliptic-curve
  nall/nall/http
  nall/nall/instruction-set.cpp
  nall/nall/instruction-set.hpp
  nall/nall/locale.hpp
  nall/nall/map.hpp
  nall/nall/pointer.hpp
  nall/nall/serial.hpp
  nall/nall/smtp.cpp
  nall/nall/smtp.hpp
  nall/nall/string/allocator/copy-on-write.hpp
  nall/nall/string/allocator/small-string-optimization.hpp
  nall/nall/string/allocator/vector.hpp
  nall/nall/string/eval
  nall/nall/string/markup/json.hpp
  nall/nall/string/markup/xml.hpp
  nall/nall/string/transform
  nall/nall/variant.hpp
  ruby/input/sdl.cpp
  ruby/ruby.cpp
  ruby/ruby.mm
  ruby/video/opengl
  tests/arm7tdmi
  tests/i8080
  tests/m68000
  tools
)

REMOTE="${1:-origin}"
BRANCH="${2:-master}"

echo "Fetching $REMOTE..."
git fetch "$REMOTE"

echo "Merging $REMOTE/$BRANCH..."
if git merge "$REMOTE/$BRANCH"; then
  echo "Clean merge, done."
  exit 0
fi

# Directories whose conflicts are auto-resolved by keeping our version.
# Everything under ruby/ is our own SDL3 wrappers,upstream changes are ignored.
OURS=(
  ruby
  desktop-ui/presentation
)

echo "Merge conflicts: auto-resolving known deletions and ours-dirs..."

RESOLVED=()
while IFS= read -r file; do
  # 1) Known deletions: delete the file.
  for dir in "${DELETED[@]}"; do
    if [[ "$file" == "$dir" || "$file" == "$dir"/* ]]; then
      git rm -f "$file" 2>/dev/null && RESOLVED+=("$file") && continue 2
    fi
  done
  # 2) Ours-dirs: keep our version, discard upstream's.  Content conflicts
  #    are resolved with --ours; modify/delete conflicts (we deleted the
  #    file) are resolved by keeping the deletion.
  for dir in "${OURS[@]}"; do
    if [[ "$file" == "$dir" || "$file" == "$dir"/* ]]; then
      if git checkout --ours -- "$file" 2>/dev/null; then
        git add "$file" && RESOLVED+=("$file") && continue 2
      elif git rm -f "$file" 2>/dev/null; then
        RESOLVED+=("$file") && continue 2
      fi
    fi
  done
done < <(git diff --name-only --diff-filter=U)

if [ ${#RESOLVED[@]} -gt 0 ]; then
  echo "Auto-resolved:"
  printf '  %s\n' "${RESOLVED[@]}"
fi

REMAINING=$(git diff --name-only --diff-filter=U)
if [ -n "$REMAINING" ]; then
  echo ""
  echo "Manual resolution needed for:"
  echo "$REMAINING"
  echo ""
  echo "Resolve them, then:  git add -u && git commit --no-edit"
  exit 1
fi

echo "All conflicts resolved, finishing merge..."
git commit --no-edit
echo "Done."
