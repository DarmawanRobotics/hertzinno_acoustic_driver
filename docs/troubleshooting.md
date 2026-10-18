# Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `login: cannot connect` / `Couldn't connect to server` | wrong address or subnet | [network.md](network.md); `ping 192.168.2.204` |
| `login: HTTP 401 (wrong user or password?)` | password | `export HERTZINO_PASSWORD=...` in the shell that runs `roslaunch` |
| `no password ...: trying the factory default` | `HERTZINO_PASSWORD` not set | set it, or `password:=` (printed by roslaunch) |
| `status` `connected: False` after a while | camera restarted or cable | the node logs in again by itself once the camera answers |
| `*_stream: False` | WebSocket refused or on another port | check the node log (`cannot open ws://...`); try `ws_port` |
| streams reconnect every minute | heartbeats not accepted | lower `heartbeat_sec` (default 20) |
| `no RTSP path found` | the camera uses a path not in `probe_paths` | set `path` (or `url`) in `camera.yaml` |
| `cannot open rtsp://...` repeating | path found but no video | check `rtsp_port` against `get_config` (`video.rtsp_port`); try `decoder: ffmpeg` |
| `detect` → `error: ... detect_times` | duration outside 1..3600 s | |
| `detect` → `no_result` | the camera stopped detecting, no result arrived | `feedback_stream` must be `True`; raise `result_grace` |
| `detect` → `timeout` | no result within detection time + `detect_timeout_margin` | raise `timeout` in the goal; see [first_run.md](first_run.md) check 5 |
| `detect` photo missing, message `sound map photo: ... 404` | photo not written yet | raise `photo_wait` |
| `set_band` refused | band narrower than 2 or wider than 20 kHz, or above half the sample rate | |
| `set_leakage`: `only available in leakage mode` | the camera is in partial discharge mode | the mode is set on the camera's web page |
| `get_file`: `plain names` | `saving_path` / `file_name` with `/` or `..` | copy both from `get_logs` |

## Logs

```bash
roslaunch hertzino_driver hertzino.launch host:=<ip> 2>&1 | tee hertzino.log
rosservice call /hertzino/http_request "{method: GET, path: /system}"   # raw API answer
```
