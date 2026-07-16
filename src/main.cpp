#include "ArchiveManager.h"
#include "BackupManager.h"
#include "FileUtils.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
// 同时保存“文本参数”和“路径参数”：选项用 UTF-8 解析，路径保留 Windows 宽字符。
// 这样即使终端代码页不是 UTF-8，中文路径也不会在进入 filesystem 前损坏。
class CommandLineArguments {
public:
    // Windows 下重新读取宽字符命令行；其他平台直接把 argv 当作 UTF-8。
    CommandLineArguments(int argc, char* argv[]) {
#ifdef _WIN32
        (void)argv;
        int wideArgc = 0;
        LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
        if (wideArgv == nullptr) {
            throw std::runtime_error("Cannot read the Unicode command line.");
        }
        try {
            text_.reserve(static_cast<std::size_t>(wideArgc));
            paths_.reserve(static_cast<std::size_t>(wideArgc));
            for (int i = 0; i < wideArgc; ++i) {
                const std::wstring value(wideArgv[i]);
                paths_.emplace_back(value);

                if (value.empty()) {
                    text_.emplace_back();
                    continue;
                }
                const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                                         value.data(), static_cast<int>(value.size()),
                                                         nullptr, 0, nullptr, nullptr);
                if (required <= 0) {
                    throw std::runtime_error("Cannot convert a command-line argument to UTF-8.");
                }
                std::string utf8(static_cast<std::size_t>(required), '\0');
                const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                                        value.data(), static_cast<int>(value.size()),
                                                        utf8.data(), required, nullptr, nullptr);
                if (written != required) {
                    throw std::runtime_error("Cannot convert a command-line argument to UTF-8.");
                }
                text_.push_back(std::move(utf8));
            }
        } catch (...) {
            LocalFree(wideArgv);
            throw;
        }
        LocalFree(wideArgv);
#else
        text_.reserve(static_cast<std::size_t>(argc));
        paths_.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            text_.emplace_back(argv[i]);
            paths_.push_back(pathFromUtf8(argv[i]));
        }
#endif
        if (text_.size() != static_cast<std::size_t>(argc)) {
            throw std::runtime_error("Command-line argument count mismatch.");
        }
    }

    // 三个只读访问器保证文本数组与路径数组始终使用相同下标。
    std::size_t size() const {
        return text_.size();
    }

    const std::string& text(std::size_t index) const {
        return text_.at(index);
    }

    const std::filesystem::path& path(std::size_t index) const {
        return paths_.at(index);
    }

private:
    std::vector<std::string> text_;
    std::vector<std::filesystem::path> paths_;
};

// 输出六类命令及可选参数，是参数不足或命令未知时的统一帮助信息。
void printUsage() {
    std::cout
        << "Simple Backup Manager\n"
        << "\n"
        << "Usage:\n"
        << "  sbm backup <source_dir> <backup_dir> [--overwrite] [--ext=.txt,.cpp]\n"
        << "             [--max-size=1048576] [--name-contains=report] [--path-contains=docs]\n"
        << "             [--modified-after=2024-01-01] [--modified-before=2026-12-31]\n"
        << "  sbm schedule <source_dir> <snapshot_root> <interval_seconds> <count> [--keep=N] [backup options]\n"
        << "  sbm restore <backup_dir> <restore_dir>\n"
        << "  sbm verify <backup_dir>\n"
        << "  sbm pack <backup_dir> <archive_file> [--password=secret]\n"
        << "  sbm unpack <archive_file> <output_dir> [--password=secret]\n";
}

// 将 --ext=.txt,.cpp 一类逗号分隔参数拆成独立过滤值。
std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

// 严格读取非负整数：除了范围外，还拒绝负号、空字符串和尾随字符。
std::uint64_t parseUnsigned(const std::string& value, const std::string& optionName) {
    if (value.empty() || value.front() == '-') {
        throw std::runtime_error(optionName + " must be a non-negative integer.");
    }
    std::size_t parsed = 0;
    std::uint64_t result = 0;
    try {
        result = std::stoull(value, &parsed);
    } catch (const std::exception&) {
        throw std::runtime_error(optionName + " must be a non-negative integer.");
    }
    if (parsed != value.size()) {
        throw std::runtime_error(optionName + " must be a non-negative integer.");
    }
    return result;
}

// 在无符号解析的基础上限制到 int 范围，供次数和间隔参数使用。
int parseNonNegativeInt(const std::string& value, const std::string& optionName) {
    const auto parsed = parseUnsigned(value, optionName);
    if (parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(optionName + " is too large.");
    }
    return static_cast<int>(parsed);
}

