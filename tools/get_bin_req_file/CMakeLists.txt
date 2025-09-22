cmake_minimum_required(VERSION 3.16)
project(gen_bin_req_file CXX)
set(CMAKE_CXX_STANDARD 17)

set(BOOST_ROOT "/home/micropenis/boost_1_84_0" CACHE PATH "")

find_package(Boost 1.75 REQUIRED COMPONENTS filesystem system)

include_directories(
  ${CMAKE_SOURCE_DIR}/../../src
  ${CMAKE_SOURCE_DIR}/../../contrib/epee/include
  ${Boost_INCLUDE_DIRS}
)

add_executable(gen_bin_req_file main.cpp)

target_link_libraries(gen_bin_req_file
  PRIVATE
    Boost::filesystem
    Boost::system
)

find_package(Threads REQUIRED)
target_link_libraries(gen_bin_req_file PRIVATE Threads::Threads)

find_package(ICU QUIET COMPONENTS uc data)
if(ICU_FOUND)
  target_link_libraries(gen_bin_req_file PRIVATE ICU::uc ICU::data)
endif()