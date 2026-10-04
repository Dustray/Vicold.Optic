# UI 布局与渲染规范

> `core/ui/`（`Gl` 渲染底座 + `Ui` 布局交互）的所有硬性约定。
> 设计原型 `camera-ui.html`（1560×720 横屏）。最后更新：2026-10-04。
> **改任何 UI 代码前先读 §1 坐标系与 §2 禁区**，这两节每条都是真机踩出来的。

## 1. 坐标系三件套（最容易搞混）

设计空间 1560×720（横屏），窗口宽高比不同时按 **cover** 等比铺满 + 双向居中。

| 用途 | 函数 | 说明 |
|---|---|---|
| 纯缩放（尺寸/圆角半径/线宽/字间距） | `dim(len)` | 只乘 `scale_` |
| 位置（**内含居中偏移**） | `screenX(x)` / `screenY(y)` | **`dim()` 不能用来定位** |
| 输入反变换 | `toDesignX` / `toDesignY` | 触摸事件 → 设计坐标 |

- **EGL 窗口原点在左下，触摸原点在左上**。Y 翻转已在顶点着色器里做过，**不要再改回来**。
- **热区一律存设计坐标**。屏幕坐标只用于绘制那一刻。曾把 JPG/RAW 角标热区存成屏幕坐标，导致点击永远命中不了。
- 右侧面板必须一直画到设计右缘 1560（由 `kRailRX + kRailRW` 决定），否则右缘会露黑缝。

## 2. 挖孔禁区（硬约束，不可违背）

```
adb shell dumpsys display | grep cutout
```

pandora 返回：竖屏顶中 `Rect(573,0-647,150)`、`cutoutSpec="M 0,0 H -37 V 150 H 37 V 0 H 0 Z"`
⇒ **横屏后紧贴左边缘，设计坐标 x ∈ [0, 88]、y ∈ [337, 380]**。

实测三轮结论（2026-10-01）：

| 尝试 | 结果 |
|---|---|
| 轨道左缘 X=64 | ❌ **真孔把轨道左侧吃掉**（中心确认区正落在孔的 y 带） |
| X=88（压包络边界） | ✅ 可行，**这是硬下限** |

> **教训**：厂商 `DisplayCutout` 矩形不是"保守包络"，对该机型就是真实不可用区（孔周围那圈黑边也算进去），
> 肉眼估孔径会严重低估。**X 不可再往左**，要和预览拉开距离只能缩 `kZoomTrackW`。

**全出血**：窗口 frame 已是 2656 全宽，`windowLayoutInDisplayCutoutMode` 必须写在**主题 style** 里
（写在 activity manifest 属性里会被静默忽略）。三件套：`OpticTheme@styles.xml` + `package.sh` aapt2 compile res + JNI `setAttributes(3)`。

**Ui::attach 的浮点陷阱**：判定是否全出血用 `overflowX > 0`，而 2656/1560 的浮点乘回来会多出 **+0.0002px**，
把全出血误判成"未全出血" ⇒ 左面板退到 x=80，0–80 露出 GL 清屏纯黑，看起来像一条"遮挡条"。
**必须带容差**：`overflowX > 1.f`。

## 3. 布局常量表（`Ui.cpp` → `namespace layout`）

改布局只动这里的常量，其余元素都由它们推导。

