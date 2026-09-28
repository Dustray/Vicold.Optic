# Vicold.Optic 开发计划

> 专业拍摄 · 极速 · RAW · 按机型深度定制的 Android 相机应用
> 首发定制机型：Xiaomi 17 Pro（保留其他机型扩展接口）
> 核心代码：C++（除前端壳层）；不使用 Java 源码

版本：v0.1（2026-09-28）
优先级定义：**P0** = 阻塞主线的必做项 | **P1** = 重要，紧跟 P0 | **P2** = 增强项，可延后

---

## 1. 产品目标与边界

### 1.1 目标（按重要性排序）

1. **极速**：快门延迟、启动速度、连拍吞吐是第一指标。任何功能不得阻塞快门路径。
2. **专业 RAW**：DNG 输出元数据完整正确（黑电平、色彩矩阵、每帧元数据），Lightroom/达芬奇直开。
3. **手动全控**：ISO / 快门 / 对焦 / 白平衡 / 手动镜头切换，实时生效。
4. **机型深度定制**：`device/` 层按机型注入 quirks 与 tuning，首发 Xiaomi 17 Pro。
5. **计算摄影**：RAW burst 对齐 + 合并（HDR+ 路线）作为画质护城河。

### 1.2 非目标（首版不做）

- 人像虚化、美颜、AI 场景识别
- 社交分享、云同步、编辑器
- 非 ARM64 架构、Android < 12

---

## 2. 总体架构

```
┌─────────────────────────────────────────────────┐
│  UI 壳层（NativeActivity，纯 C++ 渲染 HUD）        │
│  预览 Surface · 手动控制面板 · 直方图/对焦峰值      │
├─────────────────────────────────────────────────┤
│  core/（C++17，静态库，无 Android UI 依赖）        │
│  ├─ capture/  libcamera2ndk 封装：会话/请求/每帧元数据│
│  ├─ pipeline/ RAW 管线：黑电平→去马赛克→降噪→tonemap │
│  ├─ dng/      DNG writer + 元数据模型              │
│  ├─ codec/    JPEG/UltraHDR 编码 · MediaCodec 录制  │
│  ├─ preview/  GL/Vulkan 预览渲染链                 │
│  ├─ device/   ★ 机型抽象层：IOpticDevice + quirks  │
│  └─ util/     线程池 · 环形缓冲 · libyuv 封装       │
├─────────────────────────────────────────────────┤
│  NDK：libcamera2ndk · EGL/Vulkan · MediaCodec     │
│  第三方：libyuv · Halide(可选) · libultrahdr        │
└─────────────────────────────────────────────────┘
```

### 2.1 关键设计原则

- **快门路径零阻塞**：按下快门只入队请求，处理/编码/写盘全部在独立线程；UI 线程永不等待 IO。
- **每帧元数据跟图走**：`CaptureResult` 元数据与图像缓冲绑定生命周期，RAW 处理与 DNG 写入都依赖它。
- **机型差异只进 `device/` 层**：任何 `if (xiaomi)` 禁止出现在 capture/pipeline 内，一律表现为 `IOpticDevice` 接口回调或 quirk 表。
- **UI 无 Java 源码**：NativeActivity + `android_native_app_glue`；需要调框架 API 时在 C++ 内通过 JNIEnv 调用（无 .java/.kt 文件）。若某 API 复杂度失控，允许引入极薄 Kotlin shim（见决策点 D1）。

### 2.2 机型扩展层设计（项目核心卖点）

```cpp
// device/IOpticDevice.h（示意）
struct IOpticDevice {
    virtual ~IOpticDevice() = default;
    virtual DeviceInfo probe(ACameraManager*) = 0;   // 能力探测与裁剪
    virtual void applyQuirks(CaptureConfig&) = 0;    // 流配置/方向/时序修正
    virtual std::string tuningFileFor(const SensorInfo&) = 0; // per-sensor tuning
    virtual std::span<const ExtraFeature> extraFeatures() = 0; // 机型独有特性
};
// 工厂按 ro.product.device 匹配，未命中回退 GenericDevice
```

配套每机型一份 `device/xiaomi17pro/tuning.json`（参考 rpicam-apps 的 per-sensor tuning 模式）。

---

## 3. 第三方库与许可

