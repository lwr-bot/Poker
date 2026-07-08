cmake_minimum_required(VERSION 3.0)
project(POKER)


set(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} -g)


set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)


include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/include/server)
include_directories(${PROJECT_SOURCE_DIR}/thirdparty/json)
include_directories(/home/iurnewgnail/project/build/release-install-cpp11/include)


add_subdirectory(src)
