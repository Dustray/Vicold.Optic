# UI 布局与渲染规范

> `core/ui/`（`Gl` 渲染底座 + `Ui` 布局交互）的所有硬性约定。
> 设计原型 `camera-ui.html`（1560×720 横屏）。最后更新：2026-10-01。
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
| UI 整体放大 | `kUiZoom` | 1.5 |

配色：强调色 `#FF4D00`（`kAccent`）、面板底 `#0A0A0C`（`kRail`）、文字三级 `kT1/kT2/kT3` = 100%/72%/45% 白。

> `kRailRW` 必须是 404 = 1560−1156。短了会在屏幕右缘露一条比面板更黑的 GL 清屏缝（2026-10-01 用户报告"右边有遮挡条"）。

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
