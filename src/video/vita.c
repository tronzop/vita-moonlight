/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2016 Ilya Zhuravlev
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "../video.h"
#include "../config.h"
#include "../debug.h"
#include "../gui/guilib.h"
#include "../util.h"
#include "../input/vita.h"
#include "vita.h"
#include "sps.h"

#include <Limelight.h>

#include <stdbool.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/display.h>
#include <psp2/videodec.h>
#include <vita2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define printf vita_debug_log

extern void gs_sps_stop();

extern double_click_tracker dc_tracker;

void draw_streaming(vita2d_texture *frame_texture);
void draw_stats();
void draw_indicators();

enum {
  VITA_VIDEO_INIT_OK                    = 0,
  VITA_VIDEO_ERROR_NO_MEM               = 0x80010001,
  VITA_VIDEO_ERROR_INIT_LIB             = 0x80010002,
  VITA_VIDEO_ERROR_QUERY_DEC_MEMSIZE    = 0x80010003,
  VITA_VIDEO_ERROR_ALLOC_MEM            = 0x80010004,
  VITA_VIDEO_ERROR_GET_MEMBASE          = 0x80010005,
  VITA_VIDEO_ERROR_CREATE_DEC           = 0x80010006,
  VITA_VIDEO_ERROR_CREATE_RENDER_THREAD = 0x80010007,
};

// User thread priorities run from 64 (highest) to 191; the default is 160.
#define VIDEO_DECODE_PRIORITY 64
#define VIDEO_RENDER_PRIORITY 65

// One texture being decoded into, one waiting to be shown, one being drawn.
#define FRAME_TEXTURES 3

#define DECODER_BUFFER_SIZE (128 * 1024)

#define AV_INPUT_BUFFER_PADDING_SIZE 64

static char* decoder_buffer = NULL;

static size_t decoder_buffer_size = 0;

enum {
  SCREEN_WIDTH = 960,
  SCREEN_HEIGHT = 544,
  LINE_SIZE = 960,
  FRAMEBUFFER_SIZE = 2 * 1024 * 1024,
  FRAMEBUFFER_ALIGNMENT = 256 * 1024
};

enum VideoStatus {
  NOT_INIT,
  INIT_GS,
  INIT_FRAMEBUFFER,
  INIT_AVC_LIB,
  INIT_DECODER_MEMBLOCK,
  INIT_AVC_DEC,
  INIT_RENDER_THREAD,
};

static vita2d_texture *frame_textures[FRAME_TEXTURES] = {0};
enum VideoStatus video_status = NOT_INIT;

SceAvcdecCtrl *decoder = NULL;
SceUID displayblock = -1;
SceUID decoderblock = -1;
SceUID render_thread = -1;
SceVideodecQueryInitInfoHwAvcdec *init = NULL;
SceAvcdecQueryDecoderInfo *decoder_info = NULL;

typedef struct {
  bool activated;
  uint8_t alpha;
  bool plus;
} indicator_status;

static volatile bool active_video_thread = true;
static volatile bool active_render_thread = false;
static indicator_status poor_net_indicator = {0};

static SceUID frame_ready_sema = -1;
static SceUID frame_state_mutex = -1;
static SceUID draw_mutex = -1;
static int decode_idx = 0;
static int pending_idx = -1;
static int displayed_idx = -1;
static bool decode_thread_tuned = false;

// Counters only ever increase; the render thread turns them into per-second
// deltas, so unsigned wraparound is harmless.
typedef struct {
  uint32_t received;
  uint32_t net_dropped;
  uint32_t decoded;
  uint32_t rendered;
  uint32_t skipped;
  uint32_t bytes;
  uint32_t decode_us;
  uint32_t decode_us_max;
  uint32_t rx_to_decoded_us;
  uint32_t host_latency_tenths_ms;
  uint32_t host_latency_frames;
} video_stats;

