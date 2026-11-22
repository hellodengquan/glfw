# Wayland 跨屏窗口的缓冲缩放：通知到达时可读状态、像素复算、以及输出消失后的重建契约

分析对象：GLFW 3.6 Wayland 后端（分支 `task/147`）。

涉及源码：

- `src/wl_monitor.c` —— `wl_output` 监听器、监视器侧 scale
- `src/wl_window.c` —— `wl_surface` 监听器、`wp_fractional_scale_v1` 监听器、缓冲尺寸推导
- `src/wl_platform.h` —— 每窗口/每监视器的缩放状态字段
- `src/wl_init.c` —— 全局对象绑定、`global_remove`
- `src/monitor.c` —— 监视器断开与释放
- `deps/wayland/wayland.xml`、`deps/wayland/fractional-scale-v1.xml` —— 协议语义

---

## 0. 结论速览

| 问题 | 答案 |
| --- | --- |
| 通知到达时可读的三组量 | 逻辑尺寸 `window->wl.width/height`；内容缩放 `bufferScale`（整数协议）或 `scalingNumerator/120`（分数协议）；帧缓冲尺寸 `window->wl.fbWidth/Height` |
| 像素如何复算 | 整数协议：`fb = logical * max(已进入输出集合的 factor)`；分数协议：`fb = round_half_away(logical * numerator / 120)`，且 `buffer_scale` 恒为 1 + viewport 目标矩形 = 逻辑尺寸 |
| 输出退出后哪个状态变化足以触发重建 | **窗口自身的"已进入输出集合"发生变化**（进而使复算出的 `fbWidth/fbHeight` 改变），而不是监视器断开回调。当前源码在 `global_remove` 时先销毁 `wl_output` 代理、却没有从 `outputScales` 中摘除，存在顺序依赖缺陷 |
| 源码保证 vs 合成器决定 | 源码保证：单个事件到达后同步重算、configure→resizeWindow→resizeFramebuffer 的固定次序。合成器决定：`global_remove` 与 `wl_surface.leave` 的相对次序、是否发送 leave、跨屏时那个**唯一的** preferred_scale 取值等 |

---

## 1. 状态变量与不变量

### 1.1 监视器侧（`src/wl_monitor.c`、`src/wl_platform.h:639`）

```c
struct _GLFWmonitor { ... int32_t scale; ... };   // wl_output.scale 的整数因子
```

- 创建时置 1：`monitor->wl.scale = 1;`（`wl_monitor.c:180`）。
- `wl_output.scale(factor)` → `outputHandleScale`（`wl_monitor.c:110`）：
  1. `monitor->wl.scale = factor;`
  2. 遍历 `_glfw.windowListHead`，把所有 `outputScales[i].output == monitor->wl.output` 的条目改成新 factor；
  3. 对每个匹配窗口调用 `_glfwUpdateBufferScaleFromOutputsWayland(window)`。
- `_glfwGetMonitorContentScaleWayland` 直接返回 `monitor->wl.scale`（`wl_monitor.c:207`）。

注意：**监视器的 scale 是每输出一个整数**，来源于协议 `wl_output.scale`（`wayland.xml` 注释："A scale larger than 1 means the compositor will automatically scale surface buffers by this amount"）。

### 1.2 窗口侧（`src/wl_platform.h:369-421`）

```c
int   width, height;        // 逻辑/表面尺寸（surface-local，未经缩放）
int   fbWidth, fbHeight;    // 帧缓冲尺寸（像素）
GLFWbool scaleFramebuffer;  // 对应 GLFW_SCALE_FRAMEBUFFER，默认 true

// 整数协议
int32_t          bufferScale;     // 送往 wl_surface.set_buffer_scale 的值
_GLFWscaleWayland* outputScales;  // 当前 surface 进入的输出集合 {output, factor}
size_t           outputScaleCount, outputScaleSize;

// 分数协议
struct wp_viewport*            scalingViewport;   // destination = 逻辑尺寸
uint32_t                       scalingNumerator;  // preferred_scale，分母恒 120
struct wp_fractional_scale_v1* fractionalScale;   // 非 NULL 即"分数模式"
```

