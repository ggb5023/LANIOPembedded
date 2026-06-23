cmake_minimum_required(VERSION 3.26)

project(LanChat VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LAN_CHAT_BUILD_TESTS "Build LAN chat tests" ON)
option(LAN_CHAT_BUILD_APPS "Build LAN chat applications" ON)
option(LAN_CHAT_BUILD_GUI "Build optional Qt chat GUI" OFF)
option(LAN_CHAT_BUILD_AV "Build optional audio/video media support" OFF)
option(LAN_CHAT_USE_VCPKG_DEPS "Use optional vcpkg dependencies" ON)
option(LAN_CHAT_CORE_NO_DEPS "Keep C core free of third-party dependencies" ON)
option(LAN_CHAT_ENABLE_MYSQL_TESTS "Enable MySQL integration tests" OFF)
set(LAN_CHAT_MYSQL_ROOT "" CACHE PATH "Optional MySQL client installation root")

include(GNUInstallDirs)

if (LAN_CHAT_USE_VCPKG_DEPS)
    find_package(unofficial-libmysql CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(fmt CONFIG QUIET)
    find_package(spdlog CONFIG QUIET)
    find_package(Opus CONFIG QUIET)
endif()

if (LAN_CHAT_BUILD_GUI)
    if (NOT Qt6_DIR AND DEFINED ENV{WEBRTC_QT_ROOT} AND NOT "$ENV{WEBRTC_QT_ROOT}" STREQUAL "")
        list(APPEND CMAKE_PREFIX_PATH "$ENV{WEBRTC_QT_ROOT}")
    endif()
    if (WIN32)
        list(APPEND CMAKE_PREFIX_PATH "E:/Qt/6.10.2/msvc2022_64" "E:/Qt/6.9.0/msvc2022_64")
    endif()
    find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets)
    if (LAN_CHAT_BUILD_AV)
        find_package(Qt6 6.6 REQUIRED COMPONENTS Multimedia)
    endif()
endif()

if (LAN_CHAT_BUILD_AV)
    if (NOT TARGET Opus::opus)
        message(FATAL_ERROR "LAN_CHAT_BUILD_AV requires Opus::opus")
    endif()
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel/CMakeLists.txt")
        set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
        set(NO_TESTS ON CACHE BOOL "" FORCE)
        set(NO_MEDIA OFF CACHE BOOL "" FORCE)
        set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
        set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
        set(PREFER_SYSTEM_LIB OFF CACHE BOOL "" FORCE)
        add_subdirectory(third_party/libdatachannel EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "LAN_CHAT_BUILD_AV requires third_party/libdatachannel")
    endif()
endif()

if (NOT TARGET unofficial::libmysql::libmysql)
    set(_lan_chat_mysql_roots)
    if (LAN_CHAT_MYSQL_ROOT)
        list(APPEND _lan_chat_mysql_roots "${LAN_CHAT_MYSQL_ROOT}")
    endif()
    if (DEFINED ENV{MYSQL_ROOT} AND NOT "$ENV{MYSQL_ROOT}" STREQUAL "")
        list(APPEND _lan_chat_mysql_roots "$ENV{MYSQL_ROOT}")
    endif()
    if (WIN32)
        list(APPEND _lan_chat_mysql_roots "D:/mysql" "C:/Program Files/MySQL/MySQL Server 8.0")
    endif()

    find_path(LAN_CHAT_MYSQL_INCLUDE_DIR
        NAMES mysql.h
        PATHS ${_lan_chat_mysql_roots}
        PATH_SUFFIXES include
        NO_DEFAULT_PATH
    )
    find_library(LAN_CHAT_MYSQL_LIBRARY
        NAMES libmysql mysqlclient
        PATHS ${_lan_chat_mysql_roots}
        PATH_SUFFIXES lib lib64
        NO_DEFAULT_PATH
    )
    if (LAN_CHAT_MYSQL_INCLUDE_DIR AND LAN_CHAT_MYSQL_LIBRARY)
        get_filename_component(_lan_chat_mysql_root "${LAN_CHAT_MYSQL_INCLUDE_DIR}" DIRECTORY)
        find_file(LAN_CHAT_MYSQL_RUNTIME_DLL
            NAMES libmysql.dll
            PATHS "${_lan_chat_mysql_root}"
            PATH_SUFFIXES bin lib
            NO_DEFAULT_PATH
        )
        add_library(lan_chat_mysql_client UNKNOWN IMPORTED)
        set_target_properties(lan_chat_mysql_client
            PROPERTIES
                IMPORTED_LOCATION "${LAN_CHAT_MYSQL_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${LAN_CHAT_MYSQL_INCLUDE_DIR}"
        )
        if (LAN_CHAT_MYSQL_RUNTIME_DLL)
            set_property(TARGET lan_chat_mysql_client
                PROPERTY
                    IMPORTED_RUNTIME_ARTIFACTS "${LAN_CHAT_MYSQL_RUNTIME_DLL}"
            )
        endif()
    endif()
