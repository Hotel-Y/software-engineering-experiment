#pragma once

#include "Manifest.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// 备份筛选条件；未填写的字段表示不启用对应过滤规则。
struct BackupOptions {
    std::vector<std::string> includeExtensions;  // 允许的扩展名，如 .txt、.cpp。
    std::string nameContains;                    // 文件名必须包含的文本。
    std::string pathContains;                    // 相对路径必须包含的文本。
    std::optional<std::int64_t> modifiedAfter;   // 最早修改时间（Unix 秒）。
    std::optional<std::int64_t> modifiedBefore;  // 最晚修改时间（Unix 秒）。
    std::optional<std::uintmax_t> maxSizeBytes;  // 单个文件允许的最大字节数。
    bool overwrite = false;                      // 是否清空并重建已存在的备份目录。
};

// 备份、还原和校验共用的操作统计。
struct OperationStats {
    std::size_t directories = 0;     // 成功处理或确认存在的目录数。
    std::size_t files = 0;           // 成功复制或校验通过的文件数。
    std::size_t skippedFiles = 0;    // 因类型或筛选条件未处理的文件数。
    std::size_t failedFiles = 0;     // 校验中缺失或内容不匹配的项目数。
    std::uintmax_t bytes = 0;        // 成功处理的文件原始字节总数。
};

// 目录级备份管理器：复制文件、生成清单，并依靠清单完成还原和完整性校验。
class BackupManager {
public:
    explicit BackupManager(BackupOptions options = {});

    // 遍历源目录，将符合过滤条件的文件复制到备份目录并生成 manifest.sbm。
    OperationStats backup(const std::filesystem::path& sourceDir,
                          const std::filesystem::path& backupDir) const;

    // 按清单重建目录，并对每个已还原文件重新计算校验值。
    OperationStats restore(const std::filesystem::path& backupDir,
                           const std::filesystem::path& restoreDir) const;

    // 对照清单检查目录、文件大小和校验值，不修改备份内容。
    OperationStats verify(const std::filesystem::path& backupDir) const;

private:
    // 集中判断单个文件是否通过大小、时间、名称、路径和扩展名过滤。
    bool shouldIncludeFile(const std::filesystem::path& filePath,
                           const std::filesystem::path& relativePath,
                           std::uintmax_t fileSize) const;

    BackupOptions options_;  // 构造时复制并规范化，后续操作只读。
};
