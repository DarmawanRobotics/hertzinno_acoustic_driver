#!/usr/bin/env python3
"""Smoke test helper: runs the detect action and prints its result on one line.

detect_client.py detect [duration] [timeout]   one detection, photos fetched
detect_client.py preempt [duration]            cancelled after one second
"""

import sys

import actionlib
import rospy
from hertzinno_acoustic_driver.msg import DetectAction, DetectGoal


def main():
    rospy.init_node('detect_client', anonymous=True)
    mode = sys.argv[1] if len(sys.argv) > 1 else 'detect'
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    client = actionlib.SimpleActionClient('/hertzinno/detect', DetectAction)
    if not client.wait_for_server(rospy.Duration(15.0)):
        print('no action server')
        return 1
    feedback = []
    client.send_goal(DetectGoal(duration=duration, timeout=timeout, fetch_photos=True),
                     feedback_cb=feedback.append)
    if mode == 'preempt':
        rospy.sleep(1.0)
        client.cancel_goal()
    client.wait_for_result(rospy.Duration(90.0))
    r = client.get_result()
    if r is None:
        print('no result')
        return 1
    print('success=%s state=%s type=%s intensity=%d leakage=%g %s soundmap=%s prpd=%s '
          'log_id=%d feedback=%d message=%s'
          % (r.success, r.state, r.result.type, r.result.intensity, r.result.leakage_value,
             r.result.leakage_unit or '-', r.soundmap_photo.format or '-',
             r.prpd_photo.format or '-', r.result.log_id, len(feedback), r.message))
    return 0


if __name__ == '__main__':
    sys.exit(main())
