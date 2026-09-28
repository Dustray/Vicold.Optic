# Xiaomi 17 Pro 相机能力报告（M0.2 产出）

> 采集日期：2026-09-28
> 设备：Xiaomi 17 Pro · HyperOS / Android 16（API 36）· `ro.product.device = pandora`
> 数据来源：`dumpsys media.camera`（camera_dumpsys.txt，12 万行）· `getprop`（getprop.txt）· Vicold.Optic M0 app 内 NDK 实测（capabilities_app.txt + logcat）
> 依赖包版本：platform-tools 37.0.1 / build-tools 36.1.0 / NDK r30 (30.0.16248370) / CMake 3.31.6 / Microsoft OpenJDK 21.0.12

## 总结论

**三方 app 视角下前后摄均为 LEVEL_3，RAW / 全手动 / BURST_CAPTURE / 10-bit 全量可用，未被 HyperOS 降权。**
按 PLAN.md M0.3：架构冻结为「硬件 ISP 直出 RAW + 应用层 burst 合并」，无需自建 demosaic 兜底路径。

## 逐项验证（对应 PLAN.md §5）

### V1 RAW 能力 ✅

- 后置逻辑摄（app 可见 id 0）：`RAW10 + RAW_SENSOR`，**4096×3072**（= pixelArraySize 全幅面）。
- 前置（id 1）：RAW10/RAW_SENSOR 输出 4096×3072 与 4096×2304。
- NDK 实测能力集（id 0）：`BACKWARD_COMPATIBLE, RAW, READ_SENSOR_SETTINGS, MANUAL_SENSOR, BURST_CAPTURE, MANUAL_POST_PROCESSING, STREAM_USE_CASE, DYNAMIC_RANGE_TEN_BIT, COLOR_SPACE_PROFILES, LOGICAL_MULTI_CAMERA`。
- CFA：`colorFilterArrangement = RGGB`（前后摄同）。DNG writer 按此排列写 CFA 标签。

### V2 硬件级别 ✅

- 前后摄均 **LEVEL_3**；无需 HAL3 prop / root。
- `request.maxNumOutputStreams = [RAW=1, PREVIEW=3, PROC=2]`：RAW + 预览 + 分析流并行可行；RAW 同时仅 1 条流。

### V3 黑电平 ✅

- `sensor.blackLevelPattern = [64, 64, 64, 64]`（四通道同值）。DNG writer 静态值 + per-frame `dynamicBlackLevel` 双路写入。

### V4 镜头阴影校正 ✅

- `statistics.lensShadingMapModes = [OFF, ON]` → 可请求 per-frame LSC map，供 M2 DNG OpcodeList 可选增强。

### V5 多摄结构 ⚠️ 关键限制

- HAL 侧 9 个 device（HAL id，不完全对外）：后置逻辑摄 0/5/6（LOGICAL_MULTI_CAMERA）、主摄 2（f=6.62mm）、超广 3（f=2.57mm）、长焦 4（f=17.42mm）、前置 1、虚拟摄像头 7/8（EXTERNAL，12000×9000，SYSTEM_CAMERA）。
- **三方 app（NDK 枚举）只见 id 0（后置逻辑摄）与 id 1（前置），物理摄不在公开列表。**
- M1 多摄方案（已验证 API 存在）：
  - `ACAMERA_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS`（byte[n]）可从逻辑摄 characteristics 读出物理 id；
  - `ACameraDevice_createCaptureRequest_withPhysicalIds`（NdkCameraDevice.h:864）可按物理摄出流；
  - 兜底：zoomRatio 平滑切换（M1 实测两条路，选延迟低者）。

### V6 方向 ⚠️ quirk

- `sensor.orientation = 90`（后置）/ 270（前置），标准值。
- M0 预览实测：竖屏持机画面旋转 90°（直写 Surface 未做旋转补偿，属预期行为）。
- 处置：M1 预览链按 display rotation 补偿（渲染侧旋转，不走额外拷贝）。

### V7 手动范围 ✅

- 后置：曝光 25.9µs – 1s；ISO 50 – 12750；对焦最近 10D（≈10cm）。
- 前置：曝光 26.7µs – 1s；ISO 50 – 800；对焦 6.67D（≈15cm）。
- `sync.maxLatency = PER_FRAME_CONTROL` → 手动参数逐帧生效。

### V8 Camera Extensions ⏳

- 未验证（M6.3）；NDK r30 具备 `ACameraExtensionSession` 接口面，小米侧是否注册夜景/HDR 扩展待真机确认。

### V9 背屏 ✅ 系统可见

- `dumpsys SurfaceFlinger --display-id`：**两个 display**（HWC display 0 主屏 + display 5 背屏）。
- 三方 app 能否向背屏投递内容留待 M6.4 实测（Presentation / WindowManager 多屏 API）。

### V10 快门延迟基线（部分）

- 会话建立实测：INIT_WINDOW → session streaming ≈ **150ms**。
- 预览帧率：良好光照 30.1fps 稳定；暗光 AE 落在 25fps 档（AE 可选档含 [25,25]/[10,30]/[30,30]/[10,60]/[60,60]）。
- 快门点击→出图延迟 M1 补测。

## 机型 quirks（进 device/xiaomi17pro 层）

| # | 发现 | 处置 |
|---|---|---|
| Q1 | `ro.product.market.name` 为空，只有 `ro.product.device=pandora` | 机型匹配以 codename 为主（已实现） |
| Q2 | HyperOS 默认拦截 adb 安装（INSTALL_FAILED_USER_RESTRICTED），`settings put global adb_install_need_confirm 0` 无效 | 需在开发者选项开启「USB 安装」；已在 2026-09-28 开启 |
| Q3 | 前置 pixelArray 同为 4096×3072 | DNG 元数据按摄独立处理 |
| Q4 | 后置 RAW 上限 1 条流 | burst 连拍时 RAW 流独占，预览走 PRIV 流 |

## 架构决策数据支撑（PLAN.md §7 待用户拍板）

- **D2（DNG writer）**：黑电平静态 [64×4] + RGGB + LEVEL_3 元数据完整 → 「先自研最小 writer」数据上成立。
- **D3（管线）**：RAW 直出可用 → 管线聚焦 burst 合并（M4），无需自建 demosaic 主路径。
- 新增事实：多摄物理 id 对 NDK 可通过 `ACAMERA_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS` 读取（见 V5）。

## M0 退出标准核对

| 标准 | 状态 |
|---|---|
| 空 app 在 17 Pro 上以 NDK 打开相机出预览 | ✅（session active，30fps，截图 build/screen2.png） |
| 能力报告完成 | ✅（本文档） |
| 架构决策落文档 | ✅（上节；D1/D4 待用户确认） |
