cmake_minimum_required(VERSION 3.21)
project(ffplay-cpp C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 23)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(FetchContent)

if(MSVC)
    add_compile_options(/utf-8 /W4)
    set_source_files_properties(
        src/compat/cmdutils.c
        src/compat/opt_common.c
        PROPERTIES COMPILE_FLAGS "/W1 /D_CRT_SECURE_NO_WARNINGS")
endif()

option(BUILD_TESTS "Build unit tests" ON)

# ---- Find FFmpeg & SDL2 --------------------------------------------------
if(WIN32)
    set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party" CACHE PATH
        "Root directory for bundled third-party packages")
    set(LIBPLACEBO_PREBUILT_ROOT "${THIRD_PARTY_DIR}/libplacebo-360-prebuilt"
        CACHE PATH "Prebuilt libplacebo root (include/, lib/, bin/)")
    include(DownloadPrebuiltDeps)
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

# ---- Vulkan / libplacebo (optional) ----
option(ENABLE_VULKAN "Enable Vulkan renderer via libplacebo" ON)

if(ENABLE_VULKAN)
    if(WIN32)
        find_path(LIBPLACEBO_INCLUDE_DIR
            NAMES libplacebo/vulkan.h libplacebo/utils/libav.h
            PATHS "${LIBPLACEBO_PREBUILT_ROOT}/include"
            NO_DEFAULT_PATH)
        find_library(LIBPLACEBO_LIBRARY
            NAMES placebo libplacebo
            PATHS "${LIBPLACEBO_PREBUILT_ROOT}/lib"
            NO_DEFAULT_PATH)

        find_package(Vulkan QUIET)

        if(LIBPLACEBO_INCLUDE_DIR AND LIBPLACEBO_LIBRARY AND Vulkan_FOUND)
            set(HAVE_VULKAN_RENDERER 1)
            message(STATUS "Vulkan renderer: enabled")
            message(STATUS "  libplacebo include: ${LIBPLACEBO_INCLUDE_DIR}")
            message(STATUS "  libplacebo lib:     ${LIBPLACEBO_LIBRARY}")
            message(STATUS "  Vulkan lib:         ${Vulkan_LIBRARY}")
        else()
            set(HAVE_VULKAN_RENDERER 0)
            if(NOT LIBPLACEBO_INCLUDE_DIR)
                message(STATUS "Vulkan renderer: disabled (libplacebo headers not found in ${LIBPLACEBO_PREBUILT_ROOT}/include)")
            elseif(NOT LIBPLACEBO_LIBRARY)
                message(STATUS "Vulkan renderer: disabled (libplacebo library not found in ${LIBPLACEBO_PREBUILT_ROOT}/lib)")
            else()
                message(STATUS "Vulkan renderer: disabled (Vulkan SDK not found)")
            endif()
        endif()
    else()
        # Linux / Unix: use pkg-config for libplacebo and Vulkan
        pkg_check_modules(LIBPLACEBO QUIET IMPORTED_TARGET libplacebo)
        find_package(Vulkan QUIET)

        if(LIBPLACEBO_FOUND AND Vulkan_FOUND)
            set(HAVE_VULKAN_RENDERER 1)
            message(STATUS "Vulkan renderer: enabled")
            message(STATUS "  libplacebo version: ${LIBPLACEBO_VERSION}")
            message(STATUS "  Vulkan lib:         ${Vulkan_LIBRARY}")
        else()
            set(HAVE_VULKAN_RENDERER 0)
            if(NOT LIBPLACEBO_FOUND)
                message(STATUS "Vulkan renderer: disabled (libplacebo not found via pkg-config)")
            else()
                message(STATUS "Vulkan renderer: disabled (Vulkan SDK not found)")
            endif()
        endif()
    endif()
else()
    set(HAVE_VULKAN_RENDERER 0)
    message(STATUS "Vulkan renderer: disabled (ENABLE_VULKAN=OFF)")
endif()

