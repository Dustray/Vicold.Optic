# Vicold.Optic 开发计划

> 专业拍摄 · 极速 · RAW · 按机型深度定制的 Android 相机应用
> 首发定制机型：Xiaomi 17 Pro（保留其他机型扩展接口）
> 核心代码：C++（除前端壳层）；不使用 Java 源码

版本：v0.4（2026-10-04）
优先级定义：**P0** = 阻塞主线的必做项 | **P1** = 重要，紧跟 P0 | **P2** = 增强项，可延后

> **配套文档**（细节不重复进本文件）
> - 机型扩展：`doc/DEVICE_ABSTRACTION.md`（IOpticDevice 契约、新增机型步骤、quirk 清单）
> - 屏幕/交互：`doc/UI_LAYOUT.md`（设计空间、挖孔禁区、布局常量表、文字渲染与验证方法）
> - 拍摄输出：`doc/STILL_PIPELINE.md`（JPEG/RAW 两通路、WYSIWYG 裁切、相册写入）
> - 多摄/变焦：`doc/devices/xiaomi17pro/CAM_PATHS.md`（分带机制、ALL 常驻会话、HAL quirks）
> - 能力基线：`doc/devices/xiaomi17pro/CAPABILITY_REPORT.md`（M0.2 产出）

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

实际工程结构（截至 2026-10-01，无 Gradle）：

```
┌──────────────────────────────────────────────┐
│  app/  NativeActivity 壳（无 Java/Kotlin 源码） │
│        需框架 API 时走 JNI：相册/电池/触感     │
├──────────────────────────────────────────────┤
│  core/（C++20，静态库）                        │
│  ├─ capture/  libcamera2ndk 封装：设备/会话/   │
│  │            请求/RAW/JPEG 拍摄管线/设置      │
│  ├─ ui/       Gl（EGL/GLES2 底座）+ Ui（布局） │
│  │            + Battery / Haptics             │
│  ├─ dng/      自研 TIFF 构建器 + 元数据模型    │
│  ├─ device/   ★ 机型抽象层 IOpticDevice +      │
│  │            generic/ + xiaomi17pro/ quirks  │
│  └─ util/     Log / ControlFile / Gallery     │
├──────────────────────────────────────────────┤
│  NDK：camera2ndk · EGL/GLES2 · AImageReader    │
│  第三方：stb_truetype · stb_image(_write)      │
└──────────────────────────────────────────────┘
```
（原计划的 `pipeline/ codec/ preview/` 已分别落实为 `capture/Still*`、`capture/Still*`、`ui/Gl`，
第三方依赖从"libyuv + Halide + libultrahdr"收敛为 stb 系列单头文件，见 §3。）

### 2.1 关键设计原则

- **快门路径零阻塞**：按下快门只入队请求，处理/编码/写盘全部在独立线程；UI 线程永不等待 IO。
- **每帧元数据跟图走**：`CaptureResult` 元数据与图像缓冲绑定生命周期，RAW 处理与 DNG 写入都依赖它。
- **机型差异只进 `device/` 层**：任何 `if (xiaomi)` 禁止出现在 capture/pipeline 内，一律表现为 `IOpticDevice` 接口回调或 quirk 表。
- **UI 无 Java 源码**：NativeActivity + `android_native_app_glue`；需要调框架 API 时在 C++ 内通过 JNIEnv 调用（无 .java/.kt 文件）。若某 API 复杂度失控，允许引入极薄 Kotlin shim（见决策点 D1）。

### 2.2 机型扩展层设计（项目核心卖点）

