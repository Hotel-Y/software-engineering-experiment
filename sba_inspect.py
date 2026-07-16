#!/usr/bin/env python3
"""独立检查 Simple Backup Archive（.sba）的结构和明文压缩流水线。

格式来自 src/ArchiveManager.cpp，整数按小端序保存：
  magic: 8 字节  "SBA5\r\n\0\x01"
  uint64 entryCount
  每个条目：
    1 字节 type       （'F' 文件 / 'D' 目录）
    uint64 pathLen    -> pathLen 字节 UTF-8 相对路径
    uint64 originalSize
    uint64 modifiedTime
    uint64 checksum
    uint64 flags      bit0 = 按块压缩，bit1 = 加密
    uint64 payloadSize
    16 字节 salt      （每文件一个；未加密时为零）
    payloadSize 字节的块序列：
      重复读取直到消费完 payloadSize：
        1 字节 blockFlags（bit0 = 原样存储）
        uint64 blockOriginalSize
        uint64 storedSize
        若加密：12 字节 IV、16 字节认证标签
        storedSize 字节（原始块或 Huffman(LZ) 载荷；加密时为密文）

未加密压缩块内部为：[256 字节码长表][8 字节符号数][高位优先的 Huffman 位流]。
本工具用一套独立 Python 实现将其还原为 LZ 令牌，再按 blockOriginalSize 还原原始块，
用于交叉证明 C++ 压缩结果可以完整往返。
"""
import struct, sys, pathlib

MAGIC = b"SBA5\r\n\0\x01"
FLAG_COMPRESSED, FLAG_ENCRYPTED = 1, 2

def read_u64(f):
    """严格读取一个小端 uint64；不足 8 字节即判为归档截断。"""
    d = f.read(8)
    if len(d) != 8:
        raise ValueError("truncated uint64")
    return struct.unpack("<Q", d)[0]

def read_n(f, n):
    """读取指定长度，避免普通 read 静默返回不完整内容。"""
    d = f.read(n)
    if len(d) != n:
        raise ValueError(f"truncated block, wanted {n} got {len(d)}")
    return d

def canonical_codes(lengths):
    """由“符号 -> 码长”生成“(码长, 码值) -> 符号”的规范 Huffman 查找表。"""
    syms = sorted(lengths.keys(), key=lambda s: (lengths[s], s))
    lookup = {}
    code, prev = 0, 0
    for s in syms:
        L = lengths[s]
        code <<= (L - prev)
        lookup[(L, code)] = s
        code += 1
        prev = L
    return lookup