| 库 | 用途 | 许可 | 引入方式 | 优先级 |
|---|---|---|---|---|
| libyuv | YUV/RGBA/旋转/缩放 | BSD | 源码编译 | P0 |
| Adobe DNG SDK | DNG 写入备选 | Adobe 免费许可（可商用，需保留声明） | 仅在自研 writer 覆盖不足时 | P2 |
| timothybrooks/hdr-plus | HDR+ burst 算法参考实现 | MIT | 移植算法思想，代码作参考 | P1 |
| Halide | 管线 kernel 生成（NEON/GPU） | MIT | 源码 | P1 |
| google/libultrahdr | Gain Map HDR JPEG | Apache-2.0 | 源码编译 | P1 |
| LibRaw | RAW 解码（仅开发期比对工具用） | LGPL/CDDL | 不进 app | P2 |

**许可红线**：Open Camera（GPL-3）、PhotonCamera（GPL-3）、darktable（GPL-3）**只作行为与架构参考，禁止复制任何代码**进本项目。可复制代码的只有 MIT/Apache/BSD 项目（GrapheneOS Camera、hdr-plus、camera-samples 等）。

---

## 4. 里程碑计划

工期估算按 1 名全职开发（含测试），仅作排期参考；每个里程碑有明确退出标准。

### M0 — 真机能力验证与工程骨架 【P0，1 周】✅ 完成（2026-09-28）

> **状态（2026-09-28）：核心已完成 ✅**
> - 0.1 ✅ NDK r30 + CMake 3.31.6 工程跑通（无 Gradle：CMake→aapt2→zipalign→apksigner，脚本 tools/）
> - 0.2 ✅ 《17 Pro 能力报告》见 doc/devices/xiaomi17pro/CAPABILITY_REPORT.md（LEVEL_3 + RAW 全量可用）
> - 0.3 ✅ 架构冻结：硬件 ISP 直出 RAW；多摄物理 id 经 ACAMERA_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS 读取
> - 0.4 ✅ 目录骨架 + libyuv 待接（M1 接入）
> - 0.5 ◐ 打点框架已随 CameraEngine 落地（fps/会话时延），延迟预算表 M1 补
> - 遗留：D1（纯 native vs 薄 Kotlin shim）、D4（GL vs Vulkan）待拍板，不阻塞 M1 开工

一切架构决策取决于 Xiaomi 17 Pro 的实际 Camera2 能力，必须最先做。

| # | 任务 | 优先级 |
|---|---|---|
| 0.1 | 搭建 NDK (r27+) / CMake 工程，NativeActivity 空壳跑通，CI 出 arm64-v8a 包 | P0 |
| 0.2 | 真机验证清单（见 §5），产出《17 Pro 能力报告》文档（V1–V7、V9 ✅；V8/V10 顺延 M1/M6） | P0 |
| 0.3 | 根据 0.2 结论冻结架构：硬件 ISP 直出 RAW 是否可用、是否需自建 demosaic | P0 |
| 0.4 | 目录骨架 + 依赖接入脚本（libyuv 先行） | P0 |
| 0.5 | 建立性能基准脚手架：快门延迟/预览帧率打点框架 | P1 |

**退出标准**：空 app 在 17 Pro 上以 NDK 打开相机出预览；能力报告完成；架构决策（D2/D3）落文档。

### M1 — 采集核心 【P0，2–3 周】

| # | 任务 | 优先级 |
|---|---|---|
| 1.1 | `capture/`：CameraDevice/Session RAII 封装，错误恢复（断流重连） | P0 |
| 1.2 | 预览链：YUV_420 → (libyuv) → GL 纹理 → SurfaceView，目标 30/60fps 零丢帧 | P0 |
| 1.3 | 手动控制：ISO/曝光时间/对焦距离/白平衡，请求队列异步下发 | P0 |
| 1.4 | 多摄管理：logical camera 枚举、physical id 切换、变焦平滑策略 | P0 |
| 1.5 | RAW ImageReader 通路：RAW_SENSOR 缓冲 + CaptureResult 元数据绑定 | P0 |
| 1.6 | ZSL（零快门延迟）环形缓冲：常驻 N 帧缓存，快门即回溯取帧 | P1 |
| 1.7 | 会话延迟优化：shared session、最小 stream 组合切换 | P1 |

**退出标准**：预览流畅无丢帧；手动参数实时生效；RAW 帧能取出且元数据完整；快门回溯可用。

### M2 — RAW/DNG 输出链路 【P0，2 周】

