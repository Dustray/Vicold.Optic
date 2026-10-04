# Vicold.Optic 项目笔记

## 项目定位与构建
- 纯 C++ Android 专业相机（NativeActivity + glue，无 Java/Kotlin），首发 Xiaomi 17 Pro
 （ro.product.device=pandora，Android 16/API 36，MIUI）。卖点：极速快门 + RAW/DNG + 手动全控 + 机型 quirks 定制
- C++20 / CMake+Ninja，唯一 preset `android-arm64`（PLATFORM=31，c++_static，RelWithDebInfo）
- 无 Gradle：aapt2 link → 塞入 liboptic.so → zipalign → apksigner（tools/package.sh；
  minSdk 31/targetSdk 34 硬编码）。入口 tools/dev.sh <build|package|install|run|log|shot|report|all>，
  环境在 tools/env.sh（SDK 在 D:/Android/sdk）。包名 com.vicold.optic
- 目录：core/capture（采集）、core/device（机型抽象层，业务代码禁止写机型判断）、
  core/ui（GL UI）、core/util。doc/PLAN.md 唯一路线图

## 多摄架构与 pandora quirks（真机确诊，勿再走弯路）
- 逻辑摄 0 物理成员 [3 2 4]：3=超广（等效 18.5mm）、2=主摄（23.8mm）、4=长焦（119.6mm）；
  物理摄不在 getCameraIdList，无法独立打开
- **物理倍率必须用等效焦距比**（非焦距比）：uw 0.774x / main 1.0x / tele 5.016x
- 逻辑融合管线坏区：sub-1.0 与 ≥~4 持续断流 → 物理直连绕开；teleSwitch=min(原生5.016, 用户口径5.0)，
  effSettings 逻辑流钳 4.85 安全上限（4.85–5.0 由 GL crop 补足）
- **ALL 全目标常驻会话**：一个 repeating 挂全部输出永不再换，跨带=纯 GL 切显示 slot+150ms 交叉淡化
  （0 请求重建 0 gap）；RAW 恒出帧全带可拍；HAL 拒绝降级单流重建
- quirks：①per-physical ZOOM_RATIO/CROP 被忽略（带内变焦走 GL crop，crop=zoom_/az 方向勿改）
  ②物理流**继承**逻辑 ZOOM 裁切（tele 带逻辑 zoom 必须写 1.0）③per-physical 曝光键照常执行（逐摄下发统一三摄）
- az 必须按**显示帧时间戳**查结果环（64 深 (ts,zoom) 环）——结果直接驱动会泵动闪烁
- 近距规则（2026-10-04 真机标定）：对焦 **<0.18m 进 / >0.30m 出**（对齐系统相机 20cm 口径）
  推迟长焦接管到 20x；滞回带必须窄（0.12m），宽滞回会把中间距离永久锁死在近距态
  （两计数器清零→既不进也不退）。改 `near_m` 诊断键必须同时清 `nearSubject_`，否则关不掉。
  判定读数用 `lastFdDiopters_` 原子快照，**勿直读 `lastFdUi_`**（跨线程数据竞争）
- **测 focus_d 时 controls.txt 不能带 `af`**：每轮全量重放会把 afOn 重新打开、
  focus_d 因值未变不再触发 → 镜头一直自动追焦、读数卡在 0.47m 永不跟随（曾据此误判镜头物理下限）
- 息屏时启动报 `Camera "0" disabled by policy` 是**息屏导致**（先查 mWakefulness，keyevent 唤醒），
  不是权限问题，appops 怎么 set 都无效
- 三路 reader 每帧必须全部 drain
- 会话重建走退役墓地（retired_，1.5s 延迟析构，防 C2N 回调线程 UAF SIGABRT）
- 逐摄键 API：`ACaptureRequest_setEntry_physicalCamera_float(req, physicalId, tag, ...)`——physicalId 在 tag 前

## UI 架构（core/ui）
- 1560×720 横屏设计空间，cover 缩放。`dim()`=纯缩放、`screenX/screenY`=位置+居中偏移、
  输入用 toDesign 反变换。**EGL 原点左下、触摸原点左上**（shader 已 Y 翻转，勿改回）
- `Gl::text()` 必须 GL_TRIANGLES（每字形 6 顶点）；字体=静态 TrueType(glyf)，
  pandora=FZFWZhuZiAYuanJWB.TTF；**新增任何文案必须把用到的 CJK 字加进 Gl.cpp bakeFont 的
  `extra` 串，否则静默丢字形（空白）**
- 文字垂直居中用 `textCenterTop`（墨迹 bbox），勿用字号系数估算
- 静态覆盖层离屏缓存（稳态 ~4Hz 重烤，GPU 主要杠杆）；**动态元素（AF 框/休眠遮罩）逐帧直画，
  勿 markDirty 钉脏**（否则缓存失效持续发热）
