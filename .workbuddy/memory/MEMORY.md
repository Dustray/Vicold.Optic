# Vicold.Optic 项目笔记

## 项目定位
Android 专业相机应用，纯 C++（NativeActivity + android_native_app_glue，无 Java/Kotlin 源码），
首发深度定制机型 Xiaomi 17 Pro（ro.product.device=pandora，Android 16 / API 36）。
核心卖点：极速快门 + 专业 RAW/DNG + 手动全控 + 按机型 quirks 定制。

## 技术栈与构建
- C++20 / CMake 3.22+ / Ninja，唯一 preset：`android-arm64`（ANDROID_PLATFORM=31，c++_static，RelWithDebInfo）
- 无 Gradle：aapt2 link → 塞入 liboptic.so → zipalign → apksigner（tools/package.sh）
- 入口脚本 tools/dev.sh <build|package|install|run|log|shot|report|all>；环境变量在 tools/env.sh（SDK 在 D:/Android/sdk）
- 包名 com.vicold.optic；minSdk 31、targetSdk 34（在 package.sh 硬编码）

## 目录约定
core/capture（采集）、core/device（机型抽象层，禁止在业务代码里写机型判断）、core/util；
doc/PLAN.md 是唯一路线图（M0–M7），doc/devices/xiaomi17pro/CAPABILITY_REPORT.md 是 M0 真机能力结论。

## UI 架构（2026-09-29 新增 M-UI）
- 设计原型 `camera-ui.html` = 1560×720 横屏设计空间；C++ 实现在 `core/ui/{Gl,Ui}.{h,cpp}` + `core/ui/stb_truetype.h`
- `Gl`：EGL/GLES2 底座。预览 AImageReader(RGBA_8888+GPU_SAMPLED_IMAGE) → AHardwareBuffer → EGLImage → 纹理（零拷贝）；
  sdf 圆角矩形；字体图集（ASCII+按需 CJK）；`triangles()` 纯色批
- `Ui`：cover 缩放。三件套必须分清 —— `dim(len)` 纯缩放（尺寸/半径/线宽）、`screenX/screenY` 位置加居中偏移；
  输入用 `toDesignX/toDesignY` 反变换。**EGL 窗口原点在左下、触摸原点在左上**，着色器里已做 Y 翻转，勿再改回
- 变焦导轨 0.7–120（kZoomStops={0.7,1,2,5,10,50,100}）：sub-1.0 经引擎自动切超广角物理直连（P:3）
- **物理摄倍率必须用等效焦距比，不是焦距比**（2026-09-30 重大纠错）：
  `zoom_i = (f_i/sensorW_i) / (f_main/sensorW_main)`。长焦传感器常只有主摄一半宽，
  焦距比能把 5.02x 算成 2.63x。本机真值：**uw 0.774x、main 1.0x、tele 5.016x**
  （等效 18.5/23.8/119.6mm，与 MIUI 相机 17/23/115mm 口径一致）。导轨下限取
  max(HAL zoomMin, uwNative) —— 低于光学极限画不出更广。
- **长焦接管点必须落在逻辑流安全区内**：本机逻辑流 4.8 干净、**5.0 起持续断流**
  （HAL 还把逻辑流 zoom 硬钳在 5.00）→ `teleSwitch_ = min(teleNative, 4.85)`。
  按原生倍率接管会让用户拖到 5x 时画面先冻结再跳变。
- **平滑变焦 = 全带统一 crop 补偿**：`crop = zoom_（手指目标）/ az（相机实际出图倍率）`。
  逻辑带相机下发有 120ms 节流，不补差就是 8 次/s 阶梯跳（卡顿感）。
  **az 变化时 crop 必须落位到新 target，绝不能归一到 1** —— 归一会让 FOV 退回 az 再
  爬升，与纹理过渡叠加 = 泵动闪烁（2026-09-29 全程闪烁元凶）。静止时 az=zoom_ → crop=1。
- 变焦补偿上限：uw 带 crop ≤2.65、tele 带 ≤3.8（数字裁切，预览偏软，DNG 不受影响）
- 线程：glue 线程 = 渲染 + 输入；UI 命令经 `Ui::popCmd()` 交引擎线程，与 controls.txt 共用 `triggerBurst()` 配额闸门
- 每启动快门配额 `saveQuota_ = 8`（曾为 1，无法真机评估）；`controls.txt: save_quota=N` 可调

## 多摄架构（2026-09-29 真机确诊，勿再走弯路）
- 逻辑摄 0 物理成员 [3 2 4]：**3=超广角 2.57mm（等效 18.5mm）、2=主摄 6.62mm（23.8mm）、
  4=长焦 17.42mm（119.6mm，等效比 5.016x）**；
  物理摄不在 getCameraIdList 里，无法独立打开（cam=2 直开必败）。
- **逻辑融合管线有两个坏区，都必须物理直连绕开**：① sub-1.0（超广角融合）② 高倍数字区
  ≥~4（SAT/长焦融合；实测 2.0/3.0 干净、5.0/8.85/10 持续断流）。
- 分带（2026-09-30 定稿，与 MIUI 口径一致）：z∈[0.7,1.0)→uw 流；[1.0,5.0)→逻辑主摄；
  ≥5.0→tele 流（teleSwitch_=min(原生5.016, 5.0)，effSettings 逻辑流仍钳 4.85 安全上限，
  4.85–5.0 由 GL crop 补足）。导轨量程 0.7–120，关键焦 0.7/1/2/5/10/50/100；高倍段全靠
  GL 数字裁切（120x ≈ 23.9 倍裁切），引擎对 UI zoom 的钳制上界必须用导轨量程而非 HAL zoomMax。
  滞回保留：uw 退出 z≥1.03、tele 退出 z≥teleSwitch−0.08。
