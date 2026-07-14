#include "ArchiveManager.h"

#include "FileUtils.h"
#include "Manifest.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::array<char, 8> archiveMagic = {'S', 'B', 'A', '4', '\r', '\n', '\0', '\1'};
constexpr const char* manifestName = "manifest.sbm";
constexpr std::uint64_t flagCompressed = 1;  // payload is LZ+Huffman compressed, block-wise
constexpr std::uint64_t flagEncrypted = 2;
constexpr std::size_t saltSize = 16;
constexpr std::size_t ivSize = 12;
constexpr std::size_t tagSize = 16;
constexpr std::size_t keySize = 32;
constexpr int pbkdf2Iterations = 100000;
// Each file is compressed in fixed-size source blocks. Keeping the block bounded
// (rather than reading the whole file into one vector) is what fixes the peak-memory
// blowup on multi-gigabyte inputs: at any moment we only hold one block's worth of
// data plus its LZ/Huffman buffers, independent of the total file size.
constexpr std::size_t blockSize = 1u << 20;  // 1 MiB of source per block
// Per-block flags (first byte of each block record in the payload).
constexpr unsigned char blockFlagRawStored = 1;  // block payload is raw original bytes, not huffman(lz)

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

// ---- LZ stage ------------------------------------------------------------
// LZ77 with a hash-chain match finder over a sliding window. Unlike the old RLE
// stage (which only collapsed runs of identical bytes and therefore *expanded*
// ordinary data ~2x), LZ77 exploits repeated byte *sequences*, so its token stream
// has real redundancy for the Huffman stage to remove. The window and the hash
// tables are bounded, so memory stays flat regardless of input size.
//
// The token stream is byte-oriented so the existing byte-level Huffman stage works
// unchanged. Layout:
//   repeat: 1 flag byte, then up to 8 tokens
//     flag bit = 1 -> literal: next 1 byte is a literal
//     flag bit = 0 -> match : next 3 bytes = offsetLo, offsetHi, lengthCode
//                              offset 1..65535, length = lengthCode + minMatch
// Trailing unused flag bits are never read: the decoder stops as soon as it has
// produced the block's original size.
namespace lz {
constexpr std::uint32_t windowBits = 15;              // sliding window = 32768
constexpr std::uint32_t windowSize = 1u << windowBits;
constexpr std::uint32_t windowMask = windowSize - 1;
constexpr std::uint32_t minMatch = 4;                 // a match token costs 3 bytes, so >= 4 pays
constexpr std::uint32_t maxMatch = minMatch + 255;    // length byte range -> 4..259
constexpr std::uint32_t goodMatch = 32;              // accept a match this long without searching further
constexpr std::uint32_t maxChain = 128;              // cap hash-chain walks per position
constexpr std::uint32_t hashBits = 15;
constexpr std::uint32_t hashSize = 1u << hashBits;

inline std::uint32_t hash4(const unsigned char* p) {
    const std::uint32_t v = (static_cast<std::uint32_t>(p[0]) << 24)
                          | (static_cast<std::uint32_t>(p[1]) << 16)
                          | (static_cast<std::uint32_t>(p[2]) << 8)
                          |  static_cast<std::uint32_t>(p[3]);
    return (v * 2654435761u) >> (32 - hashBits);
}
}  // namespace lz

