cmake_minimum_required(VERSION 3.20)
project(timegrip-sync VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQLCLIENT REQUIRED mysqlclient)

add_executable(timegrip-sync
    src/main.cpp
    src/http.cpp
    src/adfs.cpp
    src/timegrip.cpp
    src/gcal.cpp
    src/db.cpp
    src/report.cpp
)

target_include_directories(timegrip-sync PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
target_link_directories(timegrip-sync PRIVATE ${MYSQLCLIENT_LIBRARY_DIRS})
target_link_libraries(timegrip-sync
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
        ${MYSQLCLIENT_LIBRARIES}
)

target_compile_options(timegrip-sync PRIVATE
    -Wall -Wextra -Wpedantic
)

install(TARGETS timegrip-sync DESTINATION bin)
