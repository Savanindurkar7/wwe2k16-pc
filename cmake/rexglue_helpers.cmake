#==========================================================
# rexglue_configure_target() - Configure a consumer target
# with platform-specific settings and SDK source files.
#
# Usage:
#   rexglue_configure_target(<target>)
#
# Adds:
#   - Platform entry point source (windowed_app_main_*.cpp)
#   - ReXApp base class source (rex_app.cpp)
#   - Platform-specific link/compile settings
#==========================================================
function(rexglue_configure_target target_name)
    # Platform entry point
    if(WIN32)
        target_sources(${target_name} PRIVATE ${REXGLUE_SHARE_DIR}/windowed_app_main_win.cpp)
    elseif(ANDROID)
        target_sources(${target_name} PRIVATE ${REXGLUE_SHARE_DIR}/windowed_app_main_android.cpp)
    else()
        target_sources(${target_name} PRIVATE ${REXGLUE_SHARE_DIR}/windowed_app_main_posix.cpp)
    endif()

    # ReXApp base class
    target_sources(${target_name} PRIVATE
        ${REXGLUE_SHARE_DIR}/rex_app.cpp)

    # Build config for version stamp (rex_app.cpp uses REXGLUE_BUILD_STAMP)
    target_compile_definitions(${target_name} PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>")

    # Whole-archive linking for kernel hooks
    if(WIN32)
        target_link_options(${target_name} PRIVATE
            "LINKER:/WHOLEARCHIVE:$<TARGET_FILE:rex::kernel>"
        )
    else()
        target_link_options(${target_name} PRIVATE
            -Wl,--whole-archive
            $<TARGET_FILE:rex::kernel>
            -Wl,--no-whole-archive
        )
    endif()

    # Linux platform settings
    if(UNIX AND NOT APPLE AND NOT ANDROID)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
        target_include_directories(${target_name} PRIVATE ${GTK3_INCLUDE_DIRS})
        target_link_libraries(${target_name} PRIVATE ${GTK3_LIBRARIES})
        # Large executable support
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            target_link_options(${target_name} PRIVATE -Wl,--no-relax)
            target_compile_options(${target_name} PRIVATE -mcmodel=large)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
            target_compile_options(${target_name} PRIVATE -march=armv8-a)
        endif()
    endif()

    if(NOT MSVC)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            target_compile_options(${target_name} PRIVATE -msse4.1)
        endif()
        # ARM64 NEON is enabled via -march=armv8-a above
    endif()

    # rexaudio is a static SDK library, so its private FFmpeg link dependency
    # must reach the final game link. In shared builds, add the exported FFmpeg
    # targets explicitly so CMake also sees them as runtime dependencies.
    if(TARGET rex::libavcodec AND TARGET rex::libavutil)
        get_target_property(_avcodec_type rex::libavcodec TYPE)
        get_target_property(_avutil_type rex::libavutil TYPE)
        if(_avcodec_type STREQUAL "SHARED_LIBRARY" AND _avutil_type STREQUAL "SHARED_LIBRARY")
            target_link_libraries(${target_name} PRIVATE rex::libavcodec rex::libavutil)
            if(UNIX AND NOT APPLE)
                # Resolve copied runtime libraries beside the executable, both
                # in the build tree and after a release folder is moved.
                set_target_properties(${target_name} PROPERTIES
                    BUILD_WITH_INSTALL_RPATH TRUE
                    INSTALL_RPATH "$ORIGIN"
                )
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_SONAME_FILE:rex::libavcodec>"
                        "$<TARGET_FILE_DIR:${target_name}>"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_SONAME_FILE:rex::libavutil>"
                        "$<TARGET_FILE_DIR:${target_name}>"
                    VERBATIM
                )
            elseif(WIN32)
                # The SDK kernel also calls BCrypt directly. With FFmpeg now
                # in DLLs, its former static-library bcrypt dependency no
                # longer supplies this system import to the final executable.
                target_link_libraries(${target_name} PRIVATE bcrypt)
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:rex::libavcodec>"
                        "$<TARGET_FILE_DIR:${target_name}>"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:rex::libavutil>"
                        "$<TARGET_FILE_DIR:${target_name}>"
                    VERBATIM
                )
            endif()
        endif()
    endif()

    # Copy runtime DLLs next to the executable
    if(WIN32)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:${target_name}>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:${target_name}>;$<TARGET_FILE_DIR:${target_name}>>"
            COMMAND_EXPAND_LISTS
        )
        # FidelityFX is linked PRIVATE by rexui (to avoid propagating DLL
        # requirements to tool-mode targets), so copy its DLLs explicitly.
        if(TARGET amd_fidelityfx_vk)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:amd_fidelityfx_vk>
                    $<TARGET_FILE_DIR:${target_name}>
            )
        endif()
        if(TARGET amd_fidelityfx_dx12)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:amd_fidelityfx_dx12>
                    $<TARGET_FILE_DIR:${target_name}>
            )
        endif()
    endif()
endfunction()
