#include "lha.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace lha {

// ===================== CRC16 (LHA: poly 0xA001, init 0) =====================

static uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001)
                            : static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

static uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static void le16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xff));
    s.push_back(static_cast<char>((v >> 8) & 0xff));
}

static void le32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

// ===================== LZH -lh5- =====================
//
// Ported from LHA 1.14i (huf.c, maketree.c, slide.c) for encoding and
// lhasa 0.4.0 (lh_new_decoder.c) for decoding.

namespace lh5 {

const int MAXMATCH = 256;
const int THRESHOLD = 3;
const int NC = 510;
const int CBIT = 9;
const int NT = 19;
const int TBIT = 5;
const int NP = 14;
const int PBIT = 4;
const int DICBIT = 13;
const int DICSIZ = 1 << DICBIT;
const int RING_SIZE = 1 << 14;  // lh5 decoder history
const int BLOCK_SIZE = 4096;

const uint16_t TREE_LEAF = 0x8000;

struct Cmd {
    uint16_t code;
    uint16_t offset;
};

// ---------- bit writer (MSB-first) ----------

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : m_out(out) {}

    void put(int b) {
        m_cur = static_cast<uint8_t>((m_cur << 1) | (b & 1));
        if (++m_nbits == 8) {
            m_out.push_back(m_cur);
            m_cur = 0;
            m_nbits = 0;
        }
    }

    void putbits(int n, uint32_t v) {
        for (int i = n - 1; i >= 0; i--) put((v >> i) & 1);
    }

    void putcode(int n, uint16_t x) {
        for (int i = 0; i < n; i++) put((x >> (15 - i)) & 1);
    }

    void flush() {
        if (m_nbits) {
            m_out.push_back(static_cast<uint8_t>(m_cur << (8 - m_nbits)));
            m_cur = 0;
            m_nbits = 0;
        }
    }

private:
    std::vector<uint8_t>& m_out;
    uint8_t m_cur = 0;
    int m_nbits = 0;
};

// ---------- Huffman tree construction (LHA maketree.c) ----------

static void count_len(int node, int n, int depth, const std::vector<int>& left,
                      const std::vector<int>& right, std::vector<unsigned short>& len_cnt) {
    if (node < n) {
        len_cnt[depth < 16 ? depth : 16]++;
    } else {
        depth++;
        count_len(left[node], n, depth, left, right, len_cnt);
        count_len(right[node], n, depth, left, right, len_cnt);
    }
}

// Build Huffman tree from frequencies. Returns root. Fills len[] (0..16)
// and code[] (left-aligned 16-bit canonical codes). freq must have size >= 2*n.
static int make_tree(int n, std::vector<unsigned>& freq, std::vector<unsigned char>& len,
                     std::vector<unsigned short>& code) {
    std::vector<int> left(2 * n), right(2 * n);
    std::vector<int> heap(n + 1);
    int heapsize = 0;
    for (int i = 0; i < n; i++) {
        len[i] = 0;
        if (freq[i]) heap[++heapsize] = i;
    }
    if (heapsize < 2) {
        code[heap[1]] = 0;
        return heap[1];
    }
    auto downheap = [&](int ii) {
        int kk = heap[ii], j;
        while ((j = 2 * ii) <= heapsize) {
            if (j < heapsize && freq[heap[j]] > freq[heap[j + 1]]) j++;
            if (freq[kk] <= freq[heap[j]]) break;
            heap[ii] = heap[j];
            ii = j;
        }
        heap[ii] = kk;
    };
    for (int i = heapsize / 2; i >= 1; i--) downheap(i);

    std::vector<int> sort;
    sort.reserve(n);
    int avail = n, i, j, k = 0;
    while (heapsize > 1) {
        i = heap[1];
        if (i < n) sort.push_back(i);
        heap[1] = heap[heapsize--];
        downheap(1);
        j = heap[1];
        if (j < n) sort.push_back(j);
        k = avail++;
        freq[k] = freq[i] + freq[j];
        heap[1] = k;
        downheap(1);
        left[k] = i;
        right[k] = j;
    }

    std::vector<unsigned short> len_cnt(17, 0);
    count_len(k, n, 0, left, right, len_cnt);

    unsigned cum = 0;
    for (i = 16; i > 0; i--) cum += static_cast<unsigned>(len_cnt[i]) << (16 - i);
    cum &= 0xffff;
    if (cum) {
        len_cnt[16] -= static_cast<unsigned short>(cum);
        do {
            for (i = 15; i > 0; i--) {
                if (len_cnt[i]) {
                    len_cnt[i]--;
                    len_cnt[i + 1] += 2;
                    break;
                }
            }
        } while (--cum);
    }
    size_t s = 0;
    for (i = 16; i > 0; i--) {
        unsigned short kk = len_cnt[i];
        while (kk > 0) {
            len[sort[s++]] = static_cast<unsigned char>(i);
            kk--;
        }
    }

    unsigned short weight[17], start[17];
    unsigned short sj = 0;
    unsigned short sk = 1 << 15;
    for (i = 1; i <= 16; i++) {
        start[i] = sj;
        sj += static_cast<unsigned short>((weight[i] = sk) * len_cnt[i]);
        sk >>= 1;
    }
    for (i = 0; i < n; i++) {
        unsigned char l = len[i];
        code[i] = start[l];
        start[l] = static_cast<unsigned short>(start[l] + weight[l]);
    }
    return k;
}

