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

## 已知状态（2026-09-28 更新）
- M0 已完成并已跑通真机预览（30fps，LEVEL_3，RAW 4096x3072，黑电平 64，RGGB，后置 RAW 仅 1 条流）
- M1 代码已全部接入 core/CMakeLists.txt 并编译通过（-Wall -Wextra 零警告，8/8 目标），
  新增 core/capture/{CameraDevice,CaptureSession,CaptureSettings,RawCapture} + core/util/{Log,ControlFile}
- CameraEngine 已重写为 M1 形态：预览直写 Surface + controls.txt 手动控制 + RAW ring(ZSL)/once + 断流重连
- 审查（2026-09-28）发现的未修问题：
  * P0：CaptureSession::onCaptureCompleted 用「回调 request 指针 == onceReq_」判定单拍完成 —— NDK 文档明确回调指针不等于提交的指针，
        导致 onceReq_ 永不释放，once 模式只能拍一张；onCaptureFailed 也未释放。应改用 sequenceId + onCaptureSequenceCompleted/Aborted
  * P1：ANativeWindow 未 acquire/release（M0 旧版有）；AImageReader maxImages=6 < ring4+saveQ 可能耗尽；
        ring 模式 RAW 常驻 repeating 可能拖慢预览帧率；close 后 in-flight 回调 UAF；controls.txt 用 unordered_map 遍历顺序不确定；
        ControlFile 每轮新建局部实例导致 400ms 节流失效（成员 ctl_ 是死成员）
  * P2：M0 的 capabilities.txt 探测报告被删除，tools/dev.sh report 工作流失效

## 审查约定（用户指定）
本项目由另一个 AI 写实现，我（本会话）只做**监督审查**：不改代码，只出审查结论 + 修法建议，结论须基于 git 未提交改动与实际编译结果。

## 用户偏好
注释、文档、提交信息统一简体中文；结论需基于项目实际代码与配置。
