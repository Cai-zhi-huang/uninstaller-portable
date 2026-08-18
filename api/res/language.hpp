/*
* Language support.
*
* 语言表改为运行时从外部 JSON（lang/languages.json）加载，而非硬编码在 C++ 常量里。
* 这样“增加一种语言” = 在 JSON 里加一列 + 在 languages 数组里加一个名字，无需改动源码，
* 框架可平滑扩展到几十甚至上百种语言。
* getlang(id) 的对外接口保持不变，所有 UI 代码无需修改。
*/
#pragma once
#include "qt.hpp"
#include "std.hpp"
#include "global.hpp"

// 启动时调用：优先读取 exe 同级 lang/languages.json，失败则回退到内嵌资源 :/lang/languages.json
void loadLanguageTable();

// 当前支持的语言数量
int langCount();

// 第 i 种语言的母语名称（用于菜单 / 下拉框显示，保持各自写法不随界面翻译）
QString langName(int i);

// 第 i 种语言的名称，按 uiLang 指定的界面语言翻译后显示。
// 例如 uiLang 为中文时，langName(0, uiLang) 返回"英语"而非 "English"。
// 若 nameTable 缺失该项或为空，则回退到母语名称 langName(i)。
QString langName(int i, int uiLang);

// 第 i 种语言所属的 family 字符串，约定格式为 "<一级大区> <二级系族>"
// 例如 "欧洲 Germanic" / "东亚 East Asian"。family 字段缺失则返回空串。
QString langFamily(int i);

// 一级大区列表，按语言表出现顺序去重得到（首次出现者优先）。
// 例如 ["欧洲 Germanic", "东亚 East Asian", ...] 折叠去重后为 ["欧洲", "东亚", ...]。
// 尚未加载或所有 family 为空则返回空列表。
QStringList langFamilyGroups();

// 取第 id 条文案、第 type 种语言的译文；type 省略时取当前语言 G.LANGUAGE
QStringView getlang(uint id, uint type = 0xffffffffu);