- 布局：预览 x178 宽 960；kUiZoom=1.5；HUD：FPS+SysMon+直方图（热区排除不响应点按）
- **绘制顺序铁律：paintOverlay 里后画的盖先画的**。入口按钮类浮层**必须放在函数末尾**
  （导轨/快门之后），否则被整块不透明导轨面板盖住 → 表现为「命中区在、按钮看不见」，
  调试时只点得动、屏幕上找不到（2026-10-03 踩过）。两条导轨都是整块面板：
  左 `railLX_=88` 宽 80（x88–168）、右 `kRailRX=1156` 宽 404（x1156–1560）
- **浮层底衬用近黑 {0,0,0,0.8} 而非白色低 alpha**：预览画面明暗变化大，
  白 8% 之类的浅色半透明会随背景一起消失。**构图辅助线同坑**（网格/安全框/水平仪
  十字曾用 kGrid 12% 白，亮场景下画了也看不见，2026-10-04 修）：一律
  `kGridEdge{0,0,0,0.55}` 描边 + `kGridCore{1,1,1,0.85}` 芯线双层，线宽≥2 物理px
- **截图坐标 ≠ 真机坐标**：`screencap` 出的图是 2656x1220，但看图时是缩放到 1080 宽的。
  按图上像素直接 `input tap` 会打偏，必须 `design×1.703 + off` 换算。
  面板截图两次字节数完全相同 = 空操作 信号
- **左侧变焦导轨方向=长焦在上**（0.7 在下、120 在上，专业相机惯例）。所有竖排变焦
  UI 必须同向：快速变焦圆钮 `kQzVals={5,2,1,0.7}`（自上而下递减）。
  **排顺序一律改常量数组，不要在 y 计算里翻转** —— 绘制与命中遍历同一组 i，翻转易漏命中
- 窗口全出血：cutout mode 必须写主题 style（manifest 属性被忽略）+ JNI setAttributes(3)
- 触感：core/ui/Haptics（EFFECT_CLICK/TICK）
- **`dim()/screenX()/screenY()` 在 Ui 的 private 段** —— 文件级自由函数不能直接用。
  抽图标绘制这类自由函数时，只传**已换算好的屏幕像素**（+ 显式 `scale`），
  design→screen 换算留在 Ui 成员函数里（2026-10-04 编译 13 处 private 报错）

## 触摸对焦（2026-10-02/03 定稿）
- MeteringRectangle=int32×5 `(xmin,ymin,xmax,ymax,weight≤1000)`——写成 (x,y,w,h) 会退化成点
- AF_TRIGGER 语义="每请求实例执行一次"，**不可写 repeating**；一次性请求 targets 必须与
  repeating 全量一致（否则 CamX 断流 ~500ms）
- 默认策略 0=只换 ROI（最接近系统相机，~0.8s）；trigger 会重启状态机多花一倍时间
- ROI 存归一化 fx/fy，`refreshRoi()` 在 commitSession 随 zoom 重算（唯一入口）；
  **"变化"比较基准必须是 settings_ 本身**——镜像不同步清空就会"清空后区域发不出去"（真机确诊）
- 兜底判据="点按后镜头屈光度没动过"（afScanned_ 判据错误：CAF 常态在扫描/合焦间跳变）
- 三态按钮（预览右上）：点击即对焦 / 仅选位置（白框常驻不消失）/ 对焦并拍照（roiLive 门控：
  ROI 回显生效才允许判合焦触发快门）；双击预览清空回默认测光

## 自动休眠（2026-10-03）
- 60s 无操作 → `CaptureSession::stopRepeating()`（仅停 repeating，保留会话/纹理/输出）；
  唤醒=commitSession(true) 重发 repeating（瞬启无黑帧，对比重建 ~290ms）
- **预览看门狗必须 `!sleeping_` 短路**——停 repeating 后无 result，会被误判 stall 自动重发（=白睡）
- 交互刷新点：drainUiCmds 每命令、pollControls 有 changed（休眠中直接唤醒并带新设置下发）
- Ui：`Cmd::WAKE` 通道；onDown 顶部休眠拦截（触摸只唤醒不执行动作，防误触快门）；
  预览遮罩"已休眠，触摸唤醒"逐帧直画

## 真机调试铁律（血泪）
- **绝不 force-stop/-S 本 app**：MIUI 把 component 置 disabled（无 root 无解）；恢复链见 2026-10-01.md
- 杀后台可靠组合：`am stack remove <tid>` → `am kill`（单用常静默不杀）；冷重启=HOME→等 8-12s→am kill→am start
- **`input tap` 可靠、`input swipe` 不派发**（吞零位移）；双击必须单 shell `input tap A; sleep 0.15; input tap A`；
  design→screen 换算读启动日志 `ui attached: win=WxH scale=S off=(ox,oy)`
- 截图：`MSYS_NO_PATHCONV=1; screencap -p`（不带 -d）；截图可远程诊断 UI
- **adb daemon 会被反复杀**：日志与 tap 事件大量丢失，误判成"功能失效"。
  必须单连接串起来：`adb shell 'logcat -c; input tap X Y; sleep 2; logcat -d ...'`；
  截图同理 `adb shell 'input tap X Y; sleep N; screencap -p /data/local/tmp/v.png'`