// ---------- encoder ----------

class Encoder {
public:
    Encoder(BitWriter& bw) : m_bw(bw) {
        m_c_len.resize(NC, 0);
        m_c_code.resize(NC, 0);
        m_pt_len.resize(NPT_MAX, 0);
        m_pt_code.resize(NPT_MAX, 0);
        m_c_freq.resize(2 * NC, 0);
        m_t_freq.resize(2 * NT, 0);
        m_p_freq.resize(2 * NP, 0);
    }

    void encode(const std::vector<uint8_t>& in, ProgressFn progress = nullptr,
                void* userdata = nullptr) {
        if (in.empty()) return;
        size_t n = in.size();
        std::vector<uint8_t> text(DICSIZ + n + MAXMATCH, ' ');
        std::memcpy(text.data() + DICSIZ, in.data(), n);

        std::vector<int> head(1 << 15, -1);
        std::vector<int> prev(DICSIZ + n, -1);

        auto hash3 = [&](size_t i) {
            return ((static_cast<unsigned>(text[i]) << 5 ^ text[i + 1]) << 5 ^
                    text[i + 2]) &
                   ((1 << 15) - 1);
        };

        std::vector<Cmd> block;
        auto flush_block = [&]() {
            if (!block.empty()) send_block(block);
            block.clear();
        };

        size_t pos = DICSIZ;
        size_t remaining = n;
        int last_pct = -1;
        while (remaining > 0) {
            if (progress) {
                uint64_t done = n - remaining;
                int pct = static_cast<int>(done * 100 / n);
                if (pct != last_pct) {
                    progress(done, n, userdata);
                    last_pct = pct;
                }
            }
            int best_len = 0, best_d = 0;
            int h = hash3(pos);
            int old = head[h];
            head[h] = static_cast<int>(pos);
            prev[pos] = old;
            int p = old;
            int limit = static_cast<int>(pos) - DICSIZ;
            int walked = 0;
            while (p >= limit && walked < 128) {
                int d = static_cast<int>(pos - p);
                int len = 0;
                while (len < MAXMATCH && pos + len < DICSIZ + n &&
                       text[pos + len] == text[p + len]) {
                    len++;
                }
                if (len > best_len) {
                    best_len = len;
                    best_d = d;
                }
                p = prev[p];
                walked++;
            }
            if (best_len >= THRESHOLD) {
                if (best_len > MAXMATCH) best_len = MAXMATCH;
                block.push_back(Cmd{static_cast<uint16_t>(255 + 1 - THRESHOLD + best_len),
                                    static_cast<uint16_t>(best_d - 1)});
                pos += best_len;
                remaining -= best_len;
            } else {
                block.push_back(Cmd{text[pos], 0});
                pos++;
                remaining--;
            }
            if (block.size() >= BLOCK_SIZE) flush_block();
        }
        flush_block();
    }

private:
    static const int NPT_MAX = 128;

