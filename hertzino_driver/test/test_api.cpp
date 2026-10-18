// JSON messages of the manual (chapter 5 examples), request bodies, limits and names.
#include <gtest/gtest.h>

#include <string>

#include "hertzino_driver/api.hpp"
#include "hertzino_driver/crypto.hpp"

using namespace hertzino;

// ---------------------------------------------------------------- parsers, manual examples

TEST(Api, Token)
{
  std::string err;
  EXPECT_EQ(parseToken(R"({"token": "abc.def"})", &err).value_or(""), "abc.def");
  EXPECT_EQ(parseToken(R"({"code": 0, "data": {"token": "t1"}})", &err).value_or(""), "t1");
  EXPECT_FALSE(parseToken(R"({"token": ""})", &err));
  EXPECT_FALSE(parseToken("<html>401</html>", &err));
  EXPECT_NE(err.find("not a JSON object"), std::string::npos);
}

TEST(Api, SystemExample)   // 5.1.2
{
  std::string err;
  const auto c = parseDetectionConfig(R"({
        "work_mode": 1,
  "dB_limit": 10,
        "freq_down_limit": 20,
        "freq_up_limit": 24,
        "detect_times": 3,
        "samplerate": 96000
})", &err);
  ASSERT_TRUE(c) << err;
  EXPECT_EQ(c->work_mode, 1);
  EXPECT_EQ(c->db_limit, 10);
  EXPECT_EQ(c->freq_down_limit, 20);
  EXPECT_EQ(c->freq_up_limit, 24);
  EXPECT_EQ(c->detect_times, 3);
  EXPECT_EQ(c->samplerate, 96000);
}

TEST(Api, TolerantNumbers)
{
  std::string err;
  const auto c = parseDetectionConfig(R"({"work_mode": "2", "dB_limit": 15.6, "samplerate": "130000"})", &err);
  ASSERT_TRUE(c) << err;
  EXPECT_EQ(c->work_mode, 2);
  EXPECT_EQ(c->db_limit, 16);
  EXPECT_EQ(c->samplerate, 130000);
  EXPECT_EQ(c->detect_times, -1);   // absent
  EXPECT_FALSE(parseDetectionConfig(R"({"unrelated": 1})", &err));
  EXPECT_FALSE(parseDetectionConfig("[1, 2]", &err));
}

TEST(Api, VideoExample)   // 5.1.4
{
  std::string err;
  const auto v = parseVideoConfig(R"({
        "video_bit_rate": 4096,
        "video_fps": 25,
        "video_height": 1080,
        "video_port": 443,
        "video_width": 1920
})", &err);
  ASSERT_TRUE(v) << err;
  EXPECT_EQ(v->width, 1920);
  EXPECT_EQ(v->height, 1080);
  EXPECT_EQ(v->fps, 25);
  EXPECT_EQ(v->bit_rate, 4096);
  EXPECT_EQ(v->rtsp_port, 443);
}

TEST(Api, LeakageExample)   // 5.1.6
{
  std::string err;
  const auto c = parseLeakageConfig(R"({
        "cost_display": 0,
        "leakage_distance": 0,
        "leakage_unit": 0,
        "leakage_unit_str": "l/min",
        "monetary_unit": 0,
        "monetary_unit_str": "CNY",
"energy_cost": 0,
        "specific_power": 0,
"value_of_gas": 0,
        "working_hours": 0
})", &err);
  ASSERT_TRUE(c) << err;
  EXPECT_EQ(c->unit_name, "l/min");
  EXPECT_EQ(c->monetary_unit_name, "CNY");
  EXPECT_FALSE(c->cost_display);
  const auto d = parseLeakageConfig(
    R"({"leakage_distance": 3, "monetary_unit": 2, "monetary_unit_str": "USD", "value_of_gas": 0.35, "working_hours": 6000, "cost_display": 1})",
    &err);
  ASSERT_TRUE(d) << err;
  EXPECT_EQ(d->distance, 3);
  EXPECT_DOUBLE_EQ(d->value_of_gas, 0.35);
  EXPECT_EQ(d->working_hours, 6000);
  EXPECT_TRUE(d->cost_display);
}

