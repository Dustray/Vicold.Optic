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
- 变焦导轨 0.7–120（kZoomStops={0.7,1,2,5,10,50,120}，**刻度等距**分段映射，段内对数插值，
  吸附按轨道段位距离 0.13 段）：sub-1.0 经引擎自动切超广角物理直连（P:3）
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
- **az 必须与显示帧时间戳对齐（2026-09-30 傍晚定稿，泵动根治）**：az[0] 不能由
  「最新结果」直接驱动 —— 结果领先显示纹理 1-2 帧，快拖时 crop 配旧纹理 = 显示 FOV
  在两焦距间泵动（1–5x「不丝滑/来回闪」真根因；指数平滑治标引入过冲，已删）。
  正解：结果线程只入 (ts, zoomRatio) 环（64 深）；Gl::acquirePreview 消费新帧时
  AImage_getTimestamp → azForSlot(slot, ts) 查环写回 UI；crop 直接落位。
  az×crop ≡ zoom_ 恒成立，与节流/延迟全解耦。slot1/2 恒带基常量。
  **pandora HAL 变焦是连续的**（az 1:1 跟随 zoom），旧「离散台阶式」结论错误。
  相机侧限速（小步进逼近目标 zoom）是备用的下一步手段。
- 2–5x 拖动中画面中心向长焦侧偏移 = **固件多摄视差补偿**（水平放置长焦在主摄右侧，
  放大时向长焦偏移衔接切换），刻意设计不修；SCALER_CROP_REGION 恒全幅，元数据看不出。
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
  4.85–5.0 由 GL crop 补足）。导轨量程 0.7–120，关键焦 0.7/1/2/5/10/50/120（等距刻度）；高倍段全靠
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
  **跨带切换闪帧根治（2026-09-30 晚）**：azForSlot 对物理 slot 用继承模型
  `az = 带基 × logicalZoomAt(帧ts)`（同环查询）；Ui 切显示源加「原生 FOV 就绪门」
  （目标 slot 匹配 az ≤ 带基×1.5 才翻转+淡化，未就绪旧 slot 继续 GL 补偿）。
  切换瞬间显示的长焦帧是切前捕获的继承裁切帧（~24.3x），用常量带基配 crop 必闪帧。
  **quirk 之三（2026-09-30 真机实证）**：per-physical **曝光键 CamX 照常执行**（与被忽略的
  ZOOM/CROP 相反！）——AE_MODE/AWB_MODE(u8)、EV(i32)、SENSOR_SENSITIVITY(i32)、
  EXPOSURE_TIME(i64) 逐摄下发后三镜头曝光统一。要求请求必须 withPhysicalIds 声明物理成员
  → allPhysZooms() **恒**返回成员表（perKey=false 时 rel=1/crop 空即不写 ZOOM/CROP），
  BandReq.declaredIds 变化时强制重建请求。逐摄键返回码必查（部分 HAL 静默失败）。
  跨带切换修复（2026-09-30，当晚被继承模型+就绪门取代，见上）：淡化旧层用自己
  zoom_/az_旧 的 crop（Gl::drawPreview 有 per-call zoom 参数）。
  `crop = zoom_/az` 方向正确勿再改：shader 显示倍率 = az×previewZoom_。
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
- 真机截图：`export MSYS_NO_PATHCONV=1; adb shell screencap -p /sdcard/x.png`（**不带 -d**，
  默认抓前台主屏；`-d 0` 在该 MIUI 报 invalid），pull 后可远程诊断 UI——勿再信"相机界面截不了图"。
- **文字渲染硬规则（2026-09-30 截图确诊）**：① `Gl::text()` 必须用 **GL_TRIANGLES**（每字形
  独立 6 顶点）——多字形串一条 TRIANGLE_STRIP 会生成字形间"对角连接三角形"= 斜坡锯齿连线
  （曾连误诊三次：图集串扰→VF 连笔→才找到 strip 真因）；② 字体必须是**静态 TrueType(glyf)**：
  `bakeFont` 探测所有候选并自动选中文覆盖最高的（pandora 最终 = `/product/fonts/FZFWZhuZiAYuanJWB.TTF`
  cjk=23/23）；MiSansVF.ttf 是 VF（stb 不支持 gvar）、NotoSansCJK.ttc 是 CFF（InitFont 失败）、
  MiSansC_3.005.ttf 仅西文子集（cjk=0/23）。UI 放大系数 `kUiZoom=1.5`、烘焙 56px。

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

## UI 补充（2026-09-30）
* **窗口 150px 黑条 = MIUI displayCutout 保留**（窗口 frame=[150,0][2656,1220]），
 挖孔真身仅 74px 宽。**全出血已修复（勿再误诊 MIUI 无视）**：`windowLayoutInDisplayCutoutMode`
 必须写在主题 style 里（activity manifest 属性不合法、被静默忽略）——
 OpticTheme@styles.xml + package.sh aapt2 compile res + JNI setAttributes(3) 三件套，
 真机窗口 2656 全宽。窗口尺寸变化重建仍走 engine stop→attach→start。
* **触感**：core/ui/Haptics（JNI→Vibrator，EFFECT_CLICK=按钮/EFFECT_TICK=落档），
 manifest 需 VIBRATE 权限；android_main 里 ui.setJni() 注入。
* **文字垂直居中**：用 `Gl::textCenterTop(utf8, px, cy)`（按字形墨迹 bbox），
 勿再用 fs×系数估算——Gl::text 的 y 是 ascent 行顶，估算法必然偏。
* **布局现值**：轨道 x92、预览 x198（右侧间隙 +22）、EV 面板 x222、AF x626；
 Gl::attach 同窗口尺寸变化也重建（视口跟上）。

## 真机调试铁律（2026-10-01，血泪）
* **绝不 force-stop/-S 本 app**：MIUI 会在 app 停止时把 activity component 置 disabled
 （`pm enable component` 被 SecurityException 拒，无 root 无解）→ am start 全报
 "Activity does not exist"。被冻结后的恢复链（全程不碰 force-stop）：
 `am stack list` 找 taskId → `am stack remove <tid>` 清僵尸 task →
 `pm install-existing --user 0 com.vicold.optic` 复位 component →
 `am start -f 0x10008000 -n com.vicold.optic/android.app.NativeActivity`。
 僵尸 task 症状："brought to the front" 但 ps 无进程。勿用不带 --user 的
 install-existing（会装进 user 10 隐私空间弹权限框卡前台，需 `pm uninstall --user 10` 清）。
* **冷重启（改 controls.txt 后需重启生效时）**：`input keyevent 3`(HOME) → 等 8-12s
 → `am kill com.vicold.optic`（只杀安全后台进程，**不触发 MIUI 冻结**）→ 改配置 →
 `am start -f 0x10008000`。am kill 在 HOME 后立刻执行会因"不安全"静默不杀，必须等 idle。
* **GPU 频率/busy 节点被 SELinux 全域锁死**（kgsl、/sys/kernel/gpu、devfreq 对 shell 也
 denied）——HUD GPU freq/busy 显示 -- 是设备限制；CPU/GPU 温度与 CPU 频率可读。
 thermal_zone 的 `cpu-hw-trip-*=95000` 是降频阈值非实测温度，取 max 会误读 95°C；
 真实热点看 cpu-0-0-1/gpuss-0/camera-0。
* 预览 HUD：FPS + SysMon（core/ui/SysMon，CPU/GPU 频率温度，与 fps 同步 2Hz）。
