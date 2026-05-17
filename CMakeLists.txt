cmake_minimum_required(VERSION 3.16)
project(ffplay-cpp C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---- Find FFmpeg & SDL2 --------------------------------------------------
if(WIN32)
    include(DownloadPrebuiltDeps)

    set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH
        "Root directory for bundled third-party packages")
    set(FFMPEG_PREBUILT_ROOT "${THIRD_PARTY_DIR}/ffmpeg-8.1-full_build-shared" CACHE PATH
        "Gyan FFmpeg tree (include/, lib/, bin/)")
    set(FFMPEG_LIB_DIR "${FFMPEG_PREBUILT_ROOT}/lib" CACHE PATH
        "Directory containing FFmpeg import libraries (.lib)")

    set(FFPLAY_SDL2_ROOT_DIRNAME "SDL2-2.32.10" CACHE STRING
        "Folder name under THIRD_PARTY_DIR containing SDL2")
    set(_sdl2_root "${THIRD_PARTY_DIR}/${FFPLAY_SDL2_ROOT_DIRNAME}")

    # ---- SDL2 (Windows) --------------------------------------------------
    find_package(SDL2 CONFIG QUIET)
    if(NOT TARGET SDL2::SDL2)
        find_path(SDL2_INCLUDE_DIR NAMES SDL.h
            HINTS "${_sdl2_root}/include"
            PATH_SUFFIXES SDL2 include/SDL2)
        find_library(SDL2_LIBRARY NAMES SDL2 SDL2-static
            HINTS "${_sdl2_root}/lib/x64" "${_sdl2_root}/lib/x86")
        if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
            add_library(SDL2::SDL2 UNKNOWN IMPORTED GLOBAL)
            set_target_properties(SDL2::SDL2 PROPERTIES
                IMPORTED_LOCATION "${SDL2_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR}")
        endif()
    endif()
    if(NOT TARGET SDL2::SDL2)
        message(FATAL_ERROR "SDL2 not found under ${_sdl2_root}")
    endif()

    # ---- FFmpeg libs (Windows) -------------------------------------------
    set(_ffmpeg_libnames avutil avcodec avformat avfilter avdevice swscale swresample)
    foreach(_lib IN LISTS _ffmpeg_libnames)
        find_library(_ff_lib_${_lib}
            NAMES "${_lib}" "lib${_lib}"
            PATHS "${FFMPEG_LIB_DIR}" NO_DEFAULT_PATH)
        if(NOT _ff_lib_${_lib})
            message(FATAL_ERROR "FFmpeg library '${_lib}' not found in ${FFMPEG_LIB_DIR}")
        endif()
    endforeach()
else()
    # ---- Linux / Unix: use pkg-config ------------------------------------
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavutil libavcodec libavformat libavfilter libavdevice libswscale libswresample)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
endif()

# ---- ffplay executable ----
add_executable(ffplay
    ffplay.cpp
    utils/cmdutils.c
    utils/opt_common.c
    utils/getenv_utf8.h
    utils/wchar_filename.h
    utils/va_copy.h
    utils/w32dlfcn.h
    audio_output.h
    audio_output.cpp
    audio_device.h
    demuxer.h
    demuxer.cpp
    audio_visualizer.h
    audio_visualizer.cpp
    packet_queue.h
    packet_queue.cpp
    clock.h
    clock.cpp
    frame.h
    frame_queue.h
    frame_queue.cpp
    decoder.h
    decoder.cpp
    video_types.h
    video_output.h
    video_output.cpp
    sdl_video_output.h
    sdl_video_output.cpp
    null_video_output.h
    null_video_output.cpp
    null_audio_device.h
    null_audio_device.cpp
    player.h
    player.cpp
    video_pipeline.h
    video_pipeline.cpp
    audio_pipeline.h
    audio_pipeline.cpp
    subtitle_pipeline.h
    subtitle_pipeline.cpp)