static video_stats stats_totals;
static video_stats stats_last;
static video_stats stats_window;
static uint64_t stats_window_start;
static uint32_t stats_rtt_ms;
static int last_frame_number;

typedef struct {
  unsigned int texture_width;
  unsigned int texture_height;
  float origin_x;
  float origin_y;
  float region_x1;
  float region_y1;
  float region_x2;
  float region_y2;
} image_scaling_settings;

static image_scaling_settings image_scaling = {0};

void update_scaling_settings(int width, int height) {
  image_scaling.texture_width = SCREEN_WIDTH;
  image_scaling.texture_height = SCREEN_HEIGHT;
  image_scaling.origin_x = 0;
  image_scaling.origin_y = 0;
  image_scaling.region_x1 = 0;
  image_scaling.region_y1 = 0;
  image_scaling.region_x2 = image_scaling.texture_width;
  image_scaling.region_y2 = image_scaling.texture_height;

  double scaled_width = (double) SCREEN_HEIGHT * width / height;
  double scaled_height = (double) SCREEN_WIDTH * height / width;

  if (SCREEN_WIDTH * height == SCREEN_HEIGHT * width) {
    // streaming resolution ratio matches Vita's screen ratio
    // use default setting
  } else if (SCREEN_WIDTH * height > SCREEN_HEIGHT * width) {
    // host ratio example: 4:3, 16:10
    // Vita ratio range: 2:16 (64 x 544) - native (960 x 544)
    if (config.center_region_only) {
      image_scaling.texture_height = VITA_DECODER_RESOLUTION(scaled_height);
      image_scaling.region_y1 = VITA_DECODER_RESOLUTION((scaled_height - SCREEN_HEIGHT) / 2);
      image_scaling.region_y2 = VITA_DECODER_RESOLUTION((scaled_height + SCREEN_HEIGHT) / 2);
    } else {
      image_scaling.texture_width = VITA_DECODER_RESOLUTION(scaled_width);
      image_scaling.region_x2 = VITA_DECODER_RESOLUTION(scaled_width);
      image_scaling.origin_x = round((double) (SCREEN_WIDTH - image_scaling.texture_width) / 2);
    }
  } else {
    // host ratio example: 16:9, 21:9, 32:9
    // Vita ratio range: native (960 x 544) - 15:1 (960 x 64)
    if (config.center_region_only) {
      image_scaling.texture_width = VITA_DECODER_RESOLUTION(scaled_width);
      image_scaling.region_x1 = VITA_DECODER_RESOLUTION((scaled_width - SCREEN_WIDTH) / 2);
      image_scaling.region_x2 = VITA_DECODER_RESOLUTION((scaled_width + SCREEN_WIDTH) / 2);
    } else {
      image_scaling.texture_height = VITA_DECODER_RESOLUTION(scaled_height);
      image_scaling.region_y2 = VITA_DECODER_RESOLUTION(scaled_height);
      image_scaling.origin_y = round((double) (SCREEN_HEIGHT - image_scaling.texture_height) / 2);
    }
  }

  printf("update_scaling_settings: width = %u\n", width);
  printf("update_scaling_settings: height = %u\n", height);
  printf("update_scaling_settings: scaled_width = %f\n", scaled_width);
  printf("update_scaling_settings: scaled_height = %f\n", scaled_height);
  printf("update_scaling_settings: image_scaling.texture_width = %u\n", image_scaling.texture_width);
  printf("update_scaling_settings: image_scaling.texture_height = %u\n", image_scaling.texture_height);
  printf("update_scaling_settings: image_scaling.origin_x = %f\n", image_scaling.origin_x);
  printf("update_scaling_settings: image_scaling.origin_y = %f\n", image_scaling.origin_y);
  printf("update_scaling_settings: image_scaling.region_x1 = %f\n", image_scaling.region_x1);
  printf("update_scaling_settings: image_scaling.region_y1 = %f\n", image_scaling.region_y1);
  printf("update_scaling_settings: image_scaling.region_x2 = %f\n", image_scaling.region_x2);
  printf("update_scaling_settings: image_scaling.region_y2 = %f\n", image_scaling.region_y2);
}

