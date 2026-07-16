// Simple Backup Manager 的 Catch2 v3 单元测试；每个 TEST_CASE 都创建独立临时环境。

#include <catch2/catch_all.hpp>
#include "FileUtils.h"
#include "Manifest.h"
#include "BackupManager.h"
#include "ArchiveManager.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

// ============================================================================
// 测试辅助工具
// ============================================================================

namespace {
    const auto projectRoot = std::filesystem::absolute(".");

    // RAII 临时目录：构造时创建，测试结束或异常退出时自动递归清理。
    struct TempDir {
        std::filesystem::path path;
        TempDir() : path(projectRoot / "build" / "tmp_test" /
                         ("sbm_" + std::to_string(std::random_device{}()))) {
            std::filesystem::create_directories(path);
        }
        ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
        // 按相对路径创建父目录并写入二进制内容。
        void write(const std::string& rp, const std::string& c) const {
            auto f = path / rp;
            std::filesystem::create_directories(f.parent_path());
            std::ofstream(f, std::ios::binary).write(c.data(), c.size());
        }
        void mkdir(const std::string& rp) const {
            std::filesystem::create_directories(path / rp);
        }
    };

    // 流式读取测试文件，避免文本模式换行转换影响内容断言。
    std::string readFile(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return {};
        std::string r; char buf[4096];
        while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
            r.append(buf, static_cast<std::size_t>(in.gcount()));
        return r;
    }

    // 单元测试默认允许覆盖，避免重复准备同一路径时被前置策略干扰。
    BackupOptions overwriteOpts() {
        BackupOptions o;
        o.overwrite = true;
        return o;
    }

    BackupManager manager() {
        return BackupManager(overwriteOpts());
    }
}

// ============================================================================
// FileUtils：扩展名、校验值、时间、UTF-8 路径和根目录约束
// ============================================================================

TEST_CASE("normalizeExtension", "[fileutils]") {
    CHECK(normalizeExtension("txt") == ".txt");
    CHECK(normalizeExtension("cpp") == ".cpp");
    CHECK(normalizeExtension(".txt") == ".txt");
    CHECK(normalizeExtension(".CPP") == ".cpp");
    CHECK(normalizeExtension("") == "");
}

TEST_CASE("fnv1aFileChecksum same content", "[fileutils]") {
    TempDir d; d.write("a.txt", "hello"); d.write("b.txt", "hello");
    CHECK(fnv1aFileChecksum(d.path/"a.txt") == fnv1aFileChecksum(d.path/"b.txt"));
}

TEST_CASE("fnv1aFileChecksum different content", "[fileutils]") {
    TempDir d; d.write("a.txt", "hello"); d.write("b.txt", "world");
    CHECK(fnv1aFileChecksum(d.path/"a.txt") != fnv1aFileChecksum(d.path/"b.txt"));
}

TEST_CASE("fnv1aFileChecksum empty", "[fileutils]") {
    TempDir d; d.write("e.txt", "");
    CHECK(fnv1aFileChecksum(d.path/"e.txt") == 14695981039346656037ULL);
}

TEST_CASE("fnv1aFileChecksum nonexistent", "[fileutils]") {
    TempDir d; CHECK_THROWS(fnv1aFileChecksum(d.path/"x.txt"));
}

TEST_CASE("toUnixSeconds roundtrip", "[fileutils]") {
    std::int64_t t = 1700000000;
    CHECK(std::abs(toUnixSeconds(fromUnixSeconds(t)) - t) <= 1);
}

TEST_CASE("pathToUtf8 roundtrip", "[fileutils]") {
    CHECK(pathToUtf8(pathFromUtf8("docs/report.txt")) == "docs/report.txt");
    CHECK(pathToUtf8(pathFromUtf8("文件夹/报告.txt")) == "文件夹/报告.txt");
}

TEST_CASE("isPathSameOrInside", "[fileutils]") {
    TempDir d;
    CHECK(isPathSameOrInside(d.path, d.path));
    auto c = d.path/"sub"; std::filesystem::create_directories(c);
    CHECK(isPathSameOrInside(c, d.path));
    CHECK_FALSE(isPathSameOrInside(d.path, c));
    CHECK_FALSE(isPathSameOrInside(d.path/"a", d.path/"b"));
}

TEST_CASE("resolvePathInside", "[fileutils]") {
    TempDir d; d.mkdir("sub/deep");
    CHECK(resolvePathInside(d.path, "sub/deep") == d.path/"sub"/"deep");
    CHECK_THROWS(resolvePathInside(d.path, ""));
    CHECK_THROWS(resolvePathInside(d.path, "../escape"));
    CHECK_THROWS(resolvePathInside(d.path, "/etc/passwd"));
}