// 将 YYYY-MM-DD 转成当天 00:00:00 的 Unix 秒，并校验真实日历日期。
std::int64_t parseDateStart(const std::string& value) {
    std::tm tm{};
    std::istringstream stream(value);
    stream >> std::get_time(&tm, "%Y-%m-%d");
    if (stream.fail() || stream.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("Date must use YYYY-MM-DD format: " + value);
    }
    const int expectedYear = tm.tm_year;
    const int expectedMonth = tm.tm_mon;
    const int expectedDay = tm.tm_mday;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    const auto result = std::mktime(&tm);
    if (result == static_cast<std::time_t>(-1)
        || tm.tm_year != expectedYear || tm.tm_mon != expectedMonth
        || tm.tm_mday != expectedDay) {
        throw std::runtime_error("Date is not a valid calendar day: " + value);
    }
    return static_cast<std::int64_t>(result);
}

// “截止日期”覆盖当天最后一秒，使用户输入更符合直觉。
std::int64_t parseDateEnd(const std::string& value) {
    return parseDateStart(value) + 24 * 60 * 60 - 1;
}

// 备份类操作和归档类操作的统计字段不同，分别使用两个格式化函数。
void printStats(const std::string& action, const OperationStats& stats) {
    std::cout << action << " summary:\n"
              << "  directories: " << stats.directories << '\n'
              << "  files: " << stats.files << '\n'
              << "  skipped: " << stats.skippedFiles << '\n'
              << "  failed: " << stats.failedFiles << '\n'
              << "  bytes: " << stats.bytes << '\n';
}

// 归档统计额外显示存储字节数，以便观察压缩后的实际体积。
void printArchiveStats(const std::string& action, const ArchiveStats& stats) {
    std::cout << action << " summary:\n"
              << "  files: " << stats.files << '\n'
              << "  original bytes: " << stats.bytes << '\n'
              << "  stored bytes: " << stats.storedBytes << '\n';
}

// 所有备份过滤参数的唯一解析入口；未知选项立即报错，避免静默忽略拼写错误。
BackupOptions parseOptions(const CommandLineArguments& arguments, std::size_t firstOptionIndex) {
    BackupOptions options;
    for (std::size_t i = firstOptionIndex; i < arguments.size(); ++i) {
        const std::string& arg = arguments.text(i);
        if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg.rfind("--ext=", 0) == 0) {
            options.includeExtensions = splitCsv(arg.substr(6));
            if (options.includeExtensions.empty()) {
                throw std::runtime_error("--ext must contain at least one extension.");
            }
        } else if (arg.rfind("--max-size=", 0) == 0) {
            options.maxSizeBytes = static_cast<std::uintmax_t>(
                parseUnsigned(arg.substr(11), "--max-size"));
        } else if (arg.rfind("--name-contains=", 0) == 0) {
            options.nameContains = arg.substr(16);
        } else if (arg.rfind("--path-contains=", 0) == 0) {
            options.pathContains = arg.substr(16);
        } else if (arg.rfind("--modified-after=", 0) == 0) {
            options.modifiedAfter = parseDateStart(arg.substr(17));
        } else if (arg.rfind("--modified-before=", 0) == 0) {
            options.modifiedBefore = parseDateEnd(arg.substr(18));
        } else if (arg.rfind("--keep=", 0) == 0) {
            continue;
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }
    return options;
}

// 解析 Pack/Unpack 共用的归档选项，目前仅支持非空密码。
ArchiveOptions parseArchiveOptions(const CommandLineArguments& arguments,
                                   std::size_t firstOptionIndex) {
    ArchiveOptions options;
    for (std::size_t i = firstOptionIndex; i < arguments.size(); ++i) {
        const std::string& arg = arguments.text(i);
        if (arg.rfind("--password=", 0) == 0) {
            options.password = arg.substr(11);
            if (options.password.empty()) {
                throw std::runtime_error("--password must not be empty.");
            }
        } else {
            throw std::runtime_error("Unknown archive option: " + arg);
        }
    }
    return options;
}

// 生成既可读又可按字典序排序的快照目录名，同一秒内用三位序号消除重名。
std::string snapshotName(int index) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif
    std::ostringstream stream;
    stream << "snapshot_" << std::put_time(&localTime, "%Y%m%d_%H%M%S")
           << "_" << std::setw(3) << std::setfill('0') << index;
    return stream.str();
}

// --keep 未提供时返回 -1，表示不执行快照清理；0 则表示全部清理。
int parseKeepCount(const CommandLineArguments& arguments, std::size_t firstOptionIndex) {
    for (std::size_t i = firstOptionIndex; i < arguments.size(); ++i) {
        const std::string& arg = arguments.text(i);
        if (arg.rfind("--keep=", 0) == 0) {
            return parseNonNegativeInt(arg.substr(7), "--keep");
        }
    }
    return -1;
}

