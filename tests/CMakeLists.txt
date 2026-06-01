find_package(GTest CONFIG REQUIRED)

add_executable(test_runner
    test_crc.cpp
    test_coord_transform.cpp
    ../src/RobotClient.cpp
    ../src/CoordTransform.cpp
)

target_include_directories(test_runner PRIVATE
    ../include
    ${HALCON_INCLUDE_DIRS}
)

target_link_libraries(test_runner PRIVATE
    GTest::gtest GTest::gtest_main
    Qt6::Core Qt6::Network
    HALCON::halconcpp
)

add_test(NAME UnitTests COMMAND test_runner)
