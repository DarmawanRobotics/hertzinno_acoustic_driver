# RTSP video of the camera (GET /video_config), H.264 only.
int32 width                   # 2592x1944, 1920x1080, 1280x720, 720x480 or 640x480
int32 height
int32 fps                     # 1..30
int32 bit_rate                # kbps: 4096, 2048, 1024 or 512
int32 rtsp_port