初始化（`wl_window.c:1213-1215`）：

```c
window->wl.bufferScale      = 1;
window->wl.scalingNumerator = 120;   // 即 1.0
window->wl.scaleFramebuffer = wndconfig->scaleFramebuffer;
```

### 1.3 唯一的推导函数 `resizeFramebuffer`（`wl_window.c:477-503`）

```c
static void resizeFramebuffer(_GLFWwindow* window)
{
    if (window->wl.fractionalScale)            // 分数模式
    {
        // 规范要求 toplevel 按 halfway away from zero 舍入
        window->wl.fbWidth  = (window->wl.width  * window->wl.scalingNumerator + 60) / 120;
        window->wl.fbHeight = (window->wl.height * window->wl.scalingNumerator + 60) / 120;
    }
    else                                       // 整数模式
    {
        window->wl.fbWidth  = window->wl.width  * window->wl.bufferScale;
        window->wl.fbHeight = window->wl.height * window->wl.bufferScale;
    }

    if (window->wl.egl.window)
        wl_egl_window_resize(window->wl.egl.window,
                             window->wl.fbWidth, window->wl.fbHeight, 0, 0);
    if (!window->wl.transparent)
        setContentAreaOpaque(window);
    _glfwInputFramebufferSize(window, window->wl.fbWidth, window->wl.fbHeight);
}
```

`+60 / 120` 是整数形式的四舍五入到最近（half-up）：`floor((w*n + 60)/120)`，与规范
"the size is rounded halfway away from zero" 一致（对非负尺寸，half-up == halfway away from zero）。

**不变量：**
- 分数模式下 `bufferScale` 永远保持 1（`_glfwUpdateBufferScaleFromOutputsWayland` 在
  `wl_window.c:566-568` 直接 return），缩放交给 `wp_viewport.set_destination`（逻辑尺寸）
  由合成器完成（`fractional-scale-v1.xml`："The wl_surface buffer scale should remain set to 1"）。
- 整数模式下不存在 `scalingViewport`（它只在 `fractionalScaleManager` 存在且
  `scaleFramebuffer` 为真时创建，`wl_window.c:1223-1243`），缩放靠 `set_buffer_scale`。
- `fbWidth/fbHeight` 是派生量，**任何输入变化都必须最终落到 `resizeFramebuffer`**。

---

## 2. 通知到达时能读到的三组量

Wayland 事件在 `wl_display_dispatch` 期间被 libwayland 按服务端写入次序串行回调。
GLFW 的回调在**栈内同步**更新状态，因此在回调返回、或 `glfwPollEvents` 返回后，
`glfwGetWindowSize` / `glfwGetFramebufferSize` / `glfwGetWindowContentScale` 读到的是
与该事件一致的快照。三个 getter：

- 逻辑尺寸：`_glfwGetWindowSizeWayland` → `window->wl.width/height`（`wl_window.c:2549`）
- 帧缓冲尺寸：`_glfwGetFramebufferSizeWayland` → `window->wl.fbWidth/fbHeight`（`wl_window.c:2636`）
- 内容缩放：`_glfwGetWindowContentScaleWayland`（`wl_window.c:2661`）
  - 分数模式：`scalingNumerator / 120.f`
  - 整数模式：`bufferScale`

### 2.1 各通知点触发时，状态处于什么阶段