TEST(Api, AreaExample)   // 5.1.8
{
  std::string err;
  const auto a = parseDetectionArea(R"({
        "area_height": -1,
        "area_width": -1,
        "detect_area_status": 0,
        "left_top_x": -1,
        "left_top_y": -1
})", &err);
  ASSERT_TRUE(a) << err;
  EXPECT_FALSE(a->enabled);
  EXPECT_EQ(a->x, -1);
  EXPECT_EQ(a->width, -1);
}

TEST(Api, DetectStatus)   // 5.1.12: "boolean", given as 0 / 1
{
  std::string err;
  EXPECT_EQ(parseDetectStatus(R"({ "detect_status": 0 })", &err), std::optional<bool>(false));
  EXPECT_EQ(parseDetectStatus(R"({"detect_status": 1})", &err), std::optional<bool>(true));
  EXPECT_EQ(parseDetectStatus(R"({"detect_status": true})", &err), std::optional<bool>(true));
  EXPECT_EQ(parseDetectStatus(R"({"detect_status": "1"})", &err), std::optional<bool>(true));
  EXPECT_EQ(parseDetectStatus(R"({"code": 0, "data": {"detect_status": 1}})", &err),
    std::optional<bool>(true));
  EXPECT_FALSE(parseDetectStatus(R"({"detect_status": "running"})", &err));
}

TEST(Api, PartialDischargeLogExample)   // 5.1.13
{
  std::string err;
  const auto page = parseLogPage(R"({
        "logs": [
            {
                "id": 1,
"detect_mode": 1,
                "start_time": "20230407_143129",
                "end_time": "20230407_143154",
"status": 1,
"soundmap_intensity": 46,
"noise_prob": 1,
                "corona_prob": 0,
"floating_prob": 0,
"surface_prob": 0,
                "files_status": 1,
                "saving_path": "2023_04_07_14_31_29_133/",
                "power_spect_photo_name": "power_spectrum_2023_04_07_14_31_29_133.jpg",
                "power_spect_video_name": "power_spectrum_2023_04_07_14_31_29_133.mp4",
                "prpd_photo_name": "prpd_2023_04_07_14_31_29_133.jpg",
                "prpd_video_name": "prpd_2023_04_07_14_31_29_133.mp4",
"soundmap_photo_name": "sound_2023_04_07_14_31_29_133.jpg",
                "soundmap_video_name": "sound_2023_04_07_14_31_29_133.mp4",
                "voiceprint_photo_name": "voiceprint_2023_04_07_14_31_29_133.jpg",
                "voiceprint_video_name": "voiceprint_2023_04_07_14_31_29_133.mp4"
            }
        ],
        "logs_count": 1
})", &err);
  ASSERT_TRUE(page) << err;
  EXPECT_EQ(page->total, 1);
  ASSERT_EQ(page->logs.size(), 1u);
  const auto & r = page->logs[0];
  EXPECT_EQ(r.log_id, 1);
  EXPECT_EQ(r.detect_mode, 1);
  EXPECT_EQ(r.start_time, "20230407_143129");
  EXPECT_EQ(r.end_time, "20230407_143154");
  EXPECT_EQ(r.status, 1);
  EXPECT_EQ(r.intensity, 46);
  EXPECT_DOUBLE_EQ(r.noise_prob, 1.0);
  EXPECT_EQ(r.file_status, 1);
  EXPECT_EQ(r.saving_path, "2023_04_07_14_31_29_133/");
  EXPECT_EQ(r.spectrum_photo, "power_spectrum_2023_04_07_14_31_29_133.jpg");
  EXPECT_EQ(r.prpd_video, "prpd_2023_04_07_14_31_29_133.mp4");
  EXPECT_EQ(r.soundmap_photo, "sound_2023_04_07_14_31_29_133.jpg");
  EXPECT_EQ(r.voiceprint_video, "voiceprint_2023_04_07_14_31_29_133.mp4");
}