```cpp
// device/IOpticDevice.h（2026-10-01 定稿）
struct IOpticDevice {
    virtual bool matches(std::string_view deviceName, std::string_view marketName) const = 0;
    virtual DeviceInfo probe(ACameraManager*, const std::string& cameraId,
                             ACameraMetadata* characteristics) = 0;
    virtual void applyQuirks(std::vector<StreamConfig>& configs) const = 0;

    // 以下承载**无法从 CameraCharacteristics 问出来**的真机实测结论（见 DEVICE_ABSTRACTION.md §1）
    virtual DeviceIdentity   identity() const;        // DNG Make/Model
    virtual ZoomProfile      zoomProfile() const;     // 坏区安全带/接管点/焦段/mm 基准
    virtual NearTakeoverRule nearTakeover() const;    // 近距推迟接管
    virtual PhysQuirks       physQuirks() const;      // 物理流继承裁切 / per-key 是否执行
    virtual SessionPolicy    sessionPolicy() const;   // 槽数/尺寸/流数上限/降级阶梯/帧率
    virtual UiLayoutPolicy   uiLayout() const;        // 挖孔禁区几何
    virtual std::string tuningFileFor(const std::string& physicalId) const;   // M4
};
```

> 维护要点：**有一整类 HAL 行为无法探测**（融合管线坏区、物理流继承逻辑裁切、会话流数
> 上限、近距接管距离、挖孔真实边界）。这些若写在业务代码里，第二台机型会带着 pandora
> 的参数静默出错。默认值一律取「通用保守」而非「首发机型的值」，未确认的能力默认关闭。
> 新增机型的完整步骤见 `DEVICE_ABSTRACTION.md §3`。

---

## 3. 第三方库与许可

**当前实际引入**（均为单头文件、vendored 进源码树，零外部构建依赖）：

| 库 | 位置 | 用途 | 许可 |
|---|---|---|---|
| stb_truetype | `core/ui/` | 字体烘焙（CJK 图集） | Public Domain |
| stb_image | `core/capture/` | JPEG 解码（WYSIWYG 裁切） | Public Domain |
| stb_image_write | `core/capture/` | JPEG 重编码（质量 92） | Public Domain |

**候选（尚未引入）**

| 库 | 用途 | 许可 | 引入方式 | 优先级 |
|---|---|---|---|---|
| libyuv | YUV/RGBA/旋转/缩放 | BSD | 源码编译 | P2 |
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

### M1 — 采集核心 【P0，2–3 周】✅ 核心完成（2026-09-29）

> **状态（2026-09-29）：核心完成 ✅**
> - 1.1 ✅ RAII 封装（CameraDevice/CaptureSession/CameraEngine）+ 断流重连
> - 1.2 ◐ 直写 Surface 预览 30fps 零丢帧 ✅；GL 预览链（顺带解决预览旋转 + 可视 UI 绘制）移入 M1.2 后半段
> - 1.3 ✅ 手动控制（ae/iso/exp_us/af/focus_d/awb/zoom）经 controls.txt 下发，CaptureResult 回读验证
> - 1.4 ✅ 多摄：物理 id 经 `ACAMERA_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS` 枚举（实测 [3 2 4]），zoom=[0.70,10.00]；物理流切换待 M1.4 复测
> - 1.5 ✅ RAW 通路实测通过：`RAW saved 25165824 bytes`（4096×3072×2 分毫不差）+ 元数据绑定（回退配对已生效，iso=89 真实值；精确配对改进移 M2.1）
> - 1.6 ✅ ZSL 环形缓冲 + 快门回溯实测通过（4 帧回溯保存）
> - 1.7 ⏳ 会话延迟优化（未开工）
> - **新增：拍摄按钮**（JNI 子窗口 TYPE_APPLICATION_PANEL，主线程 onWindowFocusChanged 创建）+ 每启动配额（saveQuota=1）+ 一次性命令自消费——真机验证：点按钮 → `shutter triggered (1/1)` → 恰好 4 帧 RAW（iso=89）→ 再点 `quota exhausted`
> - **新增：存储保险丝**（155GB 事故复盘：controls.txt 在 FUSE 上 mtime 抖动导致 zsl_shutter 每轮询重触发；修复 = 内容比对 + 自消费 + 配额）
> - 遗留：RawCapture 字节保险丝（补丁 D）待套；M2.1 落盘时精确配对 ✅（2026-10-01 等待在途结果闭环，见 M2 状态）