    BitWriter& m_bw;
    std::vector<unsigned char> m_c_len, m_pt_len;
    std::vector<unsigned short> m_c_code, m_pt_code;
    std::vector<unsigned> m_c_freq, m_t_freq, m_p_freq;

    void putcode(int n, uint16_t x) { m_bw.putcode(n, x); }

    void count_t_freq() {
        for (auto& v : m_t_freq) v = 0;
        int n = NC;
        while (n > 0 && m_c_len[n - 1] == 0) n--;
        int i = 0;
        while (i < n) {
            int k = m_c_len[i++];
            if (k == 0) {
                int count = 1;
                while (i < n && m_c_len[i] == 0) {
                    i++;
                    count++;
                }
                if (count <= 2) m_t_freq[0] += count;
                else if (count <= 18) m_t_freq[1]++;
                else if (count == 19) {
                    m_t_freq[0]++;
                    m_t_freq[1]++;
                } else m_t_freq[2]++;
            } else {
                m_t_freq[k + 2]++;
            }
        }
    }

    void write_pt_len(int n, int nbit, int i_special) {
        while (n > 0 && m_pt_len[n - 1] == 0) n--;
        m_bw.putbits(nbit, n);
        int i = 0;
        while (i < n) {
            int k = m_pt_len[i++];
            if (k <= 6) m_bw.putbits(3, k);
            else m_bw.putbits(k - 3, 0xFFFE);
            if (i == i_special) {
                while (i < 6 && m_pt_len[i] == 0) i++;
                m_bw.putbits(2, i - 3);
            }
        }
    }

    void encode_temp(int c) { putcode(m_pt_len[c], m_pt_code[c]); }

    void write_c_len() {
        int n = NC;
        while (n > 0 && m_c_len[n - 1] == 0) n--;
        m_bw.putbits(CBIT, n);
        int i = 0;
        while (i < n) {
            int k = m_c_len[i++];
            if (k == 0) {
                int count = 1;
                while (i < n && m_c_len[i] == 0) {
                    i++;
                    count++;
                }
                if (count <= 2) {
                    for (int kk = 0; kk < count; kk++) encode_temp(0);
                } else if (count <= 18) {
                    encode_temp(1);
                    m_bw.putbits(4, count - 3);
                } else if (count == 19) {
                    encode_temp(0);
                    encode_temp(1);
                    m_bw.putbits(4, 15);
                } else {
                    encode_temp(2);
                    m_bw.putbits(CBIT, count - 20);
                }
            } else {
                encode_temp(k + 2);
            }
        }
    }

    void encode_c(int c) { putcode(m_c_len[c], m_c_code[c]); }

    void encode_p(unsigned p) {
        int c = 0;
        unsigned q = p;
        while (q) {
            q >>= 1;
            c++;
        }
        putcode(m_pt_len[c], m_pt_code[c]);
        if (c > 1) m_bw.putbits(c - 1, p);
    }