- **ALL 全目标常驻会话（2026-09-29 深夜定稿，真机验证）**：一个 repeating 请求挂 4 输出永不再换
  （`setRepeatingAll`，withPhysicalIds([3,4]) 创建，逐摄写 ZOOM_RATIO：uw=z/0.774、
  tele=z/5.016、逻辑钳在 [1.0, 4.85]——三路流恒渲染同一用户 FOV）。跨带 = 纯 GL 切显示
  slot + 150ms 交叉淡化，请求零动作。**10 次跨带往返 0 请求重建 0 gap**（换请求方案曾
  118ms/次、重建方案曾 ~285ms+黑帧）。RAW 恒出帧 → 全带可拍。HAL 拒绝则降级单流重建。
  **pandora quirk（Xiaomi17ProDevice::physPerKeyZoom=false）**：CamX 声明支持 per-physical
  ZOOM_RATIO/CROP_REGION 但**实际忽略**（长焦带 3.0↔8.0 画面零差异确诊）→ 不写键、物理流恒
  原生 FOV，带内变焦走 GL crop。逻辑流钳制上界=kLogicalSafeMax(4.85)<teleSwitch(5.0)。
  **quirk 之二（2026-09-30）**：物理流**继承逻辑 ZOOM_RATIO 的裁切**——长焦带若逻辑写 4.85，
  长焦流实际 = 5.016×4.85≈24x。故 tele 带 effSettings 必须把逻辑 zoom 写 1.0。
  近距规则：对焦距离 <near_m(0.9m) → 长焦接管点推迟 tele_near(20x)（对齐系统相机）；
  依据 result 的 LENS_FOCUS_DISTANCE。代价：三传感器常开功耗↑；三路 reader 每帧必须全部
  drain（不消费会撑满队列拖累 repeating）。
- 逐摄键 API：`ACaptureRequest_setEntry_physicalCamera_float(req, physicalId, tag, count, data)`
  —— **physicalId 在 tag 之前**（API 29）。
- 单流降级路径仍走签名换请求（L+R / P:3 / P:4，物理直连无 RAW、拍摄被拒）。
- **会话重建必须走退役墓地**（retired_，延迟 1.5s 析构）：旧 CaptureSession 立即析构会
  UAF（C2N-dev-looper 线程 SIGABRT，destroyed mutex）。
- `activePhysId()`：zoomRatio==0 表示"未设置"，不能当 <1.0（否则冷启动误入直连）。
- 看门狗 12 次重试耗尽 → 全链路重连（不再永久黑屏）。
- controls.txt 键：`uw=1`（默认开）、`uw_phys=N`、`tele_phys=N`、`phys_min=N`、`disp=0/1/2`
  （手动锁显示源）、`tele_native=N`（强制长焦带基，肉眼校准用）。
- 真机诊断脚本（tools/）：`zoom_scan.sh`（逐点核对 eff FOV=az×crop 是否 == z）、
  `stall_probe.sh`（逐点数断流，定位逻辑流安全上限）、`logical_limit.sh`、
  `calib_fov.py`（像素标定；**近景视差下不可靠**，需 >3m 远景）。
- 真机截图必须指定主屏：pandora 有副屏，`adb shell screencap -p -d <主屏id>`，
  否则抓到副屏桌面。

## 已知状态（2026-09-28 更新）
- M0 已完成并已跑通真机预览（30fps，LEVEL_3，RAW 4096x3072，黑电平 64，RGGB，后置 RAW 仅 1 条流）
- M1 代码已全部接入 core/CMakeLists.txt 并编译通过（-Wall -Wextra 零警告，8/8 目标），
  新增 core/capture/{CameraDevice,CaptureSession,CaptureSettings,RawCapture} + core/util/{Log,ControlFile}
- CameraEngine 已重写为 M1 形态：预览直写 Surface + controls.txt 手动控制 + RAW ring(ZSL)/once + 断流重连
- 审查（2026-09-28）发现的未修问题：
  * P0：CaptureSession::onCaptureCompleted 用「回调 request 指针 == onceReq_」判定单拍完成 —— NDK 文档明确回调指针不等于提交的指针，
        导致 onceReq_ 永不释放，once 模式只能拍一张；onCaptureFailed 也未释放。应改用 sequenceId + onCaptureSequenceCompleted/Aborted
  * P1：ANativeWindow 未 acquire/release（M0 旧版有）；AImageReader maxImages=6 < ring4+saveQ 可能耗尽；
        ring 模式 RAW 常驻 repeating 可能拖慢预览帧率；~~close 后 in-flight 回调 UAF~~（2026-09-29 已修：退役墓地）；
        controls.txt 用 unordered_map 遍历顺序不确定；
        ~~ControlFile 每轮新建局部实例导致全量重放~~（2026-09-29 已修：改成员 ctl_；
        该缺陷实际危害 = UI 设置的 zoom 被 controls.txt 旧值 100ms 内盖回）；
        AE/AF/AWB 用 i32 写入是类型错误（2026-09-29 已修：改 setEntry_u8）
  * P2：M0 的 capabilities.txt 探测报告被删除，tools/dev.sh report 工作流失效

## 审查约定（用户指定）
本项目由另一个 AI 写实现，我（本会话）只做**监督审查**：不改代码，只出审查结论 + 修法建议，结论须基于 git 未提交改动与实际编译结果。
例外：2026-09-29 用户明确要求「继续完成 UI 部分」，据此直接实现了 M-UI（core/ui 全部 + CameraEngine 接线）。

## 用户偏好
注释、文档、提交信息统一简体中文；结论需基于项目实际代码与配置。
