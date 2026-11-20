add_library(hsvj_foundation STATIC
    PathConfig.cpp
)

target_include_directories(hsvj_foundation PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

add_library(hsvj_jsoncpp STATIC
    ${JSONCPP_DIR}/src/lib_json/json_reader.cpp
    ${JSONCPP_DIR}/src/lib_json/json_value.cpp
    ${JSONCPP_DIR}/src/lib_json/json_writer.cpp
)

target_include_directories(hsvj_jsoncpp PUBLIC
    ${JSONCPP_DIR}/include
)

# Architecture dependency contract (validated by scripts/check_architecture.py)
# hsvj_foundation intentionally has no project-module dependencies.

