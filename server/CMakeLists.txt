cmake_minimum_required(VERSION 3.16)
project(cbt-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(cbt-server
    main.cpp
    Logger.cpp
    DiskMap.cpp
    RingBuffer.cpp
    MessageParser.cpp
    BlockProcessor.cpp
    Server.cpp
)

target_link_libraries(cbt-server PRIVATE
    OpenSSL::Crypto
    ZLIB::ZLIB
    pthread
)
