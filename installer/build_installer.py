#!/usr/bin/env python3
# build_installer.py
# 1) 从 api/ui/version.hpp 提取版本号，编译 installer_base.exe（注入 -DUNINSTALLER_VERSION）
# 2) 把便携目录打包进 installer_base.exe 尾部，产出单文件安装器 Uninstaller-Setup.exe
import os, struct, sys, re, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(HERE, ".."))
BASE = os.path.join(HERE, "installer_base.exe")
PORTABLE = "D:/CZH720/tools/uninstaller-portable"
OUT = os.environ.get("INSTALLER_OUT", "D:/CZH720/tools/Uninstaller-Setup.exe")
TOOLCHAIN = "D:/CZH720/tools/Qt/Tools/llvm-mingw1706_64/bin"

SKIP_FILES = {"uninstaller_diag.log"}          # 运行时诊断日志
SKIP_DIRS = {".git", "log"}                     # 仓库元数据 / 运行时日志目录
SKIP_SUFFIX = (".bak",)                         # 备份文件
SKIP_NAMES = {"uninstaller_cache.json"}         # 运行时缓存（含用户数据，不应随安装包分发）

def extract_app_version():
    """版本号唯一权威来源：api/ui/version.hpp 的 #define APP_VERSION "x.y.z" """
    p = os.path.join(PROJECT_ROOT, "api", "ui", "version.hpp")
    try:
        txt = open(p, encoding="utf-8").read()
    except OSError:
        return "0.0.0"
    m = re.search(r'#define\s+APP_VERSION\s+"([0-9.]+)"', txt)
    return m.group(1) if m else "0.0.0"

def compile_installer_base(app_ver):
    """编译安装器桩，并注入版本号，使其注册表 DisplayVersion 与程序版本一致。"""
    rc = os.path.join(TOOLCHAIN, "llvm-rc.exe")
    cc = os.path.join(TOOLCHAIN, "clang++.exe")
    # rc 与 clang 均以项目根为工作目录，installer.rc 内的相对路径
    # （assets/appicon.ico、app.manifest）才能正确解析。
    subprocess.run([rc, "installer/installer.rc"], check=True, cwd=PROJECT_ROOT)
    subprocess.run([
        cc, "-O2", "-static", "-mwindows",
        '-DUNINSTALLER_VERSION="%s"' % app_ver,
        "-o", BASE, "installer/installer.cpp", "installer/installer.res",
        "-lole32", "-luuid", "-loleaut32",
    ], check=True, cwd=PROJECT_ROOT)
    print("已编译 installer_base.exe (DisplayVersion=%s)" % app_ver)

def collect(portable):
    entries = []
    for dp, dns, fs in os.walk(portable):
        # 原地修剪要跳过的目录，避免 walk 进入 .git（26MB+，且泄露仓库历史）
        dns[:] = [d for d in dns if d not in SKIP_DIRS]
        for f in fs:
            if f in SKIP_FILES or f in SKIP_NAMES or f.endswith(SKIP_SUFFIX):
                continue
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, portable).replace(os.sep, "/")
            with open(full, "rb") as fh:
                data = fh.read()
            entries.append((rel, data))
    return entries

def main():
    if not os.path.isdir(PORTABLE):
        print("ERROR: 便携目录不存在:", PORTABLE)
        sys.exit(1)

    # 不再依赖外部预先编译好的 installer_base.exe：这里统一编译（版本单一来源）
    app_ver = extract_app_version()
    compile_installer_base(app_ver)

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
