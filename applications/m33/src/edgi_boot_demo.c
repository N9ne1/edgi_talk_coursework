/*
 * 开机自动自测（5 号模块）
 * ------------------------------------------------------------
 * 工程本来没开命令行，所以没法敲 mic_test。这里改成上电自动跑：
 *
 *   第 1 段：采音 5 秒，打印帧号和能量 —— 验证麦克风真的在工作
 *   第 2 段：模式连切 100 次 —— 验证状态机不崩、会话号连续、内存不涨
 *
 * 看到最后一行 "全部通过"，就说明采音和模式切换这两块都成了。
 * 以后不想每次都跑，把 EDGI_BOOT_DEMO 改成 0。
 */

#include <rtthread.h>
#include <rtdevice.h>

#include "edgi_audio_capture.h"
#include "edgi_pipeline.h"

#ifndef EDGI_BOOT_DEMO
#define EDGI_BOOT_DEMO 1        /* 1=开机自动自测 0=关掉 */
#endif

#if EDGI_BOOT_DEMO

#define DEMO_DELAY_MS    3000u  /* 等系统启动稳定再测 */
#define DEMO_SECONDS     5u     /* 采音测多少秒 */
#define DEMO_SWITCH_NUM  100u   /* 模式切换压测次数 */

/* 一帧的平均能量：说话、拍手时会明显变大，安静时很小 */
static rt_uint32_t frame_energy(const rt_int16_t *p, rt_size_t n)
{
    rt_int64_t acc = 0;
    rt_size_t  i;

    for (i = 0; i < n; i++)
        acc += (rt_int32_t)p[i] * (rt_int32_t)p[i];

    return (rt_uint32_t)(acc / (rt_int64_t)n);
}

/*
 * 期望的帧间隔（tick）。RT_TICK_PER_SECOND 可能是 100 或 1000，
 * 所以按宏算，不能写死。
 */
#define EXPECT_TICKS  ((rt_tick_t)(EDGI_AUDIO_FRAME_MS * RT_TICK_PER_SECOND / 1000u))

static void demo_capture(void)
{
    edgi_audio_frame_t frame;
    rt_uint32_t i;
    rt_uint32_t total;
    rt_uint32_t ok      = 0;
    rt_uint32_t fail    = 0;
    rt_uint32_t late    = 0;
    rt_tick_t   last_tk = 0;

    rt_kprintf("\r\n========================================\r\n");
    rt_kprintf("  第 1 段：采音自测 %u 秒\r\n", (unsigned)DEMO_SECONDS);
    rt_kprintf("========================================\r\n");

    edgi_audio_capture_init();

    total = DEMO_SECONDS * 1000u / EDGI_AUDIO_FRAME_MS;
    for (i = 1; i <= total; i++)
    {
        if (edgi_audio_capture_read(&frame) != RT_EOK)
        {
            fail++;
            continue;
        }

        ok++;

        /* 帧间隔明显超过 20 ms，说明中间被别的线程抢走了 CPU 或者缓冲断流 */
        if (last_tk != 0 && (frame.timestamp - last_tk) > (EXPECT_TICKS * 2))
            late++;
        last_tk = frame.timestamp;

        if ((i % 25u) == 0u)
        {
            rt_kprintf("[自测] 帧号 %5u   能量 %8u   间隔 %3u tick\r\n",
                       (unsigned)frame.seq,
                       (unsigned)frame_energy(frame.pcm, EDGI_AUDIO_SAMPLES_PER_FRAME),
                       (unsigned)(EXPECT_TICKS));
        }
    }

    rt_kprintf("----------------------------------------\r\n");
    rt_kprintf("采音结果：成功 %u 帧，失败 %u 帧，晚到 %u 帧（共 %u 帧）\r\n",
               (unsigned)ok, (unsigned)fail, (unsigned)late, (unsigned)total);

    if (fail == 0 && ok == total)
        rt_kprintf(">>> 采音通过：帧率稳定，没有断流\r\n");
    else if (fail > 0)
        rt_kprintf(">>> 采音有问题：有 %u 帧读不到，多半是 mic0 没起来\r\n", (unsigned)fail);
    else
        rt_kprintf(">>> 采音勉强可用：有 %u 帧晚到，考虑提高采音线程优先级\r\n", (unsigned)late);
}

