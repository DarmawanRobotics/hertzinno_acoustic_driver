# Detection settings of the camera (GET /system).
uint8 PARTIAL_DISCHARGE=1
uint8 LEAKAGE=2

uint8 work_mode               # 1 partial discharge, 2 gas leakage, 0 unknown
string mode                   # partial_discharge | leakage | unknown
int32 db_limit                # imaging threshold, 0..100 dB
int32 freq_down_limit         # analysis band, kHz
int32 freq_up_limit
int32 detect_times            # detection time, s
int32 samplerate              # microphone array, Hz
