/*
  Simple DirectMedia Layer
  Copyright (C) 2018-2019 Ash Logan <ash@heyquark.com>
  Copyright (C) 2018-2019 Roberto Van Eeden <r.r.qwertyuiop.r.r@gmail.com>
  Copyright (C) 2022 GaryOderNichts <garyodernichts@gmail.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_RENDER_WIIU

#include "../SDL_sysrender.h"
#include "SDL_render_wiiu.h"

#include <gx2/context.h>
#include <gx2/texture.h>
#include <gx2/sampler.h>
#include <gx2/mem.h>
#include <gx2r/surface.h>
#include <gx2r/resource.h>

#include <malloc.h>
#include <stdarg.h>

static int CreatePlane(WIIU_TexturePlane * plane, const SDL_ScaleMode scaleMode, const WIIU_PixFmt gx2_fmt, uint32_t width, uint32_t height, SDL_bool use_mem2)
{
    BOOL res;
    GX2RResourceFlags surface_flags;

    /* Setup sampler */
    if (scaleMode == SDL_ScaleModeNearest) {
        GX2InitSampler(&plane->sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
    } else {
        GX2InitSampler(&plane->sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    }

    /* Setup GX2Texture */
    plane->texture.surface.width = width;
    plane->texture.surface.height = height;
    plane->texture.surface.format = gx2_fmt.fmt;
    plane->texture.surface.depth = 1;
    plane->texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    plane->texture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    plane->texture.surface.mipLevels = 1;
    plane->texture.viewNumMips = 1;
    plane->texture.viewNumSlices = 1;
    plane->texture.compMap = gx2_fmt.compMap;
    GX2CalcSurfaceSizeAndAlignment(&plane->texture.surface);
    GX2InitTextureRegs(&plane->texture);

    /* Setup GX2ColorBuffer */
    plane->cbuf.surface = plane->texture.surface;
    plane->cbuf.viewNumSlices = 1;
    GX2InitColorBufferRegs(&plane->cbuf);

    /* Texture's surface flags */
    surface_flags = GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_BIND_COLOR_BUFFER |
                    GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_CPU_READ |
                    GX2R_RESOURCE_USAGE_GPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;

    /* Allocate normal textures from MEM2 */
    if (use_mem2)
        surface_flags |= GX2R_RESOURCE_USAGE_FORCE_MEM2;

    /* Allocate the texture's surface */
    res = GX2RCreateSurface(
        &plane->texture.surface,
        surface_flags
    );
    if (!res) {
        return SDL_OutOfMemory();
    }

    /* Allocate a colour buffer, using the same backing buffer */
    res = GX2RCreateSurfaceUserMemory(
        &plane->cbuf.surface,
        plane->texture.surface.image,
        plane->texture.surface.mipmaps,
        plane->texture.surface.resourceFlags
    );
    if (!res) {
        GX2RDestroySurfaceEx(&plane->texture.surface, 0);
        return SDL_OutOfMemory();
    }

    return 0;
}

static void DestroyPlane(WIIU_TexturePlane * plane)
{
    GX2RDestroySurfaceEx(&plane->cbuf.surface, 0);
    GX2RDestroySurfaceEx(&plane->texture.surface, 0);
}

/* Somewhat adapted from SDL_render.c: SDL_LockTextureNative
   The app basically wants a pointer to a particular rectangle as well as
   write access to it. Easy GX2R! */
static void LockPlane(WIIU_TexturePlane * plane, const SDL_Rect * rect, Uint32 BytesPerPixel, void **pixels, int *pitch)
{
    void* pixel_buffer;

    pixel_buffer = GX2RLockSurfaceEx(&plane->texture.surface, 0, 0);

    /* Calculate pointer to first pixel in rect */
    *pixels = (void *) ((Uint8 *) pixel_buffer +
                        rect->y * (plane->texture.surface.pitch * BytesPerPixel) +
                        rect->x * BytesPerPixel);
    *pitch = (plane->texture.surface.pitch * BytesPerPixel);
}

static void UnlockPlane(WIIU_TexturePlane * plane, const SDL_Rect * rect, void *pixels, int pitch, SDL_bool is_565)
{
    /* The 565 formats need byte-swapping. */
    if (is_565) {
        Uint8 *pixels_pointer = (Uint8 *)pixels;
        int x, y;

        for (y = 0; y < rect->h; ++y) {
            uint16_t* line = (uint16_t*)pixels_pointer;

            for (x = 0; x < rect->w; ++x) {
                *line = __builtin_bswap16(*line);
	        ++line;
            }

            pixels_pointer += pitch;
        }
    }

    GX2RUnlockSurfaceEx(&plane->texture.surface, 0, 0);
}

static void UpdatePlane(WIIU_TexturePlane * plane, const SDL_Rect * rect, Uint32 BytesPerPixel, const void *pixels, int pitch, SDL_bool is_565)
{
    size_t length = rect->w * BytesPerPixel;
    Uint8 *src = (Uint8 *) pixels, *dst;
    int row, dst_pitch;

    /* We write the rules, and we say all textures are streaming */
    LockPlane(plane, rect, BytesPerPixel, (void**)&dst, &dst_pitch);

    for (row = 0; row < rect->h; ++row) {
        SDL_memcpy(dst, src, length);
        src += pitch;
        dst += dst_pitch;
    }

    UnlockPlane(plane, rect, dst, dst_pitch, is_565);
}

int WIIU_SDL_CreateTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    int error;
    WIIU_PixFmt gx2_fmt;
    WIIU_TextureData *tdata = (WIIU_TextureData *) SDL_calloc(1, sizeof(*tdata));
    if (!tdata) {
        return SDL_OutOfMemory();
    }

    gx2_fmt = WIIU_SDL_GetPixFmt(texture->format);
    if (gx2_fmt.fmt == -1) {
        return SDL_SetError("Unsupported texture format");
    }

    error = CreatePlane(&tdata->main_plane, texture->scaleMode, gx2_fmt, texture->w, texture->h, texture->driverdata != WIIU_TEXTURE_MEM1_MAGIC);

    if (error) {
        SDL_free(tdata);
        return error;
    }

#if SDL_HAVE_YUV
    if (texture->format == SDL_PIXELFORMAT_YV12 ||
        texture->format == SDL_PIXELFORMAT_IYUV) {
        tdata->yuv = SDL_TRUE;

        error = CreatePlane(&tdata->u_plane, texture->scaleMode, gx2_fmt, (texture->w + 1) / 2, (texture->h + 1) / 2, texture->driverdata != WIIU_TEXTURE_MEM1_MAGIC);

        if (error) {
            DestroyPlane(&tdata->main_plane);
            SDL_free(tdata);
            return error;
        }

        error = CreatePlane(&tdata->v_plane, texture->scaleMode, gx2_fmt, (texture->w + 1) / 2, (texture->h + 1) / 2, texture->driverdata != WIIU_TEXTURE_MEM1_MAGIC);

        if (error) {
            DestroyPlane(&tdata->u_plane);
            DestroyPlane(&tdata->main_plane);
            SDL_free(tdata);
            return error;
        }
    }

    tdata->shader = SHADER_TEXTURE;

#if SDL_HAVE_YUV
    if (tdata->yuv) {
        switch (SDL_GetYUVConversionModeForResolution(texture->w, texture->h)) {
            case SDL_YUV_CONVERSION_JPEG:
                tdata->shader = SHADER_YUV_JPEG;
                break;
            case SDL_YUV_CONVERSION_BT601:
                tdata->shader = SHADER_YUV_BT601;
                break;
            case SDL_YUV_CONVERSION_BT709:
                tdata->shader = SHADER_YUV_BT709;
                break;
            default:
                SDL_assert(!"unsupported YUV conversion mode");
                break;
        }
    }
#endif /* SDL_HAVE_YUV */

#endif

    /* Setup texture driver data */
    texture->driverdata = tdata;

    return 0;
}