| 元素 | 常量 | 取值（设计 px） |
|---|---|---|
| 左导轨面板右缘 | `kRailREnd` | 168 |
| 变焦轨道 | `kZoomTrackX/Y/W/H` | **88** / 130 / 80 / 448（X 下限见 §2） |
| 三滚轮上方实时值 | `kReadoutY` / `kReadoutFs` | 100 / 16（×`kUiZoom`×`scale_`） |
| 滚轮下方圆按钮 | `kJogBtnD` / `kJogBtnY` / `kJogBtnFs` | 60 / 594 / 14（三滚轮共用） |
| 预览区 | `kPreviewX/Y/W/H` | 178 / 0 / 960 / 720 |
| HUD 角标行 | `kHudY` / `kChipH` | 16 / 26（自 `kPreviewX` 推导） |
| 直方图 | `kHistW/H/Y` | 132 / 66 / 52 |
| AF 框 | `kAfX/Y/Size` | 606 / 312 / 104（恒保持预览居中） |
| EV 面板 | `kEvPanelX/Y/W/H` | 202 / 622 / 520 / 84 |
| EV 轨道 | `kEvTrackX/Y/W/H` | 214 / 664 / 476 / 24 |
| 右导轨面板 | `kRailRX` / `kRailRW` | 1156 / **404**（必须贴到设计右缘 1560） |
| ISO / 曝光时间轨道 | `kIsoTrackX` / `kSsTrackX` / `kTrackW` | 1166 / 1280 / 100 |
| 快门 | `kShutterX/Y/D` | 1410 / 318 / 106 |
| 测光锁 | `kAeLockY/D` | 238 / 54 |
| 设置入口（齿轮） | `kSetIconX/Y` / `kIconD` | **128** / 48 / 60（左变焦导轨正上方，x 与导轨中线对齐）|
| 曝光白平衡入口 | `kExpIconX/Y` / `kIconD` | 1456 / 80 / 60（右导轨顶部、快门正上方）|
| 闪光灯入口 | `kFlashIconX/Y` / `kIconD` | 1376 / 80 / 60（曝光图标**左侧**同排，圆心间距 80，边缘留 20）|
| 快速变焦圆钮 | `kQzX` / `kQzD` / `kQzY0` / `kQzGap` | 234 / 60 / 180 / 22（自上而下 5·2·1·0.7）|
| 面板内容区 | `kPanelTopY` / `kPanelBotY` | 104 / 690（全屏面板内内容块的垂直居中区间）|
| 面板行 | `kPanelRowH` / `kPanelHeadH` / `kPanelCtlH` | 64 / 44 / 48 |
| 面板标签左缘 / 控件右缘 / 控件宽 | `kPanelPadL` / `kPanelCtlR` / `kPanelCtlW` | 150 / 1410 / 720 |
| UI 整体放大 | `kUiZoom` | 1.5 |

配色：强调色 `#FF4D00`（`kAccent`）、面板底 `#0A0A0C`（`kRail`）、文字三级 `kT1/kT2/kT3` = 100%/72%/45% 白。

> `kRailRW` 必须是 404 = 1560−1156。短了会在屏幕右缘露一条比面板更黑的 GL 清屏缝（2026-10-01 用户报告"右边有遮挡条"）。

### 3.1 绘制顺序铁律（后画盖先画）

`paintOverlay` 里**不存在 z-index**，只有调用顺序。入口按钮类浮层**必须放在函数末尾**：

```
paintOverlay:  … 导轨面板 → 导轨 → 快门 → 入口图标(齿轮/曝光) → [面板]
```

入口图标若放在导轨之前，会被左右两块**整块不透明导轨面板**盖住 →
表现为「命中区在、按钮看不见」，调试时只点得动、屏幕上找不到。
2026-10-03 为此连续两轮误判为坐标/命中问题，实际是绘制顺序。**排查「点得动但看不见」先查顺序，再查坐标。**

### 3.2 浮层底衬配色

**底衬一律用近黑 `{0,0,0,0.8}`，不要用白色低 alpha。** 预览画面明暗变化大，
白 8% 之类的浅色半透明会随背景一起消失。

同一原则的延伸：**构图辅助线也不能用低 alpha 亮色**。原 `kGrid = {1,1,1,0.12}`（12% 白）
在白墙/亮桌面/高光场景上完全隐没，真机截图确认像素画进去了但人眼读不出来。
现拆成 `kGridEdge{0,0,0,0.55}` 近黑描边 + `kGridCore{1,1,1,0.85}` 亮白芯线**双层绘制**
（先整批描边再整批芯线，避免同一条线的两层交错成碎段）。`drawSafeFrame` /
`drawLevel` 中心十字同改双层。

### 3.3 全屏设置 / 曝光白平衡面板

2026-10-04 起两个面板为**全屏**（原 760×~600 小盒控件仅 336 宽，8 段白平衡预设每段
42px，文字挤成一团）。全屏实底 `{0.055,0.055,0.065,1.0}`。

- 内容块在 `kPanelTopY..kPanelBotY` 内**按实际行数垂直居中** —— 设置 9 行（3 标题+6 控件）
  占 516px，曝光 5 行只占 280px，顶对齐会让后者下半屏空着。
- 绘制与命中共用同一份 `ctlRects_`（`buildCtlRects()` 产出，`drawPanel` 与
  `handlePanelTap` 都用它），**不存在两套坐标**。
- **关闭按钮 = 入口图标位**：设置面板左上、曝光面板右上，复用圆形底衬 + `closeIcon()`。
  `handlePanelTap` **先判关闭钮再判控件**。已取消「点面板外关闭」（全屏后没有"外部"）。
- 面板画在**逐帧动态层**（不进静态覆盖缓存）。因此：

> **铁律：任何改变"面板可见状态"的 `apply*` 方法必须调 `markDirty()`。**
> 面板走动态层，不置脏就不重画 ⇒ 选中态停在旧档。真机踩过：`applyAwbPreset` 漏调导致
> WB 预设点不动（命中正确、高亮不变）。