endif()

add_library(lan_chat_core_c STATIC
    src/core/endian.c
    src/core/framer.c
    src/core/protocol.c
    src/core/ring_buffer.c
    src/core/status.c
    src/core/tlv.c
)
target_include_directories(lan_chat_core_c
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(lan_chat_core_c
    PUBLIC
        LAN_CHAT_CORE_NO_DEPS=$<BOOL:${LAN_CHAT_CORE_NO_DEPS}>
)

add_library(lan_chat_platform_win32 STATIC
    src/platform/win32/platform_win32.c
)
target_link_libraries(lan_chat_platform_win32
    PUBLIC
        lan_chat_core_c
)
if (WIN32)
    target_link_libraries(lan_chat_platform_win32
        PRIVATE
            ws2_32
    )
endif()

add_library(lan_chat_transport_c STATIC
    src/transport/transport.c
)
target_link_libraries(lan_chat_transport_c
    PUBLIC
        lan_chat_platform_win32
        lan_chat_core_c
)

add_library(lan_chat_storage_c STATIC
    src/storage/storage.c
)
target_include_directories(lan_chat_storage_c
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(lan_chat_storage_c
    PUBLIC
        lan_chat_core_c
)

add_library(lan_chat_storage_mysql_c STATIC
    src/storage/mysql/storage_mysql.c
)
target_link_libraries(lan_chat_storage_mysql_c
    PUBLIC
        lan_chat_storage_c
    PRIVATE
        lan_chat_core_c
)
if (TARGET unofficial::libmysql::libmysql)
    target_link_libraries(lan_chat_storage_mysql_c
        PRIVATE
            unofficial::libmysql::libmysql
    )
    target_compile_definitions(lan_chat_storage_mysql_c PRIVATE LAN_CHAT_HAS_MYSQL=1)
elseif (TARGET lan_chat_mysql_client)
    target_link_libraries(lan_chat_storage_mysql_c
        PRIVATE
            lan_chat_mysql_client
    )
    target_compile_definitions(lan_chat_storage_mysql_c PRIVATE LAN_CHAT_HAS_MYSQL=1)
else()
    target_compile_definitions(lan_chat_storage_mysql_c PRIVATE LAN_CHAT_HAS_MYSQL=0)
endif()

add_library(lan_chat_storage_memory_c STATIC
    src/storage/memory/storage_memory.c
)
target_link_libraries(lan_chat_storage_memory_c
    PUBLIC
        lan_chat_storage_c
    PRIVATE
        lan_chat_core_c
)

add_library(lan_chat_server_c STATIC
    src/server/server.c
)
target_link_libraries(lan_chat_server_c
    PUBLIC
        lan_chat_storage_c
        lan_chat_transport_c
        lan_chat_core_c
)

add_library(lan_chat_cpp STATIC
    cpp/src/client.cpp
    cpp/src/server.cpp
)
target_include_directories(lan_chat_cpp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include
)
target_link_libraries(lan_chat_cpp
    PUBLIC
        lan_chat_server_c
        lan_chat_transport_c
        lan_chat_storage_c
        lan_chat_core_c
)
if (TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(lan_chat_cpp PRIVATE nlohmann_json::nlohmann_json)
endif()
if (TARGET fmt::fmt)
    target_link_libraries(lan_chat_cpp PRIVATE fmt::fmt)
endif()
if (TARGET spdlog::spdlog)
    target_link_libraries(lan_chat_cpp PRIVATE spdlog::spdlog)
endif()

if (LAN_CHAT_BUILD_APPS)
    add_subdirectory(apps/chat_server)
    add_subdirectory(apps/chat_cli)
    if (LAN_CHAT_BUILD_GUI)
        add_subdirectory(apps/chat_gui)
    endif()
endif()

if (LAN_CHAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()