std::vector<unsigned char> lzCompress(const unsigned char* data, std::size_t n) {
    std::vector<unsigned char> output;
    output.reserve(n / 2 + 64);
    if (n == 0) {
        return output;
    }
    std::vector<std::uint32_t> head(lz::hashSize, 0xFFFFFFFFu);
    std::vector<std::uint32_t> prev(lz::windowSize, 0xFFFFFFFFu);

    unsigned char flag = 0;
    int flagBits = 0;
    unsigned char buf[24];  // up to 8 match tokens * 3 bytes
    int bufLen = 0;
    const auto flush = [&]() {
        if (flagBits == 0) return;
        output.push_back(flag);
        output.insert(output.end(), buf, buf + bufLen);
        flag = 0;
        flagBits = 0;
        bufLen = 0;
    };
    const auto emitLiteral = [&](unsigned char b) {
        flag |= static_cast<unsigned char>(1u << flagBits);
        buf[bufLen++] = b;
        if (++flagBits == 8) flush();
    };
    const auto emitMatch = [&](std::uint32_t offset, std::uint32_t length) {
        buf[bufLen++] = static_cast<unsigned char>(offset & 0xFF);
        buf[bufLen++] = static_cast<unsigned char>((offset >> 8) & 0xFF);
        buf[bufLen++] = static_cast<unsigned char>(length - lz::minMatch);
        if (++flagBits == 8) flush();
    };
    const auto insertHash = [&](std::size_t pos) {
        const std::uint32_t h = lz::hash4(data + pos);
        prev[pos & lz::windowMask] = head[h];
        head[h] = static_cast<std::uint32_t>(pos);
    };

    std::size_t i = 0;
    while (i < n) {
        std::size_t bestLen = 0;
        std::size_t bestOff = 0;
        if (i + lz::minMatch <= n) {
            const std::uint32_t h = lz::hash4(data + i);
            std::uint32_t cand = head[h];
            const std::size_t maxLen = std::min<std::size_t>(lz::maxMatch, n - i);
            std::uint32_t chain = 0;
            while (cand != 0xFFFFFFFFu
                   && i - cand < lz::windowSize
                   && chain < lz::maxChain) {
                std::size_t len = 0;
                while (len < maxLen && data[cand + len] == data[i + len]) {
                    ++len;
                }
                if (len > bestLen) {
                    bestLen = len;
                    bestOff = i - cand;
                    if (len >= lz::goodMatch) break;
                }
                cand = prev[cand & lz::windowMask];
                ++chain;
            }
        }
        if (bestLen >= lz::minMatch) {
            emitMatch(static_cast<std::uint32_t>(bestOff), static_cast<std::uint32_t>(bestLen));
            const std::size_t end = i + bestLen;
            for (std::size_t pos = i; pos < end && pos + lz::minMatch <= n; ++pos) {
                insertHash(pos);
            }
            i = end;
        } else {
            emitLiteral(data[i]);
            if (i + lz::minMatch <= n) {
                insertHash(i);
            }
            ++i;
        }
    }
    flush();
    return output;
}

std::vector<unsigned char> lzDecompress(const std::vector<unsigned char>& input,
                                         std::size_t expectedSize) {
    std::vector<unsigned char> output;
    output.reserve(expectedSize);
    std::size_t bi = 0;
    while (output.size() < expectedSize) {
        if (bi >= input.size()) {
            throw std::runtime_error("LZ stream truncated.");
        }
        const unsigned char flag = input[bi++];
        for (int bit = 0; bit < 8 && output.size() < expectedSize; ++bit) {
            if (flag & (1u << bit)) {
                if (bi + 1 > input.size()) {
                    throw std::runtime_error("LZ literal truncated.");
                }
                output.push_back(input[bi++]);
            } else {
                if (bi + 3 > input.size()) {
                    throw std::runtime_error("LZ match truncated.");
                }
                const std::uint32_t offset = static_cast<std::uint32_t>(input[bi])
                                           | (static_cast<std::uint32_t>(input[bi + 1]) << 8);
                const std::uint32_t length = static_cast<std::uint32_t>(input[bi + 2]) + lz::minMatch;
                bi += 3;
                if (offset == 0 || offset > output.size()) {
                    throw std::runtime_error("LZ match offset out of range.");
                }
                const std::size_t src = output.size() - offset;
                for (std::uint32_t k = 0; k < length; ++k) {
                    output.push_back(output[src + k]);  // overlapping copy is fine, byte-by-byte
                }
            }
        }
    }
    if (output.size() != expectedSize) {
        throw std::runtime_error("LZ decode size mismatch.");
    }
    return output;
}

// ---- Huffman stage --------------------------------------------------------
// A Huffman payload is laid out as:
//   [256 bytes code-lengths][8-byte symbol count, little-endian]
//   [packed bitstream, MSB-first, final byte zero-padded]
// code-lengths[i] is the bit length of symbol i's canonical Huffman code (0 means
// the symbol is absent from the input). The embedded symbol count makes the payload
// self-contained, so the LZ->Huffman pipeline can decode this stage without an
// external size (the outer LZ stage then uses the block's original size to verify).