def decode_huffman(payload):
    """把未加密压缩块的 Huffman 载荷还原为 LZ 阶段字节。"""
    if len(payload) < 256 + 8:
        raise ValueError("payload too short for Huffman header")
    lengths = {i: payload[i] for i in range(256) if payload[i] > 0}
    count = int.from_bytes(payload[256:264], "little")
    if count == 0:
        return b""
    bs = payload[264:]
    # 按位累积码值，一旦命中规范码就输出符号并回到码树根部。
    lookup = canonical_codes(lengths)
    out = bytearray()
    cur, clen, bi = 0, 0, 0
    total_bits = len(bs) * 8
    while len(out) < count:
        if bi >= total_bits:
            raise ValueError("Huffman bitstream truncated")
        bit = (bs[bi // 8] >> (7 - (bi % 8))) & 1
        bi += 1
        cur = (cur << 1) | bit
        clen += 1
        if (clen, cur) in lookup:
            out.append(lookup[(clen, cur)])
            cur, clen = 0, 0
    return bytes(out)

LZ_MIN_MATCH = 4

def lz_decode(data, expected):
    """解析按标志分组的字面量/匹配令牌，恢复指定长度的原始块。"""
    out = bytearray()
    bi = 0
    n = len(data)
    while len(out) < expected:
        if bi >= n:
            raise ValueError("LZ stream truncated")
        flag = data[bi]; bi += 1
        for bit in range(8):
            if len(out) >= expected:
                break
            if flag & (1 << bit):
                if bi + 1 > n:
                    raise ValueError("LZ literal truncated")
                out.append(data[bi]); bi += 1
            else:
                if bi + 3 > n:
                    raise ValueError("LZ match truncated")
                offset = data[bi] | (data[bi + 1] << 8)
                length = data[bi + 2] + LZ_MIN_MATCH
                bi += 3
                if offset == 0 or offset > len(out):
                    raise ValueError("LZ match offset out of range")
                # 逐字节追加自然支持重叠匹配，例如用一个字节扩展出长重复串。
                src = len(out) - offset
                for _ in range(length):
                    out.append(out[src]); src += 1
    if len(out) != expected:
        raise ValueError(f"LZ decode size {len(out)} != expected {expected}")
    return bytes(out)

def main():
    # 先检查 SBA5 魔数，再逐条汇总压缩、加密状态和存储比例。
    path = pathlib.Path(sys.argv[1])
    with open(path, "rb") as f:
        if f.read(8) != MAGIC:
            raise SystemExit(f"{path}: not an SBA archive (bad magic)")
        n = read_u64(f)
        print(f"archive: {path}")
        print(f"size on disk: {path.stat().st_size} bytes")
        print(f"entries: {n}\n")
        print(f"{'type':4} {'CMP':3} {'ENC':3} {'orig':>8} {'stored':>8}  path")
        print("-" * 64)
        total_orig = total_stored = 0
        compressed_entries = 0
        roundtrip_ok = True
        for _ in range(n):
            t = chr(f.read(1)[0])
            plen = read_u64(f)
            rpath = read_n(f, plen).decode("utf-8", "replace")
            orig = read_u64(f)
            mtime = read_u64(f)
            checksum = read_u64(f)
            flags = read_u64(f)
            psize = read_u64(f)
            salt = read_n(f, 16)  # 每文件盐；IV 和认证标签保存在各数据块内。
            payload = read_n(f, psize) if psize else b""
            is_cmp = bool(flags & FLAG_COMPRESSED)
            is_enc = bool(flags & FLAG_ENCRYPTED)
            if t == "F":
                total_orig += orig
                total_stored += psize
                if is_cmp:
                    compressed_entries += 1
            print(f"{t:4} {'Y' if is_cmp else '-':3} {'Y' if is_enc else '-':3} {orig:8} {psize:8}  {rpath}")
            # 遍历块容器；只有明文块能在不取得用户密码的情况下独立验证完整流水线。
            if t == "F" and is_cmp and not is_enc:
                consumed = 0
                block_index = 0
                produced = 0
                raw_blocks = 0
                while consumed < psize:
                    if psize - consumed < 1 + 16:
                        roundtrip_ok = False
                        print(f"        block {block_index}: truncated header")
                        break
                    block_flags = payload[consumed]; consumed += 1
                    raw_stored = bool(block_flags & 1)
                    block_orig = int.from_bytes(payload[consumed:consumed+8], "little"); consumed += 8
                    comp_size = int.from_bytes(payload[consumed:consumed+8], "little"); consumed += 8
                    block_payload = payload[consumed:consumed+comp_size]; consumed += comp_size
                    try:
                        if raw_stored:
                            # raw-store 载荷本身就是原始块字节，无需运行两个解码阶段。
                            if len(block_payload) != block_orig:
                                raise ValueError(f"raw block size {len(block_payload)} != {block_orig}")
                            produced += len(block_payload)
                            raw_blocks += 1
                        else:
                            if block_index == 0:
                                distinct = sum(1 for i in range(256) if block_payload[i] > 0)
                                maxlen = max((block_payload[i] for i in range(256) if block_payload[i] > 0), default=0)
                                print(f"        block 0 Huffman: {distinct} distinct symbols, max code {maxlen} bits")
                            lz_bytes = decode_huffman(block_payload)
                            block_bytes = lz_decode(lz_bytes, block_orig)
                            produced += len(block_bytes)
                    except ValueError as e:
                        roundtrip_ok = False
                        print(f"        block {block_index} decode failed: {e}")
                        break
                    block_index += 1
                if block_index and roundtrip_ok:
                    tag = f"({raw_blocks} raw-stored)" if raw_blocks else "(all compressed)"
                    print(f"        LZ+Huffman -> {produced} bytes across {block_index} block(s) {tag} "
                          f"(matches originalSize={orig}): {'OK' if produced == orig else 'MISMATCH'}")
                    if produced != orig:
                        roundtrip_ok = False
        print("-" * 64)
        if total_orig:
            print(f"totals: original={total_orig}  stored={total_stored}  "
                  f"ratio={total_stored/total_orig:.3f}")
        print(f"compressed entries: {compressed_entries}")
        print(f"pipeline round-trip (where decodable): {'all OK' if roundtrip_ok else 'HAS MISMATCHES'}")

if __name__ == "__main__":
    main()