| # | 任务 | 优先级 |
|---|---|---|
| 1.1 | `capture/`：CameraDevice/Session RAII 封装，错误恢复（断流重连） | P0 |
| 1.2 | 预览链：YUV_420 → (libyuv) → GL 纹理 → SurfaceView，目标 30/60fps 零丢帧 | P0 |
| 1.3 | 手动控制：ISO/曝光时间/对焦距离/白平衡，请求队列异步下发 | P0 |
| 1.4 | 多摄管理：logical camera 枚举、physical id 切换、变焦平滑策略 | P0 |
| 1.5 | RAW ImageReader 通路：RAW_SENSOR 缓冲 + CaptureResult 元数据绑定 | P0 |
| 1.6 | ZSL（零快门延迟）环形缓冲：常驻 N 帧缓存，快门即回溯取帧 | P1 |
| 1.7 | 会话延迟优化：shared session、最小 stream 组合切换 | P1 |

**退出标准**：预览流畅无丢帧 ✅；手动参数实时生效 ✅；RAW 帧能取出且元数据完整 ✅（iso=89 真实值）；快门回溯可用 ✅。

### M-UI — 原生 GL 专业界面 【P0，2026-09-29】✅ 真机验证通过

> 设计原型 `camera-ui.html`（1560×720 横屏设计空间）的 C++ 实现，无 Java/Kotlin、无 View 层。
> - `core/ui/Gl.*`：EGL/GLES2 底座。预览帧经 AImageReader(RGBA_8888 + GPU_SAMPLED_IMAGE)
>   → AHardwareBuffer → EGLImage → 2D 纹理零拷贝上屏；sdf 圆角矩形（填充+描边）；
>   stb_truetype 烘焙设备字体（MiSans 优先）出 ASCII + 常用 CJK 图集；纯色批 `triangles()`。
> - `core/ui/Ui.*`：设计空间 cover 缩放（等比铺满 + 双向居中裁切），输入反变换命中。
>   左导轨变焦（0.7×–10× 对数 + 档位吸附 + mm/× 切换）、右导轨 ISO/曝光时间、
>   曝光补偿 ±3 EV（中心吸附）、快门（按下缩放 + 白闪 + toast）、HUD chips、
>   实时 RGB 直方图（64-bin，每 16 像素采样）、三分构图网格、AF 框十字标记、时钟/电池。
> - 交互闭环：UI 命令经队列交引擎线程，与 controls.txt 走同一 `triggerBurst()` 配额闸门。
>
> **本轮修复（编译阻塞 + 正确性）**
> - 补 `EGL_EGLEXT_PROTOTYPES`/`GL_GLEXT_PROTOTYPES`、`STB_TRUETYPE_IMPLEMENTATION`：13 处编译错误
> - 着色器 Y 轴翻转：原映射把设计空间 y=0 投到屏幕底部 → 整个 UI 与预览上下颠倒
> - `text()` 顶点 stride 误用 16 字节（两条独立数组应为 8）→ 越界读
> - 预览纹理在 `AImage_delete` 后继续使用 → 补 `AHardwareBuffer_acquire/release`
> - 字体图集行数按 16px 估 → CJK 会静默丢字形，改为按 1em 估
> - 坐标变换缺 `offX_`（横向溢出未居中，绘制与触摸命中不一致）；尺寸误用 `screenY()` 混入偏移
> - 快门配额 1 → 8（1 次/启动无法用于真机评估），并增加 `notifyShot` 回执，
>   toast 如实显示「配额已用尽 n/N」而非假装已保存
>
> **真机验证（2026-09-29，全部通过）**
> - V-UI1：RGBA_8888 预览输出 HAL 接受，零拷贝上屏 30fps ✅
> - V-UI2：uvrot 自动解析正确（着色器 Y 翻转 + 顶点旋转），cover 裁切正常 ✅
> - V-UI3：MiSans CJK 图集渲染正常（16mm 读数、toast 中文）✅
> - V-UI4：叠加层满帧运行，预览不受影响 ✅
> - 导轨变焦 0.7–10 全段可用（依赖多摄分带机制，见 devices/xiaomi17pro/CAM_PATHS.md）✅

