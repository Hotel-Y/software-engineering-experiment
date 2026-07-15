#include "GuiDefaults.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    using sbm::gui::BackupPreparation;
    using sbm::gui::InputKind;
    using sbm::gui::inferSmartDefaults;
    using sbm::gui::prepareBackupForDependentAction;

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("sbm_gui_defaults_" + std::to_string(nonce));
    try {
        const auto source = root / "photos";
        const auto backup = root / "ready_backup";
        const auto archive = root / "holiday.sba";
        fs::create_directories(source);
        fs::create_directories(backup);
        std::ofstream(backup / "manifest.sbm") << "SBM1\n";
        std::ofstream(archive) << "SBA5";

        const auto sourceDefaults = inferSmartDefaults(source.wstring());
        require(sourceDefaults.kind == InputKind::SourceDirectory,
                "source directory should be detected");
        require(sourceDefaults.backup == root / "photos_backup",
                "source backup default should use sibling _backup directory");
        require(sourceDefaults.archive == root / "photos_backup.sba",
                "source archive default should follow the backup name");
        require(sourceDefaults.restore == root / "photos_restored",
                "source restore default should use sibling _restored directory");
        require(prepareBackupForDependentAction(sourceDefaults.source, sourceDefaults.backup) ==
                    BackupPreparation::CreateFromSource,
                "Pack from a source input should automatically create its prerequisite backup");

        fs::create_directories(root / "photos_backup");
        const auto collisionDefaults = inferSmartDefaults(source.wstring());
        require(collisionDefaults.backup == root / "photos_backup_2",
                "existing backup targets should receive a numeric suffix");
        require(collisionDefaults.archive == root / "photos_backup_2.sba",
                "archive default should follow the collision-free backup name");

        const auto backupDefaults = inferSmartDefaults(backup.wstring());
        require(backupDefaults.kind == InputKind::BackupDirectory,
                "manifest directory should be detected as a backup");
        require(backupDefaults.backup == backup,
                "detected backup should populate the backup field");
        require(backupDefaults.archive == root / "ready_backup.sba",
                "backup archive should be created beside the backup");
        require(prepareBackupForDependentAction(backupDefaults.source, backupDefaults.backup) ==
                    BackupPreparation::Ready,
                "manifest backup should be immediately ready for Pack");

        const auto archiveDefaults = inferSmartDefaults(archive.wstring());
        require(archiveDefaults.kind == InputKind::ArchiveFile,
                ".sba file should be detected as an archive");
        require(archiveDefaults.archive == archive,
                "detected archive should populate the archive field");
        require(archiveDefaults.backup == root / "holiday_unpacked",
                "archive output should use an _unpacked directory");
        require(prepareBackupForDependentAction(archiveDefaults.source, archiveDefaults.backup) ==
                    BackupPreparation::Unavailable,
                "archive input must not be mistaken for a source directory when Pack is clicked");

        const auto missingDefaults = inferSmartDefaults((root / "missing").wstring());
        require(!missingDefaults.valid(), "missing paths should not produce defaults");

        fs::remove_all(root);
        std::cout << "GUI smart-default tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code cleanupError;
        fs::remove_all(root, cleanupError);
        std::cerr << "GUI smart-default test failed: " << error.what() << '\n';
        return 1;
    }
}