    void send_block(const std::vector<Cmd>& cmds) {
        for (auto& v : m_c_freq) v = 0;
        for (auto& v : m_p_freq) v = 0;
        for (auto& cmd : cmds) {
            m_c_freq[cmd.code]++;
            if (cmd.code >= 256) {
                unsigned p = cmd.offset;
                int c = 0;
                while (p) {
                    p >>= 1;
                    c++;
                }
                m_p_freq[c]++;
            }
        }

        m_bw.putbits(16, static_cast<unsigned>(cmds.size()));
        int root = make_tree(NC, m_c_freq, m_c_len, m_c_code);
        if (root >= NC) {
            count_t_freq();
            root = make_tree(NT, m_t_freq, m_pt_len, m_pt_code);
            if (root >= NT) {
                write_pt_len(NT, TBIT, 3);
            } else {
                m_bw.putbits(TBIT, 0);
                m_bw.putbits(TBIT, static_cast<unsigned>(root));
            }
            write_c_len();
        } else {
            m_bw.putbits(TBIT, 0);
            m_bw.putbits(TBIT, 0);
            m_bw.putbits(CBIT, 0);
            m_bw.putbits(CBIT, static_cast<unsigned>(root));
        }
        root = make_tree(NP, m_p_freq, m_pt_len, m_pt_code);
        if (root >= NP) {
            write_pt_len(NP, PBIT, -1);
        } else {
            m_bw.putbits(PBIT, 0);
            m_bw.putbits(PBIT, static_cast<unsigned>(root));
        }
        for (auto& cmd : cmds) {
            encode_c(cmd.code);
            if (cmd.code >= 256) encode_p(cmd.offset);
        }
    }
};

void encode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
            ProgressFn progress = nullptr, void* userdata = nullptr) {
    BitWriter bw(out);
    Encoder enc(bw);
    enc.encode(in, progress, userdata);
    bw.flush();
}

// ---------- decoder (lhasa lh_new_decoder.c) ----------

class Decoder {
public:
    Decoder(const uint8_t* in, size_t inlen) : m_in(in), m_inlen(inlen) {
        m_code_tree.resize(NC * 2, TREE_LEAF);
        m_offset_tree.resize(15 * 2, TREE_LEAF);
        m_temp_tree.resize(31 * 2, TREE_LEAF);
        m_ringbuf.assign(RING_SIZE, ' ');
    }

    bool decode(long orig, std::vector<uint8_t>& out, ProgressFn progress = nullptr,
                void* userdata = nullptr) {
        long count = 0;
        int last_pct = -1;
        while (count < orig) {
            if (progress && orig > 0) {
                int pct = static_cast<int>(count * 100 / orig);
                if (pct != last_pct) {
                    progress(static_cast<uint64_t>(count), static_cast<uint64_t>(orig),
                             userdata);
                    last_pct = pct;
                }
            }
            while (m_block_remaining == 0) {
                if (!start_new_block()) return false;
            }
            m_block_remaining--;
            int code = read_from_tree(m_code_tree);
            if (code < 0) return false;
            if (code < 256) {
                out.push_back(static_cast<uint8_t>(code));
                m_ringbuf[m_ring_pos] = static_cast<uint8_t>(code);
                m_ring_pos = (m_ring_pos + 1) % RING_SIZE;
                count++;
            } else {
                int copy_count = code - 256 + THRESHOLD;
                int offset = read_offset_code();
                if (offset < 0) return false;
                unsigned start = m_ring_pos + RING_SIZE - static_cast<unsigned>(offset) - 1;
                for (int i = 0; i < copy_count && count < orig; i++) {
                    uint8_t b = m_ringbuf[(start + i) % RING_SIZE];
                    out.push_back(b);
                    m_ringbuf[m_ring_pos] = b;
                    m_ring_pos = (m_ring_pos + 1) % RING_SIZE;
                    count++;
                }
            }
        }
        return true;
    }

private:
    const uint8_t* m_in;
    size_t m_inlen;
    size_t m_inpos = 0;
    uint32_t m_bitbuf = 0;
    int m_bits = 0;
    unsigned m_block_remaining = 0;
    std::vector<uint16_t> m_code_tree, m_offset_tree, m_temp_tree;
    std::vector<uint8_t> m_ringbuf;
    unsigned m_ring_pos = 0;

    int read_bits(int n) {
        while (m_bits < n) {
            if (m_inpos >= m_inlen) break;
            uint8_t b = m_in[m_inpos++];
            m_bitbuf |= static_cast<uint32_t>(b) << (24 - m_bits);
            m_bits += 8;
        }
        if (m_bits < n) return -1;
        int result = static_cast<int>(m_bitbuf >> (32 - n));
        m_bitbuf <<= n;
        m_bits -= n;
        return result;
    }

    int read_bit() { return read_bits(1); }

