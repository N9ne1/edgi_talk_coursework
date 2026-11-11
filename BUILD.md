# 构建与对接说明（5 号维护）

> 环境声音提醒与实时字幕系统 · Edgi-Talk / PSoC Edge E84
> 这份文档解决两件事：**怎么把工程跑起来**，**你的模块怎么接进来**。

---

## 1. 拿到仓库后先做三步

### 1.1 补齐两个不随仓库下载的目录

`.gitignore` 屏蔽了两个体积很大的第三方目录，**必须自己补**，否则编译直接失败：

| 目录 | 体积 | 缺了会怎样 | 怎么补 |
|---|---|---|---|
| `libraries/` | 265 MB | 编译报 `cybsp.h`、`cy_pdl.h` 找不到 | 见下 |
| `tools/` | 162 MB | 打不开图形化配置、烧不了 Secure 固件 | 见下 |

**最快的方式**：用 RT-Thread Studio 新建一个 `Edgi_Talk_M33_Template` 工程（板卡型号选 `PSE84`），
把它生成好的 `libraries/` 和 `tools/` 整个复制过来。这两个目录是平台自带的，不含我们任何代码。

> `libraries/HAL_Drivers/` **已经在仓库里了**，不要再覆盖它。
> 里面 `drv_pdm.c` 注册了 `mic0` 麦克风设备，是我们采音的基础，我专门放行的。

### 1.2 确认目录联接

工程根目录的 `mklinks.bat` 用 `mklink /J` 把外部目录接进来。
**用 RT-Thread Studio 直接导入工程时不需要跑它**；如果手工从命令行编译才需要：

```bat
mklinks.bat
```

### 1.3 导入并编译

1. RT-Thread Studio → 文件 → 导入 → 现有项目到工作空间，选中本目录
2. 右键工程 → 构建
3. 编译通过后，产物是 `rtthread.hex`（不进仓库，各自本地生成）

---

## 2. 连板子和看日志

| 用途 | 怎么连 |
|---|---|
| 烧录 / 调试 | 板载 DAP，一根 USB 线到电脑 |
| **看串口日志** | **必须另接 USB 转串口（CH340 之类）**，波特率 `115200` |

板载 DAP 的虚拟串口**不出 M33 的日志**，这是最容易踩的坑。接线见 `figures/m33_uart.png`：
板子 RX 接转接板 TX，板子 TX 接转接板 RX，GND 共地。

上电后应该看到：

```
[mic] 硬件参数：16000 Hz / 2 声道 / 16 位
[mic] 采音就绪：每帧 320 点 / 20 ms，每次读 1280 字节原始数据
[stub] 四个模块已装配：env / asr / notify / ui
```

**注意**：如果看到的是 `[mic] 没找到 mic0，改用合成波形`，说明 `libraries/` 没补齐或
`BSP_USING_AUDIO` 没开。合成波形照样能跑通全流程，但它是假数据，不是真麦克风。

---

## 3. 开机自测（不用敲命令）

工程默认在**上电 3 秒后自动跑一遍自测**，不需要命令行：

```
第 1 段：采音 5 秒     -> 打印帧号、能量、帧间隔
第 2 段：模式切换 100 次 -> 打印成功次数、会话号增量、内存变化
```

看到两行 `>>> ... 通过` 就说明采音和模式切换都正常。

- 想关掉：`applications/m33/src/edgi_boot_demo.c` 里把 `EDGI_BOOT_DEMO` 改成 `0`
- 命令行也开了（`msh`），可以敲 `mic_test 10` 单独测采音 10 秒、`edgi_mode env|asr|idle` 切模式

---

## 4. 你的模块怎么接进来

**核心机制：弱符号替换。** 你在自己的 `.c` 里写一个**同名同参数**的函数，
**不要加 `EDGI_WEAK`、不要写 `static`**，链接器会自动用你的版本替换空实现。
不用改 `SConscript`，不用改配置文件，不用告诉我。

文件放 `applications/m33/src/`，头文件放 `applications/m33/include/`，编译时自动收进来。

### 4.1 各人要覆盖的函数

| 谁 | 在 `edgi_modules_stub.c` 里被替换的函数 | 说明 |
|---|---|---|
| **1 号** | 界面线程 | 现在是把事件打到串口。改成 LVGL 更新标签即可，**事件格式不要改** |
| **2 号** | `edgi_env_start` / `edgi_env_stop` / `edgi_env_flush` | 起线程、跑模型、判阈值，结果调 `edgi_pipeline_publish_alarm()` |
| **3 号** | `edgi_notify_alarm` | 收告警、发飞书。**这个函数跑在采音线程里**，别直接发网络请求，丢队列给通知线程 |
| **4 号** | `edgi_asr_start` / `edgi_asr_stop` / `edgi_asr_flush` | 建 WebSocket、发音频、收回文字，结果调 `edgi_pipeline_publish_asr()` |

