cmake_minimum_required(VERSION 3.20)
project(timegrip-sync VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

# MySQL — optional, enabled when libmysqlclient is found
find_package(PkgConfig QUIET)
set(HAVE_MYSQL OFF)
if(PkgConfig_FOUND)
    pkg_check_modules(MYSQLCLIENT QUIET mysqlclient)
    if(MYSQLCLIENT_FOUND)
        set(HAVE_MYSQL ON)
        message(STATUS "MySQL client found — database support enabled")
    endif()
endif()

add_executable(timegrip-sync
    src/main.cpp
    src/http.cpp
    src/adfs.cpp
    src/timegrip.cpp
    src/gcal.cpp
    src/db.cpp
)

target_link_libraries(timegrip-sync
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

target_compile_options(timegrip-sync PRIVATE
    -Wall -Wextra -Wpedantic
)

if(HAVE_MYSQL)
    target_compile_definitions(timegrip-sync PRIVATE HAVE_MYSQL)
    target_include_directories(timegrip-sync PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
    target_link_directories(timegrip-sync PRIVATE ${MYSQLCLIENT_LIBRARY_DIRS})
    target_link_libraries(timegrip-sync PRIVATE ${MYSQLCLIENT_LIBRARIES})
endif()

install(TARGETS timegrip-sync DESTINATION bin)