TEST(Api, LeakageLogExampleAndFileStatusSpelling)   // 5.1.14
{
  std::string err;
  const auto page = parseLogPage(R"({"logs": [{"id": 7, "detect_mode": 2,
    "start_time": "20230407_143129", "end_time": "20230407_143154", "status": 2,
    "soundmap_intensity": 46, "leakage_value": 1.5, "leakage_unit": "l/min",
    "leakage_cost": 320.25, "cost_unit": "CNY", "file_status": 0,
    "saving_path": "2023_04_07_14_31_29_133/"}], "logs_count": 12})", &err);
  ASSERT_TRUE(page) << err;
  EXPECT_EQ(page->total, 12);
  const auto & r = page->logs.at(0);
  EXPECT_EQ(r.log_id, 7);
  EXPECT_DOUBLE_EQ(r.leakage_value, 1.5);
  EXPECT_EQ(r.leakage_unit, "l/min");
  EXPECT_DOUBLE_EQ(r.leakage_cost, 320.25);
  EXPECT_EQ(r.cost_unit, "CNY");
  EXPECT_EQ(r.file_status, 0);
  EXPECT_TRUE(r.prpd_photo.empty());
  const auto empty = parseLogPage(R"({"logs": null, "logs_count": 0})", &err);
  ASSERT_TRUE(empty) << err;
  EXPECT_TRUE(empty->logs.empty());
}

TEST(Api, FeedbackExamples)   // 5.2.5
{
  std::string err;
  const auto pd = parseDetectionResult(R"({
"detect_mode": 1,
                "start_time": "20230407_143129",
                "end_time": "20230407_143154",
"status": 4,
"soundmap_intensity": 46,
"noise_prob": 0.01,
                "corona_prob": 0.02,
"floating_prob": 0.03,
"surface_prob": 0.94,
                "saving_path": "2023_04_07_14_31_29_133/",
"soundmap_photo_name": "sound_2023_04_07_14_31_29_133.jpg"
})", &err);
  ASSERT_TRUE(pd) << err;
  EXPECT_EQ(pd->log_id, -1);
  EXPECT_EQ(pd->status, 4);
  EXPECT_DOUBLE_EQ(pd->surface_prob, 0.94);
  EXPECT_EQ(pd->file_status, -1);
  EXPECT_EQ(pd->soundmap_photo, "sound_2023_04_07_14_31_29_133.jpg");
  const auto leak = parseDetectionResult(
    R"({"detect_mode": 2, "status": 2, "soundmap_intensity": 46, "leakage_value": 0.5, "leakage_unit": 1, "leakage_cost": 0, "cost_unit": "CNY"})",
    &err);
  ASSERT_TRUE(leak) << err;
  EXPECT_EQ(leak->leakage_unit, "ml/s");   // numeric unit as in the settings
  EXPECT_FALSE(parseDetectionResult("heartbeat", &err));
  EXPECT_FALSE(parseDetectionResult(R"({"hello": 1})", &err));
}

TEST(Api, ErrorBodies)
{
  EXPECT_EQ(jsonErrorText(R"({"code": 400, "msg": "invalid band"})"), "invalid band");
  EXPECT_EQ(jsonErrorText(R"({"message": "token expired"})"), "token expired");
  EXPECT_EQ(jsonErrorText("Bad Request"), "");
  std::string m;
  EXPECT_FALSE(jsonReportsFailure(R"({"code": 0, "msg": "success"})", &m));
  EXPECT_FALSE(jsonReportsFailure(R"({"code": 200})", &m));
  EXPECT_FALSE(jsonReportsFailure(R"({"dB_limit": 10})", &m));
  EXPECT_FALSE(jsonReportsFailure("", &m));
  EXPECT_TRUE(jsonReportsFailure(R"({"code": -1, "msg": "param error"})", &m));
  EXPECT_EQ(m, "param error");
  EXPECT_TRUE(jsonReportsFailure(R"({"success": false})", &m));
}