// ============================================================================
// Manifest：内存条目、保存/读取往返和损坏格式拒绝
// ============================================================================

TEST_CASE("Manifest add and retrieve", "[manifest]") {
    Manifest m;
    CHECK(m.entries().empty());
    ManifestEntry e;
    e.relativePath = "test.txt"; e.size = 123; e.checksum = 42;
    m.add(e);
    CHECK(m.entries().size() == 1);
    CHECK(m.entries()[0].relativePath == "test.txt");
    CHECK(m.entries()[0].size == 123);
}

TEST_CASE("Manifest multiple entries", "[manifest]") {
    Manifest m;
    for (int i = 0; i < 10; ++i) {
        ManifestEntry e; e.relativePath = "f" + std::to_string(i) + ".txt";
        m.add(e);
    }
    CHECK(m.entries().size() == 10);
}

TEST_CASE("Manifest directory entry", "[manifest]") {
    Manifest m;
    ManifestEntry e; e.relativePath = "mydir"; e.isDirectory = true;
    m.add(e);
    CHECK(m.entries().back().isDirectory);
}

TEST_CASE("Manifest save and load", "[manifest]") {
    TempDir d;
    Manifest o;
    o.add({"a.txt", 100, 1700000000, 12345, false});
    o.add({"sub/b.txt", 200, 1700000001, 67890, false});
    o.add({"empty", 0, 1700000002, 0, true});

    o.save(d.path/"m.sbm");
    auto l = Manifest::load(d.path/"m.sbm");
    REQUIRE(l.entries().size() == 3);
    CHECK(l.entries()[0].relativePath == "a.txt");
    CHECK(l.entries()[0].size == 100);
    CHECK(l.entries()[1].relativePath == "sub/b.txt");
    CHECK(l.entries()[2].isDirectory == true);
}

TEST_CASE("Manifest rejects bad header", "[manifest]") {
    TempDir d;
    std::ofstream(d.path/"b.sbm") << "BAD\n";
    CHECK_THROWS(Manifest::load(d.path/"b.sbm"));
}

TEST_CASE("Manifest rejects duplicates", "[manifest]") {
    TempDir d;
    std::ofstream(d.path/"d.sbm") << "SBM1\nF|f.txt|100|0|1\nF|f.txt|200|0|2\n";
    CHECK_THROWS(Manifest::load(d.path/"d.sbm"));
}

// ============================================================================
// BackupManager：备份筛选、覆盖策略、还原与异常场景
// ============================================================================

TEST_CASE("Backup basic files and verify", "[backup]") {
    TempDir src, dst;
    src.write("a.txt", "hello");
    src.write("b.txt", "world");
    src.mkdir("sub");
    src.write("sub/c.txt", "deep");

    auto s = manager().backup(src.path, dst.path);
    CHECK(s.files == 3);
    CHECK(s.directories >= 1);
    CHECK(s.skippedFiles == 0);
    CHECK(std::filesystem::exists(dst.path/"manifest.sbm"));
    CHECK(readFile(dst.path/"a.txt") == "hello");
    CHECK(readFile(dst.path/"sub/c.txt") == "deep");

    auto v = BackupManager{}.verify(dst.path);
    CHECK(v.files == 3);
    CHECK(v.failedFiles == 0);
}

TEST_CASE("Backup overwrite control", "[backup]") {
    TempDir src, dst;
    src.write("a.txt", "x");
    std::filesystem::create_directories(dst.path);
    CHECK_THROWS_AS(BackupManager({}).backup(src.path, dst.path), std::runtime_error);
    CHECK_NOTHROW(manager().backup(src.path, dst.path));
}

TEST_CASE("Backup rejects inside source", "[backup]") {
    TempDir src;
    src.write("a.txt", "x");
    CHECK_THROWS_AS(manager().backup(src.path, src.path/"inside"), std::runtime_error);
}

TEST_CASE("Backup extension filter", "[backup]") {
    TempDir src, dst;
    src.write("a.txt", "t"); src.write("b.cpp", "c"); src.write("c.bin", "b");
    BackupOptions o; o.includeExtensions = {".txt", ".cpp"}; o.overwrite = true;
    auto s = BackupManager(o).backup(src.path, dst.path);
    CHECK(s.files == 2);
    CHECK(s.skippedFiles == 1);
    CHECK(std::filesystem::exists(dst.path/"a.txt"));
    CHECK(std::filesystem::exists(dst.path/"b.cpp"));
    CHECK_FALSE(std::filesystem::exists(dst.path/"c.bin"));
}

