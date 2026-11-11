/*
 * 各模块的"空实现"（5 号提供，报告 5 号职责第 2 条）
 * ============================================================
 * 目的：让 1/2/3/4 号在没有真代码之前，工程就能编译、能跑、能看到日志。
 *
 * 用法（很重要，只有一句话）：
 *   谁想替换哪个模块，就在自己的 .c 里写一个【同名、同参数】的函数，
 *   不要加 EDGI_WEAK。链接器会自动用你的版本替换这里的版本，
 *   不需要改任何配置、不需要改这个文件。
 *
 *   例：2 号想接管环境音识别，就在自己的文件里写
 *       rt_err_t edgi_env_start(void) { ... }
 *   这样就行了。名字必须一模一样，不能写成 static。
 *
 * 四个可替换的模块：
 *   2 号 -> edgi_env_start / edgi_env_stop / edgi_env_flush
 *   4 号 -> edgi_asr_start / edgi_asr_stop / edgi_asr_flush
 *   3 号 -> edgi_notify_alarm
 *   1 号 -> edgi_ui_push（或直接自己收事件队列）
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>

#include "edgi_pipeline.h"
#include "edgi_events.h"
#include "edgi_audio_capture.h"

#define STUB_STACK      4096
#define STUB_PRIO       20

/*
 * 弱符号：别的文件里写了同名函数（不加这个属性），链接时会用他们的，
 * 这里的实现自动作废。这就是"2 号接管后不用改任何配置"的原理。
 * 本工程的 RT-Thread 版本没有 RT_WEAK 宏，所以直接写 GCC 属性。
 */
#if defined(__GNUC__)
#define EDGI_WEAK   __attribute__((weak))
#else
#define EDGI_WEAK
#endif

/* 事件队列：8 条够用（界面消费慢的时候不丢太多） */
#define UI_MQ_DEPTH     8

/*
 * 线程退出等待的上限（毫秒）。
 *
 * 为什么必须给到 2500ms：edgi_audio_capture_read() 在麦克风没数据时最坏会
 * 自旋 2000 次、每次 mdelay(1)，也就是最多阻塞 2 秒才返回。
 * 如果只等 30ms 就去 rt_thread_delete()，删掉的是一个"还在跑"的线程：
 * 它随后仍会执行，占着的内存一旦被下一次 rt_thread_create() 复用，
 * 线程对象的 type 字段就被改写，下一次 delete 时断言
 * (rt_object_get_type(thread) == RT_Object_Class_Thread) 就会失败。
 * 这就是 16:16 那屏 "assert failed, rt_thread_delete, line 538" 的成因。
 */
#define STUB_STOP_WAIT_MS   2500u

static struct rt_messagequeue s_ui_mq;
static rt_uint8_t s_ui_mq_pool[UI_MQ_DEPTH * (sizeof(edgi_event_t) + sizeof(void *))];

/*
 * 通用的"安全停线程"流程：
 *
 *   run_flag = FALSE   ->  等 done_flag  ->  rt_thread_delete  ->  tid = NULL
 *
 * 三个要点：
 * 1) 必须先等线程自己走出主循环（done_flag 由线程末尾置位），
 *    绝不能对着一个还在跑的线程直接 delete。
 * 2) 线程被 delete 后进入 defunct 链表，由空闲线程真正释放；
 *    只要 tid 立刻置 NULL，就不会有第二次 delete 落到旧对象上。
 * 3) 采音 read 最坏阻塞 2 秒，所以等待上限给到 2500ms；
 *    但正常情况下一帧 20ms 就返回，实际等待只有几十毫秒。
 */
static void stub_wait_thread(rt_thread_t *ptid, volatile rt_bool_t *run,
                             volatile rt_bool_t *done)
{
    rt_uint32_t waited;

    if (*ptid == RT_NULL)
        return;

    *run = RT_FALSE;            /* 第 1 步：请求退出 */

    waited = 0;
    while ((*done != RT_TRUE) && (waited < STUB_STOP_WAIT_MS))
    {
        rt_thread_mdelay(5);
        waited += 5u;
    }

    /* 第 2 步：回收。线程若已正常退出，这里是"僵尸态回收"；
     * 若等超时，也照样删并清空指针，绝不留悬空指针给下一次用 */
    rt_thread_delete(*ptid);

    *ptid = RT_NULL;
    *done = RT_FALSE;           /* 第 3 步：复位，供下一次 start 使用 */
}

/* ============================================================
 * 2 号：环境音识别模块
 * ============================================================ */