int WIIU_SDL_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                         const SDL_Rect * rect, void **pixels, int *pitch)
{
    WIIU_VideoData *videodata = (WIIU_VideoData *) SDL_GetVideoDevice()->driverdata;
    WIIU_RenderData *data = (WIIU_RenderData *) renderer->driverdata;
    WIIU_TextureData *tdata = (WIIU_TextureData *) texture->driverdata;

    if (videodata->hasForeground) {
        /* Wait for the texture rendering to finish */
        WIIU_TextureCheckWaitRendering(data, tdata);
    }

#if SDL_HAVE_YUV
    if (tdata->yuv) {
        if (!tdata->pixels) {
            *pitch = texture->w;
            tdata->pixels = (Uint8 *)SDL_malloc((texture->h * *pitch * 3) / 2);
            if (!tdata->pixels) {
                return SDL_OutOfMemory();
            }
        }
        *pixels =
            (void *) ((Uint8 *) tdata->pixels + rect->y * *pitch +
                      rect->x * SDL_BYTESPERPIXEL(texture->format));
    } else
#endif
    {
        LockPlane(&tdata->main_plane, rect, SDL_BYTESPERPIXEL(texture->format), pixels, pitch);
    }

    texture->pixels = *pixels;
    texture->pitch = *pitch;
    texture->locked_rect = *rect;

    return 0;
}

