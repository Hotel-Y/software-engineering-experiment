#!/usr/bin/env python3
"""Inspect a Simple Backup Archive (.sba).

Format (little-endian), from src/ArchiveManager.cpp:
  magic: 8 bytes  "SBA5\r\n\0\x01"
  uint64 entryCount
  per entry:
    1 byte type      ('F' file / 'D' dir)
    uint64 pathLen    -> pathLen bytes path (utf-8)
    uint64 originalSize
    uint64 modifiedTime
    uint64 checksum
    uint64 flags      bit0 = compressed (LZ+Huffman, block-wise), bit1 = encrypted
    uint64 payloadSize
    16 bytes salt    (per-file; zeros when not encrypted)
    payloadSize bytes payload, laid out as a sequence of blocks:
      repeat until payloadSize bytes consumed:
        uint64 blockOriginalSize
        uint64 compSize
        if encrypted: 12 bytes iv, 16 bytes tag
        compSize bytes  (the Huffman payload of this block's LZ stream, encrypted if flagged)

A compressed (non-encrypted) block payload is itself:
    [256 code-lengths][8-byte symbol count LE][Huffman bitstream, MSB-first]
which this tool decodes back to the LZ stage (and then to the block's original
bytes using blockOriginalSize) to prove the pipeline round-trips.
"""
import struct, sys, pathlib

MAGIC = b"SBA5\r\n\0\x01"
FLAG_COMPRESSED, FLAG_ENCRYPTED = 1, 2

def read_u64(f):
    d = f.read(8)
    if len(d) != 8:
        raise ValueError("truncated uint64")
    return struct.unpack("<Q", d)[0]

def read_n(f, n):
    d = f.read(n)
    if len(d) != n:
        raise ValueError(f"truncated block, wanted {n} got {len(d)}")
    return d

def canonical_codes(lengths):
    """lengths: {symbol: bit_length}. Returns {(length, code_int): symbol}."""
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
    """Decode a compressed (non-encrypted) block payload to the LZ-stage bytes."""
    if len(payload) < 256 + 8:
        raise ValueError("payload too short for Huffman header")
    lengths = {i: payload[i] for i in range(256) if payload[i] > 0}
    count = int.from_bytes(payload[256:264], "little")
    if count == 0:
        return b""
    bs = payload[264:]
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
    """Decode an LZ token stream (flag-grouped literals/matches) to original bytes."""
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
                src = len(out) - offset
                for _ in range(length):
                    out.append(out[src]); src += 1
    if len(out) != expected:
        raise ValueError(f"LZ decode size {len(out)} != expected {expected}")
    return bytes(out)

def main():
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
            salt = read_n(f, 16)  # per-file salt (iv/tag live inside each block now)
            payload = read_n(f, psize) if psize else b""
            is_cmp = bool(flags & FLAG_COMPRESSED)
            is_enc = bool(flags & FLAG_ENCRYPTED)
            if t == "F":
                total_orig += orig
                total_stored += psize
                if is_cmp:
                    compressed_entries += 1
            print(f"{t:4} {'Y' if is_cmp else '-':3} {'Y' if is_enc else '-':3} {orig:8} {psize:8}  {rpath}")
            # walk the block container; decode the full pipeline only for plaintext blocks
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
                            # payload is already the original block bytes
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