static void tune_current_thread(int priority, int cpu_mask) {
  SceUID self = sceKernelGetThreadId();
  sceKernelChangeThreadPriority(self, priority);
  sceKernelChangeThreadCpuAffinityMask(self, cpu_mask);
}

static void update_stats_window(void) {
  uint64_t now = sceKernelGetProcessTimeWide();
  if (now - stats_window_start < 1000000) {
    return;
  }
  video_stats snap = stats_totals;
#define STATS_DELTA(field) stats_window.field = snap.field - stats_last.field
  STATS_DELTA(received);
  STATS_DELTA(net_dropped);
  STATS_DELTA(decoded);
  STATS_DELTA(rendered);
  STATS_DELTA(skipped);
  STATS_DELTA(bytes);
  STATS_DELTA(decode_us);
  STATS_DELTA(rx_to_decoded_us);
  STATS_DELTA(host_latency_tenths_ms);
  STATS_DELTA(host_latency_frames);
#undef STATS_DELTA
  stats_window.decode_us_max = snap.decode_us_max;
  stats_totals.decode_us_max = 0;
  stats_last = snap;
  stats_window_start = now;

  uint32_t rtt, rtt_variance;
  if (LiGetEstimatedRttInfo(&rtt, &rtt_variance)) {
    stats_rtt_ms = rtt;
  }
}

static void publish_decoded_frame(void) {
  sceKernelLockMutex(frame_state_mutex, 1, NULL);
  if (pending_idx >= 0) {
    // The render thread never picked up the previous frame; show the newer one instead.
    stats_totals.skipped++;
  }
  pending_idx = decode_idx;
  for (int i = 0; i < FRAME_TEXTURES; i++) {
    if (i != pending_idx && i != displayed_idx) {
      decode_idx = i;
      break;
    }
  }
  sceKernelUnlockMutex(frame_state_mutex, 1);
  sceKernelSignalSema(frame_ready_sema, 1);
}

static int vita_render_thread_main(SceSize args, void *argp) {
  while (active_render_thread) {
    SceUInt timeout = 100000;
    if (sceKernelWaitSema(frame_ready_sema, 1, &timeout) < 0) {
      continue;
    }

    sceKernelLockMutex(frame_state_mutex, 1, NULL);
    int idx = pending_idx;
    pending_idx = -1;
    displayed_idx = idx;
    sceKernelUnlockMutex(frame_state_mutex, 1);
    if (idx < 0) {
      continue;
    }

    sceKernelLockMutex(draw_mutex, 1, NULL);
    if (active_video_thread) {
      update_stats_window();

      vita2d_start_drawing();
      draw_streaming(frame_textures[idx]);
      draw_stats();
      draw_indicators();
      vita2d_end_drawing();

      vita2d_wait_rendering_done();
      vita2d_swap_buffers();
      stats_totals.rendered++;
    }
    sceKernelUnlockMutex(draw_mutex, 1);

    sceKernelLockMutex(frame_state_mutex, 1, NULL);
    displayed_idx = -1;
    sceKernelUnlockMutex(frame_state_mutex, 1);
  }
  return 0;
}