void WIIU_SDL_UnlockTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    WIIU_TextureData *tdata = (WIIU_TextureData *) texture->driverdata;

#if SDL_HAVE_YUV
    if (tdata->yuv) {
        const SDL_Rect *rect = &texture->locked_rect;
        void *pixels =
            (void *) ((Uint8 *) tdata->pixels + rect->y * texture->pitch +
                      rect->x * SDL_BYTESPERPIXEL(texture->format));
        WIIU_SDL_UpdateTexture(renderer, texture, rect, pixels, texture->pitch);
    } else
#endif
    {
        UnlockPlane(&tdata->main_plane, &texture->locked_rect, texture->pixels, texture->pitch, SDL_PIXELLAYOUT(texture->format) == SDL_PACKEDLAYOUT_565);
    }

    /* This needs to be done because 'SDL_DestroyTexture' will try to free this if it's not NULL. */
    texture->pixels = NULL;
}

void WIIU_SDL_SetTextureScaleMode(SDL_Renderer * renderer, SDL_Texture * texture, SDL_ScaleMode scaleMode)
{
    WIIU_RenderData *data = (WIIU_RenderData *) renderer->driverdata;
    WIIU_TextureData *tdata = (WIIU_TextureData *) texture->driverdata;

    const GX2TexXYFilterMode filter_mode = scaleMode == SDL_ScaleModeNearest ? GX2_TEX_XY_FILTER_MODE_POINT : GX2_TEX_XY_FILTER_MODE_LINEAR;

    GX2InitSampler(&tdata->main_plane.sampler, GX2_TEX_CLAMP_MODE_CLAMP, filter_mode);

#if SDL_HAVE_YUV
    if (texture->format == SDL_PIXELFORMAT_YV12 ||
        texture->format == SDL_PIXELFORMAT_IYUV) {
        GX2InitSampler(&tdata->u_plane.sampler, GX2_TEX_CLAMP_MODE_CLAMP, filter_mode);
        GX2InitSampler(&tdata->v_plane.sampler, GX2_TEX_CLAMP_MODE_CLAMP, filter_mode);
    }
#endif

    /* The sampler is only sent to the GPU when the bound texture changes
       (see WIIU_SDL_SetDrawState). If this texture is the one currently bound,
       forget it so the next draw rebinds it with the new filter; otherwise a
       renderer that keeps drawing the same texture never sees the change. */
    if (data->drawState.texture == texture) {
        data->drawState.texture = NULL;
    }
}

