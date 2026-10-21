#!/usr/bin/env python3
"""Writes a binary AndroidManifest.xml (the format aapt produces).

usage: mkmanifest.py <out> <package> <launcher-activity> [label] [other-activity...]

Android never stores XML as text inside an APK: aapt compiles it into
"binary XML", a list of chunks (string pool, start/end tags) that is quick
to parse on a phone. This script writes the minimum a launcher needs.
"""
import struct
import sys

ANDROID_NS = "http://schemas.android.com/apk/res/android"


class Writer:
    def __init__(self):
        self.strings = []
        self.chunks = []

    def s(self, text):
        if text not in self.strings:
            self.strings.append(text)
        return self.strings.index(text)

    def start_ns(self):
        self.chunks.append(struct.pack("<HHIIIII", 0x0100, 16, 24, 1, 0xFFFFFFFF, self.s("android"), self.s(ANDROID_NS)))

    def end_ns(self):
        self.chunks.append(struct.pack("<HHIIIII", 0x0101, 16, 24, 1, 0xFFFFFFFF, self.s("android"), self.s(ANDROID_NS)))

    def start(self, name, attrs):
        body = b""
        for key, value in attrs:
            ns = self.s(ANDROID_NS) if key.startswith("android:") else 0xFFFFFFFF
            idx = self.s(value)
            body += struct.pack("<IIIHBBI", ns, self.s(key.split(":")[-1]), idx, 8, 0, 0x03, idx)
        ext = struct.pack("<IIHHHHHH", 0xFFFFFFFF, self.s(name), 20, 20, len(attrs), 0, 0, 0)
        size = 16 + len(ext) + len(body)
        self.chunks.append(struct.pack("<HHIII", 0x0102, 16, size, 1, 0xFFFFFFFF) + ext + body)

    def end(self, name):
        self.chunks.append(struct.pack("<HHIIIII", 0x0103, 16, 24, 1, 0xFFFFFFFF, 0xFFFFFFFF, self.s(name)))

    def string_pool(self):
        data = b""
        offsets = []
        for text in self.strings:
            raw = text.encode("utf-8")
            assert len(raw) < 128 and len(text) < 128, "long strings not supported"
            offsets.append(len(data))
            data += bytes([len(text), len(raw)]) + raw + b"\0"
        while len(data) % 4:
            data += b"\0"
        header = 28
        start = header + 4 * len(self.strings)
        size = start + len(data)
        UTF8 = 1 << 8
        return (struct.pack("<HHIIIIII", 0x0001, header, size, len(self.strings), 0, UTF8, start, 0)
                + b"".join(struct.pack("<I", o) for o in offsets) + data)

    def build(self):
        # Strings must be known before the pool is written, so tags are recorded first.
        pool = self.string_pool()
        body = pool + b"".join(self.chunks)
        return struct.pack("<HHI", 0x0003, 8, 8 + len(body)) + body


def main():
    out, package, launcher = sys.argv[1:4]
    label = sys.argv[4] if len(sys.argv) > 4 else package
    others = sys.argv[5:]
    w = Writer()
    w.start_ns()
    w.start("manifest", [("package", package)])
    w.start("application", [("android:label", label)])
    w.start("activity", [("android:name", launcher)])
    w.start("intent-filter", [])
    w.start("action", [("android:name", "android.intent.action.MAIN")])
    w.end("action")
    w.start("category", [("android:name", "android.intent.category.LAUNCHER")])
    w.end("category")
    w.end("intent-filter")
    w.end("activity")
    for name in others:
        w.start("activity", [("android:name", name)])
        w.end("activity")
    w.end("application")
    w.end("manifest")
    w.end_ns()
    with open(out, "wb") as f:
        f.write(w.build())


if __name__ == "__main__":
    main()
