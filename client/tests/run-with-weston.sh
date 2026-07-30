#!/bin/bash
set -euo pipefail

weston_bin=$1
shift

runtime_dir=$(mktemp -d /tmp/lg-render-tests.XXXXXX)
weston_pid=
cleanup()
{
  if [[ -n "$weston_pid" ]]; then
    kill "$weston_pid" 2>/dev/null || true
    wait "$weston_pid" 2>/dev/null || true
  fi
  rm -rf -- "$runtime_dir"
}
trap cleanup EXIT

chmod 700 "$runtime_dir"
export XDG_RUNTIME_DIR="$runtime_dir"
export LIBGL_ALWAYS_SOFTWARE=1
unset DISPLAY WAYLAND_DISPLAY

socket_name=lg-render-tests
"$weston_bin" \
  --backend=headless \
  --renderer=gl \
  --width=1024 \
  --height=768 \
  --idle-time=0 \
  --no-config \
  --socket="$socket_name" \
  --log="$runtime_dir/weston.log" &
weston_pid=$!

startup_deadline=$((SECONDS + 30))
while (( SECONDS < startup_deadline )); do
  if [[ -S "$runtime_dir/$socket_name" ]]; then
    break
  fi
  if ! kill -0 "$weston_pid" 2>/dev/null; then
    sed -n '1,240p' "$runtime_dir/weston.log" >&2
    exit 1
  fi
  sleep 0.1
done

if [[ ! -S "$runtime_dir/$socket_name" ]]; then
  echo "Timed out waiting for the Weston Wayland socket" >&2
  sed -n '1,240p' "$runtime_dir/weston.log" >&2
  exit 1
fi

export WAYLAND_DISPLAY="$socket_name"
set +e
"$@"
status=$?
set -e
if (( status != 0 )); then
  sed -n '1,240p' "$runtime_dir/weston.log" >&2
fi
exit "$status"