TEST_CASE("Backup max-size filter", "[backup]") {
    TempDir src, dst;
    src.write("s.txt", "small");
    src.write("l.txt", std::string(1000, 'x'));
    BackupOptions o; o.maxSizeBytes = 50; o.overwrite = true;
    auto s = BackupManager(o).backup(src.path, dst.path);
    CHECK(s.files == 1);
    CHECK(s.skippedFiles == 1);
    CHECK(std::filesystem::exists(dst.path/"s.txt"));
    CHECK_FALSE(std::filesystem::exists(dst.path/"l.txt"));
}

TEST_CASE("Backup name-contains filter", "[backup]") {
    TempDir src, dst;
    src.write("report.txt", "r"); src.write("data.txt", "d");
    BackupOptions o; o.nameContains = "re"; o.overwrite = true;
    auto s = BackupManager(o).backup(src.path, dst.path);
    CHECK(s.files == 1);
    CHECK(std::filesystem::exists(dst.path/"report.txt"));
    CHECK_FALSE(std::filesystem::exists(dst.path/"data.txt"));
}

TEST_CASE("Backup restore roundtrip", "[backup][restore]") {
    TempDir src, dst, rst;
    src.write("a.txt", "hello backup");
    src.write("b.bin", std::string(100, '\x42'));
    src.mkdir("data");
    src.write("data/nested.txt", "nested");

    CHECK(manager().backup(src.path, dst.path).files == 3);
    auto rs = manager().restore(dst.path, rst.path);
    CHECK(rs.files == 3);
    CHECK(readFile(rst.path/"a.txt") == "hello backup");
    CHECK(readFile(rst.path/"data/nested.txt") == "nested");
    CHECK(readFile(rst.path/"b.bin") == readFile(src.path/"b.bin"));
}

TEST_CASE("Restore checksum validation", "[restore]") {
    TempDir src, dst;
    src.write("a.txt", "original");
    manager().backup(src.path, dst.path);
    std::ofstream(dst.path/"a.txt", std::ios::binary | std::ios::trunc) << "corrupted";
    TempDir rst;
    CHECK_THROWS(manager().restore(dst.path, rst.path));
}

TEST_CASE("Verify detects corrupted files", "[edge]") {
    TempDir src, dst;
    src.write("g.txt", "good"); src.write("b.txt", "original");
    manager().backup(src.path, dst.path);
    std::ofstream(dst.path/"b.txt", std::ios::binary | std::ios::trunc) << "modified";
    auto v = BackupManager({}).verify(dst.path);
    CHECK(v.failedFiles == 1);
    CHECK(v.files == 1);
}

TEST_CASE("Backup all files filtered out", "[edge]") {
    TempDir src, dst;
    src.write("a.txt", "a"); src.write("b.cpp", "b");
    BackupOptions o; o.includeExtensions = {".py"}; o.overwrite = true;
    auto s = BackupManager(o).backup(src.path, dst.path);
    CHECK(s.files == 0);
    CHECK(s.skippedFiles == 2);
}

TEST_CASE("Backup empty directory", "[edge]") {
    TempDir src, dst;
    auto s = manager().backup(src.path, dst.path);
    CHECK(s.files == 0);
    auto v = BackupManager({}).verify(dst.path);
    CHECK(v.failedFiles == 0);
    TempDir rst;
    CHECK(manager().restore(dst.path, rst.path).files == 0);
}

TEST_CASE("Backup deep nesting", "[edge]") {
    TempDir src, dst;
    std::string p = "a";
    for (int i = 0; i < 10; ++i) {
        src.write(p + "/f.txt", "c" + std::to_string(i));
        p += "/b";
    }
    auto s = manager().backup(src.path, dst.path);
    CHECK(s.files == 10);
}

TEST_CASE("Backup special chars in names", "[edge]") {
    TempDir src, dst;
    src.write("file with spaces.txt", "spaces");
    src.write("file-with-dashes.txt", "dashes");
    src.write("file_with_underscores.txt", "underscores");
    src.write("file.with.dots.txt", "dots");
    CHECK(manager().backup(src.path, dst.path).files == 4);
    CHECK(std::filesystem::exists(dst.path/"file with spaces.txt"));
    CHECK(std::filesystem::exists(dst.path/"file-with-dashes.txt"));
    CHECK(std::filesystem::exists(dst.path/"file_with_underscores.txt"));
    CHECK(std::filesystem::exists(dst.path/"file.with.dots.txt"));
}

