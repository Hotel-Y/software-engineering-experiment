#include "ArchiveManager.h"

#include "FileUtils.h"
#include "Manifest.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::array<char, 8> archiveMagic = {'S', 'B', 'A', '4', '\r', '\n', '\0', '\1'};
constexpr const char* manifestName = "manifest.sbm";
constexpr std::uint64_t flagRle = 1;
constexpr std::uint64_t flagEncrypted = 2;
constexpr std::size_t saltSize = 16;
constexpr std::size_t ivSize = 12;
constexpr std::size_t tagSize = 16;
constexpr std::size_t keySize = 32;
constexpr int pbkdf2Iterations = 100000;

void ensureDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        throw std::runtime_error("Cannot create directory: " + pathToUtf8(path));
    }
}

void writeUint64(std::ostream& output, std::uint64_t value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output) {
        throw std::runtime_error("Failed to write archive data.");
    }
}

std::uint64_t readUint64(std::istream& input) {
    std::uint64_t value = 0;
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw std::runtime_error("Broken archive file.");
    }
    return value;
}

std::vector<unsigned char> readFileBytes(const std::filesystem::path& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read file: " + pathToUtf8(filePath));
    }
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>());
}

void writeFileBytes(const std::filesystem::path& filePath,
                    const std::vector<unsigned char>& bytes) {
    std::ofstream output(filePath, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Cannot write file: " + pathToUtf8(filePath));
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
}

std::vector<unsigned char> rleCompress(const std::vector<unsigned char>& input) {
    std::vector<unsigned char> output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        const unsigned char value = input[i];
        std::size_t run = 1;
        while (i + run < input.size() && input[i + run] == value && run < 255) {
            ++run;
        }
        output.push_back(static_cast<unsigned char>(run));
        output.push_back(value);
        i += run;
    }
    return output;
}

std::vector<unsigned char> rleDecompress(const std::vector<unsigned char>& input,
                                         std::uint64_t expectedSize) {
    if (input.size() % 2 != 0) {
        throw std::runtime_error("Broken RLE payload.");
    }

    std::vector<unsigned char> output;
    output.reserve(static_cast<std::size_t>(expectedSize));
    for (std::size_t i = 0; i < input.size(); i += 2) {
        const auto count = input[i];
        const auto value = input[i + 1];
        output.insert(output.end(), count, value);
    }
    if (output.size() != expectedSize) {
        throw std::runtime_error("RLE payload size mismatch.");
    }
    return output;
}

template <std::size_t N>
std::array<unsigned char, N> randomBytes() {
    std::array<unsigned char, N> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("OpenSSL failed to generate random bytes.");
    }
    return bytes;
}

std::array<unsigned char, keySize> deriveKey(
    const std::string& password,
    const std::array<unsigned char, saltSize>& salt) {
    std::array<unsigned char, keySize> key{};
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          pbkdf2Iterations, EVP_sha256(),
                          static_cast<int>(key.size()), key.data()) != 1) {
        throw std::runtime_error("OpenSSL PBKDF2 key derivation failed.");
    }
    return key;
}

std::vector<unsigned char> encryptAesGcm(
    const std::vector<unsigned char>& plaintext,
    const std::string& password,
    const std::array<unsigned char, saltSize>& salt,
    const std::array<unsigned char, ivSize>& iv,
    std::array<unsigned char, tagSize>& tag) {
    const auto key = deriveKey(password, salt);
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (!context) {
        throw std::runtime_error("OpenSSL failed to create encryption context.");
    }

    std::vector<unsigned char> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    int written = 0;
    int finalWritten = 0;
    const bool ok =
        EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1 &&
        EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), iv.data()) == 1 &&
        EVP_EncryptUpdate(context, ciphertext.data(), &written,
                          plaintext.data(), static_cast<int>(plaintext.size())) == 1 &&
        EVP_EncryptFinal_ex(context, ciphertext.data() + written, &finalWritten) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(tag.size()), tag.data()) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!ok) {
        throw std::runtime_error("OpenSSL AES-256-GCM encryption failed.");
    }
    ciphertext.resize(static_cast<std::size_t>(written + finalWritten));
    return ciphertext;
}

