cmake_minimum_required(VERSION 3.22.1)
project(license_key_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include(${PROJECT_ROOT}/cmake/LicenseKey.cmake)
add_executable(license_key_test
    main.cpp
    ${PROJECT_ROOT}/src/core/LicenseVerify.cpp
    ${PROJECT_ROOT}/vendor/jsoncpp/src/lib_json/json_reader.cpp
    ${PROJECT_ROOT}/vendor/jsoncpp/src/lib_json/json_value.cpp
    ${PROJECT_ROOT}/vendor/jsoncpp/src/lib_json/json_writer.cpp)
target_include_directories(license_key_test PRIVATE
    ${PROJECT_ROOT}/include ${PROJECT_ROOT}/vendor/jsoncpp/include)
hsvj_configure_license_key(license_key_test)
if(ANDROID)
    target_link_libraries(license_key_test PRIVATE log)
endif()