static rt_thread_t        s_env_tid;
static volatile rt_bool_t s_env_run;
static volatile rt_bool_t s_env_done;   /* 线程真正退出后由线程自己置位 */

/*
 * 空实现的行为：
 *   每帧读一次音频（20 ms），每 250 帧（约 5 秒）假报一个告警，
 *   类别按 0..8 轮流走一遍，方便 3 号测飞书、1 号测页面。
 * 2 号接管后，把这里换成"预处理 -> 模型推理 -> 阈值判断"即可。
 */
static void edgi_env_thread(void *arg)
{
    edgi_audio_frame_t frame;
    rt_uint32_t        n = 0;
    rt_uint8_t         t = 0;

    (void)arg;  /* 本模块不用传参 */

    while (s_env_run)
    {
        if (edgi_audio_capture_read(&frame) != RT_EOK)
        {
            rt_thread_mdelay(5);
            continue;
        }

        n++;

        /* 每 250 帧（5 秒）演示一次完整的事件链路 */
        if ((n % 250) == 0)
        {
            alarm_event_t ev;

            ev.type       = t;
            ev.confidence = 85;
            ev.reserved   = 0;
            ev.timestamp  = frame.timestamp;

            /* 这两个调用就是"环境的输出"，2 号接管后也照样这么调 */
            edgi_pipeline_publish_alarm(&ev);   /* 送给界面 */
            edgi_notify_alarm(&ev);             /* 送给 3 号的飞书 */

            t = (rt_uint8_t)((t + 1) % ALARM_TYPE_MAX);
        }
    }

    /* 线程走到这里说明循环已退出，通知 stop() 可以安全回收了 */
    s_env_done = RT_TRUE;
}

EDGI_WEAK rt_err_t edgi_env_start(void)
{
    if (s_env_tid != RT_NULL)
        return RT_EOK;

    s_env_done = RT_FALSE;
    s_env_run  = RT_TRUE;
    s_env_tid = rt_thread_create("edgi_env", edgi_env_thread, RT_NULL,
                                 STUB_STACK, STUB_PRIO, 10);
    if (s_env_tid == RT_NULL)
    {
        s_env_run = RT_FALSE;
        rt_kprintf("[stub] 环境音线程创建失败（内存不够？）\n");
        return -RT_ENOMEM;
    }

    rt_thread_startup(s_env_tid);
    rt_kprintf("[stub] 环境音模块：空实现已启动（2 号接管后自动替换）\n");
    return RT_EOK;
}

EDGI_WEAK void edgi_env_stop(void)
{
    stub_wait_thread(&s_env_tid, &s_env_run, &s_env_done);
}

EDGI_WEAK void edgi_env_flush(void)
{
    /* 空实现没有缓冲要清。2 号如果有滑窗/未上报结果，在这里清掉 */
}

/* ============================================================
 * 4 号：实时字幕模块
 * ============================================================ */

static rt_thread_t        s_asr_tid;
static volatile rt_bool_t s_asr_run;
static volatile rt_bool_t s_asr_done;   /* 线程真正退出后由线程自己置位 */

/*
 * 空实现的行为：每 2 秒发一条"临时"字幕，再隔 1 秒发同一条"最终"字幕，
 * 用来说清 5.5 要求的效果：临时结果逐次替换，最终结果才保留。
 */
static void edgi_asr_thread(void *arg)
{
    static const char *demo[] = { "今天", "今天天气", "今天天气不错" };
    edgi_audio_frame_t frame;
    asr_event_t        ev;
    rt_uint32_t        step = 0;

    (void)arg;  /* 本模块不用传参 */
    rt_memset(&ev, 0, sizeof(ev));

    while (s_asr_run)
    {
        if (edgi_audio_capture_read(&frame) != RT_EOK)
        {
            rt_thread_mdelay(5);
            continue;
        }

        /* 每 100 帧（2 秒）推进一步 */
        if ((frame.seq % 100) != 0)
            continue;

        ev.session_id = edgi_pipeline_session();   /* 会话号必须带上 */
        ev.is_final   = (step >= 2) ? 1u : 0u;
        ev.text_len   = (uint8_t)rt_strlen(demo[step > 2 ? 2 : step]);
        rt_strncpy(ev.text, demo[step > 2 ? 2 : step], EDGI_ASR_TEXT_MAX - 1);
        ev.text[EDGI_ASR_TEXT_MAX - 1] = '\0';

        edgi_pipeline_publish_asr(&ev);

        if (step < 2)
            step++;
        else
            step = 0;
    }

    /* 线程走到这里说明循环已退出，通知 stop() 可以安全回收了 */
    s_asr_done = RT_TRUE;
}

