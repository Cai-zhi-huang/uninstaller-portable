#pragma once
#include <QString>

// ============================================================
//  应用版本号 —— 唯一权威定义处
//  更新版本时只需修改下面两行
//  （在收到“更新版本号”指令之前，不要改动这里）：
//    APP_VERSION        : 主版本.次版本.修订号，例如 "0.0.1"
//    APP_VERSION_SUFFIX : 发布阶段，留空串 "" 表示无后缀；
//                         例如 "试用版" / "Beta" / "正式版"
//  C++ 侧通过 appVersionFull() 取得展示串（如 "v0.0.0 (试用版)"）。
//  注意：Windows 文件属性中的版本信息在 appicon.rc 的 VERSIONINFO 中同步定义，
//  更新版本号时请两处一起改。
// ============================================================
#define APP_VERSION        "0.0.5"
#define APP_VERSION_SUFFIX "试用版"

inline QString appVersionFull() {
    QString s = "v" APP_VERSION;
    if (QStringLiteral("" APP_VERSION_SUFFIX) != QStringLiteral("")) {
        s += " (" APP_VERSION_SUFFIX ")";
    }
    return s;
}
