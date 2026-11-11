/*
 * 模式切换实现（5 号负责）
 * ------------------------------------------------------------
 * 切换顺序不能错：
 *     停旧的 -> 清空残留数据 -> 会话号 +1 -> 起新的 -> 失败就回滚
 * 会话号的作用：切走之后，上一次网络请求迟到的字幕会被自动丢掉，
 * 不会串到新模式的屏幕上。
 */

#include "edgi_pipeline.h"
#include <string.h>

static edgi_module_t s_env;             /* 环境音模块，2 号绑定 */
static edgi_module_t s_asr;             /* 字幕模块，4 号绑定 */
static rt_mq_t       s_evt_mq = RT_NULL;
static edgi_mode_t   s_mode   = EDGI_MODE_IDLE;
static uint32_t      s_session = 0;
static rt_bool_t     s_verbose = RT_TRUE;

/* 压测时不打印过程，只留结果，否则 200 行日志会把串口冲爆 */
#define EDGI_LOG(fmt, ...)                    \
    do {                                      \
        if (s_verbose)                        \
            rt_kprintf(fmt, ##__VA_ARGS__);   \
    } while (0)

void edgi_pipeline_set_verbose(rt_bool_t on)
{
    s_verbose = on;
}

void edgi_pipeline_bind_env(edgi_module_t mod)
{
    s_env = mod;
    EDGI_LOG("[pipe] 环境音模块已绑定：%s\n", mod.start ? "是" : "否");
}

void edgi_pipeline_bind_asr(edgi_module_t mod)
{
    s_asr = mod;
    EDGI_LOG("[pipe] 字幕模块已绑定：%s\n", mod.start ? "是" : "否");
}

void edgi_pipeline_bind_event_mq(rt_mq_t mq)
{
    s_evt_mq = mq;
}

edgi_mode_t edgi_pipeline_mode(void)
{
    return s_mode;
}

uint32_t edgi_pipeline_session(void)
{
    return s_session;
}

static const char *mode_name(edgi_mode_t m)
{
    switch (m)
    {
    case EDGI_MODE_ENV: return "环境音";
    case EDGI_MODE_ASR: return "字幕";
    default:            return "待机";
    }
}

static void stop_old(edgi_mode_t m)
{
    if (m == EDGI_MODE_ENV && s_env.stop != RT_NULL)
        s_env.stop();
    else if (m == EDGI_MODE_ASR && s_asr.stop != RT_NULL)
        s_asr.stop();
}

/*
 * 清空上位模块自己的缓冲。停掉任务之后、起新任务之前必须做这一步，
 * 否则上一段采到的声音 / 上一句没显示完的字幕会漏到新模式的屏幕上。
 */
static void flush_old(edgi_mode_t m)
{
    if (m == EDGI_MODE_ENV && s_env.flush != RT_NULL)
        s_env.flush();
    else if (m == EDGI_MODE_ASR && s_asr.flush != RT_NULL)
        s_asr.flush();
}

/* 把事件队列里积压的旧消息全部倒掉，一条都不留给新模式 */
static void drain_event_mq(void)
{
    edgi_event_t tmp;

    if (s_evt_mq == RT_NULL)
        return;

    while (rt_mq_recv(s_evt_mq, &tmp, sizeof(tmp), 0) == RT_EOK)
    {
        /* 拿到就丢 */
    }
}

rt_err_t edgi_pipeline_switch(edgi_mode_t target)
{
    rt_err_t e = RT_EOK;

    if (target == s_mode)
        return RT_EOK;

    EDGI_LOG("[pipe] %s -> %s\n", mode_name(s_mode), mode_name(target));

    /* 第 1 步：停掉旧任务 */
    stop_old(s_mode);

    /* 第 2 步：清空旧数据 —— 模块自己的缓冲 + 公共事件队列 */
    flush_old(s_mode);
    drain_event_mq();

    /* 第 3 步：会话号 +1，已经在路上、还没到达的旧消息从此作废 */
    s_session++;

    /* 第 4 步：起新任务 */
    if (target == EDGI_MODE_ENV)
    {
        if (s_env.start != RT_NULL)
            e = s_env.start();
        else
            EDGI_LOG("[pipe] 提醒：环境音模块还没绑定，先当演示模式跑\n");
    }
    else if (target == EDGI_MODE_ASR)
    {
        if (s_asr.start != RT_NULL)
            e = s_asr.start();
        else
            EDGI_LOG("[pipe] 提醒：字幕模块还没绑定，先当演示模式跑\n");
    }

    /* 第 5 步：起不来就回滚，绝不能停在"运行中" */
    if (e != RT_EOK)
    {
        EDGI_LOG("[pipe] 启动失败(%d)，回滚到待机\n", (int)e);
        stop_old(target);
        flush_old(target);
        s_mode = EDGI_MODE_IDLE;
        return e;
    }

    s_mode = target;
    EDGI_LOG("[pipe] 已进入 %s，会话号 = %u\n", mode_name(s_mode), (unsigned)s_session);
    return RT_EOK;
}

/* ---------------- 事件投递 ---------------- */

static void post(const edgi_event_t *ev)
{
    if (s_evt_mq != RT_NULL)
        rt_mq_send(s_evt_mq, ev, sizeof(*ev));
}

void edgi_pipeline_publish_alarm(const alarm_event_t *ev)
{
    edgi_event_t e;

    if (ev == RT_NULL)
        return;

    e.kind    = EDGI_EVT_ALARM;
    e.u.alarm = *ev;
    post(&e);
}

void edgi_pipeline_publish_asr(const asr_event_t *ev)
{
    edgi_event_t e;

    if (ev == RT_NULL)
        return;

    /* 迟到的旧会话消息直接丢，防止串屏 */
    if (ev->session_id != 0 && ev->session_id != s_session)
    {
        EDGI_LOG("[pipe] 丢弃过期字幕(会话 %u，当前 %u)\n",
                 (unsigned)ev->session_id, (unsigned)s_session);
        return;
    }

    e.kind  = EDGI_EVT_ASR;
    e.u.asr = *ev;
    post(&e);
}

/* ---------------- 串口调试命令 ---------------- */
#ifdef RT_USING_FINSH
#include <finsh.h>

static int edgi_mode(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("用法: edgi_mode env | asr | idle\n");
        rt_kprintf("当前: %s，会话号 %u\n", mode_name(s_mode), (unsigned)s_session);
        return 0;
    }

    if (rt_strcmp(argv[1], "env") == 0)
        edgi_pipeline_switch(EDGI_MODE_ENV);
    else if (rt_strcmp(argv[1], "asr") == 0)
        edgi_pipeline_switch(EDGI_MODE_ASR);
    else if (rt_strcmp(argv[1], "idle") == 0)
        edgi_pipeline_switch(EDGI_MODE_IDLE);
    else
        rt_kprintf("不认识 %s，只能填 env / asr / idle\n", argv[1]);

    return 0;
}
MSH_CMD_EXPORT(edgi_mode, 切换模式: edgi_mode env|asr|idle);
#endif
