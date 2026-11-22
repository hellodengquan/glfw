# 上下文移交失败时的所有权与错误归属分析

场景：游戏在**渲染线程**与**加载线程**之间移交同一个 OpenGL 上下文（先在一
个线程 `glfwMakeContextCurrent(NULL)`  detach，再在另一个线程
`glfwMakeContextCurrent(window)` 获取）。GLFW 的错误通知（error callback）
**在报告错误的线程上同步执行**。以下结论均来自本仓库实际代码。

## 关键机制（代码事实）

### 当前上下文是每线程的 TLS 记录

- `glfwMakeContextCurrent`（`src/context.c:621`）从 `_glfw.contextSlot` 这个
  **线程局部存储**读出 `previous`，必要时先 detach 旧上下文，再调用
  `window->context.makeCurrent(window)`。
- `glfwGetCurrentContext`（`src/context.c:647`）只是返回本线程 TLS 里的窗口
  指针，**不查询驱动真实状态**。
- 因此"GLFW 认为谁拥有上下文"是每线程一本账，与驱动层的真实 current 状态可
  能不一致（见下）。

### 错误通知是每线程的、同步的

- `_glfwInputError`（`src/init.c:309`）把错误写入**调用线程**的
  `_glfw.errorSlot`（TLS），随后**在该线程上同步调用** `_glfwErrorCallback`。
- `glfwGetError`（`src/init.c:499`）只返回并清除**调用线程**的最近一次错误。
- 头文件文档（`include/GLFW/glfw3.h`，`glfwSetErrorCallback`）明确：
  "The error callback is called on the thread where the error occurred."
  且错误码在回调触发**之前**已写入，回调内调 `glfwGetError` 会取到同一个错
  误码（并把它清掉）。

### 各平台 makeCurrent 失败时的 TLS 处理不一致

| 平台 | 失败行为 | 失败后本线程 TLS |
|---|---|---|
| EGL（`src/egl_context.c:243`） | 报 `GLFW_PLATFORM_ERROR` 后直接 `return` | **保持旧值**（陈旧） |
| GLX（`src/glx_context.c:157`） | 同上 | **保持旧值**（陈旧） |
| WGL（`src/wgl_context.c:293`） | 报错后 `_glfwPlatformSetTls(..., NULL)` | **被清为 NULL**（但驱动层旧上下文可能仍 current） |
| NSGL（`src/nsgl_context.m:35`） | 不检查失败，总是设 TLS | 总是更新 |

### 各平台 swapBuffers 对"是否 current"的检查不一致

- EGL（`src/egl_context.c:281`）：**显式检查** `window == TLS 当前窗口`，否则
  报 `GLFW_PLATFORM_ERROR` 并拒绝交换。
- GLX（`src/glx_context.c:183`）、WGL（`src/wgl_context.c:318`）、NSGL
  （`src/nsgl_context.m:49`）：**不检查**，直接对窗口的 drawable/DC 交换，
  无论本线程是否持有该上下文。

## 1. 移交未成功时，两个线程各自能把下一帧交给谁

设渲染线程 R 已 detach（`glfwMakeContextCurrent(NULL)` 成功），加载线程 L
执行 `glfwMakeContextCurrent(window)` 失败（`GLFW_PLATFORM_ERROR`）。

**目标窗口的上下文处于"无主"状态**：R 已放手，L 没拿到。驱动层该上下文不
在任何线程上 current。

- **L 线程**：
  - EGL/GLX 下 TLS 仍是失败前的旧值。若 L 之前持有**另一个**窗口的上下文
    （且 source 相同，`glfwMakeContextCurrent` 不会先 detach 它，见
    `src/context.c:637-641`），则 L 仍真实拥有那个旧上下文，**下一帧只能交
    给旧窗口**；目标窗口不能交——它的上下文无主，L 的 GL 调用会进旧上下文。
  - WGL 下 TLS 被清成 NULL，但驱动层旧上下文可能仍 current：GLFW 的账与驱
    动不一致，L 若凭 `glfwGetCurrentContext()==NULL` 就调 GL，命令实际进了
    旧上下文。
  - 若 L 之前无上下文，则 L 什么都不能交。
- **R 线程**：已 detach，`glfwGetCurrentContext()` 返回 NULL。它不能把下一
  帧交给目标窗口（不再持有），除非重新 `glfwMakeContextCurrent(window)` 把
  上下文抢回来——这是唯一立即可行的兜底，因为上下文此刻无人占用，重新获取
  不会违反"一个上下文只能在一个线程上 current"的规则。

**应用从两个渠道分别能得出什么：**

- **错误通知（error callback）**：只在 L 线程上触发，内容是
  `GLFW_PLATFORM_ERROR` + 平台描述串。它能告诉 L"获取失败了"，但**不能**告
  诉 L 失败后谁还拥有上下文，也**完全不会**通知 R——R 调 `glfwGetError` 只
  能看到自己线程的错误槽，永远看不到 L 的这次失败。跨线程传递失败事实必须
  由应用自己做（回调里往应用自有队列写一条记录）。
- **当前上下文查询（`glfwGetCurrentContext`）**：返回的是本线程 TLS 账本，
  不是驱动真实状态。EGL/GLX 失败后它返回**陈旧值**（看似还拥有旧上下文，
  这点恰好与驱动一致，可用）；WGL 失败后返回 NULL（与驱动可能不一致，不可
  全信）；NSGL 不会失败。R 上查询为 NULL 只说明"我 detach 了"，**无法**区
  分"L 已接手"和"L 接手失败、上下文无主"这两种局面。