// ============================================================================
// ArchiveManager：普通/加密归档、解包、还原与非法输出位置
// ============================================================================

TEST_CASE("Archive pack and unpack basic", "[archive]") {
    TempDir src, dst, out;
    src.write("a.txt", "hello archive");
    src.write("b.txt", "world archive");
    manager().backup(src.path, dst.path);

    auto ar = out.path/"test.sba";
    ArchiveManager({}).pack(dst.path, ar);
    CHECK(std::filesystem::exists(ar));
    CHECK(std::filesystem::file_size(ar) > 0);

    auto up = out.path/"unpacked";
    ArchiveManager({}).unpack(ar, up);
    CHECK(std::filesystem::exists(up/"manifest.sbm"));
    CHECK(readFile(up/"a.txt") == "hello archive");
    CHECK(readFile(up/"b.txt") == "world archive");
    CHECK(BackupManager({}).verify(up).failedFiles == 0);
}

TEST_CASE("Archive pack and restore", "[archive][restore]") {
    TempDir src, dst, out, fr;
    src.write("data.bin", std::string(500, '\xAB'));
    src.write("notes.txt", "important");
    src.mkdir("cfg");
    src.write("cfg/s.json", "{}");

    manager().backup(src.path, dst.path);
    auto ar = out.path/"b.sba";
    ArchiveManager({}).pack(dst.path, ar);

    auto up = out.path/"r";
    ArchiveManager({}).unpack(ar, up);
    CHECK(BackupManager({}).verify(up).failedFiles == 0);
    CHECK(BackupManager({}).verify(up).files >= 2);

    CHECK(manager().restore(up, fr.path).files >= 2);
    CHECK(readFile(fr.path/"notes.txt") == "important");
    CHECK(readFile(fr.path/"data.bin") == readFile(src.path/"data.bin"));
}

TEST_CASE("Archive encrypted", "[archive][encryption]") {
    TempDir src, dst, out;
    src.write("secret.txt", "secret message");
    manager().backup(src.path, dst.path);

    auto ar = out.path/"e.sba";
    ArchiveOptions po; po.password = "pwd123";
    ArchiveManager(po).pack(dst.path, ar);
    CHECK(std::filesystem::exists(ar));

    TempDir wo; ArchiveOptions wop; wop.password = "wrong";
    CHECK_THROWS(ArchiveManager(wop).unpack(ar, wo.path));

    TempDir co; ArchiveOptions cop; cop.password = "pwd123";
    ArchiveManager(cop).unpack(ar, co.path);
    CHECK(readFile(co.path/"secret.txt") == "secret message");

    TempDir no;
    CHECK_THROWS(ArchiveManager({}).unpack(ar, no.path));
}

TEST_CASE("Archive rejects inside backup dir", "[archive]") {
    TempDir src, dst;
    src.write("a.txt", "x");
    manager().backup(src.path, dst.path);
    CHECK_THROWS_AS(ArchiveManager({}).pack(dst.path, dst.path/"n.sba"), std::runtime_error);
}

// ============================================================================
// 跨模块集成：Backup -> Pack -> Unpack -> Verify -> Restore 完整数据流
// ============================================================================

TEST_CASE("Full workflow backup-pack-unpack-verify-restore", "[integration]") {
    TempDir root;
    auto src = root.path/"source";
    std::filesystem::create_directories(src);
    std::ofstream(src/"doc.txt", std::ios::binary) << "documentation\n";
    std::ofstream(src/"code.cpp", std::ios::binary) << "int main() {}\n";
    std::filesystem::create_directories(src/"img");
    std::ofstream(src/"img/logo.png", std::ios::binary) << "PNG";

    auto bak = root.path/"backup";
    BackupOptions o; o.includeExtensions = {".txt", ".cpp"}; o.overwrite = true;
    CHECK(BackupManager(o).backup(src, bak).files == 2);
    CHECK(BackupManager{}.verify(bak).failedFiles == 0);

    auto ar = root.path/"backup.sba";
    ArchiveManager({}).pack(bak, ar);
    CHECK(std::filesystem::exists(ar));

    auto up = root.path/"unpacked";
    ArchiveManager({}).unpack(ar, up);
    CHECK(BackupManager{}.verify(up).failedFiles == 0);
    CHECK(BackupManager{}.verify(up).files == 2);

    auto rst = root.path/"restored";
    CHECK(manager().restore(up, rst).files == 2);
    CHECK(readFile(rst/"doc.txt") == "documentation\n");
    CHECK(readFile(rst/"code.cpp") == "int main() {}\n");
}
