#
# Copyright (c) TrueBrain
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

cmake_minimum_required(VERSION 3.16)

project(truemqtt VERSION 1.0.0 DESCRIPTION "A modern C++ MQTT Client library")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(THREADS_PREFER_PTHREAD_FLAG ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/vendor/cmake-modules")

set(MIN_LOGGER_LEVEL "INFO" CACHE STRING "Set minimal logger level (TRACE, DEBUG, INFO, WARNING, ERROR). No logs below this level will be omitted.")
set(CODE_COVERAGE "OFF" CACHE STRING "Enable code coverage.")

include(GNUInstallDirs)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup()

add_library(${PROJECT_NAME}
    src/Client.cpp
    src/Connection.cpp
    src/Packet.cpp
)
target_include_directories(${PROJECT_NAME}
    PUBLIC include
    PRIVATE src
)

find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE ${CONAN_LIBS} Threads::Threads)

set_target_properties(${PROJECT_NAME} PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1 PUBLIC_HEADER include/TrueMQTT.h)
configure_file(truemqtt.pc.in truemqtt.pc @ONLY)

if(MIN_LOGGER_LEVEL)
    if(("${MIN_LOGGER_LEVEL}" STREQUAL "TRACE") OR
       ("${MIN_LOGGER_LEVEL}" STREQUAL "DEBUG") OR
       ("${MIN_LOGGER_LEVEL}" STREQUAL "INFO") OR
       ("${MIN_LOGGER_LEVEL}" STREQUAL "WARNING") OR
       ("${MIN_LOGGER_LEVEL}" STREQUAL "ERROR"))
        target_compile_definitions(${PROJECT_NAME} PRIVATE MIN_LOGGER_LEVEL=LOGGER_LEVEL_${MIN_LOGGER_LEVEL})
    else()
        message(FATAL_ERROR "Unknown value provided for MIN_LOGGER_LEVEL: \"${MIN_LOGGER_LEVEL}\", must be one of TRACE, DEBUG, INFO, WARNING or ERROR")
    endif()
endif()


target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(CODE_COVERAGE)
    include(CodeCoverage)

    add_subdirectory(regression)

    target_compile_options(${PROJECT_NAME} PRIVATE -Og -g --coverage)
    target_link_libraries(${PROJECT_NAME} PRIVATE gcov)

    setup_target_for_coverage_gcovr_xml(
        NAME coverage
        EXECUTABLE truemqtt_regression
        BASE_DIRECTORY "${PROJECT_SOURCE_DIR}/src")

    setup_target_for_coverage_gcovr_html(
        NAME coverage-html
        EXECUTABLE truemqtt_regression
        BASE_DIRECTORY "${PROJECT_SOURCE_DIR}/src")
endif()

install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_BINARY_DIR}/truemqtt.pc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pkgconfig)

add_subdirectory(example/pubsub)
add_subdirectory(example/stress)
