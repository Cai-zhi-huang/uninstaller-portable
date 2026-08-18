//
// Created by xu.bw on 2026/6/7.
//
#ifndef UNINSTALLER_CODING_H
#define UNINSTALLER_CODING_H
#include "std.hpp"

// 解决 byte 冲突问题
#ifdef _MSC_VER
#else
// MinGW 环境下，取消 Windows 的 byte 定义，使用 std::byte
#ifdef byte
#undef byte
#endif
#endif

// 编码转换函数声明
std::string utf8ToGbk(const std::string& utf8Str);
std::string gbkToUtf8(const std::string& gbkStr);
std::wstring utf8ToWide(const std::string& utf8Str);
std::string wideToUtf8(const std::wstring& wideStr);

#endif //UNINSTALLER_CODING_H