int WIIU_SDL_UpdateTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                           const SDL_Rect * rect, const void *pixels, int pitch)
{
    WIIU_VideoData *videodata = (WIIU_VideoData *) SDL_GetVideoDevice()->driverdata;
    WIIU_RenderData *data = (WIIU_RenderData *) renderer->driverdata;
    WIIU_TextureData *tdata = (WIIU_TextureData *) texture->driverdata;

    if (!videodata->hasForeground) {
        return 0;
    }

    /* Wait for the texture rendering to finish */
    WIIU_TextureCheckWaitRendering(data, tdata);

#if SDL_HAVE_YUV
    if (tdata->yuv) {
	const int uv_pitch = ((pitch + 1) / 2);
        const void* pixels2 = (const void*)((const Uint8*)pixels + rect->h * pitch);
        const void* pixels3 = (const void*)((const Uint8*)pixels2 + ((rect->h + 1) / 2) * uv_pitch);

        if (texture->format == SDL_PIXELFORMAT_YV12) {
            WIIU_SDL_UpdateTextureYUV(renderer, texture, rect, pixels, pitch, pixels3, uv_pitch, pixels2, uv_pitch);
        } else {
            WIIU_SDL_UpdateTextureYUV(renderer, texture, rect, pixels, pitch, pixels2, uv_pitch, pixels3, uv_pitch);
        }
    } else
#endif
    {
        UpdatePlane(&tdata->main_plane, rect, SDL_BYTESPERPIXEL(texture->format), pixels, pitch, SDL_PIXELLAYOUT(texture->format) == SDL_PACKEDLAYOUT_565);
    }

    return 0;
}

#if SDL_HAVE_YUV
int WIIU_SDL_UpdateTextureYUV(SDL_Renderer * renderer, SDL_Texture * texture,
                    const SDL_Rect * rect,
                    const Uint8 *Yplane, int Ypitch,
                    const Uint8 *Uplane, int Upitch,
                    const Uint8 *Vplane, int Vpitch)
{
    WIIU_VideoData *videodata = (WIIU_VideoData *) SDL_GetVideoDevice()->driverdata;
    WIIU_RenderData *data = (WIIU_RenderData *) renderer->driverdata;
    WIIU_TextureData *tdata = (WIIU_TextureData *) texture->driverdata;
    const SDL_Rect uv_rect = {(rect->x + 1) / 2, (rect->y + 1) / 2, (rect->w + 1) / 2, (rect->h + 1) / 2};

    if (!videodata->hasForeground) {
        return 0;
    }

    /* Wait for the texture rendering to finish */
    WIIU_TextureCheckWaitRendering(data, tdata);

    UpdatePlane(&tdata->main_plane, rect, 1, Yplane, Ypitch, SDL_FALSE);
    UpdatePlane(&tdata->u_plane, &uv_rect, 1, Uplane, Upitch, SDL_FALSE);
    UpdatePlane(&tdata->v_plane, &uv_rect, 1, Vplane, Vpitch, SDL_FALSE);

    return 0;
}
#endif

void WIIU_SDL_DestroyTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    WIIU_VideoData *videodata = (WIIU_VideoData *) SDL_GetVideoDevice()->driverdata;
    WIIU_RenderData *data;
    WIIU_TextureData *tdata;

    if (texture == NULL || texture->driverdata == NULL) {
        return;
    }

    data = (WIIU_RenderData *) renderer->driverdata;
    tdata = (WIIU_TextureData *) texture->driverdata;

    /* Wait for the texture rendering to finish */
    if (videodata->hasForeground) {
        WIIU_TextureCheckWaitRendering(data, tdata);
    }

    if (data->drawState.texture == texture) {
        data->drawState.texture = NULL;
    }
    if (data->drawState.target == texture) {
        data->drawState.target = NULL;
    }

    DestroyPlane(&tdata->main_plane);

#if SDL_HAVE_YUV
    if (tdata->yuv) {
        DestroyPlane(&tdata->u_plane);
        DestroyPlane(&tdata->v_plane);
    }

    SDL_free(tdata->pixels);
#endif

    SDL_free(tdata);
}

#endif //SDL_VIDEO_RENDER_WIIU