| 触发事件 | 处理函数 | 逻辑尺寸 | 缩放状态 | 帧缓冲 | 通知 |
| --- | --- | --- | --- | --- | --- |
| `xdg_surface.configure` | `xdgSurfaceHandleConfigure`（`wl_window.c:787`） | 先 `ack`，再 `resizeWindow(width,height)` 更新 | 不变 | `resizeWindow` 内调 `resizeFramebuffer` | 逻辑尺寸变→`WindowSize` + `Damage`；fb 变→`FramebufferSize` |
| libdecor configure | `libdecorFrameHandleConfigure`（`wl_window.c:854`） | 同上（`resizeWindow`，`wl_window.c:930`） | 不变 | 同上 | 同上 |
| `wl_surface.enter` | `surfaceHandleEnter`（`wl_window.c:589`） | 已就绪 | 追加 `{output, monitor->wl.scale}` | 仅当 max factor 改变时 | `ContentScale` + `FramebufferSize` + `Damage` |
| `wl_surface.leave` | `surfaceHandleLeave`（`wl_window.c:616`） | 已就绪 | 删除该输出 | 仅当 max factor 改变时 | 同上 |
| `wl_output.scale` | `outputHandleScale`（`wl_monitor.c:110`） | 已就绪 | 更新 monitor.scale 与所有匹配条目 | 仅当 max factor 改变时 | 同上 |
| `wp_fractional_scale_v1.preferred_scale` | `fractionalScaleHandlePreferredScale`（`wl_window.c:703`） | 已就绪 | `scalingNumerator = numerator` | **无条件**重新推导 | `ContentScale` + `FramebufferSize` + `Damage` |
| `wl_registry.global_remove` | `registryHandleGlobalRemove`（`wl_init.c:206`） | 不变 | **不改 `outputScales`** | **不重算** | 仅 `Monitor` 断开回调 |

### 2.2 整数模式的重算点 `_glfwUpdateBufferScaleFromOutputsWayland`（`wl_window.c:555-587`）

```c
void _glfwUpdateBufferScaleFromOutputsWayland(_GLFWwindow* window)
{
    if (wl_compositor_get_version(_glfw.wl.compositor)
            < WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION)   // 需要 v3
        return;
    if (!window->wl.scaleFramebuffer) return;
    if (window->wl.fractionalScale)   return;   // 分数模式完全绕开本函数

    int32_t maxScale = 1;
    for (size_t i = 0; i < window->wl.outputScaleCount; i++)
        maxScale = _glfw_max(window->wl.outputScales[i].factor, maxScale);

    if (window->wl.bufferScale != maxScale)     // 只有 max 变了才动
    {
        window->wl.bufferScale = maxScale;
        wl_surface_set_buffer_scale(window->wl.surface, maxScale);
        _glfwInputWindowContentScale(window, maxScale, maxScale);
        resizeFramebuffer(window);
        if (window->wl.visible) _glfwInputWindowDamage(window);
    }
}
```

关键语义：**客户端自己取"已进入输出的 factor 最大值"**，并非按窗口面积占比或焦点输出。
所以横跨 scale-1 与 scale-2 的窗口，整数模式下整体按 2 倍渲染（对 1 倍那半边是过采样）。

---

## 3. 同一个跨屏窗口：整数缩放 vs 分数缩放的像素复算

设同一个逻辑窗口 `W = 801, H = 601`（取奇数以暴露舍入），横跨输出 A、B：

- A：整数因子 1 / 分数比例 1.0（numerator 120）
- B：整数因子 2 / 分数比例 1.5（numerator 180）

### 3.1 整数协议（`wayland.xml` `wl_output.scale` + `wl_surface.set_buffer_scale`）

surface 同时进入 A、B，`outputScales = [{A,1},{B,2}]`：

```
S = max(1, 2) = 2
fbW = 801 * 2 = 1602
fbH = 601 * 2 = 1202
wl_surface.set_buffer_scale(2)        // 无 viewport
```

合成器把 1602×1202 的缓冲按 `buffer_scale=2` 解释为 801×601 表面。

- 离开 B（剩下 A）：`max=1` → `fb=801×601`，`set_buffer_scale(1)`。
- 离开 A（剩下 B）：`max=2` → 不变，**不触发任何通知**。
- B 的 `wl_output.scale` 1→2：`outputHandleScale` 更新条目后重算，同上。

### 3.2 分数协议（`wp_fractional_scale_v1` + `wp_viewport`）

合成器对**整个 toplevel 只给一个** preferred_scale（跨屏时取哪个值由合成器决定，
不是客户端取最大值）。假设给 180（1.5）：

```
numerator = 180
fbW = (801*180 + 60) / 120 = (144180 + 60) / 120 = 144240/120 = 1202
fbH = ( 601*180 + 60) / 120 = (108180 + 60) / 120 = 108240/120 = 902
wl_surface.set_buffer_scale 保持 1
wp_viewport.set_destination(801, 601)
```

