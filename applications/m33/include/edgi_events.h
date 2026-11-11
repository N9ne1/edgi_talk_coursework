#ifndef EDGI_EVENTS_H
#define EDGI_EVENTS_H

/*
 * 全组共用的"事件"格式（5 号负责定，定完不要随便改）
 * ------------------------------------------------------------
 * 想象成一张统一格式的便签：
 *   2 号识别出"敲门"  -> 填一张告警便签 -> 丢给界面
 *   4 号识别出一句字幕 -> 填一张字幕便签 -> 丢给界面
 * 界面只管按便签上的内容显示，不用管是谁算出来的。
 */

#include <stdint.h>
#include <stddef.h>

/* ---------- 环境音告警 ---------- */

/*
 * 九个类别，顺序 = 报告 5.3 表格的顺序，也 = 2 号在 DEEPCRAFT Studio
 * 里添加数据标签的顺序。改任何一行的位置，都会让全部识别结果错位，
 * 所以这个表定了就不要再动（要动必须 5 号和 2 号一起改）。
 *
 * 枚举值就是模型输出向量的下标。
 */
typedef enum
{
    ALARM_CAR_HORN    = 0,   /* car_horn         车辆鸣笛   */
    ALARM_ENGINE      = 1,   /* engine           发动机声   */
    ALARM_SIREN       = 2,   /* siren            警笛       */
    ALARM_DOOR_KNOCK  = 3,   /* door_wood_knock  敲门声     */
    ALARM_CRYING_BABY = 4,   /* crying_baby      婴儿哭声   */
    ALARM_GLASS_BREAK = 5,   /* glass_breaking   玻璃破碎   */
    ALARM_CLOCK_ALARM = 6,   /* clock_alarm      闹钟       */
    ALARM_DOG         = 7,   /* dog              狗叫       */
    ALARM_OTHER       = 8,   /* other            其他声音   */
    ALARM_TYPE_MAX    = 9
} alarm_type_t;

/*
 * 界面提示语。索引就是 alarm_type_t，"其他声音"返回 NULL（不提醒、
 * 不上屏、不发飞书）。1 号画页面时直接用它，不要再自己写一份字符串表。
 */
const char *edgi_alarm_display_text(alarm_type_t t);

/* 一条识别结果，正好 8 字节 */
typedef struct
{
    uint8_t  type;          /* alarm_type_t */
    uint8_t  confidence;    /* 置信度 0-100 */
    uint16_t reserved;      /* 填 0，凑齐 8 字节让内存对齐 */
    uint32_t timestamp;
} alarm_event_t;

/* ---------- 字幕 ---------- */

#define EDGI_ASR_TEXT_MAX   96      /* 一句字幕最多 95 个 UTF-8 字节（约 31 个汉字） */

typedef struct
{
    uint8_t  is_final;      /* 0=临时结果（会被下一句覆盖） 1=最终句（保留） */
    uint8_t  text_len;      /* 有效字节数，不含结尾 \0 */
    uint16_t reserved;
    uint32_t session_id;    /* 会话号，模式切换会 +1，用来丢弃迟到的旧消息 */
    char     text[EDGI_ASR_TEXT_MAX];
} asr_event_t;

/* ---------- 界面收到的统一事件 ---------- */

typedef enum
{
    EDGI_EVT_ALARM = 1,
    EDGI_EVT_ASR   = 2,
    EDGI_EVT_MODE  = 3      /* u.mode 里是 edgi_mode_t */
} edgi_event_kind_t;

typedef struct
{
    edgi_event_kind_t kind;
    union
    {
        alarm_event_t alarm;
        asr_event_t   asr;
        uint8_t       mode;
    } u;
} edgi_event_t;

/* ---------- 3 号：飞书通知入口 ---------- */

/*
 * 环境音模块识别到告警后调用这个函数。3 号接管后在 edgi_notify_alarm 里
 * 组 HTTPS 请求发飞书（注意别在采音线程里直接阻塞发网络，丢队列里发）。
 * 空实现见 edgi_modules_stub.c，会打印一行"应该发飞书：xxx"。
 */
void edgi_notify_alarm(const alarm_event_t *ev);

#endif /* EDGI_EVENTS_H */
