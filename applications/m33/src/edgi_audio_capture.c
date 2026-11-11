/*
 * 采音模块实现（5 号负责）
 * ------------------------------------------------------------
 * 板子上麦克风已经被 BSP 注册成设备 "mic0"，我们只是去"打开、设增益、读数据"。
 * 找不到 mic0 时改走合成波形，这样别人没板子也能编译、看界面效果。
 *
 * 两个必须知道的硬件事实（来自 libraries/HAL_Drivers/drv_pdm.c）：
 *   1. mic0 的默认格式是 16000 Hz / 2 声道 / 16 位，每 10 ms 交付一包。
 *      驱动里 configure 只是把参数记下来，并不会真的改变 PDM 硬件格式，
 *      所以上层必须按"双声道交织"来理解数据，否则帧率会算错一倍。
 *   2. 一包 10 ms 立体声 = 640 字节；我们要 20 ms = 1280 字节，
 *      所以要循环读两次才能凑满一帧。
 */

#include "edgi_audio_capture.h"

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>

#define EDGI_MIC_DEV_NAME     "mic0"    /* BSP 里 HAL 的 drv_pdm.c 注册的名字 */
#define EDGI_MIC_VOLUME       30        /* 麦克风增益，0-100，现场嫌小声可以调大 */

/* 硬件实际输出几个声道，由 BSP 的开关决定，这里跟着它走 */
#ifdef ENABLE_STEREO_INPUT_FEED
#define EDGI_HW_CHANNELS      2
#else
#define EDGI_HW_CHANNELS      1
#endif

#define EDGI_FRAME_SAMPLES    EDGI_AUDIO_SAMPLES_PER_FRAME              /* 320 */
#define EDGI_RAW_SAMPLES      (EDGI_FRAME_SAMPLES * EDGI_HW_CHANNELS)   /* 640 或 320 */
#define EDGI_RAW_BYTES        (EDGI_RAW_SAMPLES * 2)                    /* 1280 或 640 */

static rt_device_t s_mic      = RT_NULL;
static rt_bool_t   s_use_stub = RT_FALSE;
static rt_bool_t   s_paused   = RT_FALSE;
static uint32_t    s_seq      = 0;

/*
 * 存一帧原始数据。放静态区而不是栈上：
 * 采音线程栈只有 2 KB，1280 字节的局部数组会把栈撑爆。
 * 全组只有采音线程会碰这个数组，不需要加锁。
 */
static int16_t s_raw[EDGI_RAW_SAMPLES];

/* 没麦克风时用的假波形，只为了流程跑得动 */
static void fill_stub(int16_t *out, rt_size_t n)
{
    static rt_uint32_t ph = 0;
    rt_size_t i;

    for (i = 0; i < n; i++)
    {
        int32_t s = (int32_t)((ph & 0xFF) * 4 - 512);
        ph++;
        out[i] = (int16_t)(s + (int32_t)((ph & 7) * 20));
    }
}