合成器把 1202×902 的缓冲缩放到 801×601 的表面矩形。

更多 numerator 的可复算结果（W=801, H=601）：

| 比例 | numerator | fbW | fbH | `set_buffer_scale` | viewport destination |
| --- | --- | --- | --- | --- | --- |
| 1.00 | 120 | 801 | 601 | 1 | 801×601 |
| 1.25 | 150 | 1001 | 751 | 1 | 801×601 |
| 1.50 | 180 | 1202 | 902 | 1 | 801×601 |
| 1.75 | 210 | 1402 | 1052 | 1 | 801×601 |
| 2.00 | 240 | 1602 | 1202 | 1 | 801×601 |

（1.75 校验：801*210 = 168210，+60 = 168270，/120 = 1402.25 → 1402；601*210 = 126210，+60 = 126270，/120 = 1052.25 → 1052。）

### 3.3 同一窗口的对照结论

| | 整数协议 | 分数协议 |
| --- | --- | --- |
| 缩放来源 | 客户端对多个输出取 max | 合成器给单个 numerator |
| 跨 1×/2× 时结果 | 1602×1202（整窗按 2×） | 1202×902（按合成器给的 1.5×） |
| 缓冲与表面关系 | `buffer_scale` 声明比例，合成器按整数解释 | `buffer_scale=1`，viewport destination 声明表面尺寸 |
| 谁决定最终比例 | 客户端（取 max） | 合成器（单值） |
| 内容缩放 API 返回值 | `2.0` | `1.5` |

也就是说：**同样"横跨 1× 与 2×/1.5× 两块屏"，整数协议下像素目标由客户端按最大整数因子复算，
分数协议下由合成器下发单一比例、客户端只做算术放大**。两条路径唯一共同的、可供渲染器
消费的输出都是 `fbWidth/fbHeight`。

---

## 4. 显示输出退出：哪次状态变化足以触发重建

### 4.1 现状链路

`wl_registry.global_remove` → `registryHandleGlobalRemove`（`wl_init.c:206-219`）：
按 `monitor->wl.name == name` 找到监视器 → `_glfwInputMonitor(monitor, GLFW_DISCONNECTED, 0)`。

`_glfwInputMonitor`（`monitor.c:118-153`）在断开分支只做两件事：
1. 对 `window->monitor == monitor`（即全屏占用该监视器的窗口）调用 `setWindowMonitor(window, NULL, ...)`；
2. 从 `_glfw.monitors` 数组移除，最后 `_glfwFreeMonitor(monitor)`。

`_glfwFreeMonitorWayland`（`wl_monitor.c:193-197`）：

```c
void _glfwFreeMonitorWayland(_GLFWmonitor* monitor)
{
    if (monitor->wl.output)
        wl_output_destroy(monitor->wl.output);   // 客户端代理销毁
}
```

**问题：**
- 断开路径**没有**从任何"仅横跨该输出"的窗口的 `outputScales` 中摘除条目，也**没有**调用
  `_glfwUpdateBufferScaleFromOutputsWayland`。
- 而 `wl_surface.leave` 事件的 `output` 参数引用的正是刚被销毁的 `wl_output` 代理；
  libwayland 在代理销毁后不再向该代理投递事件，`surfaceHandleLeave` 因而可能根本不被调用。
- 即便 leave 先于 `global_remove` 到达，两者次序也由合成器决定（见 4.3），源码无法保证。

结果：窗口的 `outputScales` 里残留一条指向已销毁 `wl_output` 的悬空条目和旧 factor，
`bufferScale` 仍按已被移除的高倍屏取值，`fbWidth/fbHeight` 偏大；由于 `max` 不变，
**不会**发出 `FramebufferSize`/`ContentScale` 通知，渲染器永远等不到重建信号
（画面可能被合成器下采样但像素数错误，且持续）。

### 4.2 足以触发重建的状态变化

