cmake_minimum_required(VERSION 3.16)
project(nautilus_video_duration C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NAUTILUS_VD_NATIVE_OPTIMIZE
    "Optimize generated code for the build machine CPU" ON
)
option(NAUTILUS_VD_LTO
    "Enable interprocedural optimization/link-time optimization for Release builds" ON
)

include(CheckIPOSupported)

add_compile_options(-Wall -Wextra -fPIC)

function(nautilus_vd_enable_max_optimization target)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-DNDEBUG>
            $<$<CONFIG:Release>:-fno-semantic-interposition>
            $<$<CONFIG:Release>:-fno-plt>
        )

        if(NAUTILUS_VD_NATIVE_OPTIMIZE)
            target_compile_options(${target} PRIVATE
                $<$<CONFIG:Release>:-march=native>
                $<$<CONFIG:Release>:-mtune=native>
            )
        endif()
    elseif(MSVC)
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GL>
        )
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/LTCG>
        )
    endif()

    if(NAUTILUS_VD_LTO)
        check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES C)
        if(ipo_supported)
            set_property(TARGET ${target} PROPERTY
                INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
            )
        else()
            message(WARNING "IPO/LTO is not supported: ${ipo_error}")
        endif()
    endif()
endfunction()

find_package(PkgConfig REQUIRED)

pkg_check_modules(NAUTILUS REQUIRED libnautilus-extension-4)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(GMODULE REQUIRED gmodule-2.0)

# Helper uses FFmpeg libs
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVUTIL REQUIRED libavutil)

# ---- Nautilus extension ----
add_library(nautilus-video-duration SHARED
    nautilus-video-duration.c vd-media.c vd-helper-pool.c vd-cache.c vd-scheduler.c
)
nautilus_vd_enable_max_optimization(nautilus-video-duration)

target_include_directories(nautilus-video-duration PRIVATE
    ${NAUTILUS_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
    ${GIO_INCLUDE_DIRS}
    ${GMODULE_INCLUDE_DIRS}
)

target_compile_options(nautilus-video-duration PRIVATE
    ${NAUTILUS_CFLAGS_OTHER}
    ${GLIB_CFLAGS_OTHER}
    ${GIO_CFLAGS_OTHER}
    ${GMODULE_CFLAGS_OTHER}
)

target_link_libraries(nautilus-video-duration
    ${NAUTILUS_LIBRARIES}
    ${GLIB_LIBRARIES}
    ${GIO_LIBRARIES}
    ${GMODULE_LIBRARIES}
)

set_target_properties(nautilus-video-duration PROPERTIES
    PREFIX ""
    OUTPUT_NAME "nautilus-video-duration"
)

# ---- Helper executable ----
add_executable(vd-ffmpeg-helper vd-ffmpeg-helper.c)
nautilus_vd_enable_max_optimization(vd-ffmpeg-helper)

target_include_directories(vd-ffmpeg-helper PRIVATE
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
)

target_compile_options(vd-ffmpeg-helper PRIVATE
    ${AVFORMAT_CFLAGS_OTHER}
    ${AVUTIL_CFLAGS_OTHER}
)

target_link_libraries(vd-ffmpeg-helper
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
)

# Install paths
set(NAUTILUS_EXT_DIR "/usr/lib/nautilus/extensions-4" CACHE PATH
    "Nautilus extensions-4 directory"
)

message(STATUS "Install Nautilus extension to: ${NAUTILUS_EXT_DIR}")

install(TARGETS nautilus-video-duration
    LIBRARY DESTINATION ${NAUTILUS_EXT_DIR}
)

# Install helper next to extension by default
install(TARGETS vd-ffmpeg-helper
    RUNTIME DESTINATION ${NAUTILUS_EXT_DIR}
)

include(CTest)
if(BUILD_TESTING)
    add_executable(test-media tests/test-media.c vd-media.c)
    target_include_directories(test-media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test-media PRIVATE PkgConfig::GLIB)
    add_test(NAME media COMMAND test-media)

    add_executable(fake-helper tests/fake-helper.c)
    target_link_libraries(fake-helper PRIVATE PkgConfig::GLIB)
    add_executable(test-helper-pool tests/test-helper-pool.c vd-helper-pool.c)
    target_include_directories(test-helper-pool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test-helper-pool PRIVATE PkgConfig::GIO)
    add_test(NAME helper-pool COMMAND test-helper-pool $<TARGET_FILE:fake-helper>)
    set_tests_properties(helper-pool PROPERTIES TIMEOUT 15)

    add_executable(test-cache tests/test-cache.c vd-cache.c)
    target_include_directories(test-cache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test-cache PRIVATE PkgConfig::GLIB)
    add_test(NAME cache COMMAND test-cache)

    add_executable(test-scheduler tests/test-scheduler.c
        vd-scheduler.c vd-cache.c vd-helper-pool.c)
    target_include_directories(test-scheduler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test-scheduler PRIVATE PkgConfig::GIO)
    add_test(NAME scheduler COMMAND test-scheduler $<TARGET_FILE:fake-helper>)
    set_tests_properties(scheduler PROPERTIES TIMEOUT 15)
endif()