// ---------------------------------------------------------------- bodies and paths

TEST(Api, Bodies)
{
  EXPECT_EQ(loginBody("admin", "123456"),
    R"({"password":"7cb8406e5a99c917a60681762d7952c2","username":"admin"})");
  EXPECT_EQ(intFieldBody("dB_limit", 25), R"({"dB_limit":25})");
  EXPECT_EQ(bandBody(20, 30), R"({"freq_down_limit":20,"freq_up_limit":30})");
  EXPECT_EQ(resolutionBody(1280, 720), R"({"video_height":720,"video_width":1280})");
  DetectionArea a;
  a.enabled = true;
  a.x = 10;
  a.y = 20;
  a.width = 300;
  a.height = 200;
  EXPECT_EQ(areaBody(a),
    R"({"area_height":200,"area_width":300,"detect_area_status":1,"left_top_x":10,"left_top_y":20})");
  EXPECT_EQ(deleteLogsBody({2, 3}), R"({"id":["2","3"]})");
  EXPECT_EQ(numberFieldBody("working_hours", 6000.0), R"({"working_hours":6000})");
  EXPECT_EQ(numberFieldBody("value_of_gas", 0.5), R"({"value_of_gas":0.5})");
}

TEST(Api, LogsPath)
{
  LogQuery q;
  EXPECT_EQ(logsPath(q),
    "/detect_logs?start_col=1&col_count=100&start_time=0&end_time=2000000000"
    "&discharge_type=15&detect_mode=1");
  q.detect_mode = kLeakage;
  q.start = 50;
  q.count = 5;
  EXPECT_EQ(logsPath(q),
    "/detect_logs?start_col=50&col_count=5&start_time=0&end_time=2000000000"
    "&discharge_type=3&detect_mode=2");
  q.types = 2 + 4;
  q.detect_mode = kPartialDischarge;
  EXPECT_NE(logsPath(q).find("discharge_type=6"), std::string::npos);
}

TEST(Api, FilePath)   // 5.1.15
{
  std::string err;
  EXPECT_EQ(filePath("2023_04_07_14_31_29_133/", "sound_2023_04_07_14_31_29_133.jpg", &err),
    "/detect_file/2023_04_07_14_31_29_133/sound_2023_04_07_14_31_29_133.jpg");
  EXPECT_EQ(filePath("/2023_04_07/", "a.mp4", &err), "/detect_file/2023_04_07/a.mp4");
  EXPECT_EQ(filePath("..", "a.jpg", &err), "");
  EXPECT_EQ(filePath("x", "../../etc/passwd", &err), "");
  EXPECT_EQ(filePath("x/y", "a.jpg", &err), "");
  EXPECT_EQ(filePath("x", "", &err), "");
  EXPECT_NE(err.find("plain names"), std::string::npos);
}

// ---------------------------------------------------------------- limits

