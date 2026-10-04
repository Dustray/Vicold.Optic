# 静态照片拍摄管线

> 适用范围：JPEG / RAW(DNG) 两条输出通路的**快门之后**发生的事。
> 预览与变焦分带见 `devices/xiaomi17pro/CAM_PATHS.md`；UI 见 `UI_LAYOUT.md`。
> 最后更新：2026-10-01（JPEG 通路 + WYSIWYG 真机三带验证通过）

## 1. 两条互斥的输出通路

会话一次 configure，拍摄输出按格式二选一挂载（互斥，避免 6 流组合被 HAL 拒）：

| 模式 | 挂载输出 | 数据源 | 落盘 |
|---|---|---|---|
| **`jpgMode_` = true（默认）** | JPEG 流 4096×3072 + uw 物理 JPEG 流 | 逻辑主摄 / 超广物理流 | `StillCapture` → MediaStore → `DCIM/Camera` |
| **`fmt=raw`**（controls.txt） | RAW_SENSOR 流 | 逻辑主摄 | `RawCapture` → DNG → 应用私有目录 |

`core/capture/` 相关文件：

```
StillPipeline.h     StillFrame / StillParams / StillProcessor 接口
StillProcessor.cpp  PassThroughProcessor、WysiwygCropProcessor（内含 stb 实现）
StillCapture.{h,cpp} JPEG reader + expectShot 登记 + 异步 saver 线程
RawCapture.{h,cpp}  RAW(ZSL ring once) + DNG 写出
Gallery.{h,cpp}     JNI MediaStore 写入
stb_image*.h        vendored 单头文件 JPEG 解码/编码（public domain）
```

## 2. 铁律：JPEG 流绝不能进 repeating 请求

**曾经的真实事故**（2026-09-30）：把 JPEG 输出挂进常驻 repeating，等于让 ISP **每一帧都编码一张 12MP JPEG**。

| 指标 | JPEG 在 repeating | JPEG 单拍（现行） |
|---|---|---|
| 预览帧率 | 18.5 fps | **30.1 fps** |
| provider 进程 CPU | 315% | 正常 |
| >70ms 断帧 | 345 次 | 0 |

RAW 之所以能常驻，是因为它是 DMA 直传、 zero-copy 廉价的；JPEG 需要 ISP 走完整编码流水线。
**结论**：JPEG 只在 session 配置里出现，快门时才通过 `captureOnce` 下发一次单拍请求。

## 3. 单拍模型：expectShot 登记制

repeating 不带 JPEG 流 ⇒ **任何到达 JPEG reader 的帧必然是单拍产物**，无需在帧回调里区分来源。

```
快门 → expectShot(StillParams)  登记一次快照（含 zoom / appliedZoom / ISO / 曝光 / EV）
     → session_->captureOnce({jpegWindow}, settings)
让 ● 帧到达且与登记配对 → processor->process() → saver 线程 → MediaStore
     ● 未登记的帧 → 直接丢弃
```

健壮性设计：

- **6s 登记超时作废**：物理流单拍若被 HAL 静默拒绝交付，`inFlight_` 会永远挂 1，UI 的 SAVING 角标卡死。超时即作废并归零计数。
- **文件名 seq 用全局原子**：两路 JPEG reader（逻辑 + uw）在同一毫秒内会撞 `IMG_<ms>_000.jpg`，曾用实例内序号导致第二张被覆盖。
- **once 请求释放用 sequenceId**：NDK 不保证回调里的 `ACaptureRequest*` 等于提交指针，用指针比较会让 `onceReq_` 永不释放 ⇒ 单拍只能拍一张（`onCaptureSequenceCompleted` 里匹配 `onceSeq_`）。

## 4. WYSIWYG：照片 FOV 必须等于预览 FOV

### 4.1 曾经的症状

单拍 JPEG 恒走逻辑主摄流，而逻辑流的 zoom 被钳在融合管线安全带 **≤4.85**（见 `CAM_PATHS.md` §2）。于是：