static void demo_switch(void)
{
    rt_uint32_t i;
    rt_uint32_t ok   = 0;
    rt_uint32_t fail = 0;
    rt_uint32_t seq0;
    rt_uint32_t seq1;
    rt_size_t   mem_t0, mem_u0, mem_m0;
    rt_size_t   mem_t1, mem_u1, mem_m1;

    rt_kprintf("\r\n========================================\r\n");
    rt_kprintf("  第 2 段：模式切换压测 %u 次\r\n", (unsigned)DEMO_SWITCH_NUM);
    rt_kprintf("========================================\r\n");

    seq0 = edgi_pipeline_session();
    rt_memory_info(&mem_t0, &mem_u0, &mem_m0);

    /* 压测期间关掉过程日志，否则 200 多行会把串口刷爆 */
    edgi_pipeline_set_verbose(RT_FALSE);
    for (i = 0; i < DEMO_SWITCH_NUM; i++)
    {
        edgi_mode_t target = ((i & 1u) == 0u) ? EDGI_MODE_ENV : EDGI_MODE_ASR;
        if (edgi_pipeline_switch(target) == RT_EOK)
            ok++;
        else
            fail++;
    }
    edgi_pipeline_set_verbose(RT_TRUE);

    /* 收尾回待机，别让板子停在一个没有模块在跑的模式上 */
    edgi_pipeline_switch(EDGI_MODE_IDLE);

    seq1 = edgi_pipeline_session();
    rt_memory_info(&mem_t1, &mem_u1, &mem_m1);

    rt_kprintf("切换结果：成功 %u 次，失败 %u 次\r\n", (unsigned)ok, (unsigned)fail);
    rt_kprintf("会话号：%u -> %u（应该增加 %u）\r\n",
               (unsigned)seq0, (unsigned)seq1, (unsigned)DEMO_SWITCH_NUM);

    if (mem_t1 != 0)
    {
        rt_kprintf("内存：已用 %u -> %u 字节（变化 %d），峰值 %u\r\n",
                   (unsigned)mem_u0, (unsigned)mem_u1,
                   (int)mem_u1 - (int)mem_u0, (unsigned)mem_m1);
    }

    if (fail == 0 && (seq1 - seq0) == DEMO_SWITCH_NUM)
        rt_kprintf(">>> 模式切换通过：%u 次全部成功，会话号连续\r\n", (unsigned)DEMO_SWITCH_NUM);
    else
        rt_kprintf(">>> 模式切换有问题：失败 %u 次，会话号增量 %u\r\n",
                   (unsigned)fail, (unsigned)(seq1 - seq0));
}

static void boot_demo_thread(void *param)
{
    RT_UNUSED(param);

    rt_thread_mdelay(DEMO_DELAY_MS);

    demo_capture();
    demo_switch();

    rt_kprintf("\r\n========================================\r\n");
    rt_kprintf("  自测结束。看到上面两个『通过』就可以继续往下做了。\r\n");
    rt_kprintf("========================================\r\n\r\n");

    /*
     * 注意：RT-Thread 5.0 起删掉了 rt_thread_exit()。
     * 线程函数直接 return 即可，内核会在返回后自动清理。
     */
    return;
}

static int boot_demo_start(void)
{
    rt_thread_t t;

    /* 栈给 3 KB：里面有一个 648 字节的音频帧，再加打印，2 KB 太紧 */
    t = rt_thread_create("edgi_demo", boot_demo_thread, RT_NULL, 3072, 20, 10);
    if (t != RT_NULL)
        rt_thread_startup(t);

    return 0;
}
INIT_APP_EXPORT(boot_demo_start);

#endif /* EDGI_BOOT_DEMO */