TEST(Api, Limits)
{
  EXPECT_EQ(checkThreshold(0), "");
  EXPECT_EQ(checkThreshold(100), "");
  EXPECT_NE(checkThreshold(101), "");
  EXPECT_NE(checkThreshold(-1), "");

  EXPECT_EQ(checkBand(20, 30, 96000), "");
  EXPECT_EQ(checkBand(28, 48, 96000), "");
  EXPECT_EQ(checkBand(0, 2, 0), "");
  EXPECT_NE(checkBand(29, 49, 96000), "");     // above 48 kHz
  EXPECT_EQ(checkBand(45, 65, 130000), "");    // 2..65 kHz on 130 kHz firmware
  EXPECT_NE(checkBand(20, 21, 96000), "");     // narrower than 2 kHz
  EXPECT_NE(checkBand(10, 31, 96000), "");     // wider than 20 kHz
  EXPECT_NE(checkBand(30, 20, 96000), "");
  EXPECT_NE(checkBand(-2, 10, 96000), "");

  EXPECT_EQ(checkDuration(3), "");
  EXPECT_NE(checkDuration(0), "");
  EXPECT_EQ(checkResolution(1920, 1080), "");
  EXPECT_EQ(checkResolution(640, 480), "");
  EXPECT_NE(checkResolution(1930, 1080), "");  // the manual's own typo
  EXPECT_EQ(checkBitRate(512), "");
  EXPECT_NE(checkBitRate(3000), "");
  EXPECT_EQ(checkFps(30), "");
  EXPECT_NE(checkFps(31), "");
  EXPECT_NE(checkFps(0), "");
  EXPECT_EQ(checkPort(554), "");
  EXPECT_NE(checkPort(70000), "");

  DetectionArea a;
  EXPECT_EQ(checkArea(a), "");                 // disabled: anything
  a.enabled = true;
  EXPECT_NE(checkArea(a), "");
  a.x = 0;
  a.y = 0;
  a.width = 10;
  a.height = 10;
  EXPECT_EQ(checkArea(a), "");

  EXPECT_EQ(checkLeakageField("leakage_distance", 3), "");
  EXPECT_NE(checkLeakageField("leakage_distance", 4), "");
  EXPECT_NE(checkLeakageField("leakage_distance", 1.5), "");
  EXPECT_EQ(checkLeakageField("value_of_gas", 0.35), "");
  EXPECT_NE(checkLeakageField("value_of_gas", -1), "");
  EXPECT_NE(checkLeakageField("monetary_unit", 5), "");
  EXPECT_NE(checkLeakageField("price", 1).find("unknown"), std::string::npos);
}

// ---------------------------------------------------------------- names and times

TEST(Api, Names)
{
  EXPECT_EQ(modeName(1), "partial_discharge");
  EXPECT_EQ(modeName(2), "leakage");
  EXPECT_EQ(modeName(0), "unknown");
  EXPECT_EQ(statusName(1, 1), "noise");
  EXPECT_EQ(statusName(1, 4), "surface");
  EXPECT_EQ(statusName(1, 8), "floating");
  EXPECT_EQ(statusName(1, 2 | 4), "corona+surface");
  EXPECT_EQ(statusName(1, 0), "unknown(0)");
  EXPECT_EQ(statusName(1, 4 | 16), "surface+unknown(16)");
  EXPECT_EQ(statusName(2, 1), "no_leak");
  EXPECT_EQ(statusName(2, 2), "leak");
  EXPECT_FALSE(isAbnormal(1, 1));
  EXPECT_TRUE(isAbnormal(1, 2));
  EXPECT_TRUE(isAbnormal(1, 8));
  EXPECT_FALSE(isAbnormal(2, 1));
  EXPECT_TRUE(isAbnormal(2, 2));
  EXPECT_EQ(allTypes(1), 15);
  EXPECT_EQ(allTypes(2), 3);
}

TEST(Api, DeviceTime)
{
  const auto a = parseDeviceTime("20230407_143129");
  const auto b = parseDeviceTime("20230407_143154");
  ASSERT_TRUE(a && b);
  EXPECT_EQ(*b - *a, 25);
  EXPECT_EQ(parseDeviceTime("2023_04_07_14_31_29_133/"), a);   // saving_path
  EXPECT_EQ(parseDeviceTime("1970-01-01 00:00:10"), std::optional<long long>(10));
  const auto leap = parseDeviceTime("20240229_235959");
  const auto march = parseDeviceTime("20240301_000000");
  ASSERT_TRUE(leap && march);
  EXPECT_EQ(*march - *leap, 1);
  const auto ny = parseDeviceTime("20231231_235959");
  const auto ny2 = parseDeviceTime("20240101_000001");
  EXPECT_EQ(*ny2 - *ny, 2);
  EXPECT_FALSE(parseDeviceTime("20231"));
  EXPECT_FALSE(parseDeviceTime("20231307_000000"));
  EXPECT_FALSE(parseDeviceTime(""));
}