结论：单次查询和单次通知都不足以判定所有权，必须两者结合并配合应用级的移
交协议（见第 2 节约定）。

## 2. 在通知中再次查询图形状态的影响与恢复约定

### 在 error callback 里再调 GLFW/GL 会发生什么

回调是在 `_glfwInputError` 内部、**平台 makeCurrent 还没返回、TLS 尚未最终
落定**的时刻被同步调用的（EGL/GLX 失败后根本不更新 TLS；WGL 是先报错再清
TLS）。此时再查询图形状态：

1. **读到的是失败前的旧账本**。回调里 `glfwGetCurrentContext` 在 EGL/GLX
   下返回失败前的窗口，在 WGL 下也仍是旧值（清 NULL 发生在回调之后）。基于
   它做的任何"我现在拥有谁"的判断都是错的。
2. **`glGetError`/`glGetIntegerv` 打到的是旧上下文**。EGL/GLX 获取失败时驱
   动层 current 未变，GL 查询进入的是旧上下文（或没有上下文），与刚失败的
   目标上下文毫无关系——结果具有误导性，且无法用来诊断目标上下文为何获取
   失败。
3. **覆盖原错误**。若回调里调的 GLFW 函数自身报错（例如无当前上下文时调
   `glfwGetProcAddress` → `GLFW_NO_CURRENT_CONTEXT`），`_glfwInputError` 会
   **递归进入并覆写本线程错误槽**（`src/init.c:374-375`），回调返回后应用
   在主流程里调 `glfwGetError` 拿到的已不是原始的 `GLFW_PLATFORM_ERROR`。
   即使回调里只调 `glfwGetError`，也会**清除**刚写入的错误码，主流程同样
   拿不到了。
4. **污染后续交换缓冲**。回调里若调 `glfwSwapBuffers`：
   - EGL 后端拿陈旧的 TLS 做 current 检查，可能误放行（对未 current 的
     surface 调 `eglSwapBuffers`，其返回值不被检查，失败静默）或误拒绝（再
     触发一次 `_glfwInputError`，回到第 3 点的覆写问题）；
   - 更隐蔽的是：`eglSwapBuffers` 失败会把 `EGL_BAD_*` 留在 EGL 的错误槽
     里，之后某次无关失败用 `eglGetError()` 拼描述串
     （`src/egl_context.c:254`）时会读到这条**陈旧 EGL 错误**，报错张冠李戴；
   - GLX/WGL/NSGL 不检查 current，回调里的交换会直接作用于窗口 drawable，
     可能把**渲染到一半的帧**呈现上屏。

### 可实施的恢复约定

**对调用线程（应用侧）的要求：**

1. **错误回调只做记录，不做调用**。回调里只允许把 `(code, description 的
   副本, 线程 id)` 追加到应用自有的无锁队列；禁止在回调里调任何 GLFW 函数
   （包括 `glfwGetError`/`glfwGetCurrentContext`）和任何 GL 函数。所有诊断
   与恢复推迟到拥有者线程的正常循环里做。
2. **失败方就地确认并上报**。`glfwMakeContextCurrent` 返回后，调用线程立即
   调 `glfwGetError` 取走并清除本线程错误（此时回调已返回，错误槽里是原始
   错误），再把"移交失败"通过应用队列通知对端线程——GLFW 不会替你做这件
   事。
3. **移交协议带确认位**。R detach 后把目标窗口标记为"移交中"；L 获取成功后
   置"已持有"，失败则置"无主"。R 在收到 L 的成功确认前不得假定上下文已送
   达；看到"无主"时由**预先指定的一方**（建议 R）重新
   `glfwMakeContextCurrent(window)` 抢回并继续渲染，或整体销毁重建。
4. **交换前自证所有权**。每次 `glfwSwapBuffers(window)` 之前要求
   `glfwGetCurrentContext() == window` 在本线程成立，否则跳过本帧交换并重
   新走获取流程。EGL 后端已强制此检查，该约定把同样的保证扩展到
   GLX/WGL/NSGL。
5. **失败后重新对账再发 GL 命令**。任何 `GLFW_PLATFORM_ERROR` 之后、下一条
   GL 命令之前，必须重新执行一次目标 `glfwMakeContextCurrent` 并验证
   `glfwGetCurrentContext()` 等于预期窗口，以消除 WGL 上"TLS 已清空但驱动
   仍 current"这类账本分叉。

**对平台（GLFW 后端）的要求：**

1. 各后端 `makeCurrent` 失败时必须把 TLS 置于**确定状态**：要么统一像
   EGL/GLX 那样保留旧值且保证驱动层也未变，要么统一像 WGL 那样清 NULL 并保
   证驱动层同步 detach；当前三者行为不一，是应用无法写出可移植恢复逻辑的
   根源。
2. 失败路径必须保证"GLFW 账本"与"驱动 current 状态"不发散（WGL 失败时清
   TLS 但驱动未 detach 即属发散）。
3. `swapBuffers` 后端应统一做 EGL 式的 current 检查，把"错线程交换"从静默
   呈现垃圾帧变成显式 `GLFW_PLATFORM_ERROR`。
4. 后端用 `eglGetError()` 等平台错误查询拼描述串时，应注意该操作会清掉平
   台错误槽，避免把回调期遗留的陈旧错误归到后续无关失败上。
