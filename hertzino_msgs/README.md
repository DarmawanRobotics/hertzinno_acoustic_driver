# hertzino_msgs

Messages, services and the action of [hertzino_driver](../README.md).

## Action

| | |
|---|---|
| `Detect` | goal: `duration`, `timeout`, `keep_running`, `fetch_photos` · result: `success`, `state` (`detected`, `no_result`, `timeout`, `preempted`, `error`), `result`, `soundmap_photo`, `prpd_photo` · feedback: `elapsed`, `detecting`, `state` |

## Messages

| | |
|---|---|
| `AcousticStatus` | connected, detecting, `config`, the four stream states, last error |
| `DetectionConfig` | work mode (1 partial discharge, 2 leakage), threshold, band, detection time, sample rate |
| `DetectionResult` | one result (live or a record): type, intensity, probabilities, leak rate and cost, camera times, record id, files |
| `DetectionArea` | the box results are limited to |
| `VideoConfig` | RTSP resolution, fps, bit rate, port |
| `LeakageConfig` | leak estimation: distance, units, currency, gas value, energy cost, specific power, hours |
| `Spectrum` | one FFT frame and the peak in the analysis band |
| `RawFrame` | a PRPD or voiceprint frame as received |

## `DetectionResult.status`

A bit field.

| `detect_mode` | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| 1 partial discharge | noise (none) | corona | surface | floating |
| 2 leakage | no leak | leak | | |

`type` is the same as text (`corona+surface` when several bits are set); `abnormal` is true for
any discharge or a leak.

## Services

| | |
|---|---|
| `GetConfig` | all settings |
| `SetValue` | `set_threshold` (dB), `set_duration` (s) |
| `SetBand` | analysis band, kHz |
| `SetVideo` | video settings, 0 keeps a field |
| `SetDetectionArea` | the detection box |
| `SetLeakage` | one leak estimation field |
| `GetLogs` | records, filtered (zeros take the defaults) |
| `GetFile` | a record's photo or video, returned or saved |
| `DeleteLogs` | delete records by id |
| `RawRequest` | any HTTP call to the camera |

```python
from hertzino_msgs.msg import DetectAction, DetectGoal, DetectionResult
from hertzino_msgs.srv import SetBand, GetLogs
```