    int read_from_tree(const std::vector<uint16_t>& tree) {
        uint16_t code = tree[0];
        while ((code & TREE_LEAF) == 0) {
            int bit = read_bit();
            if (bit < 0) return -1;
            code = tree[code + bit];
        }
        return static_cast<int>(code & ~TREE_LEAF);
    }

    void build_tree(std::vector<uint16_t>& tree, const std::vector<uint8_t>& lens,
                    unsigned num) {
        unsigned next_entry = 0;
        unsigned tree_allocated = 1;
        unsigned code_len = 0;
        auto codes_remaining = [&]() {
            for (unsigned i = 0; i < num; i++)
                if (lens[i] > code_len) return 1;
            return 0;
        };
        do {
            unsigned end_offset = tree_allocated;
            unsigned new_nodes = (tree_allocated - next_entry) * 2;
            if (tree_allocated + new_nodes <= tree.size()) {
                while (next_entry < end_offset) {
                    tree[next_entry] = static_cast<uint16_t>(tree_allocated);
                    tree_allocated += 2;
                    next_entry++;
                }
            }
            code_len++;
            for (unsigned i = 0; i < num; i++) {
                if (lens[i] == code_len) {
                    if (next_entry >= tree_allocated) break;
                    tree[next_entry] = static_cast<uint16_t>(i | TREE_LEAF);
                    next_entry++;
                }
            }
        } while (codes_remaining());
    }

    int read_length_value() {
        int len = read_bits(3);
        if (len < 0) return -1;
        if (len == 7) {
            for (;;) {
                int i = read_bit();
                if (i < 0) return -1;
                if (i == 0) break;
                ++len;
            }
        }
        return len;
    }

    bool read_temp_table() {
        int n = read_bits(TBIT);
        if (n < 0) return false;
        std::vector<uint8_t> lens(31, 0);
        if (n == 0) {
            int code = read_bits(TBIT);
            if (code < 0) return false;
            m_temp_tree[0] = static_cast<uint16_t>(code | TREE_LEAF);
            return true;
        }
        if (n > 31) n = 31;
        for (int i = 0; i < n; i++) {
            int len = read_length_value();
            if (len < 0) return false;
            lens[i] = static_cast<uint8_t>(len);
            if (i == 2) {
                int extra = read_bits(2);
                if (extra < 0) return false;
                for (int j = 0; j < extra; j++) {
                    ++i;
                    if (i < 31) lens[i] = 0;
                }
            }
        }
        build_tree(m_temp_tree, lens, n);
        return true;
    }

    int read_skip_count(int skiprange) {
        if (skiprange == 0) return 1;
        if (skiprange == 1) {
            int result = read_bits(4);
            if (result < 0) return -1;
            return result + 3;
        }
        int result = read_bits(9);
        if (result < 0) return -1;
        return result + 20;
    }

    bool read_code_table() {
        int n = read_bits(CBIT);
        if (n < 0) return false;
        std::vector<uint8_t> lens(NC, 0);
        if (n == 0) {
            int code = read_bits(CBIT);
            if (code < 0) return false;
            m_code_tree[0] = static_cast<uint16_t>(code | TREE_LEAF);
            return true;
        }
        if (n > NC) n = NC;
        int i = 0;
        while (i < n) {
            int code = read_from_tree(m_temp_tree);
            if (code < 0) return false;
            if (code <= 2) {
                int skip = read_skip_count(code);
                if (skip < 0) return false;
                for (int j = 0; j < skip && i < n; j++) {
                    lens[i] = 0;
                    ++i;
                }
            } else {
                lens[i] = static_cast<uint8_t>(code - 2);
                ++i;
            }
        }
        build_tree(m_code_tree, lens, n);
        return true;
    }

    bool read_offset_table() {
        int n = read_bits(PBIT);
        if (n < 0) return false;
        std::vector<uint8_t> lens(15, 0);
        if (n == 0) {
            int code = read_bits(PBIT);
            if (code < 0) return false;
            m_offset_tree[0] = static_cast<uint16_t>(code | TREE_LEAF);
            return true;
        }
        if (n > 15) n = 15;
        for (int i = 0; i < n; i++) {
            int len = read_length_value();
            if (len < 0) return false;
            lens[i] = static_cast<uint8_t>(len);
        }
        build_tree(m_offset_tree, lens, n);
        return true;
    }

