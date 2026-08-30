//
// Created by xu.bw on 2026/6/7.
//
#include "coding.hpp"

// 编码转换严格标志的兜底定义（低版本 SDK 可能缺失）
#ifndef MB_ERR_INVALID_CHARS
#define MB_ERR_INVALID_CHARS 0x00000008
#endif
#ifndef WC_ERR_INVALID_CHARS
#define WC_ERR_INVALID_CHARS 0x00000004
#endif

// 编码转换函数实现
//
// 安全约定：任何一步转换失败（非法/无法映射的字节序列）都显式返回空串，
// 而非产出“静默损坏”的非空串。调用方对空串均有护栏
// （如 deleteRegistryKey 的空路径 P0 拦截、isProtectedPath 拒绝空路径、
// 文件操作对空路径的过滤），因此“返回空”是安全失败而非隐患。
// 这让“非法 UTF-8”从“悄悄变成乱码路径”变为“被护栏显式拒绝”。
std::string utf8ToGbk(const std::string& utf8Str) {
    if (utf8Str.empty()) return "";

    int wideSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.c_str(), -1, nullptr, 0);
    if (wideSize <= 0) return "";
    std::wstring wideStr(wideSize, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.c_str(), -1, &wideStr[0], wideSize))
        return "";

    int gbkSize = WideCharToMultiByte(CP_ACP, 0, wideStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (gbkSize <= 0) return "";
    std::string gbkStr(gbkSize, '\0');
    if (!WideCharToMultiByte(CP_ACP, 0, wideStr.c_str(), -1, &gbkStr[0], gbkSize, nullptr, nullptr))
        return "";

    if (!gbkStr.empty() && gbkStr.back() == '\0')
        gbkStr.pop_back();
    return gbkStr;
}

std::string gbkToUtf8(const std::string& gbkStr) {
    if (gbkStr.empty()) return "";

    int wideSize = MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, nullptr, 0);
    if (wideSize <= 0) return "";
    std::wstring wideStr(wideSize, L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, &wideStr[0], wideSize))
        return "";

    int utf8Size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 0) return "";
    std::string utf8Str(utf8Size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideStr.c_str(), -1, &utf8Str[0], utf8Size, nullptr, nullptr))
        return "";

    if (!utf8Str.empty() && utf8Str.back() == '\0')
        utf8Str.pop_back();
    return utf8Str;
}

std::wstring utf8ToWide(const std::string& utf8Str) {
    if (utf8Str.empty()) return L"";

    int wideSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.c_str(), -1, nullptr, 0);
    if (wideSize <= 0) return L"";
    std::wstring wideStr(wideSize, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Str.c_str(), -1, &wideStr[0], wideSize))
        return L"";

    if (!wideStr.empty() && wideStr.back() == L'\0')
        wideStr.pop_back();
    return wideStr;
}

std::string wideToUtf8(const std::wstring& wideStr) {
    if (wideStr.empty()) return "";

    int utf8Size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 0) return "";
    std::string utf8Str(utf8Size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideStr.c_str(), -1, &utf8Str[0], utf8Size, nullptr, nullptr))
        return "";

    if (!utf8Str.empty() && utf8Str.back() == '\0')
        utf8Str.pop_back();
    return utf8Str;
}