#### UI 打磨轮（2026-09-30 ~ 10-01，全部真机截图 + 像素采样验证）

> 细节规范已抽到 `doc/UI_LAYOUT.md`，本处只记结论。
>
> - **真实电池**：JNI `ACTION_BATTERY_CHANGED` sticky 广播取电量/充电态；充电绿 / ≤20% 橙 / 正常白。
> - **全出血**：`windowLayoutInDisplayCutoutMode` 必须写在主题 style 里（manifest 属性会被静默忽略）→ 2656 全宽。
> - **左右遮挡条**：左条 = `attach` 全出血判定被浮点 +0.0002px 误判（改带容差）；右条 = 右面板宽度没到设计右缘（404）。
> - **布局左移**：预览/AF/EV 整体左移 20；变焦轨道受挖孔约束最终定在 X=88 压禁区边界（详见 UI_LAYOUT §2）。
> - **三滚轮统一**：删除 ISO/曝光时间的静态标题只留实时值，三处基线/字号/配色规则拉齐；下方圆按钮统一 60@594。
> - **拍照反馈收敛**：删掉预览区两个浮层 toast，改为 HUD 角标行的橙色 `SAVING`（写完即消失），失败才提示。
> - **EV 面板**：面板加宽到 520（9px/⅓ 档），双击归零。

### M-MC — 多摄分带：物理直连绕开坏损融合管线 【P0，2026-09-29】✅ 真机验证通过

> Xiaomi 17 Pro 逻辑多摄的 ZOOM_RATIO 融合管线存在两个坏区（sub-1.0 超广角段、
> ≥2.63x SAT 长焦段），每 ~1.2s 断流。修法：按变焦自动分带，坏区段走物理摄像头
> 直连（`ACaptureSessionPhysicalOutput` + `withPhysicalIds`），彻底绕开融合。
> 详细机制、根因链与真机数据见 `doc/devices/xiaomi17pro/CAM_PATHS.md`。
> 顺带修复：会话重建 UAF（退役墓地）、AE/AF/AWB TYPE_BYTE、冷启动误入超广角、
> ControlFile 全量重放（松手回弹根因）、看门狗耗尽后整链路重连自愈。

### M2 — RAW/DNG 输出链路 【P0，2 周】◐ 首光完成（2026-09-29）

> **状态（2026-09-29）：DNG 端到端打通 ✅**
> - 2.1 ✅ 元数据模型（StaticMeta/FrameMeta）+ WB gains 捕获（COLOR_CORRECTION_GAINS 进 FrameResult）
> - 2.2 ✅ `dng/DngWriter`：自研 TIFF 构建器（无外部依赖），IFD0(缩略图+SubIFDs)→IFD1(raw CFA)，全必需 tag：
>   DNGVersion/BackwardVersion/UniqueCameraModel/ColorMatrix1/AsShotNeutral/BlackLevel×4/WhiteLevel/
>   DefaultCropOrigin+Size/ActiveArea/CFARepeatPatternDim/CFAPattern/CFALayout/CalibrationIlluminant1/ISO/ExposureTime
> - 2.1 补充 ✅ 元数据配对移至落盘时（根治 66ms 乱序 miss；到达时配对已废弃）
> - 2.1 补充 2（2026-10-01）✅ 快门场景配对等待闭环：ZSL 回溯的是 ring 最新帧，结果仍晚 ~2-3 帧
>   （delta≈99ms）→ 落盘配对改为「结果在途时有界等待 300ms + notify 唤醒」，兜底改取时间最近
>   meta。真机验证：`meta paired after wait` 命中、无 miss 告警、首张延迟仅增 ~80ms。
>   同轮复测 RAW 冷启动首窗口即 30fps（此前 ~9fps 骤降未复现，疑为设备长跑热态）。
> - 2.3 ⏳ 机身方向/EXIF IFD/GPS（Orientation 已按 sensor 90→6 映射；EXIF 子 IFD 待做）
> - 2.4 ⏳ 校验闭环：Python 结构校验 ✅ + demosaic 预览渲染 ✅（场景可辨、文字清晰）；Lightroom 导入 + 连拍 100 张压力、ColorMatrix1 校准（当前为占位单位阵）、hot pixel（max=65535 离群点待查）待做
> - 实测：`dng saved: dng_xxx.dng (25756630 bytes, thumb 512x384)` ×4 = 24MiB bayer + 590KiB 缩略图 + tags