- `onDown` 顶部 `panel_ != NONE` 短路转发 `handlePanelTap`，防误触面板背后的快门/对焦。

### 3.4 竖排变焦 UI 方向统一

**长焦在上**（专业相机惯例）。所有竖排变焦元素必须同向：

| 元素 | 排列 |
|---|---|
| 左变焦导轨 | 0.7 在下、120 在上 |
| 快速变焦圆钮 | `kQzVals = {5, 2, 1, 0.7}`（自上而下递减）|

**顺序只改常量数组，不要在 y 计算里翻转** —— 绘制与命中遍历同一组下标 `i`，
在 y 计算里翻转会漏命中（2026-10-04 用户报"快速变焦按钮反了"）。

### 3.5 字体图集新增中文的规矩

**新增任何文案，必须把用到的 CJK 字加进 `Gl.cpp` `bakeFont()` 的 `extra` 串**，
否则该字形**静默丢失**（画成空白，不报错、不崩）。已踩过的坑：设置面板首版缺
「其/他/外/部/闭」，"其他" 标题显示为空白。

## 4. 文字渲染（三条硬规则）

1. **`Gl::text()` 必须用 `GL_TRIANGLES`**（每字形 6 顶点）。用 `TRIANGLE_STRIP` 串多个字形会生成字形间的"对角连接三角形" —— 表现为一笔一画之间出现斜坡锯齿连线。**这个 bug 曾被连续误诊三次**（图集串扰 → 可变字体连笔 → 才找到 strip 真因）。
2. **字体必须是静态 TrueType（`glyf`）**。`bakeFont` 会探测候选并选 CJK 覆盖率最高的：
   pandora 最终命中 `/product/fonts/FZFWZhuZiAYuanJWB.TTF`（cjk 23/23）。
   已排除：`MiSansVF.ttf`（VF，`gvar` stb 不支持）、`NotoSansCJK.ttc`（CFF，`InitFont` 失败）、`MiSansC_3.005.ttf`（仅西文子集）。
3. **垂直居中用 `Gl::textCenterTop(utf8, px, cy)`**（按字形墨迹 bbox）。别用 `字号 × 系数` 估算 —— `Gl::text` 的 y 是 ascent 行顶，估算法必然偏。
   注：SS 行含下伸部（`"1/250 s"`），有效字号 24px 时墨迹底 ≈ 设计 y 130 ⇒ 基线必须 ≤100 才能与轨道顶（138）留 8px。

## 5. 交互约定

- **每帧 UI 命令经 `Ui::popCmd()` 交引擎线程**，与 `controls.txt` 共用同一个 `triggerBurst()` 配额闸门。
- **触感**：`core/ui/Haptics`（JNI → Vibrator），`EFFECT_CLICK` 给按钮、`EFFECT_TICK` 给落档；manifest 需 VIBRATE 权限，`android_main` 里 `ui.setJni()` 注入。
- **EV 面板**：统一"转盘"隐喻（刻度跟着手指走，非 +/− 按钮）。双击 = 350ms 内两次且 x 差 ≤40 设计px → 归零。
  注意 `AMotionEvent_getEventTime(e)` **只有一个参数**（ns）。
- **`zoom` 输入**：刻度盘用相对位移驱动，`onMove`/`onUp` 都从按下基准重算（快速甩动时输入管线会丢末尾 MOVE，实测曾停在 8.76 而非 10.0）。
- **电池**：JNI `registerReceiver(null, ACTION_BATTERY_CHANGED)` 读 sticky，`Ui::frame` 里 30s 刷新。充电=绿 `#4ADE80`，≤20%=强调橙，正常=白。
- **拍照反馈**：只用 HUD 角标（保存中显橙色 `SAVING`），预览区不再有浮层。
- **自动休眠**：60s 无操作 → `CaptureSession::stopRepeating()`（**仅停 repeating**，保留会话/纹理/输出目标）。
  唤醒 = `commitSession(true)` 重发 repeating，瞬启无黑帧（对比 close 重建 ~290ms + 纹理失效）。
  - **预览看门狗必须 `!sleeping_` 短路** —— 停 repeating 后没有 capture result，会被误判 stall 自动重发（= 白睡）。
  - 交互刷新点：`drainUiCmds` 每条命令、`pollControls` 有 changed（休眠中直接唤醒并带新设置下发）。
  - UI 侧：`Cmd::WAKE` 通道；**`onDown` 顶部休眠拦截**（触摸只唤醒、不执行动作，防误触快门）；
    遮罩"已休眠，触摸唤醒"**逐帧直画**，不要 `markDirty` 钉脏（否则静态覆盖缓存失效持续发热）。
  - **测任何控件前先 `input tap 预览中部` 唤醒**，否则 tap 全被休眠拦截（表现为"功能失效"）。