rt_err_t edgi_audio_capture_init(void)
{
    struct rt_audio_caps caps;

    s_mic = rt_device_find(EDGI_MIC_DEV_NAME);
    if (s_mic == RT_NULL)
    {
        s_use_stub = RT_TRUE;
        s_seq      = 0;
        rt_kprintf("[mic] 没找到 %s，改用合成波形\n", EDGI_MIC_DEV_NAME);
        rt_kprintf("[mic] 原因：工程没开 BSP_USING_AUDIO，drv_pdm.c 没被编译\n");
        return RT_EOK;
    }

#ifdef RT_USING_AUDIO
    /* 设麦克风增益。这一步和官方 xiaozhi 工程一致，不用先 open */
    caps.main_type   = AUDIO_TYPE_MIXER;
    caps.sub_type    = AUDIO_MIXER_VOLUME;
    caps.udata.value = EDGI_MIC_VOLUME;
    rt_device_control(s_mic, AUDIO_CTL_CONFIGURE, &caps);
#endif

    if (rt_device_open(s_mic, RT_DEVICE_OFLAG_RDONLY) != RT_EOK)
    {
        s_mic      = RT_NULL;
        s_use_stub = RT_TRUE;
        s_seq      = 0;
        rt_kprintf("[mic] %s 打开失败，改用合成波形\n", EDGI_MIC_DEV_NAME);
        return RT_EOK;
    }

    s_use_stub = RT_FALSE;
    s_seq      = 0;

    /*
     * 把驱动里真实的参数读回来打印。
     * 这一行是"采音到底通没通"的硬证据：出现 16000 / 2 声道 / 16 位
     * 就说明 PDM 硬件已经在工作，而不是合成波形。
     */
#ifdef RT_USING_AUDIO
    caps.main_type = AUDIO_TYPE_INPUT;
    caps.sub_type  = AUDIO_DSP_PARAM;
    if (rt_device_control(s_mic, AUDIO_CTL_GETCAPS, &caps) == RT_EOK)
    {
        rt_kprintf("[mic] 硬件参数：%d Hz / %d 声道 / %d 位\n",
                   (int)caps.udata.config.samplerate,
                   (int)caps.udata.config.channels,
                   (int)caps.udata.config.samplebits);
    }
#endif

    rt_kprintf("[mic] 采音就绪：每帧 %u 点 / %u ms，每次读 %u 字节原始数据\n",
               (unsigned)EDGI_FRAME_SAMPLES,
               (unsigned)EDGI_AUDIO_FRAME_MS,
               (unsigned)EDGI_RAW_BYTES);
    return RT_EOK;
}

void edgi_audio_capture_set_paused(rt_bool_t paused)
{
    s_paused = paused;
}

rt_bool_t edgi_audio_capture_is_paused(void)
{
    return s_paused;
}

rt_err_t edgi_audio_capture_read(edgi_audio_frame_t *frame)
{
    rt_size_t  got;
    rt_uint8_t *p;
    int        spins;
    rt_size_t  i;

    if (frame == RT_NULL)
        return -RT_EINVAL;

    if (s_paused)
    {
        rt_memset(frame->pcm, 0, sizeof(frame->pcm));
    }
    else if (s_use_stub || s_mic == RT_NULL)
    {
        fill_stub(frame->pcm, EDGI_FRAME_SAMPLES);
    }
    else
    {
        /*
         * 读满 20 ms。mic0 约 10 ms 给一包，所以这里一般循环两次。
         * spins 是防死循环的保险：麦克风没起来时不能一直卡在这里，
         * 连续 2000 次读不到（约 2 秒）就返回超时，让上层自己决定怎么办。
         */
        got   = 0;
        p     = (rt_uint8_t *)s_raw;
        spins = 0;

        while (got < EDGI_RAW_BYTES)
        {
            rt_ssize_t n = rt_device_read(s_mic, 0, p + got, EDGI_RAW_BYTES - got);
            if (n <= 0)
            {
                rt_thread_mdelay(1);
                if (++spins > 2000)
                    return -RT_ETIMEOUT;
                continue;
            }
            spins = 0;
            got  += (rt_size_t)n;
        }

#ifdef ENABLE_STEREO_INPUT_FEED
        /*
         * 双声道交织排列：[L0][R0][L1][R1]...
         * 两个麦克风取平均，能压掉一部分噪声，也避免只取左声道时
         * 右声道损坏却看不出来。
         */
        for (i = 0; i < EDGI_FRAME_SAMPLES; i++)
        {
            int32_t l = s_raw[i * 2];
            int32_t r = s_raw[i * 2 + 1];
            frame->pcm[i] = (int16_t)((l + r) / 2);
        }
#else
        rt_memcpy(frame->pcm, s_raw, sizeof(frame->pcm));
#endif
    }

    frame->seq       = ++s_seq;
    frame->timestamp = rt_tick_get();
    return RT_EOK;
}
