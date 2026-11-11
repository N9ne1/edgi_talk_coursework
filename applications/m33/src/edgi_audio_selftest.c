/*
 * 采音自测命令（5 号自己用的验证工具）
 * ------------------------------------------------------------
 * 编译烧录后，在串口 msh 里输入：
 *     mic_test        默认测 5 秒
 *     mic_test 10     测 10 秒
 *
 * 正常应该看到：每秒两行（0.5 秒一行），能量随说话变化，
 * 帧号连续不断号，最后一行"丢帧 0"。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdlib.h>

#include "edgi_audio_capture.h"

/*
 * 只有开了命令行（RT-Thread Settings 里勾 "MSH: command shell"）才会有 mic_test。
 * 没开的时候整段编译掉，不会报错。
 */
#ifdef RT_USING_FINSH

/* 一帧的平均能量，说话时会明显变大，安静时很小 */
static uint32_t frame_energy(const int16_t *p, rt_size_t n)
{
    int64_t acc = 0;
    rt_size_t i;

    for (i = 0; i < n; i++)
        acc += (int32_t)p[i] * (int32_t)p[i];

    return (uint32_t)(acc / (int64_t)n);
}

static int mic_test(int argc, char **argv)
{
    edgi_audio_frame_t frame;
    int      seconds;
    uint32_t total;
    uint32_t i;
    uint32_t lost = 0;
    uint32_t last_seq = 0;
    uint32_t fail = 0;

    seconds = (argc > 1) ? atoi(argv[1]) : 5;
    if (seconds <= 0 || seconds > 60)
        seconds = 5;
    total = (uint32_t)seconds * 1000u / EDGI_AUDIO_FRAME_MS;

    rt_kprintf("\n===== 采音自测 %d 秒 =====\n", seconds);

    if (edgi_audio_capture_init() != RT_EOK)
    {
        rt_kprintf("初始化就失败了，先看上面的报错\n");
        return -1;
    }

    for (i = 1; i <= total; i++)
    {
        rt_err_t e = edgi_audio_capture_read(&frame);

        if (e != RT_EOK)
        {
            fail++;
            if (fail <= 3)
                rt_kprintf("第 %u 帧读失败，错误码 %d\n", (unsigned)i, (int)e);
            continue;
        }

        /* 帧号本该每次 +1，跳了就说明中间丢了帧 */
        if (last_seq != 0 && frame.seq != last_seq + 1)
            lost += frame.seq - last_seq - 1;
        last_seq = frame.seq;

        /* 每 25 帧（0.5 秒）打印一次，别刷屏 */
        if (i % 25 == 0)
        {
            rt_kprintf("第 %4u 帧 | 帧号 %6u | 能量 %8u\n",
                       (unsigned)i, (unsigned)frame.seq,
                       (unsigned)frame_energy(frame.pcm,
                                              EDGI_AUDIO_SAMPLES_PER_FRAME));
        }
    }

    rt_kprintf("===== 结束：共 %u 帧，读失败 %u，丢帧 %u =====\n\n",
               (unsigned)total, (unsigned)fail, (unsigned)lost);

    if (fail > 0)
        rt_kprintf(">>> 有读失败：检查 mic0 是否注册、RT-Thread Settings 里音频有没有打开\n");
    else if (lost > 20)
        rt_kprintf(">>> 丢帧偏多：提高采音线程优先级，或别在采音线程里做耗时的事\n");
    else
        rt_kprintf(">>> 采音正常，可以往下做了\n");

    return 0;
}
MSH_CMD_EXPORT(mic_test, 采音自测: mic_test [秒数]);

#endif /* RT_USING_FINSH */