std::array<unsigned char, 256> huffmanCodeLengths(const std::vector<unsigned char>& input) {
    std::array<std::uint64_t, 256> freq{};
    for (const auto byte : input) {
        ++freq[byte];
    }
    std::array<unsigned char, 256> lengths{};

    struct Node {
        std::uint64_t freq;
        int symbol;  // >=0 for a leaf, -1 for an internal node
        int left;
        int right;
    };
    std::vector<Node> nodes;
    auto greater = [&](int a, int b) {
        if (nodes[a].freq != nodes[b].freq) {
            return nodes[a].freq > nodes[b].freq;
        }
        return nodes[a].symbol > nodes[b].symbol;  // deterministic tie-break
    };
    std::priority_queue<int, std::vector<int>, decltype(greater)> queue(greater);
    for (int s = 0; s < 256; ++s) {
        if (freq[s] > 0) {
            nodes.push_back({freq[s], s, -1, -1});
            queue.push(static_cast<int>(nodes.size()) - 1);
        }
    }
    if (nodes.empty()) {
        return lengths;  // empty input: all-zero codebook
    }
    if (nodes.size() == 1) {
        // a single distinct symbol still needs a 1-bit code to be decodable
        lengths[nodes[0].symbol] = 1;
        return lengths;
    }
    while (queue.size() > 1) {
        const int a = queue.top();
        queue.pop();
        const int b = queue.top();
        queue.pop();
        nodes.push_back({nodes[a].freq + nodes[b].freq, -1, a, b});
        queue.push(static_cast<int>(nodes.size()) - 1);
    }
    const int root = queue.top();
    queue.pop();

    // iterative DFS assigns code lengths (= tree depth); avoids deep recursion
    std::vector<std::pair<int, int>> stack;
    stack.push_back({root, 0});
    while (!stack.empty()) {
        const auto [idx, depth] = stack.back();
        stack.pop_back();
        const Node& node = nodes[idx];
        if (node.symbol >= 0) {
            if (depth > 255) {
                throw std::runtime_error("Huffman code length exceeds 255 bits.");
            }
            lengths[node.symbol] = static_cast<unsigned char>(depth == 0 ? 1 : depth);
        } else {
            stack.push_back({node.right, depth + 1});
            stack.push_back({node.left, depth + 1});
        }
    }
    return lengths;
}

std::vector<unsigned char> huffmanCompress(const std::vector<unsigned char>& input) {
    if (input.empty()) {
        return {};  // decoder reconstructs an empty payload from expectedSize == 0
    }
    const auto lengths = huffmanCodeLengths(input);

    // canonical Huffman codes from the lengths (sorted by length, then symbol)
    std::vector<int> symbols;
    symbols.reserve(256);
    for (int s = 0; s < 256; ++s) {
        if (lengths[s] > 0) symbols.push_back(s);
    }
    std::sort(symbols.begin(), symbols.end(), [&](int a, int b) {
        if (lengths[a] != lengths[b]) return lengths[a] < lengths[b];
        return a < b;
    });
    std::array<std::uint64_t, 256> codes{};
    std::uint64_t code = 0;
    int previousLength = 0;
    for (const int s : symbols) {
        const int length = lengths[s];
        code <<= (length - previousLength);
        codes[s] = code;
        code += 1;
        previousLength = length;
    }

    // codebook (256 length bytes), then 8-byte symbol count, then the packed bitstream
    std::vector<unsigned char> output(256);
    for (int s = 0; s < 256; ++s) {
        output[s] = lengths[s];
    }
    const std::uint64_t symbolCount = input.size();
    for (int i = 0; i < 8; ++i) {
        output.push_back(static_cast<unsigned char>((symbolCount >> (8 * i)) & 0xFF));
    }
    unsigned char current = 0;
    int bits = 0;
    for (const auto byte : input) {
        const int length = lengths[byte];
        const std::uint64_t value = codes[byte];
        for (int i = length - 1; i >= 0; --i) {
            current = static_cast<unsigned char>((current << 1) | ((value >> i) & 1));
            if (++bits == 8) {
                output.push_back(current);
                current = 0;
                bits = 0;
            }
        }
    }
    if (bits > 0) {
        output.push_back(static_cast<unsigned char>(current << (8 - bits)));
    }
    return output;
}

