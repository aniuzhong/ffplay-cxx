// sdl_pixfmt_table.h — Shared SDL ↔ AVPixelFormat mapping table.
//
// Used by both SDLVideoOutput (SDL path) and VulkanVideoOutput (Vulkan path).
// VulkanVideoOutput does NOT call SDLVideoOutput methods, but still needs
// the same set of software pixel formats for its buffersink pixel_formats.
//
// Must stay in sync with the texture-upload logic in sdl_video_output.cpp:
// get_pix_fmt_and_blendmode() + upload_texture().

#pragma once

#include <SDL.h>

extern "C" {
#include <libavutil/pixfmt.h>
}

// One entry in the SDL texture format ↔ AVPixelFormat mapping.
struct TextureFormatEntry {
    int format;         // AVPixelFormat
    Uint32 texture_fmt; // SDL_PixelFormatEnum
};

constexpr TextureFormatEntry kTextureFormatMap[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
};

constexpr int kTextureFormatMapSize = FF_ARRAY_ELEMS(kTextureFormatMap);

// Extract just the AVPixelFormat values (for supported_pix_fmts() etc).
inline void fill_from_texture_format_map(int *out, int capacity, int &count) {
    count = 0;
    for (int i = 0; i < kTextureFormatMapSize && count < capacity; i++)
        out[count++] = kTextureFormatMap[i].format;
}