# ---- ffplay executable ----
add_executable(ffplay
    src/ffplay.cpp
    src/video/vulkan_video_output_impl.h
    src/video/vulkan_video_output_impl.c
    src/compat/cmdutils.c
    src/compat/opt_common.c
    src/compat/getenv_utf8.h
    src/compat/wchar_filename.h
    src/compat/va_copy.h
    src/compat/w32dlfcn.h
    src/audio/sdl_audio_output.h
    src/audio/sdl_audio_output.cpp
    src/audio/audio_output.h
    src/demuxer.h
    src/demuxer.cpp
    src/audio_visualizer.h
    src/audio_visualizer.cpp
    src/queue/packet_queue.h
    src/queue/packet_queue.cpp
    src/clock.h
    src/clock.cpp
    src/types/frame.h
    src/queue/frame_queue.h
    src/queue/frame_queue.cpp
    src/decoder.h
    src/decoder.cpp
    src/types/video_types.h
    src/video/video_output.h
    src/video/video_output.cpp
    src/video/sdl_video_output.h
    src/video/sdl_video_output.cpp
    src/video/null_video_output.h
    src/video/null_video_output.cpp
    src/audio/null_audio_output.h
    src/audio/null_audio_output.cpp
    src/player.h
    src/player.cpp
    src/video_pipeline.h
    src/video_pipeline.cpp
    src/audio_pipeline.h
    src/audio_pipeline.cpp
    src/subtitle_pipeline.h
    src/subtitle_pipeline.cpp)

if(HAVE_VULKAN_RENDERER)
    target_sources(ffplay PRIVATE
        src/video/vulkan_video_output.h
        src/video/vulkan_video_output.cpp)
endif()

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

if(HAVE_VULKAN_RENDERER)
    target_compile_definitions(ffplay PRIVATE
        CONFIG_LIBPLACEBO=1
        HAVE_VULKAN_RENDERER=1)
endif()

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
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/queue"
        "${CMAKE_SOURCE_DIR}/src/types"
        "${CMAKE_SOURCE_DIR}/src/video"
        "${CMAKE_SOURCE_DIR}/src/audio"
        "${CMAKE_SOURCE_DIR}/shim/include"
        $<$<BOOL:${HAVE_VULKAN_RENDERER}>:${LIBPLACEBO_INCLUDE_DIR}>)
    target_include_directories(ffplay SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/src/compat"
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
        $<$<BOOL:${HAVE_VULKAN_RENDERER}>:${LIBPLACEBO_LIBRARY}>
        $<$<BOOL:${HAVE_VULKAN_RENDERER}>:Vulkan::Vulkan>
        shell32 comdlg32)
else()
    target_include_directories(ffplay PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/queue"
        "${CMAKE_SOURCE_DIR}/src/types"
        "${CMAKE_SOURCE_DIR}/src/video"
        "${CMAKE_SOURCE_DIR}/src/audio")
    target_include_directories(ffplay SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/src/compat")
    target_link_libraries(ffplay PRIVATE
        PkgConfig::FFMPEG
        PkgConfig::SDL2
        $<$<BOOL:${HAVE_VULKAN_RENDERER}>:PkgConfig::LIBPLACEBO>
        $<$<BOOL:${HAVE_VULKAN_RENDERER}>:Vulkan::Vulkan>)
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

    # libplacebo DLL
    if(HAVE_VULKAN_RENDERER AND EXISTS "${LIBPLACEBO_PREBUILT_ROOT}/bin")
        file(GLOB _pl_dlls LIST_DIRECTORIES false "${LIBPLACEBO_PREBUILT_ROOT}/bin/*.dll")
        foreach(_d IN LISTS _pl_dlls)
            add_custom_command(TARGET ffplay POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_d}" "${_out}/"
                VERBATIM)
        endforeach()
    endif()
endif()

# ---- Unit tests ----------------------------------------------------------

if(BUILD_TESTS)
    enable_testing()

    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(test_clock
        tests/test_clock.cpp
        src/clock.cpp
    )
    target_include_directories(test_clock PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/types"
    )
    target_link_libraries(test_clock PRIVATE
        Catch2::Catch2WithMain
    )

    # Register each Catch2 SECTION / GENERATE permutation as a distinct CTest test
    include(Catch)
    catch_discover_tests(test_clock)
endif()
