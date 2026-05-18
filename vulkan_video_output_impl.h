/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef VULKAN_VIDEO_OUTPUT_IMPL_H
#define VULKAN_VIDEO_OUTPUT_IMPL_H

#ifdef __cplusplus
// C++ side: video_types.h already provides VideoBackgroundType and RenderParams.
// Include guard prevents redefinition when this header is included from C++.
#define FFPLAY_RENDERER_PARAMS_DEFINED 1
#include <SDL.h>
#else
#include <SDL.h>
#endif

#include "libavutil/frame.h"

typedef struct VkRenderer VkRenderer;

#define VIDEO_BACKGROUND_TILE_SIZE 64

#ifndef FFPLAY_RENDERER_PARAMS_DEFINED

enum VideoBackgroundType {
    VIDEO_BACKGROUND_TILES,
    VIDEO_BACKGROUND_COLOR,
    VIDEO_BACKGROUND_NONE,
};

typedef struct RenderParams {
    SDL_Rect target_rect;
    uint8_t video_background_color[4];
    enum VideoBackgroundType video_background_type;
} RenderParams;

#endif // FFPLAY_RENDERER_PARAMS_DEFINED

typedef struct AVDictionary AVDictionary;

#ifdef __cplusplus
extern "C" {
#endif

VkRenderer *vk_get_renderer(void);

int vk_renderer_create(VkRenderer *renderer, SDL_Window *window, AVDictionary *opt);
int vk_renderer_get_hw_dev(VkRenderer *renderer, AVBufferRef **dev);
int vk_renderer_display(VkRenderer *renderer, AVFrame *frame, RenderParams *params);
int vk_renderer_resize(VkRenderer *renderer, int width, int height);
void vk_renderer_destroy(VkRenderer *renderer);

#ifdef __cplusplus
}
#endif

#endif /* VULKAN_VIDEO_OUTPUT_IMPL_H */
