# Xiaomi 17 Pro 多摄会话路径与变焦分带

> 2026-09-29 真机确诊与验证。`ro.product.device=pandora`，Android 16 / API 36。
> 证据：`camera_dumpsys.txt`（5.8MB dumpsys media.camera 摘录）、`img/` 下 FOV 对比截图。

## 1. 硬件拓扑

| 角色 | 摄像头 ID | 焦距 | 传感器宽 | 等效焦距 | 相对主摄倍率 |
|---|---|---|---|---|---|
| 超广角 | **物理 3** | 2.57mm | 5.014mm | 18.5mm | **0.774x** |
| 主摄 | 物理 2（逻辑摄 0 的默认成员） | 6.62mm | 9.994mm | 23.8mm | 1.00x |
| 长焦 | **物理 4** | 17.42mm | 5.243mm | 119.6mm | **5.016x** |

> ⚠️ **倍率必须按等效焦距比 `(f/w)_i / (f/w)_main`，不能用焦距比**：长焦传感器只有主摄的一半宽，
> 焦距比 17.42/6.62 = 2.63 会把切换点算错一半（2026-09-30 修复；MIUI 相机口径 1x=23mm /
> 5x=115mm / 0.7x=17mm 与等效焦距计算一致）。

- 物理摄不占公共 ID 列表，只能经逻辑摄 0 通过物理流（`ACaptureSessionPhysicalOutput`）触达。
- 后置 RAW 仅 1 条流（主摄）；超广角/长焦物理直连时**无 RAW**。
- 注意：早期假设"超广角 = ID 2"是错的（ID 2 是主摄）；以焦距探测为准（最短 = 超广角）。

## 2. HAL 融合管线的两个坏区

逻辑摄 0 用 `CONTROL_ZOOM_RATIO` 跨段时走 CamX 融合管线（deblur / SAT / GME + QSEE），
真机实测该管线存在两个坏区，症状一致：**每 ~1.2s 断流一波**
（`REQUEST_ERROR/RESULT_ERROR/BUFFER_ERROR` 风暴 + `No Provider Object with cameraId` +
`GetSensorModeValidationTable() without SensorModeValidationTable`）：

| 变焦区间 | 路径 | 状态 |
|---|---|---|
| `[0.7, 1.0)` | 融合切超广角 | ❌ 坏区一：持续断流 |
| `[1.0, 4.9]` | 主摄（数字变焦） | ✅ 干净（4.6/4.8 实测 0 stall） |
| `[5.0, 10]` | SAT 切长焦 + 融合 | ❌ 坏区二：**5.0 起持续断流**；HAL 把逻辑流 zoom 硬钳在 5.00 |

> 逻辑流安全上限实测边界：4.8 干净 / 5.0 断流（2026-09-30，每点静置 6s 数 `preview stall`）。

结论：**凡是让逻辑请求跨进融合坏区的用法都不可用**；绕过方式 = 物理直连（其他 app 正是这么做的）。

## 3. 通用分带机制（显示源判定，现行实现）

`CameraEngine::activePhysId()` 按用户变焦 z 判定应显示哪路流（带间滞回防边界抖动）：

| z | 显示源 | 带基 az | 该带变焦实现 |
|---|---|---|---|
| `[0.774, 1.0)` | 超广角物理流 | 0.774 | GL crop = `z/0.774`（per-physical 变焦键被 HAL 忽略） |
| `[1.0, 4.85]` | 逻辑主摄流 | = z | 逻辑 ZOOM_RATIO=z（钳到 4.85） |
| `(4.85, 10]` | 长焦物理流 | 5.016 | GL crop = `z/5.016`（10x 只需裁 1.99 倍） |

- **接管点 `teleSwitch_` = min(长焦原生倍率, 4.85)**：必须落在逻辑流安全区内。若按原生 5.016
  接管，用户拖到 5x 时逻辑流已在断流（画面冻结 → 长焦接管瞬间跳变，即用户所见
  「5x 瞬间放大好多」）。代价是 4.85–5.02 之间约 3.5% 的小死区（crop 不能 <1）。
- 诊断覆盖键（controls.txt）：`tele_phys=N`、`phys_min=N`、`uw_phys=N`、`uw=0/1`、`cam=N`、
  `tele_native=N`（强制长焦带基，肉眼校准切带是否连续）、`disp=0/1/2`（手动锁显示源）。

## 4. ALL 全目标常驻会话（现行实现，2026-09-29 晚）

**一个 repeating 请求挂全部输出，永不再换**。启动时一次 configure 配齐：

- 3 路预览 `ACaptureSessionPhysicalOutput`：slot0 = 逻辑 L、slot1 = 物理 3（uw）、slot2 = 物理 4（tele）；
- 第 4/5 路：**拍摄输出，按格式互斥挂载**
  - JPEG 模式（默认）：第 4 路 JPEG（逻辑输出）+ 第 5 路 JPEG（**绑物理 3**，超广带专用）；
  - RAW 模式：第 4 路 RAW_SENSOR（逻辑输出）。
  > JPEG 两路都在会话里，但**绝不能进 repeating**（见 `STILL_PIPELINE.md` §2：ISP 逐帧编码 12MP ⇒ 18.5fps）。
- repeating 请求经 `createCaptureRequest_withPhysicalIds([3,4])` 创建（`CaptureSession::setRepeatingAll`，
  按 "ALL" 缓存），**逐摄写 ZOOM_RATIO**（`ACaptureRequest_setEntry_physicalCamera_float`，API 29）：
  uw = max(1, z/0.7)、tele = max(1, z/teleMin)、逻辑 = 钳在 [1.0, teleMin−0.05] 干净带 ——
  **三路流恒渲染同一用户 FOV**，跨带切显示源时画面内容连续。
