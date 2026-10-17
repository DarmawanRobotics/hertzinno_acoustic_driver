cmake_minimum_required(VERSION 3.0.2)
project(hertzinno_acoustic_driver)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra)
endif()

find_package(catkin REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3 REQUIRED)

catkin_package(
  INCLUDE_DIRS include
  LIBRARIES hertzinno_client
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

install(TARGETS hertzinno_client
  ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  RUNTIME DESTINATION ${CATKIN_GLOBAL_BIN_DESTINATION}
)
install(DIRECTORY include/${PROJECT_NAME}/ DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION})

