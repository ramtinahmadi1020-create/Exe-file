#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace vpnecho {

// Clean, bounds-checked RFC 1950 (zlib) + RFC 1951 (DEFLATE) decompressor
class ZlibInflater {
    const uint8_t* src_;
    size_t len_;
    size_t bytePos_ = 0;
    uint32_t bitBuf_ = 0;
    int bitCnt_ = 0;

    void EnsureBits(int n) {
        while (bitCnt_ < n) {
            if (bytePos_ >= len_) throw std::runtime_error("Truncated deflate stream.");
            bitBuf_ |= static_cast<uint32_t>(src_[bytePos_++]) << bitCnt_;
            bitCnt_ += 8;
        }
    }

    uint32_t PeekBits(int n) {
        EnsureBits(n);
        return bitBuf_ & ((1u << n) - 1u);
    }

    void DropBits(int n) {
        bitBuf_ >>= n;
        bitCnt_ -= n;
    }

    uint32_t ReadBits(int n) {
        if (n == 0) return 0;
        uint32_t v = PeekBits(n);
        DropBits(n);
        return v;
    }

    void AlignByte() {
        int skip = bitCnt_ & 7;
        DropBits(skip);
    }

    struct HuffTable {
        static constexpr int FAST_BITS = 9;
        uint32_t fast[1 << FAST_BITS]{}; // (len << 16) | sym, or 0
        int counts[16]{};
        int symbols[320]{};
        int maxLen = 0;

        void Build(const uint8_t* lengths, int numSyms) {
            std::memset(fast, 0, sizeof(fast));
            std::memset(counts, 0, sizeof(counts));
            maxLen = 0;
            for (int i = 0; i < numSyms; ++i) {
                int l = lengths[i];
                if (l > 15) throw std::runtime_error("Invalid Huffman code length.");
                if (l > 0) {
                    counts[l]++;
                    if (l > maxLen) maxLen = l;
                }
            }
            int nextCode[16]{};
            int code = 0;
            counts[0] = 0;
            for (int bits = 1; bits <= 15; ++bits) {
                code = (code + counts[bits - 1]) << 1;
                nextCode[bits] = code;
            }
            int offsets[16]{};
            int sum = 0;
            for (int bits = 1; bits <= 15; ++bits) {
                offsets[bits] = sum;
                sum += counts[bits];
            }
            for (int sym = 0; sym < numSyms; ++sym) {
                int l = lengths[sym];
                if (l == 0) continue;
                symbols[offsets[l]++] = sym;
                int c = nextCode[l]++;
                if (l <= FAST_BITS) {
                    // Reverse l bits of c
                    int rev = 0;
                    for (int b = 0; b < l; ++b) {
                        rev = (rev << 1) | ((c >> b) & 1);
                    }
                    uint32_t entry = (static_cast<uint32_t>(l) << 16) | static_cast<uint32_t>(sym);
                    for (int fill = rev; fill < (1 << FAST_BITS); fill += (1 << l)) {
                        fast[fill] = entry;
                    }
                }
            }
        }
    };