# ---- Compile definitions (replaces the old config.h mechanism) ------------
# Always-on FFmpeg components required by ffplay
target_compile_definitions(ffplay PRIVATE
    CONFIG_AVUTIL=1           # core utility library
    CONFIG_AVCODEC=1          # codec library
    CONFIG_AVFORMAT=1         # demuxer/muxer library
    CONFIG_AVDEVICE=1         # avdevice input/output device support
    CONFIG_SWSCALE=1          # software scaling
    CONFIG_SWRESAMPLE=1       # audio resampling
    CONFIG_AVFILTER=1         # filter graph support
    CONFIG_RTSP_DEMUXER=1     # RTSP streaming demuxer
    CONFIG_MMSH_PROTOCOL=1    # MMSH streaming protocol
    CONFIG_NONFREE=0          # no nonfree components
    CONFIG_GPLV3=0            # not GPLv3
    CONFIG_GPL=0              # not GPLv2
    CONFIG_LGPLV3=0           # LGPLv2.1+ (falls to else branch)
    CONFIG_THIS_YEAR=2026     # copyright end-year
)

if(WIN32)
    target_compile_definitions(ffplay PRIVATE
        HAVE_SETDLLDIRECTORY=1     # SetDllDirectory for DLL loading
        HAVE_COMMANDLINETOARGVW=1  # CommandLineToArgvW for arg parsing
        HAVE_GETMODULEHANDLE=1     # GetModuleHandle for datadir discovery
        HAVE_GETENV=1              # getenv_utf8
    )
else()
    target_compile_definitions(ffplay PRIVATE
        HAVE_GETENV=1              # getenv_utf8
    )
endif()

# ---- Include directories --------------------------------------------------
if(WIN32)
    target_include_directories(ffplay PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/utils"
        "${CMAKE_SOURCE_DIR}/shim/include"
        "${FFMPEG_PREBUILT_ROOT}/include")
    target_link_libraries(ffplay PRIVATE
        ${_ff_lib_avutil}
        ${_ff_lib_avcodec}
        ${_ff_lib_avformat}
        ${_ff_lib_avfilter}
        ${_ff_lib_avdevice}
        ${_ff_lib_swscale}
        ${_ff_lib_swresample}
        SDL2::SDL2
        shell32 comdlg32)
else()
    target_include_directories(ffplay PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/utils")
    target_link_libraries(ffplay PRIVATE
        PkgConfig::FFMPEG
        PkgConfig::SDL2)
endif()

if(MSVC)
    target_compile_options(ffplay PRIVATE /utf-8)
    target_compile_options(ffplay PRIVATE /W0)
endif()

# ---- copy runtime DLLs (Windows only) ----------------------------------
if(WIN32)
    set(_out "$<TARGET_FILE_DIR:ffplay>")

    # SDL2.dll
    get_target_property(_sdl2_loc SDL2::SDL2 IMPORTED_LOCATION)
    if(_sdl2_loc AND EXISTS "${_sdl2_loc}" AND _sdl2_loc MATCHES "\.dll$")
        set(_sdl_dll "${_sdl2_loc}")
    else()
        foreach(_c IN ITEMS "${_sdl2_root}/lib/x64/SDL2.dll" "${_sdl2_root}/lib/x86/SDL2.dll")
            if(EXISTS "${_c}")
                set(_sdl_dll "${_c}")
                break()
            endif()
        endforeach()
    endif()
    if(_sdl_dll)
        add_custom_command(TARGET ffplay POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_sdl_dll}" "${_out}/"
            VERBATIM)
    endif()

    # FFmpeg DLLs
    if(EXISTS "${FFMPEG_PREBUILT_ROOT}/bin")
        file(GLOB _ff_dlls LIST_DIRECTORIES false "${FFMPEG_PREBUILT_ROOT}/bin/*.dll")
        foreach(_d IN LISTS _ff_dlls)
            add_custom_command(TARGET ffplay POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_d}" "${_out}/"
                VERBATIM)
        endforeach()
    endif()
endif()
