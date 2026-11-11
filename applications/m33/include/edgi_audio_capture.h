#ifndef EDGI_AUDIO_CAPTURE_H
#define EDGI_AUDIO_CAPTURE_H

/*
 * 采音模块（5 号负责）
 * ------------------------------------------------------------
 * 全组只允许通过这个文件拿音频，谁都不许自己去找 mic0。
 * 好处：以后换麦克风、改采样率，只改这一个文件，别人代码不用动。
 *
 * 约定：16 kHz / 单声道 / 16 位有符号 PCM，每帧 20 ms = 320 个采样点。
 */

#include <rtthread.h>
#include <stdint.h>

#define EDGI_AUDIO_SAMPLE_RATE_HZ    16000u
#define EDGI_AUDIO_FRAME_MS          20u
#define EDGI_AUDIO_SAMPLES_PER_FRAME \
    ((EDGI_AUDIO_SAMPLE_RATE_HZ * EDGI_AUDIO_FRAME_MS) / 1000u)

/* 一帧音频。seq 和 timestamp 是给排查丢帧用的，务必保留 */
typedef struct
{
    uint32_t seq;                                   /* 帧号，1 开始连续递增 */
    uint32_t timestamp;                             /* 取到的时刻，单位 tick */
    int16_t  pcm[EDGI_AUDIO_SAMPLES_PER_FRAME];     /* 320 个采样点 */
} edgi_audio_frame_t;

/* 初始化麦克风。找不到设备时不报错，自动切到"合成波形"，方便没板子时写代码 */
rt_err_t edgi_audio_capture_init(void);

/* 读一帧，读满 320 点才返回。超时返回 -RT_ETIMEOUT */
rt_err_t edgi_audio_capture_read(edgi_audio_frame_t *frame);

/* 暂停/继续：暂停时只返回静音帧，不碰麦克风 */
void      edgi_audio_capture_set_paused(rt_bool_t paused);
rt_bool_t edgi_audio_capture_is_paused(void);

#endif /* EDGI_AUDIO_CAPTURE_H */