    bool start_new_block() {
        int len = read_bits(16);
        if (len < 0) return false;
        m_block_remaining = static_cast<unsigned>(len);
        if (!read_temp_table()) return false;
        if (!read_code_table()) return false;
        if (!read_offset_table()) return false;
        return true;
    }

    int read_offset_code() {
        int bits = read_from_tree(m_offset_tree);
        if (bits < 0) return -1;
        if (bits == 0) return 0;
        if (bits == 1) return 1;
        int result = read_bits(bits - 1);
        if (result < 0) return -1;
        return result + (1 << (bits - 1));
    }
};

void decode(const uint8_t* src, size_t comp_len, long orig, std::vector<uint8_t>& out,
            ProgressFn progress = nullptr, void* userdata = nullptr) {
    Decoder dec(src, comp_len);
    dec.decode(orig, out, progress, userdata);
}

}  // namespace lh5

// ===================== LHA container (level 2) =====================

bool archive_write(const std::string& out_path, const std::vector<Member>& members,
                   std::string& err, ProgressFn progress, void* userdata) {
    (void)err;
    uint64_t total = 0;
    for (auto& m : members) total += m.data.size();
    uint64_t done = 0;
    std::string blob;
    uint32_t now_ts = static_cast<uint32_t>(std::time(nullptr));
    for (auto& m : members) {
        std::vector<uint8_t> comp;
        std::string method;
        if (m.data.empty()) {
            method = "-lh0-";
        } else {
            method = "-lh5-";
            lh5::encode(m.data, comp,
                        [&](uint64_t d, uint64_t, void*) {
                            if (progress) progress(done + d, total, userdata);
                        },
                        nullptr);
        }
        done += m.data.size();
        uint16_t crc = crc16(m.data.data(), m.data.size());
        std::string name = m.name.empty() ? "data" : m.name;

        std::string exts;
        {
            uint16_t len = static_cast<uint16_t>(3 + name.size());
            le16(exts, len);
            exts.push_back(0x01);
            exts += name;
        }
        if (!m.pkg_name.empty()) {
            uint16_t len = static_cast<uint16_t>(3 + m.pkg_name.size());
            le16(exts, len);
            exts.push_back(0x60);
            exts += m.pkg_name;
        }
        if (!m.pkg_version.empty()) {
            uint16_t len = static_cast<uint16_t>(3 + m.pkg_version.size());
            le16(exts, len);
            exts.push_back(0x61);
            exts += m.pkg_version;
        }
        {
            uint16_t len = 5;
            le16(exts, len);
            exts.push_back(0x50);
            le16(exts, static_cast<uint16_t>((m.mode & 07777) | (m.mode & 0170000)));
        }
        le16(exts, 0);  // end of extended headers

        uint16_t header_len = static_cast<uint16_t>(24 + exts.size());

        std::string head;
        le16(head, header_len);
        head += method;
        le32(head, static_cast<uint32_t>(comp.size()));
        le32(head, static_cast<uint32_t>(m.data.size()));
        le32(head, now_ts);
        head.push_back(0x20);  // attribute: regular file
        head.push_back(2);     // level 2
        le16(head, crc);
        head.push_back('U');  // OS type: Unix
        head += exts;

        blob += head;
        blob.append(reinterpret_cast<const char*>(comp.data()), comp.size());
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    out << blob;
    out.close();
    return true;
}

// Read one member from an in-memory archive. Returns:
//   1 = member read, 0 = clean end of archive, -1 = error.
static int read_one_member(const std::string& data, size_t& pos, Member& m,
                           std::string& err, uint64_t total_orig, uint64_t& orig_done,
                           ProgressFn progress, void* userdata) {
    if (pos + 2 > data.size()) return 0;
    uint16_t header_len = rd16(reinterpret_cast<const uint8_t*>(&data[pos]));
    if (header_len == 0) return 0;
    if (pos + header_len > data.size()) {
        err = "header out of range";
        return -1;
    }

    const uint8_t* h = reinterpret_cast<const uint8_t*>(&data[pos]);
    std::string method(h + 2, h + 7);
    uint32_t packed = rd32(h + 7);
    uint32_t orig = rd32(h + 11);
    uint16_t crc = rd16(h + 21);
    int level = h[20];

    std::string name;
    uint32_t mode = 0644;
    if (level == 2) {
        size_t ext = 24;
        while (ext + 2 <= header_len) {
            uint16_t elen = rd16(h + ext);
            if (elen == 0) break;
            uint8_t type = h[ext + 2];
            if (type == 0x01) {
                name.assign(reinterpret_cast<const char*>(h + ext + 3), elen - 3);
            } else if (type == 0x50 && elen >= 5) {
                mode = rd16(h + ext + 3) & 0177777;
            } else if (type == 0x60) {
                m.pkg_name.assign(reinterpret_cast<const char*>(h + ext + 3), elen - 3);
            } else if (type == 0x61) {
                m.pkg_version.assign(reinterpret_cast<const char*>(h + ext + 3), elen - 3);
            }
            ext += elen;
        }
    } else if (level == 0 || level == 1) {
        uint8_t path_len = h[21];
        name.assign(reinterpret_cast<const char*>(h + 22), path_len);
    } else {
        err = "unsupported header level " + std::to_string(level);
        return -1;
    }

    size_t data_off = pos + header_len;
    if (data_off + packed > data.size()) {
        err = "data out of range";
        return -1;
    }
    const uint8_t* comp = reinterpret_cast<const uint8_t*>(&data[data_off]);

    m.name = name;
    m.mode = mode;
    if (method == "-lh5-" || method == "-lh4-") {
        lh5::decode(comp, packed, orig, m.data,
                    [&](uint64_t d, uint64_t, void*) {
                        if (progress)
                            progress(orig_done + d, total_orig, userdata);
                    },
                    nullptr);
    } else if (method == "-lh0-") {
        m.data.assign(comp, comp + packed);
    } else {
        err = "unsupported method " + method;
        return -1;
    }
    if (m.data.size() != orig) {
        err = "decompressed size mismatch for " + name;
        return -1;
    }
    uint16_t got = crc16(m.data.data(), m.data.size());
    if (crc != got) {
        err = "crc mismatch for " + name;
        return -1;
    }
    orig_done += orig;
    pos = data_off + packed;
    return 1;
}

bool archive_read(const std::string& in_path, std::vector<Member>& members,
                  std::string& err, ProgressFn progress, void* userdata) {
    std::ifstream f(in_path, std::ios::binary);
    if (!f) {
        err = "cannot open";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();
    size_t pos = 0;
    uint64_t total_orig = 0;
    for (size_t q = 0; q + 2 <= data.size();) {
        uint16_t hl = rd16(reinterpret_cast<const uint8_t*>(&data[q]));
        if (hl == 0) break;
        if (q + hl > data.size()) break;
        total_orig += rd32(reinterpret_cast<const uint8_t*>(&data[q + 11]));
        q += hl + rd32(reinterpret_cast<const uint8_t*>(&data[q + 7]));
    }
    uint64_t orig_done = 0;
    for (;;) {
        Member m;
        int r = read_one_member(data, pos, m, err, total_orig, orig_done, progress,
                                userdata);
        if (r == 0) break;
        if (r < 0) return false;
        members.push_back(std::move(m));
    }
    return true;
}

bool archive_read_first(const std::string& in_path, Member& m, std::string& err) {
    std::ifstream f(in_path, std::ios::binary);
    if (!f) {
        err = "cannot open";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();
    size_t pos = 0;
    uint64_t orig_done = 0;
    int r = read_one_member(data, pos, m, err, 0, orig_done, nullptr, nullptr);
    return r == 1;
}

}  // namespace lha
