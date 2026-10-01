# 机型抽象层（device/）使用契约

> 约束来源：`doc/PLAN.md §2.1` —— **机型差异只允许出现在 `core/device/` 层**。
> 最后更新：2026-10-01（抽象层重写：此前 device 层是空壳，pandora 的全部 quirk
> 都以硬编码常量/内联分支的形式散落在 `capture/` 与 `ui/`）

## 1. 为什么需要这层

有一整类 HAL 行为**无法从 CameraCharacteristics 问出来**，只能真机试错得到：

| 现象 | 能探测吗 | pandora 实测值 |
|---|---|---|
| 逻辑融合管线的「坏区」边界（超过就持续断流） | ❌ | 4.8 干净 / 5.0 起断流 → 安全上限 4.85 |
| 物理流是否**继承逻辑请求的 ZOOM_RATIO 裁切** | ❌ | 继承（长焦带逻辑写 4.85 → 实际 ≈24x） |
| per-physical ZOOM/CROP 键是否被真实执行 | 声明了≠执行 | 忽略；但 per-physical **曝光键照常执行** |
| 会话最多能挂几路输出、被拒时先拆哪一路 | ❌ | 5 路可行；拒则拆 uw-JPEG 退 4 路 |
| 近距时推迟长焦接管的距离阈值 | ❌ | 0.9m 进入 / 1.4m 退出，推迟到 20x |
| 屏幕挖孔的可用边界 | 部分（包络不准） | 设计 x 0–88 是**实黑区**（X=64 会被吃掉） |
| 导轨关键焦段 / mm 读数基准（对齐系统相机口径） | ❌ | 0.7/1/2/5/10/50/120，1x = 23mm |

这些东西如果写在业务代码里，第二台机型就会**带着 pandora 的参数跑**，而且症状很隐蔽
（照片 FOV 错、近距规则错、UI 被挖孔吃掉、会话反复被拒重建）。

## 2. 接口总览（`core/device/IOpticDevice.h`）

业务代码一律通过 `optic::device::currentDevice()` 取，**不许自己带默认值**。

| 方法 | 返回 | 承载的内容 |
|---|---|---|
| `matches(dev, market)` | bool | 机型如何识别（`pandora` 精确、`ro.product.*` 兜底） |
| `probe(mgr, id, chars)` | DeviceInfo | 走**通用** capabilities 解析（所有机型共用 GenericDevice 的实现） |
| `identity()` | DeviceIdentity | DNG 的 Make / Model |
| `zoomProfile()` | ZoomProfile | 量程、关键焦段、逻辑安全带、接管点、mm 基准 |
| `nearTakeover()` | NearTakeoverRule | 近距推迟接管的距离/倍率/去抖 |
| `physQuirks()` | PhysQuirks | 物理流继承裁切、per-key 是否真实执行 |
| `sessionPolicy()` | SessionPolicy | 预览槽数、预览尺寸、**预览缓冲格式**（`previewFormat`，多数 HAL 只有 PRIVATE 零拷贝稳定）、**副摄预览尺寸**（`auxPreviewW/H`，非显示带降分辨率省带宽）、最大输出数、降级阶梯、**帧率目标**与 **UI 渲染节拍上限**（`uiRenderFps`） |
| `uiLayout()` | UiLayoutPolicy | 挖孔禁区边界与避让带宽 |
| `fontCandidates()` | vector<string> | 候选中文字体路径（按优先级）。不同 ROM 字体名/格式差异巨大且**无法探测**，只能按机型列举；运行时选静态 TrueType(glyf) 且中文覆盖率最高的 |
| `applyQuirks(cfgs)` | void | 会话创建前对流配置的最后修正（Hook） |
| `tuningFileFor(id)` | string | per-sensor tuning 路径（M4） |

## 3. 新增机型该怎么做

只需三步，**不需要碰 `capture/`、`ui/`、`dng/`**：

1. 新建 `core/device/<oem>/XxxDevice.{h,cpp}`，实现上表里需要偏离默认值的项
2. 在 `core/CMakeLists.txt` 加一行源文件
3. 在 `core/device/DeviceRegistry.cpp` 的 `kRegistry` 数组里加一行 factory（顺序 = 优先级，GenericDevice 永远最后）

