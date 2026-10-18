#!/usr/bin/env bash
# End-to-end smoke test: hertzino.launch against test/fake_device.py on the loopback.
# Status, WebSocket streams, settings, the detect action (live feedback, records fallback,
# restart of continuous monitoring, cancel, timeout), records and files, leakage mode, the
# camera node finding the RTSP path, token renewal and heartbeats. Needs a sourced workspace.
set -u
# rospy / rostopic output into a pipe is block-buffered and lost when timeout kills it
export PYTHONUNBUFFERED=1

here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
port=18090
rtsp=18554
ns=/hertzino
fail=0
pids=()

cleanup() {
  for p in "${pids[@]}"; do kill -INT "$p" 2>/dev/null; done
  sleep 2
  for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null; done
}
trap cleanup EXIT

result() {  # result <name> <0 ok>
  if [ "$2" = 0 ]; then
    echo "ok   $1"
  else
    echo "FAIL $1"
    fail=1
  fi
}

check() {  # check <name> <pattern> <command...>
  local name="$1" pattern="$2"
  shift 2
  local out
  out="$(timeout 60 "$@" 2>&1)"
  if grep -qE -- "$pattern" <<<"$out"; then
    echo "ok   $name"
  else
    echo "FAIL $name"
    echo "$out" | tail -n 25
    fail=1
  fi
}

in_order() {  # in_order <name> <first pattern> <second pattern> <file>: second after first
  if awk -v a="$2" -v b="$3" '$0 ~ a {seen=1; next} seen && $0 ~ b {found=1} END {exit !found}' "$4"
  then
    echo "ok   $1"
  else
    echo "FAIL $1"
    tail -n 15 "$4"
    fail=1
  fi
}

device() {  # device <json>: change the fake camera's behaviour
  curl -s -X POST -d "$1" "http://127.0.0.1:$port/debug/set" > /dev/null
}

since() {  # since <line count>: the device log after that line
  tail -n +"$(($1 + 1))" "$work/device.log"
}

# the password goes through the environment, so roslaunch never prints it
export HERTZINO_PASSWORD=smoke-pass-7
python3 "$here/fake_device.py" --port "$port" --password "$HERTZINO_PASSWORD" \
  --rtsp-port "$rtsp" --rtsp-path live --rtsp-auth --token-ttl 20 --ws-idle 30 \
  --log "$work/device.log" > /dev/null 2>&1 &
pids+=($!)
roslaunch hertzino_driver hertzino.launch host:=127.0.0.1 port:="$port" \
  rtsp_port:="$rtsp" > "$work/launch.log" 2>&1 &
pids+=($!)
for _ in $(seq 1 30); do
  rostopic list 2>/dev/null | grep -q "$ns/status" && break
  sleep 1
done
sleep 4

# ---- status and streams
check "status connected" "connected: True" rostopic echo -n 1 $ns/status
check "status mode" 'mode: "partial_discharge"' rostopic echo -n 1 $ns/status
check "feedback stream" "feedback_stream: True" rostopic echo -n 1 $ns/status
check "spectrum" "bin_hz: 93.75" rostopic echo -n 1 --noarr $ns/spectrum
check "spectrum band peak" "band_peak_hz: 30000.0" rostopic echo -n 1 --noarr $ns/spectrum
check "prpd frames" 'stream: "prpdData"' rostopic echo -n 1 --noarr $ns/prpd
check "voiceprint frames" 'stream: "voiceprintData"' rostopic echo -n 1 --noarr $ns/voiceprint

# ---- settings
check "get_config" "rtsp_port: 443" rosservice call $ns/get_config
check "set_threshold" "db_limit: 25" rosservice call $ns/set_threshold "value: 25"
check "threshold reached the camera" 'PUT /system \{"dB_limit":25\}' cat "$work/device.log"
check "set_band" "freq_down_limit: 25" \
  rosservice call $ns/set_band "{freq_down_limit: 25, freq_up_limit: 40}"
check "set_band too wide" "2 to 20 kHz wide" \
  rosservice call $ns/set_band "{freq_down_limit: 10, freq_up_limit: 40}"
check "set_video" "fps: 30" rosservice call $ns/set_video "{fps: 30}"
check "set_video bad resolution" "success: False" \
  rosservice call $ns/set_video "{width: 1000, height: 1000}"
