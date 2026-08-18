# The Sub code api

## Why use it

To set the workspace more clearly. It is compiled as a **static library** (`api` target) and linked into the main program.

## Structure

### dir

```txt
\api
-\base       (std / struct / qt / constexpr / global 等基础层)
-\reg        (registry.hpp / registry.cpp 注册表核心)
-\res        (place / language / error / global 等资源)
-\str        (coding.hpp / coding.cpp 编码转换)
-\ui         (mainwindow.hpp / mainwindow.cpp 主窗口)
-\unilts     (systools.hpp / systools.cpp 工具)
```

### function