足以让渲染器重建绘制目标的变化，是**窗口派生的帧缓冲尺寸 `(fbWidth, fbHeight)` 改变**；
其上游的充分输入是**该窗口"已进入输出集合"变化导致重算结果改变**：

- 整数协议：`outputScales` 集合成员或其 `factor` 变化 → `max` 变化 → `bufferScale` 变化 →
  `fb` 变化。
- 分数协议：`preferred_scale` 的 numerator 变化 → `fb` 变化（与输出集合无直接关系）。

因此"输出退出"要真正传导到渲染器，必须做的是**从窗口的输出集合中移除该输出并重算**，
而不是依赖监视器断开回调。监视器断开（及其回调）**不是**缩放信号：它不携带窗口的
逻辑尺寸/像素目标，也不保证与 surface 的 leave 同序。

### 4.3 源码保证的顺序 vs 合成器决定的顺序

**源码保证（GLFW 自身可控、可依赖）：**

1. `xdg_surface.configure`（或 libdecor configure）→ `ack_configure` → `resizeWindow`
   → 更新逻辑尺寸 → `resizeFramebuffer` → `_glfwInputFramebufferSize`（`wl_window.c:793-835`、
   `896-939`）。逻辑尺寸先于帧缓冲尺寸更新。
2. `preferred_scale` → `scalingNumerator` → `_glfwInputWindowContentScale` → `resizeFramebuffer`
   （`wl_window.c:709-711`）。同一 dispatch 内同步完成。
3. `enter` 时记录 `monitor->wl.scale`，随后若 `wl_output.scale` 迟到，`outputHandleScale`
   会更新**所有已匹配条目**再重算（`wl_monitor.c:118-129`）。
4. `_glfwUpdateBufferScaleFromOutputsWayland` 是整数路径唯一的重算出口，且只在 `max` 变化时
   发射通知（`wl_window.c:577-586`）。
5. `resizeWindow` 在逻辑尺寸未变时返回 false，从而不触碰帧缓冲（`wl_window.c:510-511`）。
6. `resizeFramebuffer` 内部先 `wl_egl_window_resize`，再回调（`wl_window.c:491-502`），
   所以回调里读 getter 拿到的是新值。

**由合成器决定（客户端无法保证，契约必须容忍）：**

1. `registry.global_remove` 与 `wl_surface.leave` 的**相对次序**。
2. 输出移除时是否/何时发送 `wl_surface.leave`（surface 未映射时可能不发）。
3. 跨多个输出时 `preferred_scale` 的**单一取值**（不是逐输出；可能取最大、可能按占比）。
4. `wl_output.scale` 相对于 bind/`enter` 的到达时机（协议只承诺 scale 后跟 done）。
5. `preferred_scale` 的初值与是否在输出变化后重发；重复发送相同值也是合法的
   （GLFW 会对相同值仍走一遍 `resizeFramebuffer` 并再发一次 `FramebufferSize`）。
6. `enter` 是否会对同一输出重复投递（GLFW 追加时未去重，`wl_window.c:609-611`）。
7. configure 携带的尺寸与缩放事件之间没有跨对象次序保证。

### 4.4 推荐的通知契约（兼容两种缩放协议）

把平台无关的契约收敛到三个派生量上，渲染器只依赖它们：

```
输入（全部由 GLFW 同步派生）：
  L = (window->wl.width, window->wl.height)          // 逻辑尺寸
  S = fractional ? numerator/120 : bufferScale       // 内容缩放
  F = (window->wl.fbWidth, window->wl.fbHeight)      // 绘制目标像素尺寸
且恒有 F = ApplyScale(L, S)，其中
  ApplyScale：fractional ? round_half_away(L*S) : L*S
  分数模式额外的协议侧不变量：buffer_scale==1 且 viewport.destination==L
```

**契约条款：**

- **C1 同步派生**：任一输入事件到达时，在本次事件派发内完成 `L/S/F` 更新；`glfwPollEvents`
  返回后，getter 读到的一定是最新一致快照。（源码已满足，见 4.3 第 1、2、6 条。）
