#include "hertzinno_acoustic_driver/ros_utils.hpp"

#include "hertzinno_acoustic_driver/api.hpp"

namespace hertzinno_acoustic_driver
{

DetectionConfig toMsg(const hertzinno::DetectionConfig & c)
{
  DetectionConfig m;
  m.work_mode = static_cast<uint8_t>(c.work_mode > 0 && c.work_mode < 256 ? c.work_mode : 0);
  m.mode = hertzinno::modeName(c.work_mode);
  m.db_limit = c.db_limit;
  m.freq_down_limit = c.freq_down_limit;
  m.freq_up_limit = c.freq_up_limit;
  m.detect_times = c.detect_times;
  m.samplerate = c.samplerate;
  return m;
}

VideoConfig toMsg(const hertzinno::VideoConfig & v)
{
  VideoConfig m;
  m.width = v.width;
  m.height = v.height;
  m.fps = v.fps;
  m.bit_rate = v.bit_rate;
  m.rtsp_port = v.rtsp_port;
  return m;
}

DetectionArea toMsg(const hertzinno::DetectionArea & a)
{
  DetectionArea m;
  m.enabled = a.enabled;
  m.x = a.x;
  m.y = a.y;
  m.width = a.width;
  m.height = a.height;
  return m;
}

LeakageConfig toMsg(const hertzinno::LeakageConfig & c)
{
  LeakageConfig m;
  m.distance = c.distance;
  m.unit = c.unit;
  m.unit_name = c.unit_name;
  m.monetary_unit = c.monetary_unit;
  m.monetary_unit_name = c.monetary_unit_name;
  m.value_of_gas = static_cast<float>(c.value_of_gas);
  m.energy_cost = static_cast<float>(c.energy_cost);
  m.specific_power = static_cast<float>(c.specific_power);
  m.working_hours = c.working_hours;
  m.cost_display = c.cost_display;
  return m;
}

DetectionResult toMsg(
  const hertzinno::DetectionResult & r, const ros::Time & stamp, const std::string & frame_id)
{
  DetectionResult m;
  m.header.stamp = stamp;
  m.header.frame_id = frame_id;
  m.detect_mode = static_cast<uint8_t>(r.detect_mode > 0 && r.detect_mode < 256 ? r.detect_mode : 0);
  m.status = r.status;
  m.type = hertzinno::statusName(r.detect_mode, r.status);
  m.abnormal = hertzinno::isAbnormal(r.detect_mode, r.status);
  m.intensity = r.intensity;
  m.noise_prob = static_cast<float>(r.noise_prob);
  m.corona_prob = static_cast<float>(r.corona_prob);
  m.surface_prob = static_cast<float>(r.surface_prob);
  m.floating_prob = static_cast<float>(r.floating_prob);
  m.leakage_value = static_cast<float>(r.leakage_value);
  m.leakage_unit = r.leakage_unit;
  m.leakage_cost = static_cast<float>(r.leakage_cost);
  m.cost_unit = r.cost_unit;
  m.start_time = r.start_time;
  m.end_time = r.end_time;
  m.log_id = r.log_id;
  m.file_status = r.file_status;
  m.saving_path = r.saving_path;
  m.soundmap_photo = r.soundmap_photo;
  m.soundmap_video = r.soundmap_video;
  m.prpd_photo = r.prpd_photo;
  m.prpd_video = r.prpd_video;
  m.spectrum_photo = r.spectrum_photo;
  m.spectrum_video = r.spectrum_video;
  m.voiceprint_photo = r.voiceprint_photo;
  m.voiceprint_video = r.voiceprint_video;
  return m;
}

hertzinno::WebSocketStream::Options streamOptions(
  ros::NodeHandle & pnh, const hertzinno::AcousticCamera & camera, const std::string & path)
{
  hertzinno::WebSocketStream::Options o;
  int ws_port = 0;
  pnh.param("ws_port", ws_port, 0);
  pnh.param("heartbeat_sec", o.heartbeat_sec, 20.0);
  o.host = camera.config().host;
  o.port = ws_port > 0 ? ws_port : camera.config().port;
  o.path = path;
  o.connect_timeout = camera.config().timeout;
  return o;
}

void logStream(const std::string & message)
{
  if (message.rfind("connected", 0) == 0) {
    ROS_INFO("%s", message.c_str());
  } else {
    ROS_WARN("%s", message.c_str());
  }
}

}  // namespace hertzinno_acoustic_driver
