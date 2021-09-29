#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
WAYLAND_SHELL="${WAYLAND_SHELL:-xdg-shell}"
CLANG_QUERY="${CLANG_QUERY:-clang-query}"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/client/build}"

case "$WAYLAND_SHELL" in
  xdg-shell) shell_ignore=shell_libdecor.c ;;
  libdecor)  shell_ignore=shell_xdg.c ;;
  *)
    echo "Unknown Wayland shell: $WAYLAND_SHELL"
    exit 1
esac

if ! command -v "$CLANG_QUERY" &> /dev/null; then
  echo "clang-query is not installed"
  exit 1
fi

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
  echo "compile_commands.json not found in $BUILD_DIR"
  echo 'Run cmake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON to enable'
  exit 1
fi

tempdir="$(mktemp -d)"
cleanup() {
  rm -rf "$tempdir"
}
trap cleanup EXIT

sed '/EGL_VERSION_1_5/,$d' /usr/include/EGL/egl.h > "$tempdir/egl1.4.h"

getCalls() {
  query="$1"
  dir="$2"

  command=(find "$dir" -name '*.c')
  if [ $# -gt 2 ]; then
    command+=(-not)
    shift 2
    for exclude in "$@"; do
      command+=(-name "$exclude")
    done
  fi

  cat > "$tempdir/query" <<EOF
set output print
set bind-root false
m callExpr(callee(functionDecl(matchesName("::$query")).bind("func")))
EOF

  "${command[@]}" -exec "$CLANG_QUERY" -p "$BUILD_DIR/compile_commands.json" -f "$tempdir/query" {} + | \
    grep -Po "(?<= )\b$query(?=\()" | sort -u
}

checkCalls() {
  name="$1"
  file="$2"
  standard="$3"
  while read -r func; do
    if ! grep -q "$func" "$file"; then
      echo "Found $func in $name, which is not in $standard"
      fails=$((fails+1))
    fi
  done
}

getGLCalls() {
  getCalls 'gl[A-WYZ]\w*' "$@"
}

getEGLCalls() {
  getCalls 'egl[A-Z]\w*' "$@"
}

getGLXCalls() {
  getCalls 'glX\w+' "$@"
}

checkGLESCalls() {
  name="$1"
  shift
  checkCalls "$name" /usr/include/GLES3/gl3.h 'OpenGL ES 3.0' < <(getGLCalls "$@")
}

checkEGLCalls() {
  name="$1"
  shift
  checkCalls "$name" "$tempdir/egl1.4.h" 'EGL 1.4' < <(getEGLCalls "$@")
}

checkGLCalls() {
  name="$1"
  shift
  checkCalls "$name" /usr/include/GL/gl.h 'OpenGL 1.3' < <(getGLCalls "$@")
}

checkGLXCalls() {
  name="$1"
  shift
  checkCalls "$name" /usr/include/GL/glx.h 'GLX 1.3' < <(getGLXCalls "$@")
}

forbidCalls() {
  name="$1"
  while read -r func; do
    echo "Found $func in $name, which is forbidden"
    fails=$((fails+1))
  done
}

forbidEGLCalls() {
  name="$1"
  shift
  forbidCalls "$name" < <(getEGLCalls "$@")
}

forbidGLXCalls() {
  name="$1"
  shift
  forbidCalls "$name" < <(getGLXCalls "$@")
}

forbidGLCalls() {
  name="$1"
  shift
  forbidCalls "$name" < <(getGLCalls "$@")
}

fails=0

checkGLESCalls 'EGL backend'    client/renderers/EGL
checkEGLCalls  'EGL backend'    client/renderers/EGL
forbidGLXCalls 'EGL backend'    client/renderers/EGL
checkGLCalls   'OpenGL backend' client/renderers/OpenGL
forbidGLXCalls 'OpenGL backend' client/renderers/OpenGL

checkGLXCalls  'X11 display server'     client/displayservers/X11
forbidGLCalls  'X11 display server'     client/displayservers/X11
checkEGLCalls  'Wayland display server' client/displayservers/Wayland "$shell_ignore"
forbidGLCalls  'Wayland display server' client/displayservers/Wayland "$shell_ignore"
forbidGLXCalls 'Wayland display server' client/displayservers/Wayland "$shell_ignore"

if [ "$fails" -eq 0 ]; then
  echo 'All GL function calls look fine.'
else
  echo 'Use indirection for the listed GL function calls.'
  exit 1
fi
