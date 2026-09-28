#!/usr/bin/env bash
# Vicold.Optic 构建环境（source tools/env.sh）
# SDK 安装在 D:/Android/sdk，JDK 在 D:/Android/jdk
export ANDROID_HOME="${ANDROID_HOME:-/d/Android/sdk}"
export ANDROID_SDK_ROOT="$ANDROID_HOME"

# NDK：取版本号最大目录
export ANDROID_NDK_HOME="$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1)"

# JDK：优先已有 JAVA_HOME，否则 D:/Android/jdk 下唯一目录
if [ -z "${JAVA_HOME:-}" ]; then
    export JAVA_HOME="$(ls -d /d/Android/jdk/jdk-* 2>/dev/null | head -1)"
fi
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmake/3.31.6/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"

# 打包用的 build-tools 版本
export BUILD_TOOLS="${BUILD_TOOLS:-$ANDROID_HOME/build-tools/36.1.0}"
export PLATFORM_JAR="$ANDROID_HOME/platforms/android-36/android.jar"

export PKG=com.vicold.optic
export ANDROID_JAR_DIR="$JAVA_HOME/lib"

echo "env: ANDROID_HOME=$ANDROID_HOME"
echo "env: ANDROID_NDK_HOME=$ANDROID_NDK_HOME"
echo "env: JAVA_HOME=$JAVA_HOME"
echo "env: BUILD_TOOLS=$BUILD_TOOLS"
