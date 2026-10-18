cmake_minimum_required(VERSION 3.0.2)
project(hertzino_msgs)

find_package(catkin REQUIRED COMPONENTS
  std_msgs
  sensor_msgs
  actionlib_msgs
  message_generation
)

add_message_files(FILES
  AcousticStatus.msg
  DetectionArea.msg
  DetectionConfig.msg
  DetectionResult.msg
  LeakageConfig.msg
  RawFrame.msg
  Spectrum.msg
  VideoConfig.msg
)
add_service_files(FILES
  DeleteLogs.srv
  GetConfig.srv
  GetFile.srv
  GetLogs.srv
  RawRequest.srv
  SetBand.srv
  SetDetectionArea.srv
  SetLeakage.srv
  SetValue.srv
  SetVideo.srv
)
add_action_files(FILES
  Detect.action
)
generate_messages(DEPENDENCIES std_msgs sensor_msgs actionlib_msgs)

catkin_package(
  CATKIN_DEPENDS std_msgs sensor_msgs actionlib_msgs message_runtime
)