# Download gyan FFmpeg 8.1 and SDL2 MSVC dev packages into THIRD_PARTY_DIR
# when the expected folders are missing. Extraction uses cmake -E tar xf.

option(FFPLAY_FETCH_PREBUILT_DEPS
    "Download and extract FFmpeg / SDL2 into third_party when missing"
    ON)

set(FFPLAY_FFMPEG_PREBUILT_URL
    "https://github.com/GyanD/codexffmpeg/releases/download/8.1/ffmpeg-8.1-full_build-shared.zip"
    CACHE STRING "URL for gyan FFmpeg 8.1 full_build-shared archive (.zip)")
set(FFPLAY_SDL2_DEV_ZIP_URL
    "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip"
    CACHE STRING "URL for SDL2 MSVC development package (.zip)")
set(FFPLAY_LIBPLACEBO_PREBUILT_URL
    "https://github.com/aniuzhong/libplacebo-prebuilt/releases/download/v0.1.1/libplacebo-360-prebuilt.zip"
    CACHE STRING "URL for prebuilt libplacebo (.zip)")

function(_ffplay_download url destfile)
    get_filename_component(_parent "${destfile}" DIRECTORY)
    file(MAKE_DIRECTORY "${_parent}")
    message(STATUS "Downloading: ${url}")
    file(DOWNLOAD "${url}" "${destfile}" SHOW_PROGRESS TLS_VERIFY ON STATUS _st)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _st 1 _err)
        message(FATAL_ERROR "Download failed: ${_err}")
    endif()
endfunction()

function(_ffplay_extract zipfile dest_parent)
    message(STATUS "Extracting: ${zipfile}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xf "${zipfile}"
        WORKING_DIRECTORY "${dest_parent}"
        RESULT_VARIABLE _r)
    if(NOT _r EQUAL 0)
        message(FATAL_ERROR "Extract failed (exit ${_r}): ${zipfile}")
    endif()
endfunction()

function(ffplay_ensure_prebuilt_deps)
    if(NOT FFPLAY_FETCH_PREBUILT_DEPS)
        return()
    endif()

    set(_cache "${THIRD_PARTY_DIR}/.cache")
    file(MAKE_DIRECTORY "${_cache}")

    # FFmpeg
    if(NOT EXISTS "${FFMPEG_PREBUILT_ROOT}/include/libavutil/avutil.h")
        set(_ff_zip "${_cache}/ffmpeg-8.1-full_build-shared.zip")
        if(NOT EXISTS "${_ff_zip}")
            _ffplay_download("${FFPLAY_FFMPEG_PREBUILT_URL}" "${_ff_zip}")
        endif()
        _ffplay_extract("${_ff_zip}" "${THIRD_PARTY_DIR}")
    endif()

    # SDL2
    set(_sdl2_root "${THIRD_PARTY_DIR}/${FFPLAY_SDL2_ROOT_DIRNAME}")
    if(NOT EXISTS "${_sdl2_root}/cmake/sdl2-config.cmake")
        set(_sdl_zip "${_cache}/SDL2-devel-2.32.10-VC.zip")
        if(NOT EXISTS "${_sdl_zip}")
            _ffplay_download("${FFPLAY_SDL2_DEV_ZIP_URL}" "${_sdl_zip}")
        endif()
        _ffplay_extract("${_sdl_zip}" "${THIRD_PARTY_DIR}")
    endif()

    # libplacebo (prebuilt)
    set(_pl_root "${LIBPLACEBO_PREBUILT_ROOT}")
    if(NOT EXISTS "${_pl_root}/include/libplacebo/vulkan.h")
        set(_pl_zip "${_cache}/libplacebo-360-prebuilt.zip")
        if(NOT EXISTS "${_pl_zip}")
            _ffplay_download("${FFPLAY_LIBPLACEBO_PREBUILT_URL}" "${_pl_zip}")
        endif()
        _ffplay_extract("${_pl_zip}" "${THIRD_PARTY_DIR}")
    endif()
endfunction()

ffplay_ensure_prebuilt_deps()