- **C2 变化才通知**：仅当 `F` 改变时发 framebuffer-size 通知；仅当 `S` 改变时发 content-scale
  通知。渲染器把 `F` 变化作为"重建绘制目标"（重设 viewport、重建 FBO/swapchain extent）的唯一判据。
  当前实现有偏离：`resizeFramebuffer` 无条件 `_glfwInputFramebufferSize`（`wl_window.c:502`），
  分数路径对相同 numerator 也会重复通知；消费方应自行比较新旧 `F`。
- **C3 协议无关的等价性**：对同一 `L` 和同一目标比例，两种协议必须给出同一个 `F` 的语义；
  整数协议下 `S` 必须是整数，分数协议下 `S` 为 `numerator/120`（如 1.25/1.5/1.75/2.0）。
  渲染器不需要知道当前用了哪条协议，只需读 `S` 与 `F`。
- **C4 输出集合不变量（输出退出的正确做法）**：窗口的缩放输入集合就是"当前进入的输出集合"。
  **在销毁 `wl_output` 代理之前**，先从所有窗口的 `outputScales` 中删除该输出并调用
  `_glfwUpdateBufferScaleFromOutputsWayland`；随后再 `_glfwInputMonitor(...DISCONNECTED)` /
  `wl_output_destroy`。这样无论合成器把 `leave` 排在 `global_remove` 之前还是之后，
  重算都会发生一次且只发生一次，`F` 与 `S` 的变化不漏发。
  分数模式下输出退出不改变 `preferred_scale` 的推导路径，`F` 只在合成器重发
  `preferred_scale`（或 configure 改 `L`）时变化——这也属于 C1 覆盖范围。
- **C5 监视器回调不是缩放信号**：`glfwSetMonitorCallback` 的 CONNECTED/DISCONNECTED
  仅描述输出对象生命周期，不携带 `L/S/F`，也不与 surface 事件同序。渲染器不得用
  monitor 断开去推断"该重建绘制目标"；只能以 `F`（或 `S`）变化为准。

**行为对照表（接入该契约后）：**

| 场景 | `S` | `F` | 是否重建 |
| --- | --- | --- | --- |
| 整数：跨 {1,2}，离开 2 倍屏 | 2→1 | 缩小 | 是 |
| 整数：跨 {1,2}，离开 1 倍屏 | 2→2 | 不变 | 否 |
| 整数：高倍屏在不可见时被拔除（先 prune 再 destroy） | 2→1 | 缩小 | 是（C4 保证） |
| 分数：preferred_scale 1.5→1.25 | 1.5→1.25 | 变小 | 是 |
| 分数：preferred_scale 重发相同值 | 不变 | 不变 | 否（消费方按 C2 比较） |
| 任意：configure 改逻辑尺寸 | 不变 | 按 `L*S` 变化 | 是 |

---

## 5. 附：值得注意的实现细节

1. `wl_compositor` 只绑定到 v3（`wl_init.c:112-117`，`_glfw_min(3, version)`）；整数缩放路径在
   compositor 版本低于 `WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION`(3) 时整体禁用
   （`wl_window.c:557-561`），此时 `F == L`，由合成器上采样。
2. `wp_viewport` 仅在 `fractionalScaleManager` 存在且 `scaleFramebuffer` 为真时创建
   （`wl_window.c:1223-1243`）；因此 `scalingViewport != NULL` 与 `fractionalScale != NULL`
   在该实现中同时成立，两条路径不会混用。
3. 分数模式下 `resizeWindow` 每次都会刷新 viewport destination（`wl_window.c:518-523`），
   这是"高度系数不变的逻辑尺寸变化"能正确映射到新像素缓冲的必要步骤。
4. `swapBuffersEGL` 在 Wayland 下对不可见窗口直接返回（`egl_context.c:293-295`），
   所以即便 `F` 已更新，重建动作仍应延迟到窗口可见后的下一次绘制。
5. `wl_egl_window_resize` 不会重建 EGLSurface，只更新其尺寸元数据；应用侧仍需在
   `FramebufferSize` 回调里调整 viewport/FBO/swapchain，这正是"重建绘制目标"的落点。
