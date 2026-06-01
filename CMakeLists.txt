cmake_minimum_required(VERSION 3.25)
project(RobotVisionSystem VERSION 1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC 2022 specific
if(MSVC)
    add_compile_options(/W4 /utf-8 /MP)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ── Qt6 ──────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS
    Core Widgets Sql Network PrintSupport
)
qt_standard_project_setup()

# ── HALCON 24.11 ─────────────────────────────────────────────────
# Adjust HALCON_ROOT to your actual install path, or pass via -DHALCON_ROOT=
if(NOT DEFINED HALCON_ROOT)
    set(HALCON_ROOT "$ENV{HALCONROOT}" CACHE PATH "HALCON install root")
endif()

set(HALCON_INCLUDE_DIRS "${HALCON_ROOT}/include" "${HALCON_ROOT}/include/halconcpp")
set(HALCON_LIB_DIRS "${HALCON_ROOT}/lib/x64-win64")

find_library(HALCON_LIB    halconcpp   PATHS "${HALCON_LIB_DIRS}" REQUIRED)
find_library(HALCONXL_LIB  halconxl    PATHS "${HALCON_LIB_DIRS}" QUIET)

if(NOT HALCON_LIB)
    message(FATAL_ERROR "HALCON not found. Set HALCON_ROOT or HALCONROOT env var.")
endif()

add_library(HALCON::halconcpp UNKNOWN IMPORTED)
set_target_properties(HALCON::halconcpp PROPERTIES
    IMPORTED_LOCATION "${HALCON_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${HALCON_INCLUDE_DIRS}"
)

# ── OpenCV 4.8 ───────────────────────────────────────────────────
find_package(OpenCV 4.8 REQUIRED COMPONENTS core imgproc imgcodecs videoio)

# ── Sources ──────────────────────────────────────────────────────
set(HEADERS
    include/MainWindow.h
    include/ImageView.h
    include/CameraManager.h
    include/VisionProcessor.h
    include/CalibrationManager.h
    include/MeasurementEngine.h
    include/CoordTransform.h
    include/RobotClient.h
    include/DatabaseManager.h
    include/ReportManager.h
    include/JsonSettings.h
    include/Logger.h
    include/CalibrationWizard.h
)

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/ImageView.cpp
    src/CameraManager.cpp
    src/VisionProcessor.cpp
    src/CalibrationManager.cpp
    src/MeasurementEngine.cpp
    src/CoordTransform.cpp
    src/RobotClient.cpp
    src/DatabaseManager.cpp
    src/ReportManager.cpp
    src/JsonSettings.cpp
    src/Logger.cpp
    src/CalibrationWizard.cpp
)

# ── Target ───────────────────────────────────────────────────────
qt_add_executable(${PROJECT_NAME} ${HEADERS} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${HALCON_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Sql Qt6::Network Qt6::PrintSupport
    HALCON::halconcpp
    ${OpenCV_LIBS}
)

# Copy config and resources at build time via custom commands
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/settings.json"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config/settings.json"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_SOURCE_DIR}/resources/"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources/"
)

# ── Tests ────────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install ──────────────────────────────────────────────────────
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
install(DIRECTORY config/ resources/ DESTINATION share/${PROJECT_NAME})
