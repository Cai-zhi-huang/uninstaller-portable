#!/usr/bin/env python3
# build_installer.py
# 把便携目录打包进 installer_base.exe 尾部，产出单文件安装器 Uninstaller-Setup.exe
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.join(HERE, "installer_base.exe")
PORTABLE = "D:/CZH720/tools/uninstaller-portable"
OUT = os.environ.get("INSTALLER_OUT", "D:/CZH720/tools/Uninstaller-Setup.exe")

SKIP = {"uninstaller_diag.log"}  # 排除运行时生成的诊断日志

def collect(portable):
    entries = []
    for dp, _, fs in os.walk(portable):
        for f in fs:
            if f in SKIP:
                continue
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, portable).replace(os.sep, "/")
            with open(full, "rb") as fh:
                data = fh.read()
            entries.append((rel, data))
    return entries

def main():
    if not os.path.exists(BASE):
        print("ERROR: installer_base.exe 不存在，请先编译 installer.cpp")
        sys.exit(1)
    if not os.path.isdir(PORTABLE):
        print("ERROR: 便携目录不存在:", PORTABLE)
        sys.exit(1)

    entries = collect(PORTABLE)
    print("打包文件数:", len(entries))

    blob = struct.pack("<I", len(entries))
    for rel, data in entries:
        pb = rel.encode("utf-8")
        blob += struct.pack("<H", len(pb)) + pb
        blob += struct.pack("<Q", len(data)) + data

    with open(BASE, "rb") as fh:
        base = fh.read()
    with open(OUT, "wb") as fh:
        fh.write(base)
        fh.write(blob)
        fh.write(struct.pack("<Q", len(blob)))

    total = os.path.getsize(OUT)
    print("已生成:", OUT)
    print("体积: %.1f MB  (基础 %.1f MB + 负载 %.1f MB)" % (
        total / 1e6, len(base) / 1e6, len(blob) / 1e6))

if __name__ == "__main__":
    main()
