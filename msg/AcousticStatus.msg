# Camera state, published at status_rate.
Header header
bool connected                # logged in and answering
bool detecting                # a detection is running (GET /detect_status)
DetectionConfig config        # last settings read from the camera
bool fft_stream               # WebSocket streams connected
bool prpd_stream
bool voiceprint_stream
bool feedback_stream
string error                  # last error, empty while everything works
