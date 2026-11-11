#ifndef EDGI_PIPELINE_H
#define EDGI_PIPELINE_H

/*
 * 模式切换与总线（5 号负责）
 * ------------------------------------------------------------
 * 三件事：
 *   1. 记住现在处于哪个模式（待机 / 环境音 / 字幕）
 *   2. 切模式时按正确顺序停旧的、起新的
 *   3. 给 2 号 4 号一个"把结果丢出去"的口子（publish）
 */

#include <rtthread.h>
#include <stdint.h>
#include "edgi_events.h"

typedef enum
{
    EDGI_MODE_IDLE = 0,     /* 待机 */
    EDGI_MODE_ENV  = 1,     /* 环境音识别 */
    EDGI_MODE_ASR  = 2      /* 实时字幕 */
} edgi_mode_t;

/* 各模块提供三个函数：起来 / 停下 / 清空 */
typedef rt_err_t (*edgi_start_fn)(void);
typedef void     (*edgi_stop_fn)(void);
typedef void     (*edgi_flush_fn)(void);

typedef struct
{
    edgi_start_fn start;
    edgi_stop_fn  stop;
    edgi_flush_fn flush;    /* 清空本模块残留的音频和待显示结果，可以填 RT_NULL */
} edgi_module_t;

/* ============================================================
 * 2 号 / 4 号要实现的那 6 个函数（弱符号空实现在 edgi_modules_stub.c）
 * ------------------------------------------------------------
 * 接管方法：在自己的 .c 里写同名函数，不要加 EDGI_WEAK，
 * 链接器会自动用你的版本替换空实现，不用改任何配置。
 *
 * 为什么不在这里直接做成 edgi_module_t？因为 2 号和 4 号是「各写一个函数」
 * 就完成接管，不需要知道 pipeline 内部怎么排队的。
 * ============================================================ */

/* ---- 2 号：环境音识别 ---- */
/* 起线程/开模型，成功返回 RT_EOK */
rt_err_t edgi_env_start(void);
/* 请求停止并回收线程，必须阻塞到线程真正退出才返回 */
void     edgi_env_stop(void);
/* 清空滑窗、未上报的候选结果（可以什么都不做） */
void     edgi_env_flush(void);

/* ---- 4 号：云端字幕 ---- */
rt_err_t edgi_asr_start(void);
void     edgi_asr_stop(void);
void     edgi_asr_flush(void);

/* 2 号在自己的代码里调这个，把"环境音模块"挂进来 */
void edgi_pipeline_bind_env(edgi_module_t mod);

/* 4 号在自己的代码里调这个，把"字幕模块"挂进来 */
void edgi_pipeline_bind_asr(edgi_module_t mod);

/* 界面收到消息队列，事件会往这里投 */
void edgi_pipeline_bind_event_mq(rt_mq_t mq);

/* 切模式。返回 RT_EOK 表示切成功，其它值表示启动失败已回滚到 IDLE */
rt_err_t edgi_pipeline_switch(edgi_mode_t target);

/* 压测时关掉过程日志，只留结果。默认是开着的 */
void edgi_pipeline_set_verbose(rt_bool_t on);

edgi_mode_t edgi_pipeline_mode(void);
uint32_t    edgi_pipeline_session(void);    /* 当前会话号 */

/* 2 号识别完调这个 */
void edgi_pipeline_publish_alarm(const alarm_event_t *ev);

/* 4 号拿到字幕调这个 */
void edgi_pipeline_publish_asr(const asr_event_t *ev);

#endif /* EDGI_PIPELINE_H */