| 变焦带 | 预览实际显示 | 修复前的照片 | 偏差 |
|---|---|---|---|
| 长焦带 z=10 | 长焦物理流 + GL crop → 10x | 主摄 4.85x | **照片比预览广一倍** |
| 超广带 z<1 | uw 物理流 0.774x | 主摄 1.0x | 照片明显更窄 |
| 主摄带 z≤4.85 | 一致 | 一致 | — |

因为主摄带本来就没问题，所以表现为用户描述的"**有时候**照片焦距跟预览不一样"。

### 4.2 两层修复

**① 超广物理 JPEG 流**（第五路输出，`ACaptureSessionPhysicalOutput` 绑物理 3）
超广带快门直连超广，`appliedZoom = 0.774`，照片就是原生超广视野。
> 会话 5 输出真机一次创建成功；若被 HAL 拒绝，拆掉 uw JPEG 重试 4 路并记忆失败，**绝不直接砸进单流降级**。

**② `WysiwygCropProcessor`**（兜底）
`StillParams` 记录两个值：`zoom`（用户导轨值，= 预览 FOV）与 `appliedZoom`（照片实际出图倍率）。
当 `zoom / appliedZoom > 1.002` 时做软件中心裁切：stb 解码 → 按倍率比取中心区域 → stb 重编码（质量 92）。差异 ≤0.2% 直通，**保留 HAL 直出码流的 EXIF 与画质**。

真机验证（均落入 `DCIM/Camera`）：

| 场景 | appliedZoom | 结果 |
|---|---|---|
| z=10（长焦带） | 4.85 | 裁切到 **1986×1488** = 4096×4.85/10 精确，FOV=10x |
| z=0.7（超广带） | 0.774（uw 物理流） | 6429KB 全幅直通 |
| z=1.0（主摄带） | 1.0 | 7367KB 直通，无重编码 |

**已知代价**：触发裁切的帧经过软重编码，**EXIF 丢失**（仅 z>4.85 发生；MediaStore 的拍摄时间由 GalleryWriter 补写）。
彻底保留长焦光学优势需要给长焦也配独立 JPEG 流（当前未做）。

## 5. 可插拔处理阶段（滤镜系统的预留口）

```cpp
class StillProcessor { virtual bool process(StillFrame&) = 0; };   // false = 丢弃该帧
```

- `StillCapture::setProcessor()` 可替换；当前默认挂 `WysiwygCropProcessor`。
- `PassThroughProcessor` 保留作对照/降级。
- 后续接滤镜时：reader 换 `YUV_420_888` 通路，`StillFrame::Fmt::Yuv420` 已有枚举位；实时滤镜可复用 GL shader 做预览、快门时同一算法走离屏渲染。
- `StillParams` 携带快门时刻的全部用户设置快照（zoom/ISO/曝光/EV/AE/AWB，后续加滤镜 id/强度/LUT/水印），处理阶段据此生效。

## 6. 落盘与反馈

- `GalleryWriter`：JNI → MediaStore 两段提交（`IS_PENDING=1` → 写流 → `IS_PENDING=0`），无需存储权限，系统相册可见。失败降级到应用私有 `jpg/` 目录。
- `ContentValues.put()` 返回 `void`，JNI 签名是 `)V` 不是 `)I`（曾静默失败）。
- **UI 反馈一律走 HUD 角标**：保存中 → 橙色 `SAVING`，写完即消失；只有失败（配额用尽）才短暂显示提示。预览区**不允许**再有浮层 toast。
- 每启动配额默认 `saveQuota_ = 8`（设置面板「照片质量 → 连拍配额」可改为 1/4/8/不限；
  `controls.txt: save_quota=N` 同样可调，面板与配置文件都是绝对值语义）。

## 7. 遗留

- [ ] 长焦独立 JPEG 流（拿到真正的长焦光学画质，而非主摄高倍裁切）
- [ ] 裁切帧的 EXIF 保留（把解码前码流的 APP1 段搬到重编码结果里）
- [ ] 软编码耗时 ~1s（12MP），考虑改 HAL `JPEG_ORIENTATION`/crop 或 SIMD 编码
- [ ] 超广带 RAW 仍不可得（HAL 后置 RAW 仅逻辑主摄 1 条流）