- **休眠态会吞掉所有点击**（onDown 顶部只发 WAKE）：测任何控件前先 `input tap 预览中部` 唤醒
- **「点得动但看不见」≠ 命中问题，先查绘制顺序**：2026-10-03 两个入口按钮连续两轮
  调试都误判为坐标/命中问题，实际是被后画的导轨面板整块盖住。裁剪截图放大确认
- **改 C++ 源码禁用 `sed -i` 批量替换**：`{0,0,0,0.55f}, kNone, 0);` 这类子串会跨函数
  误匹配（实测把 drawSettingsButton/drawExposureButton 的参数行替换成了 `XX`），
  必须用 Edit 工具做精确匹配
- 调试期别反复 `kill-server`，会把设备彻底弄掉线（USB 断开无法远程恢复）
- controls.txt：缺 zoom 键=保留当前值（复位须显式 zoom=1.0）；调试整写（`>`）勿 `>>` 叠加；测完恢复文件
- GPU 频率节点被 SELinux 锁死（HUD 显示 -- 是设备限制）；thermal trip 值不是实测温度

## 设置面板与持久化（2026-10-04 前后）
- **两个入口按钮已改纯图标圆形**（2026-10-04 用户要求，去掉文字）：
  齿轮=设置，位置 `kSetIconX=128,kSetIconY=48`（**左侧变焦导轨正上方**，导轨中线 (88+168)/2，
  在轨道 kZoomTrackY=130 与读数行 kReadoutY=100 之上）；半黑半白圆=曝光白平衡，
  位置 `kExpIconX=1456,kExpIconY=80`（右导轨顶部、快门正上方）。直径 `kIconD=60`。
  齿轮=圆环+8齿方块+中心孔；半黑白=圆环描边+14 个扇形三角形填上半。
  命中用**圆形** `hypot ≤ d/2+kIconHitPad(8)`，与 quickZoomHit 同口径
- 设置面板：照片质量(格式 JPG/RAW、RAW 模式环形/单次、连拍配额 1/4/8/不限)+辅助构图(网格线/水平仪/安全框)+持久化总开关
- 曝光白平衡面板：AE/AWB 开关 + 8 档白平衡预设(直映 ACAMERA_CONTROL_AWB_MODE 枚举 1..8)
- 两按钮均在 paintOverlay **末尾**绘制 + 近黑 0.8 底衬（原因见 UI 架构节的顺序铁律）
- 面板画逐帧动态层(勿进静态覆盖缓存);onDown 顶部 `panel_!=NONE` 短路转发 handlePanelTap，防误触背后快门/对焦
- **MIUI 会弹系统 toast**（"本次不再提醒应用敏感行为"）盖住面板中部会吞 tap，
  等 ~4s 消失再点，别误判成代码 bug；预览右上偶发绿点是 MIUI 相机隐私指示（系统层，非本项目绘制）
- **铁律：任何改变"面板可见状态"的 apply* 方法必须调 `markDirty()`**（面板走动态层，
  不置脏就不重画 ⇒ 选中态停在旧档。真机踩过：applyAwbPreset 漏调导致 WB 预设点不动）
- **两个面板已改全屏显示（2026-10-04）**：不再是 760 宽小盒。全屏实底
  `{0.055,0.055,0.065,1.0}`，控件宽 `kPanelCtlW=720` 右对齐到 `kPanelCtlR=1410`，
  标签右对齐到控件左侧 24px（`kPanelPadL=150`），行高 `kPanelRowH=64`/控件高 48/标题行高 44，
  内容块在 `kPanelTopY=104`~`kPanelBotY=690` 内**垂直居中**（设置 9 行 / 曝光 5 行行数不同）。
  8 段 WB 预设每段 90px（原 42px）。
- **关闭按钮 = 入口图标位**（用户要求）：设置面板左上、曝光面板右上，复用圆形底衬 +
  `closeIcon()`（圆环+两根 ±45° 斜条）。handlePanelTap **先判关闭钮**再判控件；
  **已取消「点面板外关闭」**（全屏后无"外部"）。`panelH()` 已删（draw/命中共用 ctlRects_）。
- 持久化写 `dataDir_/settings.txt`，由 `persist_` 开关控制读写；`commitPersist`/`loadPersistedSettings`(解析后 apply，最后 `persist_=persist`)
- SET_FMT 改绝对语义(v>0.5=JPG)修复反复切换漂移；chip 角标同改绝对赋值；白平衡预设经 CaptureEngine SET_WB_PRESET 映射 kEnum[8]
- 水平仪：SensorManager 加速度计 `roll_=atan2(gx,gz)`，无传感器降级居中；符号/轴待真机校正
- 字体 extra 串已补 CJK：设置平衡照片质量辅助构图网格线水平仪安全框持久化格式模单次连拍开关自动日光阴天白炽荧光钨丝测预限暖暮影

## 用户偏好
- 注释、文档、提交信息统一简体中文；结论必须基于项目实际代码与真机验证
- 完成改动后编译（零警告）+ 真机验证通过才宣告完成；git commit 引用
- 审查约定：另一 AI 写实现时本会话只监督审查（不改码），除非用户明确要求动手
