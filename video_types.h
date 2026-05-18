#pragma once

// Prevent vulkan_video_output_impl.h from redefining these types in C++ TUs.
#define FFPLAY_RENDERER_PARAMS_DEFINED

#include <cstdint>

// Shared geometry and rendering parameter types.
// No dependency on SDL, FFmpeg, or any rendering API.

struct Rect {
    int x = 0, y = 0;
    int w = 0, h = 0;
};

enum class VideoBackgroundType { Tiles, Color, None };

struct RenderParams {
    Rect target_rect;
    uint8_t video_background_color[4];
    VideoBackgroundType video_background_type = VideoBackgroundType::None;
};
