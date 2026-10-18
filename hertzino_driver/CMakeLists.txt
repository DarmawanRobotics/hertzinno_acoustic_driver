cmake_minimum_required(VERSION 3.0.2)
project(hertzino_driver)

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
  cv_bridge
  image_transport
  hertzino_msgs
)
find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3 REQUIRED)

catkin_package(
  INCLUDE_DIRS include
  LIBRARIES hertzino_client hertzino_rtsp
  CATKIN_DEPENDS roscpp std_msgs std_srvs sensor_msgs actionlib actionlib_msgs
    cv_bridge image_transport hertzino_msgs
)

# Camera client: HTTP with token login, JSON messages, WebSocket streams. No ROS.
add_library(hertzino_client
  src/crypto.cpp
  src/http_client.cpp
  src/api.cpp
  src/websocket.cpp
  src/device.cpp
  src/detection.cpp
)
target_include_directories(hertzino_client PUBLIC include)
target_include_directories(hertzino_client SYSTEM PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(hertzino_client
  ${CURL_LIBRARIES} OpenSSL::Crypto nlohmann_json::nlohmann_json Threads::Threads)

# RTSP capture and path discovery (OpenCV, no ROS)
add_library(hertzino_rtsp src/rtsp_capture.cpp src/rtsp_probe.cpp)
target_include_directories(hertzino_rtsp PUBLIC include)
target_include_directories(hertzino_rtsp SYSTEM PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(hertzino_rtsp hertzino_client ${OpenCV_LIBS} Threads::Threads)

# Node: status and settings, detect action and records, WebSocket streams as topics
add_executable(hertzino_acoustic_node
  src/acoustic_node.cpp
  src/ros_utils.cpp
  src/status_interface.cpp
  src/detection_interface.cpp
  src/stream_interface.cpp
)
target_include_directories(hertzino_acoustic_node PRIVATE include ${catkin_INCLUDE_DIRS})
target_link_libraries(hertzino_acoustic_node hertzino_client ${catkin_LIBRARIES})
add_dependencies(hertzino_acoustic_node ${catkin_EXPORTED_TARGETS})

# Camera node: the sound map video
add_executable(hertzino_camera_node src/camera_node.cpp)
target_include_directories(hertzino_camera_node PRIVATE include ${catkin_INCLUDE_DIRS})
target_link_libraries(hertzino_camera_node hertzino_rtsp ${catkin_LIBRARIES})
add_dependencies(hertzino_camera_node ${catkin_EXPORTED_TARGETS})

install(TARGETS hertzino_acoustic_node hertzino_camera_node
  RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
)
install(TARGETS hertzino_client hertzino_rtsp
  ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  RUNTIME DESTINATION ${CATKIN_GLOBAL_BIN_DESTINATION}
)
install(DIRECTORY include/${PROJECT_NAME}/ DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION})
install(DIRECTORY launch config DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION})

if(CATKIN_ENABLE_TESTING)
  catkin_add_gtest(test_crypto test/test_crypto.cpp test/main.cpp)
  target_link_libraries(test_crypto hertzino_client)
  catkin_add_gtest(test_api test/test_api.cpp test/main.cpp)
  target_link_libraries(test_api hertzino_client)
  catkin_add_gtest(test_detection test/test_detection.cpp test/main.cpp)
  target_link_libraries(test_detection hertzino_client)
  catkin_add_gtest(test_websocket test/test_websocket.cpp test/main.cpp TIMEOUT 60)
  target_link_libraries(test_websocket hertzino_client)
  # end to end against test/fake_device.py (HTTP, token, WebSocket streams, detections)
  catkin_add_gtest(test_device_fake test/test_device_fake.cpp test/main.cpp TIMEOUT 120)
  target_link_libraries(test_device_fake hertzino_client nlohmann_json::nlohmann_json)
  target_compile_definitions(test_device_fake PRIVATE
    FAKE_DEVICE_PY="${CMAKE_CURRENT_SOURCE_DIR}/test/fake_device.py")
  catkin_add_gtest(test_rtsp test/test_rtsp.cpp test/main.cpp TIMEOUT 60)
  target_link_libraries(test_rtsp hertzino_rtsp)
endif()