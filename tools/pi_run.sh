#!/usr/bin/env bash
# Runs a FreeRTOS image on the Pi once and collects its results over SSH,
# with no UART adapter. Linux stays the normal boot; the image runs through
# a one-shot tryboot and hands results back in reserved RAM.
#
#   tools/pi_run.sh setup                       # once: reserve RAM, reboot
#   tools/pi_run.sh persist                     # RAM persistence test
#   tools/pi_run.sh log <image> <out.log>       # one RAM_LOG=1 benchmark run
#
# PI must be given explicitly, e.g. PI=vanshika@rpi4-freertos.local. There is
# no default because 172.20.10.3 has also been the PRTC exhibit Pi, and setup
# rewrites the boot config of whatever it reaches.
set -euo pipefail

PI="${PI:?set PI=user@host, for example PI=vanshika@rpi4-freertos.local}"
HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGES="$HERE/../FreeRTOS/Demo/CORTEX_A72_64-bit_Raspberrypi4/uart/ramlog-images"
SSH=(ssh -o ConnectTimeout=5 -o BatchMode=yes)

remote() { "${SSH[@]}" "$PI" "$@"; }

# Installed outside /tmp so a result can still be read by hand after a reboot.
push_tool() {
  scp -q "$HERE/pi_ramlog.py" "$PI:/tmp/pi_ramlog.py"
  remote "sudo install -m 755 /tmp/pi_ramlog.py /usr/local/bin/pi_ramlog.py"
}

wait_for_return() {
  local deadline=$((SECONDS + $1))
  echo "waiting for the Pi to go down..."
  while remote true 2>/dev/null; do
    (( SECONDS < deadline )) || { echo "Pi never went down" >&2; exit 1; }
    sleep 2
  done
  echo "waiting for Linux to come back (up to $1 s)..."
  until remote true 2>/dev/null; do
    (( SECONDS < deadline )) || { echo "Pi did not come back; power-cycle it to boot Linux" >&2; exit 1; }
    sleep 3
  done
}

tryboot_image() {
  local image="$1" budget="$2"
  push_tool
  scp -q "$image" "$PI:/tmp/freertos-kernel8.img"
  remote "sudo /usr/local/bin/pi_ramlog.py install-image /tmp/freertos-kernel8.img &&
          sudo /usr/local/bin/pi_ramlog.py arm"
  remote "sudo reboot '0 tryboot'" || true
  wait_for_return "$budget"
  push_tool
}

case "${1:-}" in
  setup)
    push_tool
    remote "command -v dtc >/dev/null || sudo apt-get install -y device-tree-compiler"
    remote "sudo /usr/local/bin/pi_ramlog.py install-overlay"
    remote "sudo reboot" || true
    wait_for_return 300
    push_tool
    remote "sudo /usr/local/bin/pi_ramlog.py status"
    ;;
  persist)
    tryboot_image "$IMAGES/ram-persist-test.img" 1500
    remote "sudo /usr/local/bin/pi_ramlog.py check-persist"
    ;;
  log)
    [[ $# -eq 3 ]] || { echo "usage: $0 log <image> <out.log>" >&2; exit 2; }
    tryboot_image "$2" 1800
    status=0
    remote "sudo /usr/local/bin/pi_ramlog.py read --out /tmp/freertos-run.log" || status=$?
    mkdir -p "$(dirname "$3")"
    scp -q "$PI:/tmp/freertos-run.log" "$3" && echo "saved $3"
    exit "$status"
    ;;
  *)
    sed -n '2,10p' "$0" >&2
    exit 2
    ;;
esac
