# One detection: the camera's live feedback (/detectFeedback) or a stored log (/detect_logs).
Header header                 # stamp: when the driver received it
uint8 PARTIAL_DISCHARGE=1
uint8 LEAKAGE=2
uint8 detect_mode             # 1 partial discharge, 2 gas leakage

# status bits. Partial discharge: 1 noise (no discharge), 2 corona, 4 surface, 8 floating.
# Leakage: 1 no leak, 2 leak.
int32 NOISE=1
int32 CORONA=2
int32 SURFACE=4
int32 FLOATING=8
int32 NO_LEAK=1
int32 LEAK=2
int32 status
string type                   # noise | corona | surface | floating | no_leak | leak ('+' joined)
bool abnormal                 # a discharge or a leak
int32 intensity               # soundmap_intensity: discharge amplitude / leak magnitude

# partial discharge: probability of each class
float32 noise_prob
float32 corona_prob
float32 surface_prob
float32 floating_prob

# leakage: estimated leak rate and yearly cost
float32 leakage_value
string leakage_unit           # l/min | ml/s
float32 leakage_cost
string cost_unit              # CNY | GBP | USD | EUR | RUB

string start_time             # camera clock, e.g. 20230407_143129
string end_time
int32 log_id                  # -1 when not known (live feedback)
int32 file_status             # 1 files kept, 0 deleted, -1 unknown

# files of this detection on the camera: get_file with saving_path and a name
string saving_path
string soundmap_photo
string soundmap_video
string prpd_photo
string prpd_video
string spectrum_photo
string spectrum_video
string voiceprint_photo
string voiceprint_video
