#!/bin/sh
# Builds an APK from Java sources without the Android SDK:
#   javac (Java -> .class) -> dx (.class -> classes.dex) -> zip with AndroidManifest.xml
#
# usage: samples/build-apk.sh <app-dir> <package> <LauncherActivity> [label] [OtherActivity...]
# needs: javac (JDK 8+), python3, zip, and dx.jar (DX_JAR=..., see docs/navod.md)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
app=$1 package=$2 launcher=$3
shift 3
label=${1:-$package}
[ $# -gt 0 ] && shift

: "${DX_JAR:=$(find "$HOME/.m2" -name 'dalvik-dx-*.jar' 2>/dev/null | head -n 1)}"
[ -n "$DX_JAR" ] && [ -f "$DX_JAR" ] || { echo "set DX_JAR to dalvik-dx.jar" >&2; exit 1; }

out=$app/build
rm -rf "$out"
mkdir -p "$out/stubs" "$out/classes" "$out/apk"

# 1) The android.* stubs only exist so javac can type-check the app.
javac -nowarn --release 8 -d "$out/stubs" $(find "$here/sdk-stubs" -name '*.java') 2>&1 | grep -v -e '^warning' -e 'warnings$' || true
javac -nowarn --release 8 -cp "$out/stubs" -d "$out/classes" $(find "$app/src" -name '*.java') 2>&1 | grep -v -e '^warning' -e 'warnings$' || true

# 2) dx translates JVM bytecode (stack machine) into Dalvik bytecode (register machine).
java -cp "$DX_JAR" com.android.dx.command.Main --dex --output="$out/apk/classes.dex" "$out/classes"

# 3) The manifest tells the launcher which activity to start.
others=""
for a in "$@"; do others="$others $package.$a"; done
python3 "$here/tools/mkmanifest.py" "$out/apk/AndroidManifest.xml" "$package" "$package.$launcher" "$label" $others

# 4) An APK is just a ZIP file.
(cd "$out/apk" && zip -q -r "../$(basename "$app").apk" .)
echo "$out/$(basename "$app").apk"
