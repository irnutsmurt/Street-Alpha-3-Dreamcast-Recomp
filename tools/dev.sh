#!/bin/sh
# Developer loop: regenerate recompiled C, incremental build, run headless.
# With "run", entry points discovered missing at runtime are added to
# tools/recomp/extra_entries.txt and the loop repeats (up to $ITER times).
set -e
# VISIBLE=1 shows the window and plays audio; otherwise runs are headless and silent.
cd "$(dirname "$0")/.."
build() {
  python3 -c "import sys; sys.path.insert(0,'tools/recomp'); import recompile; recompile.run('build/disc','build/generated')"
  cmake -S . -B build/cmake -G Ninja ${RENDERER:+-DRENDERER=$RENDERER} >/dev/null
  cmake --build build/cmake 2>&1 | grep -E "error|FAILED" || true
}
build
[ "$1" = "run" ] || exit 0
shift
i=0
while :; do
  rm -f build/missing_entries.txt
  if [ -n "$VISIBLE" ]; then
    timeout ${T:-20} ./build/cmake/sfa3 "$@" > build/run.txt 2>&1 || true
  else
    SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-offscreen} SDL_AUDIO_DRIVER=${SDL_AUDIO_DRIVER:-dummy} \
      timeout ${T:-20} ./build/cmake/sfa3 "$@" > build/run.txt 2>&1 || true
  fi
  tail -5 build/run.txt
  [ -s build/missing_entries.txt ] || break
  i=$((i+1)); [ $i -lt ${ITER:-10} ] || break
  a=$(cut -c1-8 build/missing_entries.txt | head -1)
  if grep -q "^$a" tools/recomp/extra_entries.txt; then echo "== $a already listed; not code?"; break; fi
  cat build/missing_entries.txt >> tools/recomp/extra_entries.txt
  echo "== added $(cat build/missing_entries.txt | head -1), rebuilding ($i)"
  build
done
