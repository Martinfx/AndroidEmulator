#!/bin/sh
# Rebuilds tests/data from the Java sources (needs a JDK and dx.jar, see docs/navod.md).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
: "${DX_JAR:=$(find "$HOME/.m2" -name 'dalvik-dx-*.jar' 2>/dev/null | head -n 1)}"
tmp=$(mktemp -d)
javac -nowarn --release 8 -d "$tmp" "$root/tests/java/VmTest.java"
java -cp "$DX_JAR" com.android.dx.command.Main --dex --output="$root/tests/data/vmtest.dex" "$tmp"
java -Dstdout.encoding=UTF-8 -cp "$tmp" VmTest > "$root/tests/data/VmTest.expected"
"$root/samples/build-apk.sh" "$root/samples/ahoj" cz.priklad.ahoj MainActivity Ahoj DrawActivity
cp "$root/samples/ahoj/build/ahoj.apk" "$root/tests/data/"
rm -rf "$tmp"