默认值设计成「通用保守」而非「pandora 值」：未声明的能力**关闭**
（如 `NearTakeoverRule::enabled = false`），这样未知机型不会因为套用了别人的
quirk 而出错。

## 4. 业务层的接线位置（改这里要同步更新本文档）

| 消费方 | 读的接口 | 替换掉了什么硬编码 |
|---|---|---|
| `CameraEngine` | zoomProfile / nearTakeover / physQuirks / sessionPolicy / identity | `kLogicalSafeMax 4.85`、`kTeleSwitchUser 5.0`、`0.9m/1.4m/12帧/20x`、`:606` 的魔法数 `2.5f`、内联的"物理流继承裁切"分支、`"Xiaomi 17 Pro"` 机型名、5→4 流的降级阶梯 |
| `CaptureSettings` | sessionPolicy.targetFps* | 原先完全没写 `AE_TARGET_FPS_RANGE` |
| `Ui` | uiLayout / zoomProfile / sessionPolicy | 挖孔常量 88/80、`kZoomMin/Max/BaseMm 0.7/120/23`、关键焦段表、预览尺寸 1920×1440、预览槽数 3、过期的 `teleMin_ = 2.63` |
| `Gl`(经由 `Ui`) | sessionPolicy.previewFormat / auxPreviewW/H、fontCandidates() | 预览 `AImageReader` 的硬编码 `AIMAGE_FORMAT_PRIVATE`、副摄同主摄满分辨率、字体路径里的 pandora 硬编码 `/product/fonts/...` 列表（现由机型层提供候选，运行时择优） |
| `RawCapture`(经由 engine) | identity | `StaticMeta::make/model` 的 `"Xiaomi"` / `"Xiaomi 17 Pro"` 默认值（已改为 `Unknown` 兜底） |
| `CameraDevice` | — | 镜头角色判定改为**按 35mm 等效焦距选主摄**（原先"≥3 成员才认长焦"，会把「主摄+长焦」双摄机型的主摄误判成超广角） |

诊断/debug 覆盖仍然优先：`controls.txt` 的 `near_m` / `tele_near` / `phys_min` /
`preview_w` / `uv_rot` 等键早于机型默认值生效。

## 5. 测试钩子

`device::setDeviceForTest(IOpticDevice*)` 可强制指定机型实例（传 `nullptr` 恢复自动匹配），
用于在没有真机的情况下验证「非 pandora 机型」的行为路径。

## 6. 踩过的坑

- **市场名匹配必须带品牌前缀**：只判 `"17 pro"` 子串会误命中其它品牌的同名机型。
- **`EGL_SWAP_BEHAVIOR = EGL_BUFFER_DESTROYED` 在 pandora 上会让 `eglCreateWindowSurface`
  直接失败**（2026-10-01 实测）。理论上能省掉每 swap 拷回上一帧的 13MB 读写，但 Adreno
  对 window surface 不接受覆盖 swap behavior —— 别再试了。
- **预览槽数 ≠ 3 时**要确认 `Ui::previewSlots_` 与 `CameraEngine` 的 `previewWindow(slot)`
  循环一致，否则会出现"会话挂了 slot 但 UI 没建 reader"。
- **UI 静态覆盖层走离屏 FBO 缓存 + 逐帧合成**（2026-10-01 优化）：轨道/刻度/文字/HUD 等
  不随预览帧变化的元素只在「脏」（输入/状态变化）或直方图刷新（~4Hz）时重烤进一张 RGBA
  纹理，稳态每帧只做 1 次全屏合成（≈2 draw call），把 ~120 次 UI draw call 从 30Hz 降到
  ~4Hz —— 这是 glue 线程 GPU/发热的主要杠杆，且机型无关。预览、闪光、跨带淡化不进缓存
  （逐帧绘制）。要在新机型上复现此收益，只需正常接 `sessionPolicy`；覆盖层缓存是 Gl 内部机制。