| # | 任务 | 优先级 |
|---|---|---|
| 2.1 | `dng/` 元数据模型：静态（色彩矩阵/黑电平）+ 动态（每帧行噪声/时间戳） | P0 |
| 2.2 | DNG writer：TIFF 基础 + DNG 版本 tag + SubIfds，最小实现先保正确 | P0 |
| 2.3 | 机身方向/EXIF/GPS 写入 | P0 |
| 2.4 | 校验闭环：Adobe DNG Validator + Lightroom/达芬奇打开比对（连拍 100 张无坏文件） | P0 |
| 2.5 | 半透明水印/版权 tag、厂商 opcode（LensShadingMap 可选） | P2 |

**退出标准**：DNG 在 Lightroom 中色彩正确（与厂商相机 RAW 直出比对）、元数据无告警。

### M-JPEG — JPEG 输出 + 系统相册 + WYSIWYG 【P0，2026-09-30】✅ 真机三带验证通过

> 用户：照片默认要普通 JPG，且系统相册里能看到；JPEG 通路要为后续滤镜/自定义算法留出扩展位。
> 完整实现与验证数据见 `doc/STILL_PIPELINE.md`。
>
> - **可插拔管线**：`StillPipeline.h` 定义 `StillProcessor`（滤镜系统预留口），
>   当前挂 `WysiwygCropProcessor`，`PassThroughProcessor` 保留对照。
> - **JPEG 恒不在 repeating 里**：曾挂进常驻请求，ISP 逐帧编码 12MP ⇒ 18.5fps / provider CPU 315%；
>   改「会话里配流、快门才 `captureOnce`」后回到 **30.1fps，断帧 0**。
> - **系统相册**：`GalleryWriter`（JNI MediaStore 两段提交 `IS_PENDING`）→ `DCIM/Camera`，免存储权限。
> - **WYSIWYG**：照片 FOV 严格对齐预览。第五路**超广物理 JPEG 流** + 软件中心裁切兜底 ——
>   修复前长焦带 z=10 拍出主摄 4.85x、超广带拍出主摄 1.0x（用户主诉"有时候焦距跟预览不一样"）。
> - **单拍健壮性**：`onceReq_` 改 sequenceId 释放（指针比较会让第二张永远拍不了）；
>   登记 6s 超时作废；文件名 seq 改全局原子（双 reader 同毫秒撞名）。

### M-LIFE — 全异步生命周期与看门狗自愈 【P0，2026-09-30】✅ 两轮打盹零 ANR

