# wwe2k16 - WWE 2K16 (Xbox 360) static recompilation with the ReXGlue SDK.
# SDK boilerplate lives in generated/rexglue.cmake (rendered by tools/codegen.ps1 via `rexglue init`).

cmake_minimum_required(VERSION 3.25)
project(wwe2k16 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Clang < 19 reports __cpp_concepts=201907L, which makes GCC 13's libstdc++ hide std::expected. Raise the macro only
# for targets compiled as C++20 or later: dependencies pinned to older standards (glslang forces C++11) must not see
# it, or libstdc++ emits concept/requires code those standards cannot parse. Not needed on Android (NDK libc++).
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19 AND NOT ANDROID)
    set(_wwe2k16_cxx20 "$<AND:$<COMPILE_LANGUAGE:CXX>,$<VERSION_GREATER_EQUAL:$<TARGET_PROPERTY:CXX_STANDARD>,20>>")
    add_compile_options("$<${_wwe2k16_cxx20}:-D__cpp_concepts=202002L>" "$<${_wwe2k16_cxx20}:-Wno-builtin-macro-redefined>")
endif()

include(generated/rexglue.cmake)

set(WWE2K16_SOURCES
    src/main.cpp
    src/disc_image.cpp
    src/test_input.cpp
    src/resolve_coherency.cpp
    src/frame_rate.cpp
    src/launcher_settings.cpp
    src/screenshot.cpp
    src/achievement_toast.cpp
    src/achievement_sound.cpp
    src/fps_overlay.cpp
    src/shadows.cpp
    src/cp_profiler.cpp
    src/exit_menu.cpp
    src/display_refresh_rate.cpp
    src/frame_monitor.cpp
    src/android_driver.cpp
    src/touch_input.cpp
    src/native_renderer.cpp
)

if(ANDROID)
    add_library(wwe2k16 SHARED ${WWE2K16_SOURCES})
    set_target_properties(wwe2k16 PROPERTIES OUTPUT_NAME main)
elseif(WIN32)
    add_executable(wwe2k16 WIN32 ${WWE2K16_SOURCES})
else()
    add_executable(wwe2k16 ${WWE2K16_SOURCES})
endif()

rexglue_setup_target(wwe2k16)
# disc_image.cpp includes the SDK's disc reader from its source tree: the SDK checkout this build uses.
if(REXSDK_DIR)
    target_include_directories(wwe2k16 PRIVATE "${REXSDK_DIR}/src")
else()
    target_include_directories(wwe2k16 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk/rexglue/src")
endif()

if(ANDROID)
    target_link_libraries(wwe2k16 PRIVATE android log)
    # Android denies libraries that ask for an executable stack (logcat: "denied { execstack }").
    target_link_options(wwe2k16 PRIVATE "LINKER:-z,noexecstack")
    # Custom Vulkan drivers (src/android_driver.cpp): libadrenotools (BSD-2-Clause), cloned by tools/android-build.sh.
    # Its hook libraries (libhook_impl.so, libmain_hook.so) go into the APK next to libmain.so.
    set(WWE2K16_ADRENOTOOLS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libadrenotools")
    if(EXISTS "${WWE2K16_ADRENOTOOLS}/CMakeLists.txt")
        if(NOT CMAKE_ANDROID_ARCH_ABI)
            set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
        endif()
        add_subdirectory("${WWE2K16_ADRENOTOOLS}" adrenotools EXCLUDE_FROM_ALL)
        target_link_libraries(wwe2k16 PRIVATE adrenotools)
        target_compile_definitions(wwe2k16 PRIVATE WWE2K16_HAVE_ADRENOTOOLS=1)
        add_dependencies(wwe2k16 hook_impl main_hook)
    else()
        message(WARNING "thirdparty/libadrenotools is missing: custom GPU drivers are off in this build")
    endif()
endif()

# Title update 3 build: wwe2k16_tu.exe from generated_tu\ (tools/build-tu.ps1). Same app code; WWE2K16_TU selects the
# title update's function addresses where the app hooks the game.
set(WWE2K16_TARGETS wwe2k16)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated_tu/sources.cmake")
    set(WWE2K16_RETAIL_SOURCES ${GENERATED_SOURCES})
    include(generated_tu/sources.cmake)
    set(WWE2K16_TU_SOURCES ${GENERATED_SOURCES})
    set(GENERATED_SOURCES ${WWE2K16_RETAIL_SOURCES})
    if(WIN32)
        add_executable(wwe2k16_tu WIN32 ${WWE2K16_SOURCES} ${WWE2K16_TU_SOURCES})
    else()
        add_executable(wwe2k16_tu ${WWE2K16_SOURCES} ${WWE2K16_TU_SOURCES})
    endif()
    target_include_directories(wwe2k16_tu PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/generated_tu)
    target_link_libraries(wwe2k16_tu PRIVATE rex::core rex::system rex::kernel rex::graphics rex::ui)
    rexglue_configure_target(wwe2k16_tu)
    target_compile_definitions(wwe2k16_tu PRIVATE WWE2K16_TU=1)
    list(APPEND WWE2K16_TARGETS wwe2k16_tu)
endif()

foreach(target IN LISTS WWE2K16_TARGETS)
if(WIN32)
    enable_language(RC)
    target_sources(${target} PRIVATE res/wwe2k16.rc)
    target_link_libraries(${target} PRIVATE windowscodecs ole32 shell32 winmm gdiplus)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/res/achievement.ogg"
                "$<TARGET_FILE_DIR:${target}>/achievement.ogg")
    target_link_options(${target} PRIVATE "LINKER:/map:$<TARGET_FILE_DIR:${target}>/${target}.map")
endif()
endforeach()