std::vector<unsigned char> decryptAesGcm(
    const std::vector<unsigned char>& ciphertext,
    const std::string& password,
    const std::array<unsigned char, saltSize>& salt,
    const std::array<unsigned char, ivSize>& iv,
    const std::array<unsigned char, tagSize>& tag) {
    const auto key = deriveKey(password, salt);
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (!context) {
        throw std::runtime_error("OpenSSL failed to create decryption context.");
    }

    std::vector<unsigned char> plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
    int written = 0;
    int finalWritten = 0;
    bool ok =
        EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) == 1 &&
        EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), iv.data()) == 1 &&
        EVP_DecryptUpdate(context, plaintext.data(), &written,
                          ciphertext.data(), static_cast<int>(ciphertext.size())) == 1;
    if (ok) {
        auto mutableTag = tag;
        ok = EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG,
                                 static_cast<int>(mutableTag.size()), mutableTag.data()) == 1 &&
             EVP_DecryptFinal_ex(context, plaintext.data() + written, &finalWritten) == 1;
    }
    EVP_CIPHER_CTX_free(context);
    if (!ok) {
        throw std::runtime_error("Archive authentication failed: wrong password or modified data.");
    }
    plaintext.resize(static_cast<std::size_t>(written + finalWritten));
    return plaintext;
}

template <std::size_t N>
void writeArray(std::ostream& output, const std::array<unsigned char, N>& bytes) {
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("Failed to write archive cryptographic metadata.");
    }
}

template <std::size_t N>
std::array<unsigned char, N> readArray(std::istream& input) {
    std::array<unsigned char, N> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("Broken archive cryptographic metadata.");
    }
    return bytes;
}

std::vector<unsigned char> readPayload(std::istream& input, std::uint64_t bytes) {
    std::vector<unsigned char> payload(static_cast<std::size_t>(bytes));
    if (!payload.empty()) {
        input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!input) {
            throw std::runtime_error("Failed to read archive payload.");
        }
    }
    return payload;
}

std::filesystem::path safeOutputPath(const std::filesystem::path& outputDir,
                                     const std::string& relativePath) {
    const auto base = std::filesystem::weakly_canonical(outputDir);
    const auto candidate = std::filesystem::weakly_canonical(outputDir / pathFromUtf8(relativePath));

    auto baseIt = base.begin();
    auto candidateIt = candidate.begin();
    for (; baseIt != base.end(); ++baseIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *candidateIt != *baseIt) {
            throw std::runtime_error("Archive entry path escapes output directory: " + relativePath);
        }
    }
    return candidate;
}
}

ArchiveManager::ArchiveManager(ArchiveOptions options) : options_(std::move(options)) {}

