# Camera API summary

What the driver uses from *Online Acoustic Camera User Manual and API* V5.1
([PDF](hz-fa-371_api_v5.1.pdf), chapter 5). Default address `http://192.168.2.204`.

## Login

```
POST /login
{"username": "admin", "password": "<md5(password + 'lfxs')>"}     → {"token": "..."}
```

The password field is the lower-case MD5 of the password followed by `lfxs`
(`123456` → `md5("123456lfxs")` = `7cb8406e5a99c917a60681762d7952c2`). Every other request
carries `Authorization: Bearer <token>`; an invalid or expired token answers **401**, and the
driver logs in again by itself.

## HTTP

| Method, path | Body / answer | Driver |
|---|---|---|
| `GET /system` | `work_mode` (1 partial discharge, 2 leakage), `dB_limit` (0..100), `freq_down_limit`, `freq_up_limit` (kHz), `detect_times` (s), `samplerate` (Hz) | `status`, `get_config` |
| `PUT /system` | one field at a time; the band as a pair, `2 ≤ up − down ≤ 20` | `set_threshold`, `set_band`, `set_duration` |
| `GET` / `PUT /video_config` | `video_width`+`video_height` (2592×1944, 1920×1080, 1280×720, 720×480, 640×480), `video_fps` (1..30), `video_bit_rate` (4096, 2048, 1024, 512 kbps), `video_port` (RTSP, 443) | `get_config`, `set_video` |
| `GET` / `PUT /signal_filtering_area` | `detect_area_status`, `left_top_x`, `left_top_y`, `area_width`, `area_height`; all five on PUT | `set_detection_area` |
| `GET` / `PUT /leakage_estimation` | leakage mode only; one field per PUT (`leakage_distance`, `leakage_unit`, `monetary_unit`, `cost_display`, `value_of_gas`, `energy_cost`, `specific_power`, `working_hours`) | `set_leakage` |
| `POST /turn_on_detect`, `POST /turn_off_detect` | no body; results come on the WebSocket | `detect`, `start_detection`, `stop_detection` |
| `GET /detect_status` | `{"detect_status": 0 \| 1}` | `status` |
| `GET /detect_logs?start_col=&col_count=&start_time=&end_time=&discharge_type=&detect_mode=` | `{"logs": [...], "logs_count": n}`; the camera keeps the latest 100 | `get_logs` |
| `GET /detect_file/<saving_path>/<file_name>` | the JPEG or MP4 of a record | `get_file`, `detect` photos |
| `DELETE /detect_logs` | `{"id": ["2", "3"]}` | `delete_logs` |

## WebSocket (`ws://<ip>:80/...`)

Clients must send something at least once a minute or the camera closes the connection (the
driver sends `heartbeat` every 20 s).

| Path | Content |
|---|---|
| `/fftData` | 5 per second: 512 16-bit integers, the first half of the FFT (`bin = samplerate / 1024` Hz) |
| `/prpdData` | 1 per second: PRPD image as integers (layout not documented) |
| `/voiceprintData` | 1 per second: voiceprint image as integers (layout not documented) |
| `/detectFeedback` | after each detection: a JSON result (fields below) |

## Results

| Field | Partial discharge | Leakage |
|---|---|---|
| `detect_mode` | 1 | 2 |
| `status` (bits) | 1 noise, 2 corona, 4 surface, 8 floating | 1 no leak, 2 leak |
| `soundmap_intensity` | discharge amplitude | leak magnitude |
| probabilities | `noise_prob`, `corona_prob`, `surface_prob`, `floating_prob` | — |
| leak estimate | — | `leakage_value` + `leakage_unit` (l/min, ml/s), `leakage_cost` + `cost_unit` per year |
| times | `start_time`, `end_time` as `YYYYMMDD_hhmmss` on the camera clock | same |
| files | `saving_path` + `power_spect_*`, `prpd_*`, `soundmap_*`, `voiceprint_*` `_photo_name` / `_video_name` | same, without PRPD |

Records (`/detect_logs`) add `id` and `files_status` (1 kept, 0 deleted).

## RTSP

H.264 on port `video_port` (443 by default). The manual gives no path: the camera node asks
common paths with `DESCRIBE` (see [first_run.md](first_run.md)).