static void vita_cleanup() {
  if (video_status == INIT_RENDER_THREAD) {
    active_render_thread = false;
    sceKernelSignalSema(frame_ready_sema, 1);
    // wait 10sec
    SceUInt timeout = 10000000;
    int ret;
    sceKernelWaitThreadEnd(render_thread, &ret, &timeout);
    sceKernelDeleteThread(render_thread);
    render_thread = -1;
    video_status--;
  }

  if (video_status == INIT_AVC_DEC) {
    sceAvcdecDeleteDecoder(decoder);
    video_status--;
  }

  if (video_status == INIT_DECODER_MEMBLOCK) {
    if (decoderblock >= 0) {
      sceKernelFreeMemBlock(decoderblock);
      decoderblock = -1;
    }
    if (decoder != NULL) {
      free(decoder);
      decoder = NULL;
    }
    if (decoder_info != NULL) {
      free(decoder_info);
      decoder_info = NULL;
    }
    video_status--;
  }

  if (video_status == INIT_AVC_LIB) {
    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);

    if (init != NULL) {
      free(init);
      init = NULL;
    }
    video_status--;
  }

  if (video_status == INIT_FRAMEBUFFER) {
    for (int i = 0; i < FRAME_TEXTURES; i++) {
      if (frame_textures[i] != NULL) {
        vita2d_free_texture(frame_textures[i]);
        frame_textures[i] = NULL;
      }
    }
    if (frame_ready_sema >= 0) {
      sceKernelDeleteSema(frame_ready_sema);
      frame_ready_sema = -1;
    }
    if (frame_state_mutex >= 0) {
      sceKernelDeleteMutex(frame_state_mutex);
      frame_state_mutex = -1;
    }
    if (draw_mutex >= 0) {
      sceKernelDeleteMutex(draw_mutex);
      draw_mutex = -1;
    }

    if (decoder_buffer != NULL) {
      free(decoder_buffer);
      decoder_buffer = NULL;
    }
    video_status--;
  }

  if (video_status == INIT_GS) {
    gs_sps_stop();
    video_status--;
  }
}

