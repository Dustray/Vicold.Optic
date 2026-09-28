#!/usr/bin/env bash
# 手工打包：aapt2 link → 塞入 .so → zipalign → apksigner（全程无 Gradle/Java 源码）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/tools/env.sh"

OUT="$ROOT/build/pkg"
DIST="$ROOT/dist"
mkdir -p "$OUT" "$DIST"
rm -f "$OUT/base.apk" "$OUT/aligned.apk"

[ -f "$PLATFORM_JAR" ] || { echo "missing $PLATFORM_JAR"; exit 1; }

SO="$(ls "$ROOT"/build/android-arm64/liboptic.so 2>/dev/null || true)"
[ -n "$SO" ] || { echo "liboptic.so not built yet"; exit 1; }

echo ">> aapt2 link"
"$BUILD_TOOLS/aapt2" link -o "$OUT/base.apk" \
    -I "$PLATFORM_JAR" \
    --manifest "$ROOT/app/src/main/AndroidManifest.xml" \
    --min-sdk-version 31 --target-sdk-version 34 \
    --version-code 1 --version-name 0.1.0

echo ">> add native lib"
python - "$OUT/base.apk" "$SO" <<'PYEOF'
import sys, zipfile
apk, so = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(so, 'lib/arm64-v8a/liboptic.so')
PYEOF

echo ">> zipalign"
"$BUILD_TOOLS/zipalign" -f 4 "$OUT/base.apk" "$OUT/aligned.apk"

echo ">> apksigner"
KS="$ROOT/tools/debug.keystore"
if [ ! -f "$KS" ]; then
    keytool -genkeypair -keystore "$KS" -alias androiddebugkey \
        -storepass android -keypass android \
        -dname "CN=Vicold Debug,O=Vicold,C=CN" \
        -keyalg RSA -keysize 2048 -validity 10000
fi
"$BUILD_TOOLS/apksigner.bat" sign \
    --ks "$KS" --ks-pass pass:android --ks-key-alias androiddebugkey --key-pass pass:android \
    --out "$DIST/Optic-debug.apk" "$OUT/aligned.apk"

"$BUILD_TOOLS/apksigner.bat" verify "$DIST/Optic-debug.apk" && echo "OK: $DIST/Optic-debug.apk"
