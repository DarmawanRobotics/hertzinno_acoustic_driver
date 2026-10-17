cmake_minimum_required(VERSION 3.0.2)
project(hertzinno_acoustic_driver)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra)
endif()

find_package(catkin REQUIRED COMPONENTS
  roscpp
  std_msgs
  std_srvs
  sensor_msgs
  actionlib
  actionlib_msgs
  message_generation
)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3 REQUIRED)

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
  INCLUDE_DIRS include
  LIBRARIES hertzinno_client
  CATKIN_DEPENDS roscpp std_msgs std_srvs sensor_msgs actionlib actionlib_msgs message_runtime
)

# Camera client: HTTP with token login, JSON messages, WebSocket streams. No ROS.
add_library(hertzinno_client
  src/crypto.cpp
  src/http_client.cpp
  src/api.cpp
  src/websocket.cpp
  src/device.cpp
  src/detection.cpp
)
target_include_directories(hertzinno_client PUBLIC include)
target_include_directories(hertzinno_client SYSTEM PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(hertzinno_client
  ${CURL_LIBRARIES} OpenSSL::Crypto nlohmann_json::nlohmann_json Threads::Threads)

# The node: status and settings, detect action and records, WebSocket streams as topics
add_executable(hertzinno_acoustic_node
  src/acoustic_node.cpp
  src/ros_utils.cpp
  src/status_interface.cpp
  src/detection_interface.cpp
  src/stream_interface.cpp
)
target_include_directories(hertzinno_acoustic_node PRIVATE include ${catkin_INCLUDE_DIRS})
target_link_libraries(hertzinno_acoustic_node hertzinno_client ${catkin_LIBRARIES})
add_dependencies(hertzinno_acoustic_node
  ${${PROJECT_NAME}_EXPORTED_TARGETS} ${catkin_EXPORTED_TARGETS})

install(TARGETS hertzinno_acoustic_node
  RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
)
install(DIRECTORY launch config DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION})
install(TARGETS hertzinno_client
  ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  RUNTIME DESTINATION ${CATKIN_GLOBAL_BIN_DESTINATION}
)
install(DIRECTORY include/${PROJECT_NAME}/ DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION})

if(CATKIN_ENABLE_TESTING)
  catkin_add_gtest(test_crypto test/test_crypto.cpp test/main.cpp)
  target_link_libraries(test_crypto hertzinno_client)
  catkin_add_gtest(test_api test/test_api.cpp test/main.cpp)
  target_link_libraries(test_api hertzinno_client)
  catkin_add_gtest(test_detection test/test_detection.cpp test/main.cpp)
  target_link_libraries(test_detection hertzinno_client)
  catkin_add_gtest(test_websocket test/test_websocket.cpp test/main.cpp TIMEOUT 60)
  target_link_libraries(test_websocket hertzinno_client)
  # end to end against test/fake_device.py (HTTP, token, WebSocket streams, detections)
  catkin_add_gtest(test_device_fake test/test_device_fake.cpp test/main.cpp TIMEOUT 120)
  target_link_libraries(test_device_fake hertzinno_client nlohmann_json::nlohmann_json)
  target_compile_definitions(test_device_fake PRIVATE
    FAKE_DEVICE_PY="${CMAKE_CURRENT_SOURCE_DIR}/test/fake_device.py")
endif()