- **静态覆盖层 vs 逐帧动态层**：静态元素走离屏纹理缓存（稳态 ~4Hz 重烤，GPU 主要杠杆）；
  **动态元素（AF 框 / 休眠遮罩 / 面板）必须逐帧直画**。给动态元素调 `markDirty()` 会让缓存永久失效。
  - 反过来：**状态变化必须置脏**，见 §3.3 铁律。

## 5.1 设置项状态真值源（避免双真值源打架）

面板开关与滚轮状态若各存一份，会出现「面板显示已关、滚轮仍显示自动」。约定：

| 状态 | 真值源 | 回推接口 |
|---|---|---|
| `isoAuto_` / `ssAuto_` | **`CameraEngine`** | `Ui::setExpAuto(isoAuto, ssAuto, iso, expNs)` |
| 面板其余开关 | `Ui` 自身 | `pushCmd(Cmd::SET_*)` 单向 |

**关自动曝光必须走 `recomputeMixed()` 语义**（不能只翻 `aeOn`）：
`CaptureSettings::apply` 在 `!aeOn` 时**无条件**写 SENSITIVITY/EXPOSURE_TIME，
若此时 iso/exposureNs 还是双自动态的 0，HAL 会按「ISO 0 / 曝光 0ns」成像 → **预览全黑**。
关 AE 时用 AE on 期间冻结的 `lastAeIso_` / `lastAeExpNs_` 补成有效值（曝光守恒，亮度不变）。
注意 `recomputeMixed()` 在双手动时**不填值**（补偿分支都以 `isoAuto_`/`ssAuto_` 为条件），必须显式补。

`CaptureSettings::apply` 另有最后兜底：非正值退回 ISO 100 / 33ms —— 宁可曝光不准也不能给黑图。

## 5.2 闪光灯（camera2 语义，容易踩坑）

入口：右导轨顶部曝光图标左侧，点击循环 **关 → 自动 → 开 → 常亮 → 关**。
档位是自定义枚举 `CaptureSettings::flashMode`（0/1/2/3），**不是** camera2 的 `FLASH_MODE` 枚举。

**主语义落在 `CONTROL_AE_MODE` 上，不能只写 `FLASH_MODE`**：

| 档位 | `CONTROL_AE_MODE` | `FLASH_MODE` |
|---|---|---|
| 0 关 | `ON` | 不写 |
| 1 自动 | `ON_AUTO_FLASH` | 不写 |
| 2 开 | `ON_ALWAYS_FLASH` | 手动曝光时**仅单拍**写 `SINGLE` |
| 3 常亮 | `ON` | `TORCH`（任何请求都写）|

三条必须记住的坑：

1. **多数 HAL 在 `AE_MODE=ON` 时忽略 `FLASH_MODE=SINGLE`** —— 只写 FLASH_MODE 不改 AE_MODE，
   表现就是「开关切了但灯不闪」。
2. **repeating 上绝不能写 `FLASH_MODE=SINGLE`** —— camera2 语义是「每个请求实例放一次电」，
   会让闪光灯每帧放电（费电伤灯）。手动曝光（AE_MODE=OFF）下需要靠 `forPreview=false`
   把 SINGLE 限定在单拍请求上（`captureOnce` 走 TEMPLATE_STILL_CAPTURE 且传 `forPreview=false`）。
3. **档位 3（常亮）的 AE_MODE 必须保持 `ON`**，写成 `ALWAYS_FLASH` 会叠加「每次 capture 再放一次电」。

**无闪光灯单元的设备必须清零**：`AE_MODE_ON_ALWAYS_FLASH` 会被 HAL 拒绝整包
（连带同请求其它 entry 一起丢）。两道守卫：
- `openCamera` 按 `traits.flashAvailable` 校正（含把持久化残留的旧档位清零）
- `SET_FLASH` 在**相机已打开**（traits 可信）时才按能力拒绝并回推 `Ui::setFlash(0)`；
  未打开时不拒，否则 UI attach 阶段恢复出的档位会被空 traits 误清（glue 线程与引擎线程并发）

UI 侧 `setFlashAvail(false)` 时**不画入口**（与其下发失败不如不显示）。
图标档位靠**附加符号**区分而非颜色：关 = 深色斜杠划断；自动 = 右下 "A"；开 = 纯闪电；常亮 = 四向短芒。

