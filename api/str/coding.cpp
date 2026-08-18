//
// Created by xu.bw on 2026/6/7.
//
#include "coding.hpp"

// 编码转换函数实现
std::string utf8ToGbk(const std::string& utf8Str) {
    if (utf8Str.empty()) return "";

    // UTF-8 转 UTF-16
    int wideSize = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, nullptr, 0);
    std::wstring wideStr(wideSize, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, &wideStr[0], wideSize);

    // UTF-16 转 GBK
    int gbkSize = WideCharToMultiByte(CP_ACP, 0, wideStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string gbkStr(gbkSize, '\0');
    WideCharToMultiByte(CP_ACP, 0, wideStr.c_str(), -1, &gbkStr[0], gbkSize, nullptr, nullptr);

    if (!gbkStr.empty() && gbkStr.back() == '\0') {
        gbkStr.pop_back();
    }
    return gbkStr;
}

std::string gbkToUtf8(const std::string& gbkStr) {
    if (gbkStr.empty()) return "";

    // GBK 转 UTF-16
    int wideSize = MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, nullptr, 0);
    std::wstring wideStr(wideSize, L'\0');
    MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, &wideStr[0], wideSize);

    // UTF-16 转 UTF-8
    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Str(utf8Size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, &utf8Str[0], utf8Size, nullptr, nullptr);

    if (!utf8Str.empty() && utf8Str.back() == '\0') {
        utf8Str.pop_back();
    }
    return utf8Str;
}

std::wstring utf8ToWide(const std::string& utf8Str) {
    if (utf8Str.empty()) return L"";

    int wideSize = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, nullptr, 0);
    std::wstring wideStr(wideSize, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), -1, &wideStr[0], wideSize);

    if (!wideStr.empty() && wideStr.back() == L'\0') {
        wideStr.pop_back();
    }
    return wideStr;
}

std::string wideToUtf8(const std::wstring& wideStr) {
    if (wideStr.empty()) return "";

    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Str(utf8Size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, &utf8Str[0], utf8Size, nullptr, nullptr);

    if (!utf8Str.empty() && utf8Str.back() == '\0') {
        utf8Str.pop_back();
    }
    return utf8Str;
}
