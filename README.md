<div align="center">

# hertzino_driver

**ROS Noetic driver for the Hertzinno HZ-FA-371 online acoustic camera — partial discharge and
gas leak detection with results, photos, records and the sound map video.**


[![CI](https://github.com/DarmawanRobotics/hertzino_driver/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/DarmawanRobotics/hertzino_driver/actions/workflows/ci.yml)
[![ROS](https://img.shields.io/badge/ROS-Noetic-22314E?logo=ros&logoColor=white)](https://wiki.ros.org/noetic)
[![Ubuntu](https://img.shields.io/badge/Ubuntu-20.04-E95420?logo=ubuntu&logoColor=white)](https://releases.ubuntu.com/20.04/)
![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue)](LICENSE)

</div>

---

Start a detection and get its result when it is really done (type, intensity, leak rate,
photos), change every setting, read the camera's records and watch the sound map video. All
over Ethernet: **HTTP** (token login), **WebSocket** (spectrum, results) and **RTSP** (video),
following the vendor manual *Online Acoustic Camera User Manual and API* V5.1
([PDF](docs/hz-fa-371_api_v5.1.pdf)).

**Docs:** [API summary](docs/api.md) · [network setup](docs/network.md) ·
[first run on the camera](docs/first_run.md) · [troubleshooting](docs/troubleshooting.md) ·
[messages](hertzino_msgs/README.md)

| Package | |
|---|---|
| [`hertzino_msgs`](hertzino_msgs) | messages, services and the `Detect` action |
| [`hertzino_driver`](hertzino_driver) | the camera client (no ROS), `hertzino_acoustic_node`, `hertzino_camera_node` |

## Quick start

```bash
mkdir -p ~/catkin_ws/src && cd ~/catkin_ws/src
git clone https://github.com/DarmawanRobotics/hertzino_driver.git
cd ~/catkin_ws
rosdep update --rosdistro noetic --include-eol-distros
rosdep install --from-paths src --ignore-src -y --rosdistro noetic
catkin_make && source devel/setup.bash

export HERTZINO_PASSWORD='<camera password>'        # never printed by roslaunch
roslaunch hertzino_driver hertzino.launch host:=192.168.2.204
```

The camera's factory address is `192.168.2.204` (user `admin`, password `123456`): give the
robot computer an address in `192.168.2.x`, e.g. `sudo ip addr add 192.168.2.10/24 dev eth0`.

## Interface

Everything under `/hertzino`.

| Name | Type | |
|---|---|---|
| `detect` | action `Detect` | one detection; succeeds only with a result measured **after** the goal |
| `status` | `AcousticStatus` | connected, detecting, mode, band, threshold, streams |
| `detection` | `DetectionResult` | every result: type, intensity, probabilities, leak rate, files |
| `spectrum` | `Spectrum` | FFT 5 Hz with the peak in the analysis band |
| `prpd`, `voiceprint` | `RawFrame` | raw frames, 1 Hz |
| `camera/image_raw` | `sensor_msgs/Image` | video with the sound map |
| `get_config`, `set_band`, `set_threshold`, `set_duration`, `set_video`, `set_detection_area`, `set_leakage` | services | settings |
| `start_detection`, `stop_detection` | `std_srvs/Trigger` | continuous monitoring |
| `get_logs`, `get_file`, `delete_logs` | services | the camera's records and files |
| `http_request` | `RawRequest` | any other API call |

Result types: `noise`, `corona`, `surface`, `floating` (partial discharge), `no_leak`, `leak`.

**`detect` states:** `detected` · `no_result` (camera stopped, no result) · `timeout` ·
`preempted` · `error`. A running detection is restarted first, so a result measured while the
robot was still moving is never taken.

## Usage

```bash
# one detection
rostopic pub -1 /hertzino/detect/goal hertzino_msgs/DetectActionGoal \
  "{goal: {duration: 3, fetch_photos: true}}"
rostopic echo -n 1 /hertzino/detect/result

# settings
rosservice call /hertzino/set_band "{freq_down_limit: 20, freq_up_limit: 40}"   # kHz
rosservice call /hertzino/set_threshold "value: 10"                              # dB

# records
rosservice call /hertzino/get_logs "{count: 10}"
rosservice call /hertzino/get_file "{saving_path: '<saving_path>', file_name: '<name>.jpg', save_to: '~/inspection'}"
```

```python
import actionlib, rospy
from hertzino_msgs.msg import DetectAction, DetectGoal

rospy.init_node('acoustic_inspection')
detect = actionlib.SimpleActionClient('/hertzino/detect', DetectAction)
detect.wait_for_server()
detect.send_goal_and_wait(DetectGoal(duration=3.0, fetch_photos=True))
r = detect.get_result()
if r.success and r.result.abnormal:
    open('/tmp/soundmap.jpg', 'wb').write(r.soundmap_photo.data)
```

Parameters are commented in [hertzino.yaml](hertzino_driver/config/hertzino.yaml) and
[camera.yaml](hertzino_driver/config/camera.yaml).

## Verify on the real camera

The manual leaves some points open; check these on the first run:

1. `status` shows `connected: True` and all `*_stream: True`.
2. The camera node logs `RTSP path /<path>`; if none is found, set `path` in `camera.yaml`.
3. `detect` ends `detected` and matches the camera's web page (*Data view*).
4. `spectrum/band_peak_hz` follows a known ultrasonic source.
5. Note the `data` length of `prpd` / `voiceprint` so the frames can be decoded.

## Develop

```bash
catkin_make run_tests && catkin_test_results build/test_results
bash src/hertzino_driver/hertzino_driver/test/smoke_test.sh
```

Unit tests cover the manual's messages, WebSocket, RTSP and the client against a fake camera;
the smoke test runs the launch file against it. CI runs both on `ros:noetic`.

Released under the [Apache License 2.0](LICENSE).