## 6. 真机验证方法（截图 + 像素采样）

```bash
export MSYS_NO_PATHCONV=1               # 只对 adb 开；package.sh 需要 POSIX 路径，别全局开
adb shell input keyevent 224            # 唤醒；息屏下 screencap 会返回 ~18KB 锁屏黑图
adb shell input swipe 600 1800 600 600 300
adb shell am start -n com.vicold.optic/android.app.NativeActivity
sleep 8; adb shell screencap -p /sdcard/x.png && adb pull /sdcard/x.png .workbuddy/
```

设备像素 → 设计坐标换算系数 **1.7026**（2656/1560）。

用 PIL 沿某一行/列扫色变点即可验证元素精确落位。**注意**：`screencap` 读的是 framebuffer，挖孔下方的像素照样有内容 ——
**截图看不出挖孔，布局约束必须以 `dumpsys display` 的 cutout bounds 为准**。

> 临时验证技巧：短生命周期的 UI 状态（如 SAVING 只显示 ~150ms）截不到图，可临时把显示窗口延长到 2.5s
> 截图确认位置与配色，**验完务必 revert 再出正式包**。

### 6.1 截图与点击的坐标系（连踩两次）

`screencap` 拉下来的 PNG 是**真机分辨率 2656×1220**，但看图时会被缩放到 1080 宽。
**按截图上量到的像素直接 `input tap` 会打偏** —— 必须先换算：

```
真机X = 截图X × 2.4593     真机Y = 截图Y × 2.526
```

识别方法：两次不同操作后截图**字节数一字不差**（如都是 136565B）⇒ 高度疑似空操作，
先怀疑坐标而不是代码。

面板是**垂直居中**的，行 y 随行数变化，**不要手推 design 值**。直接从截图量、再乘系数换算。

### 6.2 adb 调试铁律（血泪，每条都踩过）

| # | 规则 | 后果 |
|---|---|---|
| 1 | **绝不 `am force-stop` / `am -S`** | MIUI 把 component 置 disabled，无 root 无解 |
| 2 | 杀后台用 `am stack remove <tid>` → `am kill`（单用常静默不杀）；冷重启 = HOME → 等 8-12s → `am kill` → `am start` | — |
| 3 | **`input tap` 可靠、`input swipe` 不派发**（吞零位移）；双击必须单 shell：`input tap A; sleep 0.15; input tap A` | swipe 静默无效，误判"手势没生效" |
| 4 | **adb daemon 会被反复杀**：日志与 tap 大量丢失，误判成"功能失效"。必须单连接串起来 `adb shell 'logcat -c; input tap X Y; sleep 2; logcat -d ...'` | — |
| 5 | **休眠态吞掉所有点击**（`onDown` 顶部只发 WAKE）。测控件前先 `input tap 预览中部` 唤醒 | — |
| 6 | **MIUI 会弹系统 toast**（"本次不再提醒应用敏感行为"）盖住面板中部并吞 tap，等 ~4s 消失再点 | 误判成代码 bug |
| 7 | 预览右上偶发绿点 = MIUI 相机隐私指示（系统层），非本项目绘制 | — |
| 8 | **`MSYS_NO_PATHCONV=1` 只给 adb 用**，`tools/package.sh` 前必须 unset | 设了会让 aapt2 找不到 `res` 目录 ⇒ 装上旧包，表现为"改了代码行为没变" |
| 9 | **改 C++ 源码禁用 `sed -i` 批量替换**。`{0,0,0,0.55f}, kNone, 0);` 这类子串会跨函数误匹配（实测把两个按钮的参数行替换成 `XX`）| 必须用 Edit 精确匹配 |
| 10 | 调试期别反复 `kill-server` | 会把设备彻底弄掉线（USB 断开无法远程恢复） |
| 11 | `controls.txt`：**缺键 = 保留当前值**（复位须显式写 `zoom=1.0`）；调试整写（`>`）勿 `>>` 叠加；**测完恢复文件** | — |
| 12 | **测 `focus_d` 时只能写 `focus_d`，不能同时带 `af=1`** | 每轮轮询全量重放会重开 `afOn`，`focus_d` 因值未变不再触发 → 读数永不跟随，误判"镜头物理下限" |
| 13 | 息屏时启动报 `Camera "0" disabled by policy`：先查 `mWakefulness`，`input keyevent KEYCODE_WAKEUP` + `keyevent 82` 唤醒 | 不是权限问题，`appops` 怎么设都无效 |
| 14 | 状态机类改动采样要留去抖余量（如近距判定 12 帧 ≈0.4s）| 看到"没生效"其实是采样早了 |