EDGI_WEAK rt_err_t edgi_asr_start(void)
{
    if (s_asr_tid != RT_NULL)
        return RT_EOK;

    s_asr_done = RT_FALSE;
    s_asr_run  = RT_TRUE;
    s_asr_tid = rt_thread_create("edgi_asr", edgi_asr_thread, RT_NULL,
                                 STUB_STACK, STUB_PRIO, 10);
    if (s_asr_tid == RT_NULL)
    {
        s_asr_run = RT_FALSE;
        rt_kprintf("[stub] 字幕线程创建失败（内存不够？）\n");
        return -RT_ENOMEM;
    }

    rt_thread_startup(s_asr_tid);
    rt_kprintf("[stub] 字幕模块：空实现已启动（4 号接管后自动替换）\n");
    return RT_EOK;
}

EDGI_WEAK void edgi_asr_stop(void)
{
    stub_wait_thread(&s_asr_tid, &s_asr_run, &s_asr_done);
}

EDGI_WEAK void edgi_asr_flush(void)
{
    /* 空实现没有缓冲要清。4 号在这里断开 WebSocket 会话、清未确认的临时句 */
}

/* ============================================================
 * 3 号：飞书通知
 * ============================================================ */

/*
 * 环境音模块识别到告警后调这个。3 号接管后在这里组 HTTPS 请求发飞书。
 * 注意：本函数跑在采音线程里，发网络请求会阻塞采音，3 号应该把
 *       "类别 + 时间"丢进自己的队列，由通知线程去发。
 */
EDGI_WEAK void edgi_notify_alarm(const alarm_event_t *ev)
{
    const char *text;

    if (ev == RT_NULL)
        return;

    text = edgi_alarm_display_text((alarm_type_t)ev->type);
    if (text == NULL)
        return;     /* "其他声音"不提醒 */

    rt_kprintf("[stub][notify] 应该发飞书：%s（置信度 %u%%）\n",
               text, (unsigned)ev->confidence);
}

/* ============================================================
 * 1 号：界面
 * ============================================================ */

/*
 * 空实现的行为：起一个线程收事件队列，把要显示的内容打到串口。
 * 1 号接管后，把 rt_kprintf 换成 LVGL 的标签更新即可，
 * 不要改事件格式。
 */
static void edgi_ui_thread(void *arg)
{
    edgi_event_t ev;

    (void)arg;  /* 本模块不用传参 */

    while (1)
    {
        if (rt_mq_recv(&s_ui_mq, &ev, sizeof(ev), RT_WAITING_FOREVER) != RT_EOK)
            continue;

        switch (ev.kind)
        {
        case EDGI_EVT_ALARM:
        {
            const char *text = edgi_alarm_display_text((alarm_type_t)ev.u.alarm.type);
            if (text != NULL)
                rt_kprintf("[stub][UI] 告警：%s\n", text);
            break;
        }
        case EDGI_EVT_ASR:
            rt_kprintf("[stub][UI] 字幕%s：%s\n",
                       ev.u.asr.is_final ? "（最终）" : "（临时）",
                       ev.u.asr.text);
            break;
        default:
            break;
        }
    }
}

/* ============================================================
 * 装配：把上面四个模块挂到 pipeline 上
 * ============================================================ */

static int edgi_modules_init(void)
{
    static edgi_module_t env_mod;
    static edgi_module_t asr_mod;

    /* 1. 事件队列（静态池，不吃堆内存） */
    rt_mq_init(&s_ui_mq, "edgi_ev", s_ui_mq_pool, sizeof(edgi_event_t),
               sizeof(s_ui_mq_pool), RT_IPC_FLAG_FIFO);
    edgi_pipeline_bind_event_mq(&s_ui_mq);

    /* 2. 绑定两个模式模块 —— 2/4 号不用自己绑，写同名函数就行 */
    env_mod.start = edgi_env_start;
    env_mod.stop  = edgi_env_stop;
    env_mod.flush = edgi_env_flush;
    edgi_pipeline_bind_env(env_mod);

    asr_mod.start = edgi_asr_start;
    asr_mod.stop  = edgi_asr_stop;
    asr_mod.flush = edgi_asr_flush;
    edgi_pipeline_bind_asr(asr_mod);

    /* 3. 界面线程 */
    rt_thread_t tid = rt_thread_create("edgi_ui", edgi_ui_thread, RT_NULL,
                                       STUB_STACK, STUB_PRIO + 1, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);

    rt_kprintf("[stub] 四个模块已装配：env / asr / notify / ui\n");
    return 0;
}
INIT_APP_EXPORT(edgi_modules_init);
