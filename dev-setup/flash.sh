#!/usr/bin/env bash
# flash.sh — flash WetGreg over USB, then push this computer's local date/time
# to it so the clock never needs setting by hand.
#
#   ./flash.sh                 flash build/wetgreg_hub_rtos.uf2, then sync time
#   ./flash.sh path/to/fw.uf2  flash a specific .uf2, then sync time
#   ./flash.sh --sync          only push the time (device already running)
#   ./flash.sh --query         ask the device for its clock, uptime and last
#                              reset reason (no change made)
#   WETGREG_PORT=/dev/ttyACM1 ./flash.sh --sync   pick the serial port yourself
#
# The firmware listens on USB serial for "TIME YYYY-MM-DD HH:MM:SS" and replies
# "TIME OK ...". Until it hears that (or gets NTP / a manual set), clock
# readouts fall back to the build time. "TIME?" is answered with "TIME NOW ...".
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
UF2="$HERE/wetgreg-hub-rtos/build/wetgreg_hub_rtos.uf2"
SYNC_ONLY=0
QUERY_ONLY=0

case "${1:-}" in
    --sync) SYNC_ONLY=1 ;;
    --query) QUERY_ONLY=1 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    "") ;;
    *) UF2="$1" ;;
esac

find_port() {
    local p
    [[ -n "${WETGREG_PORT:-}" ]] && { echo "$WETGREG_PORT"; return 0; }
    for p in /dev/serial/by-id/*Raspberry_Pi* /dev/serial/by-id/*Pico* /dev/ttyACM*; do
        [[ -e "$p" ]] && { readlink -f "$p"; return 0; }
    done
    return 1
}

sync_time() {
    local port="" i
    echo "Waiting for WetGreg USB serial..."
    for i in $(seq 1 40); do
        port="$(find_port)" && break
        sleep 0.25
    done
    [[ -n "$port" ]] || { echo "No WetGreg serial port found (is it plugged in and running?)" >&2; return 1; }

    stty -F "$port" 115200 raw -echo
    exec 3<>"$port"
    local attempt line now
    for attempt in 1 2 3 4 5; do
        now="$(date '+%Y-%m-%d %H:%M:%S')"
        printf 'TIME %s\n' "$now" >&3
        # The board also streams its log here; skip lines until our reply.
        while IFS= read -r -t 2 line <&3; do
            line="${line%$'\r'}"
            if [[ "$line" == "TIME OK"* ]]; then
                echo "Clock set on $port: ${line#TIME OK }"
                exec 3<&-
                return 0
            fi
        done
        sleep 0.5
    done
    exec 3<&-
    echo "Device did not confirm the time (firmware too old?)" >&2
    return 1
}

query_time() {
    local port="" i attempt line
    port="$(find_port)" || { echo "No WetGreg serial port found (is it plugged in and running?)" >&2; return 1; }
    stty -F "$port" 115200 raw -echo
    exec 3<>"$port"
    for attempt in 1 2 3; do
        printf 'TIME?\n' >&3
        while IFS= read -r -t 2 line <&3; do
            line="${line%$'\r'}"
            if [[ "$line" == "TIME NOW"* ]]; then
                echo "$port: ${line#TIME NOW }"
                echo "This computer: $(date '+%Y-%m-%d %H:%M:%S')"
                exec 3<&-
                return 0
            fi
        done
    done
    exec 3<&-
    echo "Device did not answer TIME? (firmware too old?)" >&2
    return 1
}

if (( QUERY_ONLY )); then
    query_time
    exit $?
fi

if (( ! SYNC_ONLY )); then
    [[ -f "$UF2" ]] || { echo "Firmware not found: $UF2 (build it first)" >&2; exit 1; }
    echo "Flashing $UF2 ..."
    # -f reboots a running WetGreg into BOOTSEL first; -x runs it afterwards.
    picotool load -x -f "$UF2"
    sleep 1.5   # let the new firmware boot and re-enumerate USB
fi

sync_time
