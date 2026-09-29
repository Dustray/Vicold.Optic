# Xiaomi 17 Pro 多摄会话路径与变焦分带

> 2026-09-29 真机确诊与验证。`ro.product.device=pandora`，Android 16 / API 36。
> 证据：`camera_dumpsys.txt`（5.8MB dumpsys media.camera 摘录）、`img/` 下 FOV 对比截图。

## 1. 硬件拓扑

| 角色 | 摄像头 ID | 焦距 | 说明 |
|---|---|---|---|
| 超广角 | **物理 3** | 2.57mm | 不在 `getCameraIdList`，无法独立打开 |
| 主摄 | 物理 2（逻辑摄 0 的默认成员） | 6.62mm | 逻辑摄 0 对外，物理成员 `[3 2 4]` |
| 长焦 | **物理 4** | 17.42mm | 浮动长焦，原生倍率 ≈ **2.63x**（f_tele/f_main） |

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
| `[1.0, 2.63)` | 主摄（数字变焦） | ✅ 干净 |
| `[2.63, 10]` | SAT 切长焦 + 融合 | ❌ 坏区二：持续断流（5x/10x 实测，3.0x 尚可） |

结论：**凡是让逻辑请求跨进融合坏区的用法都不可用**；绕过方式 = 物理直连（其他 app 正是这么做的）。

## 3. 通用分带机制（现行实现）

`CameraEngine::activePhysId()` 按用户变焦 z 自动选择会话路径：

| z | 会话签名 | 实现 |
|---|---|---|
| `[0.7, 1.0)` | `P:3` | 超广角物理直连，恒原生 FOV（不写 ZOOM_RATIO） |
| `[1.0, 2.63)` | `L` / `L+R` | 逻辑主摄，写 ZOOM_RATIO=z，RAW 环随会话 |
| `[2.63, 10]` | `P:4` | 长焦物理直连，写**相对数字变焦** `z/2.63`（10x → 3.80x） |

- 切换点 = `teleNativeZoom`（2.63），恰在 SAT 坏区开始之前切走，逻辑请求永远不进坏区。
- 诊断覆盖键（controls.txt）：`tele_phys=N`、`phys_min=N`、`uw_phys=N`、`uw=0/1`、`cam=N`。
- 物理直连模式下 RAW 拍摄（快门/ZSL/shot_raw）被拒（该物理镜头无 RAW 流），toast 如实提示。

## 4. 多流常驻会话（现行实现，2026-09-29 晚）

跨带**不再重建会话**。启动时一次 `ACameraManager_createCaptureSession` 配齐全部输出：

- 3 路预览 `ACaptureSessionPhysicalOutput`：slot0 = 逻辑 L、slot1 = 物理 3（uw）、slot2 = 物理 4（tele）；
- 可选第 4 路：RAW 逻辑输出（与预览同一会话）。
- **HAL 实测接受该 4 输出组合**（pandora / CamX，30fps 下无流饥饿）。

跨带切换 = `ACameraDevice_createCaptureRequest_withPhysicalIds` 换 repeating 请求
（请求按签名缓存于 `CaptureSession::reqCache_`，不重复建请求）+ GL 侧 `setPreviewSlot`
切纹理源（新源首帧到达才切显示，旧画面保持 → 无黑帧）。纹理永不失效。

- 会话只在启动 / 设备重连 / 致命错误时重建；重建仍走退役墓地（防 UAF，见下）。
- HAL 拒绝多流组合时自动降级单流重建路径（`multiStreamFailed_`）。
- RAW 环只在逻辑带出帧：repeating 为 `P:3`/`P:4` 时 RAW 流暂停（帧率计冻结属正常），
  回逻辑带自动恢复；拍摄键在物理带仍被拒（该镜头无 RAW 流）。
- 预览看门狗与墓地机制保留：>1.2s 无 result → 重发 setRepeating；重建后旧会话延迟 1.5s 析构
  （立即析构会触发 `C2N-dev-looper` UAF → SIGABRT）。

## 5. 真机验证记录（2026-09-29）

多流常驻会话（controls.txt 推 zoom，与触摸同一条 commitSession 路径）：

- 冷启动：4 输出会话被 HAL 接受，30.1fps，gaps=0（max 46ms），RAW 30.6fps。
- **14 次快速跨带往返（L↔P:3↔P:4）：0 会话重建**，max gap 恒定 118ms
  （旧实现每次重建 ~285ms + 黑帧闪烁）；其中 P:4↔P:3 物理直连互切 **0 gap**。
- RAW 拍摄：逻辑带 `zsl_shutter=1` → ZSL 4 帧、4 张 DNG（各 25.7MB）正常落盘；
  物理带 RAW 流按设计暂停、回逻辑带自动恢复（实测 total 冻结→恢复）。
- 预览渲染：slot 切换无黑帧，UI/AF/HUD 正常（`build/screen2.png`）。

单流重建时期的历史结论（仍适用于降级路径）：

- 全段阶梯 0 断流（修复前 5.0x 断流 5 次、10.0x 持续死锁、0.7x 每 1.2s 断流）。
- 带内实时重发 0 丢帧（gaps=0, max 47ms）；卡顿全部来自跨带会话重建（~285ms 冻结）。

## 6. 已知限制 / 遗留

- 物理直连下 result 元数据 `zoomRatio` 报**相对值**（10x 显示 3.80）；用户倍率以导轨/HUD 为准。
- 物理直连带无 RAW 流（能力限制）：RAW 环暂停、拍摄被拒；仅逻辑带可拍 RAW。
- 逻辑↔物理带切换仍有 ~118ms 的 HAL 结果间隔（传感器模式切换固有，约 3-4 帧），
  GL 侧以旧画面保持 + 首帧到达才切显示兜底，无黑帧；物理↔物理互切无间隔。

## 7. 平滑拖拽变焦（2026-09-29）

真机量化（帧节奏探针：相邻 result 间隔 >70ms 计一次断裂）：

| 场景 | gaps>70ms | max gap |
|---|---|---|
| 静置基线 6s | 0 | 43ms |
| 带内拖动（~8 次/s 实时重发） | **0** | 47ms |
| 跨带 1.0→5.0（旧实现，拖动中重建） | +2 | **285ms** |

结论：带内实时重发零丢帧；**卡顿全部来自跨带会话重建（~285ms 冻结）**。

现行机制：
- 带内实时跟随：拖拽 ≥120ms 节流下发 SET_ZOOM（引擎每轮合并 → ≤8 次 setRepeating/s）。
  超广角带也写相对数字变焦（rel = z/0.7），0.7–1.0 段真实连续变焦。
- 上拉越带（主摄→长焦）：相机侧钳在 teleMin−0.08，越带部分由 **GL 数字裁切**模拟
  （crop = zoom_/appliedZoom，引擎按 result 回传 appliedZoom），松手才推真实值触发重建
  —— 拖动途中 0 冻结，冻结只发生在落位瞬间（自然节拍）。
- onUp 用松手位置重算终值（快速甩动时输入管线丢弃末尾 MOVE，实测旧逻辑停在 8.76 而非 10.0）。
- 带间滞回：uw 退出 z≥1.03、tele 退出 z≥teleMin−0.08（进带阈值不变），
  防导轨在边界微动触发重建风暴（实测边界摆动 3 次重建 → 0）。
- 下拉越带（tele→main、main→uw）无法用裁切模拟变宽，旧单流实现中重建发生在拖动途中
  （每次 1~3 个帧断裂，~300ms）—— **已由第 4 节多流常驻会话彻底消除**（跨带 0 重建，
  仅剩 ~118ms HAL 切换间隔，无黑帧）。