check "set_detection_area" "width: 320" rosservice call $ns/set_detection_area \
  "{enabled: true, x: 100, y: 80, width: 320, height: 240}"
check "set_leakage outside leakage mode" "HTTP 400: leakage estimation" \
  rosservice call $ns/set_leakage "{field: monetary_unit, value: 2}"

# ---- detections
timeout 40 rostopic echo -n 1 $ns/detection > "$work/detection.txt" 2>&1 &
sleep 1
check "detect (live feedback)" \
  "success=True state=detected type=surface intensity=17 .*soundmap=jpeg prpd=jpeg log_id=1 " \
  python3 "$here/detect_client.py" detect 2
check "detection topic" 'type: "surface"' cat "$work/detection.txt"
check "get_logs" "total: 1" rosservice call $ns/get_logs "{}"
logs="$(rosservice call $ns/get_logs "{}" 2>&1)"
saving="$(sed -nE 's/.*saving_path: "?([^"]*)"?$/\1/p' <<<"$logs" | head -n 1)"
photo="$(sed -nE 's/.*soundmap_photo: "?([^"]*)"?$/\1/p' <<<"$logs" | head -n 1)"
check "get_file" "success: True" rosservice call $ns/get_file \
  "{saving_path: '$saving', file_name: '$photo', save_to: '$work/files'}"
check "file on disk" "sound_" ls -R "$work/files"

device '{"no_feedback": true}'
check "detect (records, no live feedback)" "success=True state=detected type=surface .*log_id=2 " \
  python3 "$here/detect_client.py" detect 1
device '{"no_feedback": false, "continuous": true}'

check "start_detection" "success: True" rosservice call $ns/start_detection
sleep 2
check "status detecting" "detecting: True" rostopic echo -n 1 $ns/status
mark="$(wc -l < "$work/device.log")"
check "detect restarts continuous monitoring" "success=True state=detected type=surface" \
  python3 "$here/detect_client.py" detect 1
since "$mark" > "$work/restart.log"
in_order "stopped, then started again" "POST /turn_off_detect" "POST /turn_on_detect" \
  "$work/restart.log"
device '{"continuous": false}'

mark="$(wc -l < "$work/device.log")"
check "detect cancelled" "state=preempted" python3 "$here/detect_client.py" preempt 5
since "$mark" > "$work/cancel.log"
in_order "cancel stops the detection" "POST /turn_on_detect" "POST /turn_off_detect" \
  "$work/cancel.log"

device '{"hang": true}'
check "detect timeout" "success=False state=timeout" python3 "$here/detect_client.py" detect 1 3
device '{"hang": false}'

check "delete_logs" "success: True" rosservice call $ns/delete_logs "ids: [1]"
check "deletion reached the camera" 'DELETE /detect_logs \{"id":\["1"\]\}' cat "$work/device.log"
check "start / stop" "success: True" rosservice call $ns/stop_detection
check "http_request" "dB_limit" rosservice call $ns/http_request "{method: GET, path: /system}"

# ---- leakage mode
device '{"mode": 2, "result": "leak"}'
check "leakage settings" "has_leakage: True" rosservice call $ns/get_config
check "set_leakage" 'monetary_unit_name: "USD"' \
  rosservice call $ns/set_leakage "{field: monetary_unit, value: 2}"
check "detect a leak" "success=True state=detected type=leak intensity=46 leakage=1.25 l/min" \
  python3 "$here/detect_client.py" detect 1

# ---- camera node, sessions
check "camera found the RTSP path" "RTSP path /live" cat "$work/launch.log"
check "camera node streaming attempts" "cannot open rtsp://admin@127.0.0.1:$rtsp/live" \
  cat "$work/launch.log"
logins="$(grep -c '^POST /login' "$work/device.log")"
[ "$logins" -ge 2 ]
result "expired token renewed ($logins logins)" $?
! grep -q "WS idle close" "$work/device.log"
result "websocket heartbeats" $?
! grep -q "smoke-pass-7" "$work/launch.log"
result "password never in the log" $?
! grep -qE "process has died|REQUIRED process" "$work/launch.log"
result "no node died" $?

if [ "$fail" != 0 ]; then
  echo "---- launch log"
  tail -n 80 "$work/launch.log"
  echo "---- device log"
  tail -n 40 "$work/device.log"
fi
exit "$fail"