    int DecodeSym(const HuffTable& ht) {
        if (bitCnt_ < HuffTable::FAST_BITS && bytePos_ < len_) {
            while (bitCnt_ <= 24 && bytePos_ < len_) {
                bitBuf_ |= static_cast<uint32_t>(src_[bytePos_++]) << bitCnt_;
                bitCnt_ += 8;
            }
        }
        if (bitCnt_ >= HuffTable::FAST_BITS) {
            uint32_t f = ht.fast[bitBuf_ & ((1 << HuffTable::FAST_BITS) - 1)];
            if (f != 0) {
                int l = static_cast<int>(f >> 16);
                DropBits(l);
                return static_cast<int>(f & 0xFFFFu);
            }
        }
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= static_cast<int>(ReadBits(1));
            int count = ht.counts[len];
            if (code - count < first) {
                return ht.symbols[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("Invalid Huffman symbol in deflate stream.");
    }

public:
    ZlibInflater(const uint8_t* src, size_t len) : src_(src), len_(len) {}

    void DecompressZlib(std::vector<uint8_t>& out, size_t expectedSizeHint = 0) {
        if (len_ < 6) throw std::runtime_error("Zlib buffer too small.");
        uint8_t cmf = src_[0];
        uint8_t flg = src_[1];
        if ((cmf & 0x0F) != 8 || (((static_cast<uint32_t>(cmf) << 8) + flg) % 31) != 0 || (flg & 0x20) != 0) {
            throw std::runtime_error("Invalid zlib header.");
        }
        bytePos_ = 2;
        out.clear();
        if (expectedSizeHint > 0) out.reserve(expectedSizeHint);

        static const int lenBase[29] = {
            3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
            35,43,51,59,67,83,99,115,131,163,195,227,258
        };
        static const int lenExtra[29] = {
            0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
            3,3,3,3,4,4,4,4,5,5,5,5,0
        };
        static const int distBase[30] = {
            1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
            257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
        };
        static const int distExtra[30] = {
            0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
            7,7,8,8,9,9,10,10,11,11,12,12,13,13
        };
        static const int clOrder[19] = {
            16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
        };

        HuffTable litLenTable{}, distTable{}, clTable{};
        bool finalBlock = false;
        while (!finalBlock) {
            finalBlock = (ReadBits(1) != 0);
            uint32_t btype = ReadBits(2);
            if (btype == 0) {
                AlignByte();
                uint32_t bLen = ReadBits(16);
                uint32_t bNlen = ReadBits(16);
                if ((bLen ^ 0xFFFFu) != bNlen) throw std::runtime_error("Corrupt uncompressed block length.");
                if (bytePos_ + bLen > len_) throw std::runtime_error("Truncated uncompressed block.");
                out.insert(out.end(), src_ + bytePos_, src_ + bytePos_ + bLen);
                bytePos_ += bLen;
            } else if (btype == 1 || btype == 2) {
                if (btype == 1) {
                    uint8_t llLens[288]{};
                    for (int i = 0; i <= 143; ++i) llLens[i] = 8;
                    for (int i = 144; i <= 255; ++i) llLens[i] = 9;
                    for (int i = 256; i <= 279; ++i) llLens[i] = 7;
                    for (int i = 280; i <= 287; ++i) llLens[i] = 8;
                    uint8_t dLens[32]{};
                    for (int i = 0; i < 32; ++i) dLens[i] = 5;
                    litLenTable.Build(llLens, 288);
                    distTable.Build(dLens, 32);
                } else {
                    int hlit = static_cast<int>(ReadBits(5)) + 257;
                    int hdist = static_cast<int>(ReadBits(5)) + 1;
                    int hclen = static_cast<int>(ReadBits(4)) + 4;
                    uint8_t clLens[19]{};
                    for (int i = 0; i < hclen; ++i) {
                        clLens[clOrder[i]] = static_cast<uint8_t>(ReadBits(3));
                    }
                    clTable.Build(clLens, 19);
                    uint8_t allLens[320]{};
                    int idx = 0;
                    int totalCodes = hlit + hdist;
                    while (idx < totalCodes) {
                        int sym = DecodeSym(clTable);
                        if (sym < 16) {
                            allLens[idx++] = static_cast<uint8_t>(sym);
                        } else if (sym == 16) {
                            if (idx == 0) throw std::runtime_error("Invalid repeat code 16.");
                            int rep = static_cast<int>(ReadBits(2)) + 3;
                            uint8_t prev = allLens[idx - 1];
                            while (rep-- > 0 && idx < totalCodes) allLens[idx++] = prev;
                        } else if (sym == 17) {
                            int rep = static_cast<int>(ReadBits(3)) + 3;
                            while (rep-- > 0 && idx < totalCodes) allLens[idx++] = 0;
                        } else if (sym == 18) {
                            int rep = static_cast<int>(ReadBits(7)) + 11;
                            while (rep-- > 0 && idx < totalCodes) allLens[idx++] = 0;
                        } else {
                            throw std::runtime_error("Bad code-length symbol.");
                        }
                    }
                    litLenTable.Build(allLens, hlit);
                    distTable.Build(allLens + hlit, hdist);
                }

                while (true) {
                    int sym = DecodeSym(litLenTable);
                    if (sym < 256) {
                        out.push_back(static_cast<uint8_t>(sym));
                    } else if (sym == 256) {
                        break;
                    } else {
                        int lIdx = sym - 257;
                        if (lIdx < 0 || lIdx >= 29) throw std::runtime_error("Invalid match length symbol.");
                        int matchLen = lenBase[lIdx] + static_cast<int>(ReadBits(lenExtra[lIdx]));
                        int dSym = DecodeSym(distTable);
                        if (dSym < 0 || dSym >= 30) throw std::runtime_error("Invalid distance symbol.");
                        size_t dist = static_cast<size_t>(distBase[dSym] + static_cast<int>(ReadBits(distExtra[dSym])));
                        if (dist == 0 || dist > out.size()) throw std::runtime_error("Invalid backreference distance.");
                        size_t curSize = out.size();
                        out.resize(curSize + static_cast<size_t>(matchLen));
                        uint8_t* dst = out.data() + curSize;
                        const uint8_t* ref = dst - dist;
                        if (dist >= static_cast<size_t>(matchLen)) {
                            std::memcpy(dst, ref, static_cast<size_t>(matchLen));
                        } else {
                            for (int k = 0; k < matchLen; ++k) dst[k] = ref[k];
                        }
                    }
                }
            } else {
                throw std::runtime_error("Invalid deflate block type 3.");
            }
        }

        // Verify Adler-32 checksum at end of zlib stream (read via bit reader since lookahead may have buffered trailer bytes)
        AlignByte();
        uint32_t a0 = ReadBits(8);
        uint32_t a1 = ReadBits(8);
        uint32_t a2 = ReadBits(8);
        uint32_t a3 = ReadBits(8);
        uint32_t expectedAdler = (a0 << 24) | (a1 << 16) | (a2 << 8) | a3;
        uint32_t s1 = 1, s2 = 0;
        const uint8_t* p = out.data();
        size_t rem = out.size();
        while (rem > 0) {
            size_t tlen = rem > 5552 ? 5552 : rem;
            rem -= tlen;
            do {
                s1 += *p++;
                s2 += s1;
            } while (--tlen);
            s1 %= 65521;
            s2 %= 65521;
        }
        uint32_t actualAdler = (s2 << 16) | s1;
        if (actualAdler != expectedAdler) {
            throw std::runtime_error("Adler-32 checksum mismatch in zlib payload.");
        }
    }
};

} // namespace vpnecho