ArchiveStats ArchiveManager::pack(const std::filesystem::path& backupDir,
                                  const std::filesystem::path& archiveFile) const {
    const auto manifestFile = backupDir / manifestName;
    const auto manifest = Manifest::load(manifestFile);

    if (archiveFile.has_parent_path()) {
        ensureDirectory(archiveFile.parent_path());
    }

    std::ofstream output(archiveFile, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Cannot create archive: " + pathToUtf8(archiveFile));
    }

    output.write(archiveMagic.data(), static_cast<std::streamsize>(archiveMagic.size()));
    writeUint64(output, static_cast<std::uint64_t>(manifest.entries().size()));

    ArchiveStats stats;
    for (const auto& entry : manifest.entries()) {
        const auto pathBytes = entry.relativePath;
        output.put(entry.isDirectory ? 'D' : 'F');
        writeUint64(output, static_cast<std::uint64_t>(pathBytes.size()));
        output.write(pathBytes.data(), static_cast<std::streamsize>(pathBytes.size()));
        writeUint64(output, static_cast<std::uint64_t>(entry.size));
        writeUint64(output, static_cast<std::uint64_t>(entry.modifiedTime));
        writeUint64(output, entry.checksum);
        std::uint64_t flags = 0;
        std::array<unsigned char, saltSize> salt{};
        std::array<unsigned char, ivSize> iv{};
        std::array<unsigned char, tagSize> tag{};

        if (!entry.isDirectory) {
            auto payload = readFileBytes(backupDir / pathFromUtf8(entry.relativePath));
            if (options_.useRleCompression) {
                payload = rleCompress(payload);
                flags |= flagRle;
            }
            if (!options_.password.empty()) {
                salt = randomBytes<saltSize>();
                iv = randomBytes<ivSize>();
                payload = encryptAesGcm(payload, options_.password, salt, iv, tag);
                flags |= flagEncrypted;
            }
            writeUint64(output, flags);
            writeUint64(output, static_cast<std::uint64_t>(payload.size()));
            writeArray(output, salt);
            writeArray(output, iv);
            writeArray(output, tag);
            if (!payload.empty()) {
                output.write(reinterpret_cast<const char*>(payload.data()),
                             static_cast<std::streamsize>(payload.size()));
            }
            ++stats.files;
            stats.bytes += entry.size;
            stats.storedBytes += payload.size();
        } else {
            writeUint64(output, flags);
            writeUint64(output, 0);
            writeArray(output, salt);
            writeArray(output, iv);
            writeArray(output, tag);
        }
    }

    return stats;
}

ArchiveStats ArchiveManager::unpack(const std::filesystem::path& archiveFile,
                                    const std::filesystem::path& outputDir) const {
    std::ifstream input(archiveFile, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open archive: " + pathToUtf8(archiveFile));
    }

    std::array<char, 8> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (magic != archiveMagic) {
        throw std::runtime_error("Unsupported or broken archive format.");
    }

    ensureDirectory(outputDir);
    Manifest manifest;
    ArchiveStats stats;
    const auto entryCount = readUint64(input);

    for (std::uint64_t i = 0; i < entryCount; ++i) {
        const int type = input.get();
        if (type != 'D' && type != 'F') {
            throw std::runtime_error("Broken archive entry type.");
        }

        const auto pathLength = readUint64(input);
        std::string relativePath(pathLength, '\0');
        input.read(relativePath.data(), static_cast<std::streamsize>(pathLength));
        if (!input) {
            throw std::runtime_error("Broken archive path.");
        }

        ManifestEntry entry;
        entry.isDirectory = type == 'D';
        entry.relativePath = relativePath;
        entry.size = static_cast<std::uintmax_t>(readUint64(input));
        entry.modifiedTime = static_cast<std::int64_t>(readUint64(input));
        entry.checksum = readUint64(input);
        const auto flags = readUint64(input);
        const auto payloadSize = readUint64(input);
        const auto salt = readArray<saltSize>(input);
        const auto iv = readArray<ivSize>(input);
        const auto tag = readArray<tagSize>(input);
        manifest.add(entry);

        const auto outputPath = safeOutputPath(outputDir, relativePath);
        if (entry.isDirectory) {
            ensureDirectory(outputPath);
            continue;
        }

        ensureDirectory(outputPath.parent_path());
        auto payload = readPayload(input, payloadSize);
        if ((flags & flagEncrypted) != 0) {
            if (options_.password.empty()) {
                throw std::runtime_error("Archive is encrypted. Provide --password=<value>.");
            }
            payload = decryptAesGcm(payload, options_.password, salt, iv, tag);
        }
        if ((flags & flagRle) != 0) {
            payload = rleDecompress(payload, static_cast<std::uint64_t>(entry.size));
        }
        writeFileBytes(outputPath, payload);

        const auto checksum = fnv1aFileChecksum(outputPath);
        if (checksum != entry.checksum) {
            throw std::runtime_error("Unpacked file checksum mismatch: " + pathToUtf8(outputPath));
        }
        std::filesystem::last_write_time(outputPath, fromUnixSeconds(entry.modifiedTime));

        ++stats.files;
        stats.bytes += entry.size;
        stats.storedBytes += payloadSize;
    }

    manifest.save(outputDir / manifestName);
    return stats;
}