static int vita_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  int ret;
  printf("vita video setup\n");

  if (video_status == NOT_INIT) {
    // INIT_GS
    gs_sps_init(width, height);
    video_status++;
  }

  if (video_status == INIT_GS) {
    // INIT_FRAMEBUFFER
    update_scaling_settings(width, height);

    decoder_buffer_size = DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE;
    decoder_buffer = malloc(decoder_buffer_size);
    if (decoder_buffer == NULL) {
      printf("decoder_buffer: not enough memory\n");
      ret = VITA_VIDEO_ERROR_NO_MEM;
      goto cleanup;
    } 

    for (int i = 0; i < FRAME_TEXTURES; i++) {
      frame_textures[i] = vita2d_create_empty_texture_format(image_scaling.texture_width, image_scaling.texture_height, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
      if (frame_textures[i] == NULL) {
        printf("frame_texture: not enough memory\n");
        ret = VITA_VIDEO_ERROR_NO_MEM;
        goto cleanup;
      }
    }

    frame_ready_sema = sceKernelCreateSema("frame_ready", 0, 0, 1, NULL);
    frame_state_mutex = sceKernelCreateMutex("frame_state", 0, 0, NULL);
    draw_mutex = sceKernelCreateMutex("frame_draw", 0, 0, NULL);
    if (frame_ready_sema < 0 || frame_state_mutex < 0 || draw_mutex < 0) {
      printf("frame sync objects: 0x%x 0x%x 0x%x\n", frame_ready_sema, frame_state_mutex, draw_mutex);
      ret = VITA_VIDEO_ERROR_NO_MEM;
      goto cleanup;
    }
    decode_idx = 0;
    pending_idx = -1;
    displayed_idx = -1;
    decode_thread_tuned = false;
    last_frame_number = 0;
    memset(&stats_totals, 0, sizeof(stats_totals));
    memset(&stats_last, 0, sizeof(stats_last));
    memset(&stats_window, 0, sizeof(stats_window));
    stats_window_start = sceKernelGetProcessTimeWide();
    stats_rtt_ms = 0;

    printf("Initalized Framebuffer");

    video_status++;
  }

  if (video_status == INIT_FRAMEBUFFER) {
    // INIT_AVC_LIB
    if (init == NULL) {
      init = calloc(1, sizeof(SceVideodecQueryInitInfoHwAvcdec));
      if (init == NULL) {
        printf("init_framebuffer: not enough memory\n");
        ret = VITA_VIDEO_ERROR_NO_MEM;
        goto cleanup;
      }
    }
    init->size = sizeof(SceVideodecQueryInitInfoHwAvcdec);
    init->horizontal = VITA_DECODER_RESOLUTION(width);
    init->vertical = VITA_DECODER_RESOLUTION(height);
    init->numOfRefFrames = config.enable_ref_frame_invalidation ? 16 : 4;
    init->numOfStreams = 1;

    ret = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, init);
    if (ret < 0 && init->numOfRefFrames > 4) {
      printf("sceVideodecInitLibrary 0x%x with %d ref frames, retrying with 4\n", ret, init->numOfRefFrames);
      init->numOfRefFrames = 4;
      ret = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, init);
    }
    if (ret < 0) {
      printf("sceVideodecInitLibrary 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_INIT_LIB;
      goto cleanup;
    }
    printf("Initalized AVC library");
    video_status++;
  }

  if (video_status == INIT_AVC_LIB) {
    // INIT_DECODER_MEMBLOCK
    if (decoder_info == NULL) {
      decoder_info = calloc(1, sizeof(SceAvcdecQueryDecoderInfo));
      if (decoder_info == NULL) {
        printf("decoder_info: not enough memory\n");
        ret = VITA_VIDEO_ERROR_NO_MEM;
        goto cleanup;
      }
    }
    decoder_info->horizontal = init->horizontal;
    decoder_info->vertical = init->vertical;
    decoder_info->numOfRefFrames = init->numOfRefFrames;

    SceAvcdecDecoderInfo decoder_info_out = {0};

    ret = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, decoder_info, &decoder_info_out);
    if (ret < 0) {
      printf("sceAvcdecQueryDecoderMemSize 0x%x size 0x%x\n", ret, decoder_info_out.frameMemSize);
      ret = VITA_VIDEO_ERROR_QUERY_DEC_MEMSIZE;
      goto cleanup;
    }

    decoder = calloc(1, sizeof(SceAvcdecCtrl));
    if (decoder == NULL) {
      printf("not enough memory\n");
      ret = VITA_VIDEO_ERROR_ALLOC_MEM;
      goto cleanup;
    }

    size_t sz = (decoder_info_out.frameMemSize + 0xFFFFF) & ~0xFFFFF;
    decoder->frameBuf.size = sz;
    printf("allocating size 0x%x\n", sz);

    decoderblock = sceKernelAllocMemBlock("decoder", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
    if (decoderblock < 0) {
      printf("decoderblock: 0x%08x\n", decoderblock);
      ret = VITA_VIDEO_ERROR_ALLOC_MEM;
      goto cleanup;
    }

    ret = sceKernelGetMemBlockBase(decoderblock, &decoder->frameBuf.pBuf);
    if (ret < 0) {
      printf("sceKernelGetMemBlockBase: 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_GET_MEMBASE;
      goto cleanup;
    }
    video_status++;
  }

  if (video_status == INIT_DECODER_MEMBLOCK) {
    // INIT_AVC_DEC
    printf("base: 0x%08x\n", (unsigned int)decoder->frameBuf.pBuf);

    ret = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, decoder, decoder_info);
    if (ret < 0) {
      printf("sceAvcdecCreateDecoder 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_CREATE_DEC;
      goto cleanup;
    }
    video_status++;
  }

  if (video_status == INIT_AVC_DEC) {
    // INIT_RENDER_THREAD
    ret = sceKernelCreateThread("video_render", vita_render_thread_main, VIDEO_RENDER_PRIORITY, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (ret < 0) {
      printf("sceKernelCreateThread 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_CREATE_RENDER_THREAD;
      goto cleanup;
    }
    render_thread = ret;
    active_render_thread = true;
    sceKernelStartThread(render_thread, 0, NULL);
    video_status++;
  }

  return VITA_VIDEO_INIT_OK;

cleanup:
  vita_cleanup();
  return ret;
}

static int vita_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  if (!decode_thread_tuned) {
    // Called on moonlight-common-c's receive thread, which we don't create ourselves.
    tune_current_thread(VIDEO_DECODE_PRIORITY, SCE_KERNEL_CPU_MASK_USER_0);
    decode_thread_tuned = true;
  }

  stats_totals.received++;
  stats_totals.bytes += decodeUnit->fullLength;
  if (last_frame_number && decodeUnit->frameNumber > last_frame_number + 1) {
    stats_totals.net_dropped += decodeUnit->frameNumber - last_frame_number - 1;
  }
  last_frame_number = decodeUnit->frameNumber;
  if (decodeUnit->frameHostProcessingLatency) {
    stats_totals.host_latency_tenths_ms += decodeUnit->frameHostProcessingLatency;
    stats_totals.host_latency_frames++;
  }

  SceAvcdecAu au = {0};
  SceAvcdecArrayPicture array_picture = {0};
  struct SceAvcdecPicture picture = {0};
  struct SceAvcdecPicture *pictures = { &picture };
  array_picture.numOfElm = 1;
  array_picture.pPicture = &pictures;

  picture.size = sizeof(picture);
  picture.frame.pixelType = 0;
  picture.frame.framePitch = image_scaling.texture_width;
  picture.frame.frameWidth = image_scaling.texture_width;
  picture.frame.frameHeight = image_scaling.texture_height;
  picture.frame.pPicture[0] = vita2d_texture_get_datap(frame_textures[decode_idx]);

  //ensure_buf_size((void *)&decoder_buffer, &decoder_buffer_size, decodeUnit->fullLength + 64);

  if (decoder_buffer_size < (decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE)) {
    printf("Reallocating decoder buffer to %u bytes", (size_t)decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);
    decoder_buffer = realloc(decoder_buffer, decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);
    decoder_buffer_size = decodeUnit->fullLength+AV_INPUT_BUFFER_PADDING_SIZE;
    if (decoder_buffer == NULL) {
      printf("Out of memory! could not reallocate buffer!!");
      exit(1);
    }
  }


/*   if (decodeUnit->fullLength >= DECODER_BUFFER_SIZE + 64) {
    printf("Video decode buffer too small\n");
    exit(1);
  } */

  PLENTRY entry = decodeUnit->bufferList;
  uint32_t length = 0;
  while (entry != NULL) {
    if (entry->bufferType == BUFFER_TYPE_SPS) {
      gs_sps_fix(entry, GS_SPS_BITSTREAM_FIXUP | (config.enable_ref_frame_invalidation ? GS_SPS_KEEP_REF_FRAMES : 0), decoder_buffer, &length);
    } else {
      memcpy(decoder_buffer+length, entry->data, entry->length);
      length += entry->length;
    }
    entry = entry->next;
  }

  au.es.pBuf = decoder_buffer;
  // The rewritten SPS is longer than the original, so use the bytes actually copied.
  au.es.size = length;
  au.dts.lower = 0xFFFFFFFF;
  au.dts.upper = 0xFFFFFFFF;
  au.pts.lower = 0xFFFFFFFF;
  au.pts.upper = 0xFFFFFFFF;

  uint64_t decode_start = sceKernelGetProcessTimeWide();
  int ret = sceAvcdecDecode(decoder, &au, &array_picture);
  uint32_t decode_us = sceKernelGetProcessTimeWide() - decode_start;
  if (ret < 0) {
    printf("sceAvcdecDecode (len=0x%x): 0x%x numOfOutput %d\n", decodeUnit->fullLength, ret, array_picture.numOfOutput);
    return DR_NEED_IDR;
  }

  if (array_picture.numOfOutput != 1) {
    return DR_OK;
  }

  stats_totals.decoded++;
  stats_totals.decode_us += decode_us;
  if (decode_us > stats_totals.decode_us_max) {
    stats_totals.decode_us_max = decode_us;
  }
  stats_totals.rx_to_decoded_us += LiGetMicroseconds() - decodeUnit->receiveTimeUs;

  publish_decoded_frame();
  return DR_OK;
}

void draw_streaming(vita2d_texture *frame_texture) {
  // ui is still rendering in the background, clear the screen first
  vita2d_clear_screen();
  vita2d_draw_texture_part(frame_texture,
                           image_scaling.origin_x,
                           image_scaling.origin_y,
                           image_scaling.region_x1,
                           image_scaling.region_y1,
                           image_scaling.region_x2,
                           image_scaling.region_y2);
}

void draw_stats() {
  if (!config.show_fps) {
    return;
  }
  const video_stats *w = &stats_window;
  uint32_t decoded = w->decoded ? w->decoded : 1;
  double host_ms = w->host_latency_frames ? w->host_latency_tenths_ms / 10.0 / w->host_latency_frames : 0;
  unsigned int color = RGBA8(0xFF, 0xFF, 0xFF, 0xFF);

  vita2d_draw_rectangle(30, 4, 640, 44, RGBA8(0, 0, 0, 0xA0));
  vita2d_font_draw_textf(font, 40, 20, color, 16,
                         "fps %u rx / %u dec / %u shown   lost %u  skipped %u   %.1f Mbps",
                         w->received, w->decoded, w->rendered, w->net_dropped, w->skipped,
                         w->bytes * 8 / 1000000.0);
  vita2d_font_draw_textf(font, 40, 40, color, 16,
                         "decode %.1f ms (max %.1f)   rx->decoded %.1f ms   host %.1f ms   rtt %u ms",
                         w->decode_us / 1000.0 / decoded, w->decode_us_max / 1000.0,
                         w->rx_to_decoded_us / 1000.0 / decoded, host_ms, stats_rtt_ms);
}

void draw_indicators() {
  if (poor_net_indicator.activated) {
    vita2d_font_draw_text(font, 40, 500, RGBA8(0xFF, 0xFF, 0xFF, poor_net_indicator.alpha), 64, ICON_NETWORK);
    poor_net_indicator.alpha += (0x4 * (poor_net_indicator.plus ? 1 : -1));
    if (poor_net_indicator.alpha == 0) {
      poor_net_indicator.plus = !poor_net_indicator.plus;
      poor_net_indicator.alpha += (0x4 * (poor_net_indicator.plus ? 1 : -1));
    }
  }

  if (dc_tracker.currently_sprinting) {
    vita2d_font_draw_text(font, 40, 50, RGBA8(0xFF, 0xFF, 0xFF, 0xAA), 48, ICON_SPRINTING);
  }

}

void vitavideo_start() {
  active_video_thread = true;
  vita2d_set_vblank_wait(config.enable_vita_vblank_wait);
}

void vitavideo_stop() {
  active_video_thread = false;
  // Wait out a frame that is mid-draw so the UI gets vita2d to itself.
  if (draw_mutex >= 0) {
    sceKernelLockMutex(draw_mutex, 1, NULL);
    sceKernelUnlockMutex(draw_mutex, 1);
  }
  vita2d_set_vblank_wait(true);
}

void vitavideo_show_poor_net_indicator() {
  poor_net_indicator.activated = true;
}

void vitavideo_hide_poor_net_indicator() {
  //poor_net_indicator.activated = false;
  memset(&poor_net_indicator, 0, sizeof(indicator_status));
}

int vitavideo_initialized() {
  return video_status != NOT_INIT;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_vita = {
  .setup = vita_setup,
  .cleanup = vita_cleanup,
  .submitDecodeUnit = vita_submit_decode_unit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SLICES_PER_FRAME(2)
};
