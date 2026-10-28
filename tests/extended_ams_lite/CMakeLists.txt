cmake_minimum_required(VERSION 3.13)
project(ExtendedAMSLiteTests LANGUAGES CXX)
enable_testing()

set(AMS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src/libslic3r/ExtendedAMSLite")
add_library(extended_ams_lite_core STATIC "${AMS_SOURCE_DIR}/Planner.cpp")
target_include_directories(extended_ams_lite_core PUBLIC "${AMS_SOURCE_DIR}")
target_compile_features(extended_ams_lite_core PUBLIC cxx_std_17)

add_executable(extended_ams_lite_tests test_planner.cpp)
target_link_libraries(extended_ams_lite_tests PRIVATE extended_ams_lite_core)
add_test(NAME extended_ams_lite_tests COMMAND extended_ams_lite_tests)

add_executable(extended_ams_lite_demo demo.cpp)
target_link_libraries(extended_ams_lite_demo PRIVATE extended_ams_lite_core)

# The upstream aggregate test executable includes stale OBJ/raster tests on
# this release. Keep the real-processor check independently buildable while
# still linking the actual slicer and its normal dependencies.
if(TARGET libslic3r AND TARGET test_common AND TARGET libcurl
   AND TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    add_executable(extended_ams_lite_integration
        integration_main.cpp
        ../libslic3r/test_extended_ams_lite.cpp
        ../../src/slic3r/Utils/Http.cpp
        ../../src/slic3r/Utils/BBLUtil.cpp)
    # libslic3r's LogSink references the existing HTTP implementation, while
    # NSVGUtils expects NanoSVG's implementation (normally in BitmapCache.cpp).
    # Supply the real non-UI implementations without starting a GUI application.
    target_link_libraries(extended_ams_lite_integration PRIVATE
        test_common libslic3r libcurl OpenSSL::SSL OpenSSL::Crypto)
    if(WIN32)
        target_link_libraries(extended_ams_lite_integration PRIVATE bcrypt)
    endif()
    add_test(NAME extended_ams_lite_integration COMMAND extended_ams_lite_integration)
endif()

if(MSVC)
    target_compile_options(extended_ams_lite_core PRIVATE /W4 /WX /utf-8)
    target_compile_options(extended_ams_lite_tests PRIVATE /W4 /WX /utf-8)
    target_compile_options(extended_ams_lite_demo PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(extended_ams_lite_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(extended_ams_lite_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(extended_ams_lite_demo PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