- **跨带切换 = 纯 GL 显示层**（`Ui::setPreviewSlot` + 150ms 交叉淡化），请求零动作、会话零重建；
  带内变焦 = 同一请求改 entry 重发（复用缓存请求，0 间隔）。
- 三路常流副产物：uw/tele 的 AE/AWB 持续收敛，切换瞬间色彩已稳定（业界"预收敛"思路白拿）。
- RAW 恒出帧 → **全带可拍 RAW**（物理带的 DNG 来自主摄逻辑流，FOV=主摄当前倍率）。
- 会话只在启动 / 设备重连 / 致命错误时重建；重建仍走退役墓地（防 UAF，见下）。
- HAL 拒绝该输出组合时：先**拆掉 uw 物理 JPEG（第 5 路）重试 4 流**；仍被拒才降级单流重建路径
  （`multiStreamFailed_`；降级模式物理带无拍摄输出）。
- 三路 reader 每帧必须全部 drain（不消费会撑满 maxImages=3 队列拖累整条 repeating）。
- 预览看门狗与墓地机制保留：>1.2s 无 result → 重发 setRepeating（ALL 请求同路径）；
  重建后旧会话延迟 1.5s 析构（立即析构会触发 `C2N-dev-looper` UAF → SIGABRT）。

## 5. 真机验证记录（2026-09-29）

ALL 全目标常驻会话（controls.txt 推 zoom，与触摸同一条 commitSession 路径）：

- 冷启动：4 输出 + withPhysicalIds([3,4]) 请求被 HAL 接受，30fps，gaps=0，RAW 30.6fps，
  三路 slot 帧计数逐秒同步增长（L=uw=tele，真三路常流）。
- **10 次跨带往返（0.7↔10.0 全覆盖）：0 请求重建、0 会话重建、0 新增 gap**
  （压测全程 max gap 恒为启动时一次 71ms；对比：换请求方案每次跨带 118ms，
  最初的单流重建方案 ~285ms + 黑帧）。
- 显示源切换：0.8x 超广视角 / 8.0x 长焦窄视角截屏确认 FOV 正确切换（`build/v_uw*.png`、`v_tele*.png`）。
- **物理带 RAW 拍摄解锁**：8.0x 长焦带 `zsl_shutter=1` → ZSL 4 帧、4 张 DNG（各 25.7MB）落盘
  （DNG 来自主摄逻辑流，FOV=主摄当前倍率）。

单流重建时期的历史结论（仍适用于降级路径）：

- 全段阶梯 0 断流（修复前 5.0x 断流 5 次、10.0x 持续死锁、0.7x 每 1.2s 断流）。
- 带内实时重发 0 丢帧（gaps=0, max 47ms）；卡顿全部来自跨带会话重建（~285ms 冻结）。

## 6. 已知限制 / 遗留

- ALL 常驻下 result 元数据 `zoomRatio` 是**逻辑流**的（钳在干净带内）；
  UI 的 appliedZoom 由引擎按显示带换算（relUw×0.7 / relTele×teleMin / 逻辑原值）。
- RAW 模式下物理带拍摄的 DNG 仍来自主摄逻辑流（RAW 只挂逻辑输出），FOV ≠ 当前显示的超广/长焦视角。
  **JPEG 模式已解决**：超广带走第 5 路物理 JPEG 直连、其余带由 `WysiwygCropProcessor` 软件裁齐，
  照片 FOV 严格 = 预览 FOV —— 详见 `doc/STILL_PIPELINE.md` §4。
- 三传感器常开：功耗高于单流方案（方案 A 的固有代价，换取零切换卡顿）。
- controls.txt 的 zoom 不联动 UI 导轨读数（UI 本地状态，触摸路径不受影响）。
- 跨镜头色彩/亮度差异仍在（三路各自收敛，主摄与副摄的 AE/AWB 目标不同），
  GL 150ms 交叉淡化兜底；彻底统一需 HAL 级 3A 同步（SAT 内部行为，应用层不可达）。

## 7. 平滑拖拽变焦（2026-09-29）

真机量化（帧节奏探针：相邻 result 间隔 >70ms 计一次断裂）：

| 场景 | gaps>70ms | max gap |
|---|---|---|
| 静置基线 6s | 0 | 43ms |
| 带内拖动（~8 次/s 实时重发） | **0** | 47ms |
| 跨带 1.0→5.0（旧实现，拖动中重建） | +2 | **285ms** |

结论：带内实时重发零丢帧；**卡顿全部来自跨带会话重建（~285ms 冻结）**。

现行机制（ALL 常驻后简化）：
- 全向实时跟随：拖拽 ≥120ms 节流下发 SET_ZOOM（引擎每轮合并 → ≤8 次重发/s，复用同一请求
  改 entry，带内/跨带全部 0 间隔）；不再需要"放大方向冻结相机"的双模式 —— 跨带已零代价，
  松手无任何请求切换。
- crop = zoom_/appliedZoom 仅补元数据与像素间 1~2 帧滞后差（拖拽中 ≈1），
  az 变化时 cropSmooth 立即归一（防双向补偿互搏错位）。
- 跨带瞬间：显示 slot 切换 + **150ms 交叉淡化**（旧源叠画淡出）遮跨镜头 AE/AWB/内容跳变。
- onUp 用松手位置重算终值（快速甩动时输入管线丢弃末尾 MOVE，实测旧逻辑停在 8.76 而非 10.0）。
- 带间滞回保留：uw 退出 z≥1.03、tele 退出 z≥teleMin−0.08 —— 防显示源在边界高频抖动
  （切换虽零代价，FOV/色彩仍会在两路间来回跳）。
