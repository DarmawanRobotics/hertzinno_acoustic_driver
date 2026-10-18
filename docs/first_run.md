# First run on the camera

The driver follows the manual and is tested against a simulated camera
([fake_device.py](../hertzino_driver/test/fake_device.py)). These checks cover what the manual
leaves open. Note what you see in the **Result** column: it decides the defaults that should go
into the config files.

```bash
export HERTZINO_PASSWORD='<camera password>'
roslaunch hertzino_driver hertzino.launch host:=192.168.2.204
```

| # | Check | Command | Expected | Result |
|---|---|---|---|---|
| 1 | Login | `rostopic echo -n 1 /hertzino/status` | `connected: True`, mode, band, detection time | |
| 2 | Streams | same | `fft_stream`, `prpd_stream`, `voiceprint_stream`, `feedback_stream` all `True` | |
| 3 | RTSP path | node log | `RTSP path /<path>` | path: |
| 4 | Video | `rosrun rqt_image_view rqt_image_view /hertzino/camera/image_raw` | the camera image with the sound map | |
| 5 | Detection | `rostopic pub -1 /hertzino/detect/goal hertzino_msgs/DetectActionGoal "{goal: {duration: 3, fetch_photos: true}}"` then `rostopic echo -n 1 /hertzino/detect/result` | `state: "detected"`, type and intensity as on the web page (*Data view*) | |
| 6 | Photos | same result | `soundmap_photo.format: "jpeg"` with data | |
| 7 | Spectrum | `rostopic echo /hertzino/spectrum/band_peak_hz` near a known ultrasonic source (compressed air leak, ultrasonic emitter) | follows the source frequency | |
| 8 | PRPD / voiceprint | `rostopic echo -n 1 --noarr /hertzino/prpd` and `/hertzino/voiceprint` | note the `data` length | prpd: · voiceprint: |
| 9 | Detection area | `rosservice call /hertzino/set_detection_area "{enabled: true, x: 640, y: 300, width: 640, height: 480}"` | the box on the web page where expected | |
| 10 | Records | `rosservice call /hertzino/get_logs "{count: 5}"` | the detection of check 5 | |

## If a check fails

- **3:** no path answered: ask the vendor for the RTSP URL (or read it from the web page's
  player) and set `path` in [camera.yaml](../hertzino_driver/config/camera.yaml).
- **5 `no_result` / `timeout`:** the camera reports the end of a detection differently; keep
  the node log and the output of `rostopic echo /hertzino/detection` during the goal.
- **7:** the peak does not follow the source: the 16-bit values may be big-endian or unsigned.
- **8:** with the lengths the frames can be decoded into images in a later version.

See also [troubleshooting.md](troubleshooting.md).