> 用户：从后台返回 app 有时卡死。根因 = 主线程同步 `join` 引擎线程，而 HAL `close` 在 Doze 期间挂起
> （曾触发 `NativeActivity.surfaceDestroyed` 51s ANR）。
>
> - `stop()` 只置标志，收摊由引擎线程在 `run()` 尾部自清理；`requestStart()/tryStart()` 意图 + 重试。
> - 窗口 attach/detach 延后处理（`pendingWin` + `wantAttach/wantDetach`）：GL 预览 reader 在旧会话
>   拆除期间仍被引用，立即 detach 会 UAF。
> - 看门狗状态在 `run()` 入口重置（否则重启后误报 `stall 51958ms`）；12 次重试耗尽 → 整链路重连。
> - 真机：TERM 后 428ms 干净收摊，RESUME 后 20ms 重启，两轮息屏/唤醒零冻结。
> - 遗留：HAL `close` 在深度打盹下仍可能挂起（异步化只保证不冻结 UI，冷启动时若进程已被杀属正常）。

### M-SET — 设置 / 曝光白平衡面板 + 构图辅助 + 自动休眠 【P0，2026-10-03/04】✅ 真机验证通过

> 用户需求：设置入口 + 照片质量/辅助构图/持久化；曝光白平衡独立入口；构图辅助三件套；
> 60s 无操作自动休眠。布局与调试规范见 `doc/UI_LAYOUT.md` §3.1–§3.5、§5.1、§6.2。
>
> **入口与面板**
> - 两个入口为**纯图标圆形按钮**：设置=齿轮（`(128,48)`，左变焦导轨正上方，x 与导轨中线对齐）、
>   曝光白平衡=半黑半白圆（`(1456,80)`，右导轨顶部、快门正上方），直径 60，圆形命中外扩 8px。
> - 面板 2026-10-04 改**全屏**（原 760 小盒控件仅 336 宽，8 段 WB 每段 42px 文字挤成一团）。
>   **关闭按钮复用入口图标位**（设置左上 / 曝光右上），取消「点外部关闭」。
> - 快速变焦圆钮 `5/2/1/0.7` **自上而下递减**，与左导轨同向（长焦在上）。
>
> **逐项体检修掉的 4 个 bug**（2026-10-04）
> 1. **关自动曝光 → 预览全黑**：`SET_AE` 只翻 `aeOn`，而 `CaptureSettings::apply` 在 `!aeOn` 时
>    无条件下发 SENSITIVITY/EXPOSURE_TIME，双自动态下两值为 0 → HAL 按「ISO 0/曝光 0ns」成像。
>    修为关 AE = 双手动 + 用冻结的 `lastAeIso_/lastAeExpNs_` 补有效值；`apply` 加 ISO 100/33ms 兜底；
>    新增 `Ui::setExpAuto()` 让滚轮状态跟随引擎真值源。
> 2. **关持久化形同虚设**：`commitPersist()` 只 `return` 不写，旧文件留在盘上照旧恢复。
>    修为关闭时 `std::remove` 删文件。
> 3. **持久化加载有行序依赖**：`applyAwbPreset()` 强制 `awbOn_=true`，边解析边应用 ⇒
>    同一份配置因行序得到两种 AWB 状态。改为先全量解析再按固定顺序应用。
> 4. **构图辅助线画了但看不见**：`kGrid{1,1,1,0.12}`（12% 白）在亮场景完全隐没。
>    改为近黑描边 + 亮白芯线**双层绘制**（网格线 / 安全框 / 水平仪中心十字）。
>
> **自动休眠**：60s 无操作 → `stopRepeating()`（仅停 repeating，保留会话/纹理/输出），
> 唤醒重发瞬启无黑帧。预览看门狗必须 `!sleeping_` 短路，否则无 result 会被误判 stall 自动重发。
>
> **同时完成**：屏幕触摸对焦三态模式（点击即对焦 / 仅选位置 / 对焦并拍照）；
> 近距接管阈值按系统相机口径重标定（`0.9/1.4m → 0.18/0.30m`，`DEVICE_ABSTRACTION.md` §1/§6）。

### M-FLASH — 闪光灯 【P1，2026-10-04】

