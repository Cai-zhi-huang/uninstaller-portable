#include "std.hpp"
#include "struct.hpp"
#include "global.hpp"
#include <QString>
#include <vector>
#include <string>

namespace informat{
    ll getsize(std::string& path);
    // 返回路径所在卷的访问状态：
    //   0 = 正常可访问；
    //   1 = 只读卷（FILE_READ_ONLY_VOLUME，如被写保护的移动盘 / 锁定为只读的 C 盘）；
    //   2 = 无访问权限 / 卷不可访问（如 C 盘被锁定、ACL 拒绝、BitLocker 未解锁等）。
    // 用于：① 图标提取前判断是否需要跳过该卷（避免 ExtractIconExW 在无法访问的卷上
    //         阻塞或报错，导致“加载进度条访问文件出问题”）；
    //       ② 卸载删除文件失败时推断原因，给用户友好提示。
    int volumeAccessState(const std::string& path);
    // 根据待删除文件列表推断删除失败的可能原因，用于向用户给出可操作的友好提示。
    QString diagnoseDeleteFailure(const std::vector<std::string>& files);
}

namespace ioapi{
    void frewirte(std::string& include);
    void freread(
        std::set<std::pair<std::string,int64_t>>&,
        std::string& path
    );
}

