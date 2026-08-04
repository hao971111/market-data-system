include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
# gtest 自己的测试 / 安装规则不需要进本项目
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

if(NOT TARGET mds_core)
    message(FATAL_ERROR "MDS_BUILD_TESTS=ON requires MDS_BUILD_LIBRARY=ON")
endif()

add_executable(mds_unit_tests
    unit/types_size_test.cpp
    unit/config_test.cpp
    unit/parse_trade_test.cpp
)

target_include_directories(mds_unit_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(mds_unit_tests
    PRIVATE
        mds_core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(mds_unit_tests)