> 入口：右导轨顶部、曝光图标左侧（`(1376,80)`，直径 60），点击循环
> **关 → 自动 → 开 → 常亮 → 关**。档位随设置持久化（`settings.txt` 的 `flash=`）。
>
> **camera2 语义（关键）**：主语义落在 `CONTROL_AE_MODE`（ON / ON_AUTO_FLASH /
> ON_ALWAYS_FLASH），**不能只写 `FLASH_MODE`** —— 多数 HAL 在 AE_MODE=ON 时忽略
> `FLASH_MODE=SINGLE`，表现为「开关切了但灯不闪」。只有「常亮」需要 `FLASH_MODE=TORCH`。
> 手动曝光（AE_MODE=OFF）下「开」档改在**单拍请求**写 SINGLE：repeating 上写 SINGLE
> 会让闪光灯每帧放电。详见 `doc/UI_LAYOUT.md` §5.2。
>
> **无闪光灯设备的两道守卫**：`openCamera` 按 `traits.flashAvailable` 清零（含持久化残留），
> `SET_FLASH` 仅在相机已打开（traits 可信）时才拒绝并回推；UI 侧能力为 false 时**不画入口**。
>
> **待真机验证**：4 档 AE_MODE/FLASH_MODE 下发是否正确、常亮是否可见、拍照是否真闪、
> 自动档在未跑 precapture 序列时的表现（可能需要补 `AE_PRECAPTURE_TRIGGER`）。
> 调试键 `controls.txt: flash=off|auto|on|torch`（也接受 0..3）。

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
| 7.1 | 直方图（实时）✅、斑马纹 ⏳、对焦峰值 ⏳、网格 ✅ | P2 |
| 7.2 | RAW+JPEG 双格式 ✅、间隔拍摄 ⏳、RAW burst 单张导出 ⏳ | P2 |
| 7.3 | 设置持久化 ✅、按机型记忆参数 ⏳ | P2 |

> 7.1 的直方图与网格、7.2 的 RAW+JPEG 双格式、7.3 的设置持久化已随 M-SET 一并落地（2026-10-04）。

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

## 9. 目录结构（实际，2026-10-01）

```
Vicold.Optic/
├─ doc/
│  ├─ PLAN.md                  # 本文件（唯一路线图）
│  ├─ STILL_PIPELINE.md        # 拍摄输出管线（JPEG/RAW、WYSIWYG、相册）
│  ├─ UI_LAYOUT.md             # UI 布局/渲染/交互规范
│  └─ devices/xiaomi17pro/     # 能力报告 + 多摄分带 + dumpsys 证据
├─ app/                        # NativeActivity 壳 + android_native_app_glue + res
├─ core/
│  ├─ capture/  设备/会话/请求/设置 + RAW/JPEG 拍摄 + stb vendor
│  ├─ ui/       Gl(EGL/GLES2) + Ui(布局) + Battery + Haptics + stb_truetype
│  ├─ dng/      DngWriter + 元数据模型
│  ├─ device/   IOpticDevice + generic/ + xiaomi17pro/（quirks 唯一出处）
│  └─ util/     Log / ControlFile(controls.txt) / Gallery(MediaStore)
├─ tools/       dev.sh(env.sh) / package.sh + 真机诊断脚本
│               # zoom_scan / stall_probe / logical_limit / calib_fov.py
└─ CMakePresets.json           # 唯一 preset：android-arm64
```

> 约定：注释、文档、commit message 统一**简体中文**；机型判断只允许出现在 `core/device/` 层。

## 10. 参考项目（详见对话研究结论）

- 采集：android/ndk-samples(camera)、android/camera-samples(Camera2Raw)
- 管线：timothybrooks/hdr-plus (MIT)、libyuv、libultrahdr、Adobe DNG SDK
- 架构：raspberrypi/rpicam-apps（per-sensor tuning 模式）、libcamera
- 行为参考（GPL 禁抄码）：Open Camera、PhotonCamera；MIT 可借鉴：GrapheneOS/Camera