| # | 任务 | 优先级 |
|---|---|---|
| 2.1 | `dng/` 元数据模型：静态（色彩矩阵/黑电平）+ 动态（每帧行噪声/时间戳） | P0 |
| 2.2 | DNG writer：TIFF 基础 + DNG 版本 tag + SubIfds，最小实现先保正确 | P0 |
| 2.3 | 机身方向/EXIF/GPS 写入 | P0 |
| 2.4 | 校验闭环：Adobe DNG Validator + Lightroom/达芬奇打开比对（连拍 100 张无坏文件） | P0 |
| 2.5 | 半透明水印/版权 tag、厂商 opcode（LensShadingMap 可选） | P2 |

**退出标准**：DNG 在 Lightroom 中色彩正确（与厂商相机 RAW 直出比对）、元数据无告警。

### M3 — 极速体验打磨 【P0/P1，2 周】

| # | 任务 | 优先级 |
|---|---|---|
| 3.1 | 快门延迟预算表（按帧计时：入队→取帧→回显 ≤ 目标值） | P0 |
| 3.2 | 连拍：burst 队列 + 后台落盘不阻塞预览 | P0 |
| 3.3 | 冷启动 < 目标秒数（懒加载全部非关键初始化） | P1 |
| 3.4 | 双流水：预览同时出 RAW+分析流（统计/直方图低分辨率通路） | P1 |

**退出标准**：达到 §6 性能指标表中的快门与连拍指标。

### M4 — 计算摄影（画质护城河）【P1，3–4 周】

| # | 任务 | 优先级 |
|---|---|---|
| 4.1 | RAW burst 对齐（金字塔光流，参考 hdr-plus） | P1 |
| 4.2 | 时域降噪合并 + 保守 merge（运动区域回退） | P1 |
| 4.3 | Tone mapping / 逆伽马输出（HDR+ 式色调映射） | P1 |
| 4.4 | Halide 移植热点 kernel；量化：1080p 一帧合并耗时 | P1 |
| 4.5 | 夜景模式（长曝光序列 + 合并）与手持防抖裁切 | P2 |

**退出标准**：暗光 8 帧 burst 合并画质显著优于单帧；耗时在目标内（后台完成，不阻塞下一拍）。

### M5 — 视频与 HDR 静态图 【P1，2 周】

| # | 任务 | 优先级 |
|---|---|---|
| 5.1 | MediaCodec NDK 录像（H.264/HEVC，10bit 探测） | P1 |
| 5.2 | libultrahdr：单帧 RAW → UltraHDR JPEG（SDR/HDR 双增益图） | P1 |
| 5.3 | 音频（AAudio）+ 音画同步 | P2 |

### M6 — 机型层完善与第二机型验证 【P1，1–2 周】

| # | 任务 | 优先级 |
|---|---|---|
| 6.1 | quirk 体系实战化：把 17 Pro 上发现的全部怪癖收进 device/ 层 | P1 |
| 6.2 | GenericDevice 回退实现，在一台非小米机型上验证「仅靠配置」跑通 | P1 |
| 6.3 | Camera Extensions（ACameraExtensionSession）真机可用性验证（夜景/HDR 扩展） | P1 |
| 6.4 | 17 Pro 背屏：multi-display API 验证，可用则做背屏取景/控制 | P2 |

### M7 — 专业 UI 与工具 【P2，2–3 周】

| # | 任务 | 优先级 |
|---|---|---|
| 7.1 | 直方图（实时）、斑马纹、对焦峰值、网格 | P2 |
| 7.2 | RAW+JPEG 双格式、间隔拍摄、RAW burst 单张导出 | P2 |
| 7.3 | 设置持久化、按机型记忆参数 | P2 |

---

## 5. Xiaomi 17 Pro 真机验证清单（M0 核心，产出《能力报告》）

```
adb shell dumpsys media.camera        # 全量能力导出，归档到 doc/devices/xiaomi17pro/
```

逐项确认并记录：

