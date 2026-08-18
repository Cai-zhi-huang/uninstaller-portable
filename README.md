# Uninstaller

## 1.关于本项目

### 目标

尽可能的卸载干净软件

## 2.关于Qt项目

### 核心

c++ 23, Qt 6

### 结构

```
/
-main.cpp           ->主入口
-/reg               ->注册表
--registry.h        ->注册表头文件
--registry.cpp      ->注册表实现
-/str               ->字符
--coding.h          ->编码头文件
--coding.cpp        ->编码实现
-/ui                ->gui
--mainwindow.h      ->主窗口申明
--mainwindow.cpp    ->主窗口实现
-/unilts            ->工具
-/res               ->资源
--language.h        ->i18
--place.h        ->注册表资源位置
```
