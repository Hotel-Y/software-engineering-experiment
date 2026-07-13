#include "ArchiveManager.h"
#include "BackupManager.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
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

std::int64_t parseDateStart(const std::string& value) {
    std::tm tm{};
    std::istringstream stream(value);
    stream >> std::get_time(&tm, "%Y-%m-%d");
    if (stream.fail()) {
        throw std::runtime_error("Date must use YYYY-MM-DD format: " + value);
    }
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return static_cast<std::int64_t>(std::mktime(&tm));
}

std::int64_t parseDateEnd(const std::string& value) {
    return parseDateStart(value) + 24 * 60 * 60 - 1;
}

void printStats(const std::string& action, const OperationStats& stats) {
    std::cout << action << " summary:\n"
              << "  directories: " << stats.directories << '\n'
              << "  files: " << stats.files << '\n'
              << "  skipped: " << stats.skippedFiles << '\n'
              << "  failed: " << stats.failedFiles << '\n'
              << "  bytes: " << stats.bytes << '\n';
}

void printArchiveStats(const std::string& action, const ArchiveStats& stats) {
    std::cout << action << " summary:\n"
              << "  files: " << stats.files << '\n'
              << "  original bytes: " << stats.bytes << '\n'
              << "  stored bytes: " << stats.storedBytes << '\n';
}

BackupOptions parseOptions(int argc, char* argv[], int firstOptionIndex) {
    BackupOptions options;
    for (int i = firstOptionIndex; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg.rfind("--ext=", 0) == 0) {
            options.includeExtensions = splitCsv(arg.substr(6));
        } else if (arg.rfind("--max-size=", 0) == 0) {
            options.maxSizeBytes = static_cast<std::uintmax_t>(std::stoull(arg.substr(11)));
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

ArchiveOptions parseArchiveOptions(int argc, char* argv[], int firstOptionIndex) {
    ArchiveOptions options;
    for (int i = firstOptionIndex; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--password=", 0) == 0) {
            options.password = arg.substr(11);
        } else {
            throw std::runtime_error("Unknown archive option: " + arg);
        }
    }
    return options;
}

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

int parseKeepCount(int argc, char* argv[], int firstOptionIndex) {
    for (int i = firstOptionIndex; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--keep=", 0) == 0) {
            const int keep = std::stoi(arg.substr(7));
            if (keep < 0) {
                throw std::runtime_error("--keep must be >= 0.");
            }
            return keep;
        }
    }
    return -1;
}

void pruneSnapshots(const std::filesystem::path& snapshotRoot, int keepCount) {
    if (keepCount < 0) {
        return;
    }

    std::vector<std::filesystem::directory_entry> snapshots;
    for (const auto& item : std::filesystem::directory_iterator(snapshotRoot)) {
        if (item.is_directory() && item.path().filename().string().rfind("snapshot_", 0) == 0) {
            snapshots.push_back(item);
        }
    }

    std::sort(snapshots.begin(), snapshots.end(),
              [](const auto& left, const auto& right) {
                  return left.path().filename().string() < right.path().filename().string();
              });

    while (static_cast<int>(snapshots.size()) > keepCount) {
        std::filesystem::remove_all(snapshots.front().path());
        std::cout << "Pruned old snapshot: " << snapshots.front().path().string() << '\n';
        snapshots.erase(snapshots.begin());
    }
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            printUsage();
            return 1;
        }

        const std::string command = argv[1];
        if (command == "backup") {
            if (argc < 4) {
                printUsage();
                return 1;
            }
            BackupManager manager(parseOptions(argc, argv, 4));
            const auto stats = manager.backup(argv[2], argv[3]);
            std::cout << "Backup completed.\n";
            printStats("Backup", stats);
            return 0;
        }

        if (command == "restore") {
            if (argc != 4) {
                printUsage();
                return 1;
            }
            BackupManager manager;
            const auto stats = manager.restore(argv[2], argv[3]);
            std::cout << "Restore completed.\n";
            printStats("Restore", stats);
            return 0;
        }

        if (command == "schedule") {
            if (argc < 6) {
                printUsage();
                return 1;
            }
            const int intervalSeconds = std::stoi(argv[4]);
            const int count = std::stoi(argv[5]);
            const int keepCount = parseKeepCount(argc, argv, 6);
            if (intervalSeconds < 0 || count <= 0) {
                throw std::runtime_error("Schedule interval must be >= 0 and count must be > 0.");
            }

            auto options = parseOptions(argc, argv, 6);
            options.overwrite = true;
            BackupManager manager(options);
            OperationStats total;
            std::filesystem::create_directories(argv[3]);

            for (int i = 1; i <= count; ++i) {
                const auto snapshotDir = std::filesystem::path(argv[3]) / snapshotName(i);
                const auto stats = manager.backup(argv[2], snapshotDir);
                total.directories += stats.directories;
                total.files += stats.files;
                total.skippedFiles += stats.skippedFiles;
                total.failedFiles += stats.failedFiles;
                total.bytes += stats.bytes;
                std::cout << "Snapshot " << i << " completed: " << snapshotDir.string() << '\n';
                pruneSnapshots(argv[3], keepCount);
                if (i != count && intervalSeconds > 0) {
                    std::this_thread::sleep_for(std::chrono::seconds(intervalSeconds));
                }
            }

            std::cout << "Scheduled backup completed.\n";
            printStats("Schedule", total);
            return 0;
        }

        if (command == "verify") {
            if (argc != 3) {
                printUsage();
                return 1;
            }
            BackupManager manager;
            const auto stats = manager.verify(argv[2]);
            const bool ok = stats.failedFiles == 0;
            std::cout << (ok ? "Backup is valid.\n" : "Backup is broken.\n");
            printStats("Verify", stats);
            return ok ? 0 : 2;
        }

        if (command == "pack") {
            if (argc < 4) {
                printUsage();
                return 1;
            }
            ArchiveManager manager(parseArchiveOptions(argc, argv, 4));
            const auto stats = manager.pack(argv[2], argv[3]);
            std::cout << "Archive created.\n";
            printArchiveStats("Pack", stats);
            return 0;
        }

        if (command == "unpack") {
            if (argc < 4) {
                printUsage();
                return 1;
            }
            ArchiveManager manager(parseArchiveOptions(argc, argv, 4));
            const auto stats = manager.unpack(argv[2], argv[3]);
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
