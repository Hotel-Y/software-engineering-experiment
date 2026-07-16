#pragma once

#include <filesystem>
#include <string>

// 打包或解包完成后返回的汇总数据，可用于 CLI 输出和性能测试。
struct ArchiveStats {
    std::size_t files = 0;              // 成功处理的普通文件数。
    std::uintmax_t bytes = 0;           // 压缩前或解压后的原始字节数。
    std::uintmax_t storedBytes = 0;     // 归档中数据块实际占用的字节数。
};

// 归档功能的可选参数；密码为空表示只压缩、不加密。
struct ArchiveOptions {
    std::string password;
};

// SBA5 归档管理器：按块压缩目录，并可使用 AES-256-GCM 加密每个数据块。
class ArchiveManager {
public:
    explicit ArchiveManager(ArchiveOptions options = {});

    // 将含 manifest.sbm 的备份目录写成单个 .sba 文件。
    ArchiveStats pack(const std::filesystem::path& backupDir,
                      const std::filesystem::path& archiveFile) const;

    // 校验并解出 .sba；先写临时目录，全部成功后才替换目标目录。
    ArchiveStats unpack(const std::filesystem::path& archiveFile,
                        const std::filesystem::path& outputDir) const;

private:
    // 每个管理器实例保存一组不可变的归档选项，便于多次调用保持一致。
    ArchiveOptions options_;
};