| # | 检查项 | 影响 |
|---|---|---|
| V1 | `REQUEST_AVAILABLE_CAPABILITIES` 含 RAW_SENSOR？支持 RAW10/RAW16/PaHDR？ | 决定 RAW 通路（0.3） |
| V2 | 硬件级别是否 `LEVEL_3`？最大 RAW stream 数？ | ZSL+RAW 并行是否可行 |
| V3 | 黑电平：`BLACK_LEVEL_PATTERN` 值；RAW 帧黑电平是否已扣（拍暗场直方图验证） | DNG 正确性 |
| V4 | LSC：`STATISTICS_LENS_SHADING_MAP_MODE` 可用性 | DNG OpcodeList 方案 |
| V5 | logical/physical camera 结构、各 physical 的 RAW 支持 | 多摄策略 |
| V6 | 三方 app 下传感器方向/镜像是否正确（历史机型有翻转 quirk） | quirks 表 |
| V7 | 手动传感器的曝光时间/ISO 实际范围与生效延迟 | 手动模式设计 |
| V8 | `ACameraExtensionSession` 扩展（夜景/HDR）是否注册 | M6.3 |
| V9 | 第二背屏 display id 对三方 app 可见性；Presentation 能否投递 | M6.4 |
| V10 | 快门延迟实测基线（官方相机 vs NDK 最小 demo 对比） | 性能预算 |

风险预案：若 V1/V2 受 HyperOS 限制（历史机型需开 HAL3 prop 或 root），记录并评估「降级方案 = 自建 demosaic 管线」，这是最大的架构分叉点。

---

## 6. 性能指标（M3 退出门槛）

| 指标 | 目标 |
|---|---|
| 快门点击 → 捕获完成 | ≤ 100ms（ZSL 生效时感知为 0） |
| 预览帧率 | 30fps 稳定，支持 60fps 时零丢帧 |
| 连拍 | RAW burst ≥ 8 帧无丢帧 |
| 单张 RAW 落盘 | 后台 ≤ 300ms，UI 零卡顿 |
| 冷启动 → 可拍 | ≤ 1.5s |
| 1080p burst 合并（M4） | ≤ 2s/张（后台） |

---

## 7. 决策点（需要拍板，已给推荐）

| # | 决策 | 推荐 | 备注 |
|---|---|---|---|
| D1 | UI 壳层：纯 native（NativeActivity）vs 薄 Kotlin shim | **先纯 native**；仅在框架 API 通过 JNIEnv 调用过重时引入最小 shim | M0 可定 |
| D2 | DNG writer：自研最小实现 vs Adobe DNG SDK | **先自研**（我们产出元数据子集可控），SDK 作后备 | 影响许可合规负担 |
| D3 | 管线实现：手写 NEON vs Halide | **先手写保正确**，M4 用 Halide 重写热点 kernel | |
| D4 | 预览渲染：GL ES vs Vulkan | **GL ES 起步**（预览≠渲染重负载），Vulkan 仅在需要高级后处理时评估 | |

---

## 8. 风险登记

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 17 Pro 三方 RAW/LEVEL_3 受 HyperOS 限制 | 中 | 架构分叉 | M0 第一周验证；备选自建 demosaic |
| RAW 元数据缺失导致 DNG 色偏 | 中 | 核心卖点受损 | M2 校验闭环 + 与官方相机 RAW 盲比 |
| 背屏/厂商扩展不对三方开放 | 中 | 定制特性缩水 | 已列为 P2，不阻塞主线 |
| 画质追不上 GCam/官方 | 高 | 竞争力 | 定位差异：极速+全手动+开放元数据；burst 合并逐步逼近 |
| GPL 代码污染 | 低 | 法务 | §3 红线；CI 可加许可证扫描 |
| 独占档期（小米 OTA 变更行为） | 低 | 回归 | M6 quirk 表 + 每次大版本回归清单 |

---

## 9. 目录结构（M0.4 落地）

```
Vicold.Optic/
├─ doc/                    # 本计划、能力报告、架构决策记录(ADR)
│  └─ devices/xiaomi17pro/
├─ app/                    # NativeActivity 壳 + android_native_app_glue
├─ core/
│  ├─ capture/  pipeline/  dng/  codec/  preview/
│  ├─ device/              # IOpticDevice + generic/ + xiaomi17pro/
│  └─ util/
├─ third_party/            # libyuv, libultrahdr, halide(可选)
├─ tools/                  # RAW 比对脚本、性能打点分析（桌面 Python）
└─ CMakePresets.json
```

## 10. 参考项目（详见对话研究结论）

- 采集：android/ndk-samples(camera)、android/camera-samples(Camera2Raw)
- 管线：timothybrooks/hdr-plus (MIT)、libyuv、libultrahdr、Adobe DNG SDK
- 架构：raspberrypi/rpicam-apps（per-sensor tuning 模式）、libcamera
- 行为参考（GPL 禁抄码）：Open Camera、PhotonCamera；MIT 可借鉴：GrapheneOS/Camera
