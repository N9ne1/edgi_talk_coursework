/*
 * 公共事件表（5 号负责）
 * ------------------------------------------------------------
 * 这里只放"所有模块都要用"的东西：类别 -> 界面提示语。
 * 提示语表放在 .c 里而不是头文件里，避免每个 .c 包含时都复制一份。
 */

#include "edgi_events.h"

static const char *const s_alarm_text[ALARM_TYPE_MAX] =
{
    "检测到车辆鸣笛",       /* ALARM_CAR_HORN    */
    "检测到发动机声",       /* ALARM_ENGINE      */
    "检测到警笛",           /* ALARM_SIREN       */
    "检测到敲门声",         /* ALARM_DOOR_KNOCK  */
    "检测到婴儿哭声",       /* ALARM_CRYING_BABY */
    "检测到玻璃破碎声",     /* ALARM_GLASS_BREAK */
    "检测到闹钟声",         /* ALARM_CLOCK_ALARM */
    "检测到狗叫声",         /* ALARM_DOG         */
    NULL                    /* ALARM_OTHER：不提醒 */
};

const char *edgi_alarm_display_text(alarm_type_t t)
{
    if ((int)t < 0 || (int)t >= ALARM_TYPE_MAX)
        return NULL;

    return s_alarm_text[t];
}