// 快照名按 YYYYMMDD_HHMMSS_序号 排序，字典序即时间顺序，可直接删除最旧项。
void pruneSnapshots(const std::filesystem::path& snapshotRoot, int keepCount) {
    if (keepCount < 0) {
        return;
    }

    std::vector<std::filesystem::directory_entry> snapshots;
    for (const auto& item : std::filesystem::directory_iterator(snapshotRoot)) {
        if (item.is_directory()
            && pathToUtf8(item.path().filename()).rfind("snapshot_", 0) == 0) {
            snapshots.push_back(item);
        }
    }

    std::sort(snapshots.begin(), snapshots.end(),
              [](const auto& left, const auto& right) {
                  return pathToUtf8(left.path().filename())
                       < pathToUtf8(right.path().filename());
              });

    while (static_cast<int>(snapshots.size()) > keepCount) {
        std::filesystem::remove_all(snapshots.front().path());
        std::cout << "Pruned old snapshot: " << pathToUtf8(snapshots.front().path()) << '\n';
        snapshots.erase(snapshots.begin());
    }
}
}

int main(int argc, char* argv[]) {
    try {
        // CLI 入口只负责参数校验与命令分发；实际文件操作全部交给两个 Manager。
        const CommandLineArguments arguments(argc, argv);
        if (arguments.size() < 2) {
            printUsage();
            return 1;
        }

        const std::string& command = arguments.text(1);
        if (command == "backup") {
            if (arguments.size() < 4) {
                printUsage();
                return 1;
            }
            BackupManager manager(parseOptions(arguments, 4));
            const auto stats = manager.backup(arguments.path(2), arguments.path(3));
            std::cout << "Backup completed.\n";
            printStats("Backup", stats);
            return 0;
        }

        if (command == "restore") {
            if (arguments.size() != 4) {
                printUsage();
                return 1;
            }
            BackupManager manager;
            const auto stats = manager.restore(arguments.path(2), arguments.path(3));
            std::cout << "Restore completed.\n";
            printStats("Restore", stats);
            return 0;
        }

        if (command == "schedule") {
            if (arguments.size() < 6) {
                printUsage();
                return 1;
            }
            const int intervalSeconds = parseNonNegativeInt(arguments.text(4), "interval_seconds");
            const int count = parseNonNegativeInt(arguments.text(5), "count");
            const int keepCount = parseKeepCount(arguments, 6);
            if (count == 0) {
                throw std::runtime_error("Schedule count must be > 0.");
            }

            // 每次快照目录名都不同，但仍允许覆盖同名秒级快照，保证定时任务可重试。
            auto options = parseOptions(arguments, 6);
            options.overwrite = true;
            BackupManager manager(options);
            OperationStats total;
            std::filesystem::create_directories(arguments.path(3));

            // 当前实现为进程内定时循环：备份、执行保留策略、等待，再进入下一轮。
            for (int i = 1; i <= count; ++i) {
                const auto snapshotDir = arguments.path(3) / snapshotName(i);
                const auto stats = manager.backup(arguments.path(2), snapshotDir);
                total.directories += stats.directories;
                total.files += stats.files;
                total.skippedFiles += stats.skippedFiles;
                total.failedFiles += stats.failedFiles;
                total.bytes += stats.bytes;
                std::cout << "Snapshot " << i << " completed: " << pathToUtf8(snapshotDir) << '\n';
                pruneSnapshots(arguments.path(3), keepCount);
                if (i != count && intervalSeconds > 0) {
                    std::this_thread::sleep_for(std::chrono::seconds(intervalSeconds));
                }
            }

            std::cout << "Scheduled backup completed.\n";
            printStats("Schedule", total);
            return 0;
        }

        if (command == "verify") {
            if (arguments.size() != 3) {
                printUsage();
                return 1;
            }
            BackupManager manager;
            const auto stats = manager.verify(arguments.path(2));
            const bool ok = stats.failedFiles == 0;
            std::cout << (ok ? "Backup is valid.\n" : "Backup is broken.\n");
            printStats("Verify", stats);
            // 0 表示清单完全匹配，2 专门表示发现损坏，便于脚本区别于普通参数错误。
            return ok ? 0 : 2;
        }

        if (command == "pack") {
            if (arguments.size() < 4) {
                printUsage();
                return 1;
            }
            ArchiveManager manager(parseArchiveOptions(arguments, 4));
            const auto stats = manager.pack(arguments.path(2), arguments.path(3));
            std::cout << "Archive created.\n";
            printArchiveStats("Pack", stats);
            return 0;
        }

        if (command == "unpack") {
            if (arguments.size() < 4) {
                printUsage();
                return 1;
            }
            ArchiveManager manager(parseArchiveOptions(arguments, 4));
            const auto stats = manager.unpack(arguments.path(2), arguments.path(3));
            std::cout << "Archive unpacked.\n";
            printArchiveStats("Unpack", stats);
            return 0;
        }

        printUsage();
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}