std::vector<unsigned char> huffmanDecompress(const std::vector<unsigned char>& input) {
    if (input.empty()) {
        return {};  // empty payload -> empty original
    }
    if (input.size() < 256 + 8) {
        throw std::runtime_error("Broken Huffman payload.");
    }
    std::array<unsigned char, 256> lengths{};
    for (int i = 0; i < 256; ++i) {
        lengths[i] = input[i];
    }
    std::uint64_t expectedSize = 0;  // symbol count embedded in the payload
    for (int i = 0; i < 8; ++i) {
        expectedSize |= static_cast<std::uint64_t>(input[256 + i]) << (8 * i);
    }
    if (expectedSize == 0) {
        return {};
    }

    // rebuild the canonical codes (same assignment as the encoder) into a trie
    std::vector<int> symbols;
    symbols.reserve(256);
    for (int s = 0; s < 256; ++s) {
        if (lengths[s] > 0) symbols.push_back(s);
    }
    std::sort(symbols.begin(), symbols.end(), [&](int a, int b) {
        if (lengths[a] != lengths[b]) return lengths[a] < lengths[b];
        return a < b;
    });
    struct Trie {
        int left = -1;
        int right = -1;
        int symbol = -1;
    };
    std::vector<Trie> trie;
    trie.push_back({});
    std::uint64_t code = 0;
    int previousLength = 0;
    for (const int s : symbols) {
        const int length = lengths[s];
        code <<= (length - previousLength);
        int node = 0;
        for (int i = length - 1; i >= 0; --i) {
            const int bit = static_cast<int>((code >> i) & 1);
            if (bit == 0) {
                if (trie[node].left == -1) {
                    trie.push_back({});
                    trie[node].left = static_cast<int>(trie.size()) - 1;
                }
                node = trie[node].left;
            } else {
                if (trie[node].right == -1) {
                    trie.push_back({});
                    trie[node].right = static_cast<int>(trie.size()) - 1;
                }
                node = trie[node].right;
            }
        }
        trie[node].symbol = s;
        code += 1;
        previousLength = length;
    }
    if (trie[0].symbol >= 0) {
        throw std::runtime_error("Broken Huffman codebook.");
    }

    std::vector<unsigned char> output;
    output.reserve(static_cast<std::size_t>(expectedSize));
    const unsigned char* bitstream = input.data() + 256 + 8;
    const std::size_t totalBits = (input.size() - 256 - 8) * 8;
    std::size_t bitIndex = 0;
    std::uint64_t produced = 0;
    int node = 0;
    while (produced < expectedSize) {
        if (bitIndex >= totalBits) {
            throw std::runtime_error("Huffman bitstream truncated.");
        }
        const int bit = (bitstream[bitIndex / 8] >> (7 - (bitIndex % 8))) & 1;
        ++bitIndex;
        node = (bit == 0) ? trie[node].left : trie[node].right;
        if (node == -1) {
            throw std::runtime_error("Huffman decode path broken.");
        }
        if (trie[node].symbol >= 0) {
            output.push_back(static_cast<unsigned char>(trie[node].symbol));
            ++produced;
            node = 0;
        }
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
    const std::array<unsigned char, keySize>& key,
    const std::array<unsigned char, ivSize>& iv,
    std::array<unsigned char, tagSize>& tag) {
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
    const std::array<unsigned char, keySize>& key,
    const std::array<unsigned char, ivSize>& iv,
    const std::array<unsigned char, tagSize>& tag) {
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
        const bool encrypted = !entry.isDirectory && !options_.password.empty();
        if (!entry.isDirectory) flags |= flagCompressed;
        if (encrypted) {
            salt = randomBytes<saltSize>();
            flags |= flagEncrypted;
        }
        writeUint64(output, flags);
        const std::streamoff sizePos = output.tellp();
        writeUint64(output, 0);  // placeholder payloadSize, patched after streaming the blocks
        writeArray(output, salt);  // per-file salt (zeros when not encrypted); iv/tag are per block

        if (entry.isDirectory) {
            continue;  // payloadSize stays 0
        }

        // Stream the file block-by-block so memory stays bounded on multi-gigabyte
        // inputs (problem 4 fix): we never hold the whole file, its LZ expansion, or
        // an assembled compressed copy in memory -- only one block at a time.
        const auto srcPath = backupDir / pathFromUtf8(entry.relativePath);
        std::ifstream in(srcPath, std::ios::binary);
        if (!in) {
            throw std::runtime_error("Cannot read file: " + pathToUtf8(srcPath));
        }
        std::array<unsigned char, keySize> key{};
        if (encrypted) {
            key = deriveKey(options_.password, salt);  // PBKDF2 once per file, not per block
        }

        std::vector<unsigned char> block(blockSize);
        std::uint64_t payloadBytes = 0;
        while (true) {
            in.read(reinterpret_cast<char*>(block.data()),
                    static_cast<std::streamsize>(block.size()));
            const auto got = static_cast<std::size_t>(in.gcount());
            if (got == 0) break;
            auto lzStream = lzCompress(block.data(), got);
            auto huff = huffmanCompress(lzStream);
            // raw-store bypass: if compression doesn't beat the original (true for random /
            // already-compressed / tiny tail blocks), store the raw bytes instead. This keeps
            // the archive from ever expanding any block beyond its original size.
            const bool rawStored = huff.size() >= got;
            const unsigned char blockFlags = rawStored ? blockFlagRawStored : 0;
            // payload we actually store (pre-encryption): raw block bytes, or the Huffman stream
            const std::vector<unsigned char> toStore =
                rawStored ? std::vector<unsigned char>(block.begin(), block.begin() + got) : huff;
            output.put(static_cast<char>(blockFlags));
            writeUint64(output, static_cast<std::uint64_t>(got));       // block original size
            if (encrypted) {
                auto iv = randomBytes<ivSize>();
                std::array<unsigned char, tagSize> tag{};
                auto ct = encryptAesGcm(toStore, key, iv, tag);
                writeUint64(output, static_cast<std::uint64_t>(ct.size()));
                writeArray(output, iv);
                writeArray(output, tag);
                if (!ct.empty()) {
                    output.write(reinterpret_cast<const char*>(ct.data()),
                                 static_cast<std::streamsize>(ct.size()));
                }
                payloadBytes += 1 + 8 + 8 + ivSize + tagSize + ct.size();
            } else {
                writeUint64(output, static_cast<std::uint64_t>(toStore.size()));
                if (!toStore.empty()) {
                    output.write(reinterpret_cast<const char*>(toStore.data()),
                                 static_cast<std::streamsize>(toStore.size()));
                }
                payloadBytes += 1 + 8 + 8 + toStore.size();
            }
            if (got < block.size()) break;  // last (partial) block
        }
        in.close();

        // patch the placeholder payloadSize with the real total
        const std::streamoff endPos = output.tellp();
        output.seekp(sizePos);
        writeUint64(output, payloadBytes);
        output.seekp(endPos);
        if (!output) {
            throw std::runtime_error("Failed to finalize archive entry: " + pathToUtf8(srcPath));
        }

        ++stats.files;
        stats.bytes += entry.size;
        stats.storedBytes += payloadBytes;
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
        const auto salt = readArray<saltSize>(input);  // per-file; iv/tag live inside each block
        manifest.add(entry);

        const auto outputPath = safeOutputPath(outputDir, relativePath);
        if (entry.isDirectory) {
            ensureDirectory(outputPath);
            continue;
        }

        ensureDirectory(outputPath.parent_path());
        const bool encrypted = (flags & flagEncrypted) != 0;
        const bool compressed = (flags & flagCompressed) != 0;
        if (encrypted && options_.password.empty()) {
            throw std::runtime_error("Archive is encrypted. Provide --password=<value>.");
        }
        std::array<unsigned char, keySize> key{};
        if (encrypted) {
            key = deriveKey(options_.password, salt);  // once per file
        }

        std::ofstream out(outputPath, std::ios::binary);
        if (!out) {
            throw std::runtime_error("Cannot write file: " + pathToUtf8(outputPath));
        }

        // decode block-by-block (reverse pipeline: decrypt -> Huffman -> LZ) so the
        // working set stays bounded by one block, not the whole file.
        std::uint64_t consumed = 0;
        while (consumed < payloadSize) {
            const int blockFlagsByte = input.get();
            if (blockFlagsByte == EOF) {
                throw std::runtime_error("Broken archive: missing block flags.");
            }
            consumed += 1;
            const auto rawStored = (static_cast<unsigned char>(blockFlagsByte) & blockFlagRawStored) != 0;
            const auto blockOriginalSize = readUint64(input);
            consumed += 8;
            const auto compSize = readUint64(input);
            consumed += 8;
            std::array<unsigned char, ivSize> iv{};
            std::array<unsigned char, tagSize> tag{};
            if (encrypted) {
                iv = readArray<ivSize>(input);
                tag = readArray<tagSize>(input);
                consumed += ivSize + tagSize;
            }
            auto blockPayload = readPayload(input, compSize);
            consumed += compSize;
            if (encrypted) {
                blockPayload = decryptAesGcm(blockPayload, key, iv, tag);
            }
            if (rawStored) {
                // payload is already the original bytes; write through (verify size matches)
                if (blockPayload.size() != blockOriginalSize) {
                    throw std::runtime_error("Raw-stored block size mismatch.");
                }
                if (!blockPayload.empty()) {
                    out.write(reinterpret_cast<const char*>(blockPayload.data()),
                              static_cast<std::streamsize>(blockPayload.size()));
                }
            } else if (compressed && !blockPayload.empty()) {
                auto lzStream = huffmanDecompress(blockPayload);
                auto block = lzDecompress(lzStream, static_cast<std::size_t>(blockOriginalSize));
                if (!block.empty()) {
                    out.write(reinterpret_cast<const char*>(block.data()),
                              static_cast<std::streamsize>(block.size()));
                }
            }
            // (compressed + empty payload -> empty block; we wrote nothing, as expected)
        }
        out.close();
        if (!out) {
            throw std::runtime_error("Failed to write file: " + pathToUtf8(outputPath));
        }

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