### 4.2 拿音频只有一条路

```c
#include "edgi_audio_capture.h"

edgi_audio_frame_t frame;

edgi_audio_capture_init();                      /* 只需一次 */

while (running)
{
    if (edgi_audio_capture_read(&frame) == RT_EOK)
    {
        /* frame.seq       帧号，从 1 连续递增，跳号=丢帧 */
        /* frame.timestamp 取到时刻（tick） */
        /* frame.pcm[320]  16 kHz 单声道 int16，正好 20 ms */
    }
}
```

**谁都不许自己去找 `mic0`。** 以后换麦克风、改采样率只改我这一个文件。

- 一帧 = 320 点 = 640 字节 = 20 ms
- 1 秒 = 50 帧 ≈ 32 kB
- 读满 320 点才返回，最坏阻塞 2 秒后返回 `-RT_ETIMEOUT`（别在拿它当定时器用）

### 4.3 三个公共头文件

| 文件 | 内容 |
|---|---|
| `edgi_audio_capture.h` | 音频帧结构、采音接口、暂停开关 |
| `edgi_events.h` | 九类声音枚举、告警事件、字幕事件、统一事件、界面提示语表 |
| `edgi_pipeline.h` | 模式枚举、模块三函数原型、切换接口、两个 publish 接口、会话号 |

**关键约定：`alarm_type_t` 的顺序不能动。**
它的枚举值就是模型输出向量的下标，也是报告 5.3 表格的顺序，也是 2 号在 DEEPCRAFT Studio
里加数据标签的顺序。**改一行位置，全部识别结果错位。**

```
0 car_horn   1 engine   2 siren   3 door_wood_knock   4 crying_baby
5 glass_breaking   6 clock_alarm   7 dog   8 other
```

`ALARM_OTHER`（其他声音）不提醒、不上屏、不发飞书，`edgi_alarm_display_text()` 对它返回 `NULL`。

### 4.4 会话号为什么重要

每次切模式 `session` 都 +1。你发字幕时必须带上 `edgi_pipeline_session()` 拿到的值：

```c
ev.session_id = edgi_pipeline_session();
```

pipeline 会把 `session_id` 对不上的字幕直接丢掉。
**这是防止「切走了，上一次网络请求的结果迟到，串到新模式屏幕上」。** 别省这一步。

---

## 5. 交给我合并时请一并给

按分工约定，每个人提交时请附：

1. **运行入口**：哪个函数被调用、什么时候启动（`INIT_APP_EXPORT`？还是等 pipeline 调 `start`？）
2. **新增文件清单**：哪些 `.c` / `.h`，放哪个目录
3. **依赖**：要不要加 RT-Thread 软件包、要不要改 `rtconfig.h`、要不要加静态库（`applications/m33/libs/*.a` 会自动链接）
4. **一份你已跑过的测试记录**：测了什么、结果如何、串口日志截图

业务问题由原负责人自己改，我不动你们的逻辑；我只检查**模块连接、线程和启动顺序**。

---

## 6. 当前基线状态

| 项 | 状态 |
|---|---|
| 采音 `mic0`（16 kHz / 2ch / 16bit，20 ms 一帧下混成单声道） | 已通 |
| 模式切换（停旧 → 清缓冲 → 会话号+1 → 起新 → 失败回滚） | 已通 |
| 四个模块空实现 + 事件队列 + 串口 UI | 已通，全流程能跑 |
| Wi-Fi / 飞书 / 模型 / 云端字幕 | 空实现，等 1/2/3/4 号接管 |
| 系统时间、TLS 基础配置 | 待验证（1 号、3 号、4 号各自验证目标服务证书） |

---

## 7. 常见问题

**Q：编译报 `fatal error: cybsp.h: No such file or directory`**
`libraries/` 没补齐，见 1.1。

**Q：串口全是乱码**
两种可能：① 波特率不是 115200；② 接了 DAP 的虚拟串口而不是外接 USB 转串口。
另外确认 `board.c` 里 `SOC_Enable_CM55` 是关的（当前就是关的），否则 M55 会往同一个串口打日志。

**Q：`[mic] 没找到 mic0`**
`libraries/HAL_Drivers/drv_pdm.c` 没参与编译，或 `rtconfig.h` 里 `BSP_USING_AUDIO` 没开。

**Q：我写的函数没生效，还是走空实现**
检查三点：名字一模一样、参数一模一样、没加 `static`、没加 `EDGI_WEAK`。

**Q：改了 `design.modus` 后编译报一堆 `cycfg_*` 错**
用 Device Configurator 重新生成代码，`libs/TARGET_APP_KIT_PSE84_EVAL_EPC2/config/GeneratedSource/` 会更新，
这些生成文件在仓库里，记得一起提交。
