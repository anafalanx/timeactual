// h2.c -- minimal HTTP/2 client for DoH (see h2.h).
//
// Two halves: an HPACK codec (static table, a bounded dynamic table, the
// canonical Huffman code) and the framing for exactly one request/response
// on stream 1. Everything that arrives is bounds-checked against fixed
// buffers; nothing here allocates except the one per-exchange context.

#include "h2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// HPACK static table (RFC 7541 Appendix A), index 1..61
// ---------------------------------------------------------------------------
static const struct { const char *name; const char *value; } kStatic[61] = {
    { ":authority", "" },                    //  1
    { ":method", "GET" },                    //  2
    { ":method", "POST" },                   //  3
    { ":path", "/" },                        //  4
    { ":path", "/index.html" },              //  5
    { ":scheme", "http" },                   //  6
    { ":scheme", "https" },                  //  7
    { ":status", "200" },                    //  8
    { ":status", "204" },                    //  9
    { ":status", "206" },                    // 10
    { ":status", "304" },                    // 11
    { ":status", "400" },                    // 12
    { ":status", "404" },                    // 13
    { ":status", "500" },                    // 14
    { "accept-charset", "" },                // 15
    { "accept-encoding", "gzip, deflate" },  // 16
    { "accept-language", "" },               // 17
    { "accept-ranges", "" },                 // 18
    { "accept", "" },                        // 19
    { "access-control-allow-origin", "" },   // 20
    { "age", "" },                           // 21
    { "allow", "" },                         // 22
    { "authorization", "" },                 // 23
    { "cache-control", "" },                 // 24
    { "content-disposition", "" },           // 25
    { "content-encoding", "" },              // 26
    { "content-language", "" },              // 27
    { "content-length", "" },                // 28
    { "content-location", "" },              // 29
    { "content-range", "" },                 // 30
    { "content-type", "" },                  // 31
    { "cookie", "" },                        // 32
    { "date", "" },                          // 33
    { "etag", "" },                          // 34
    { "expect", "" },                        // 35
    { "expires", "" },                       // 36
    { "from", "" },                          // 37
    { "host", "" },                          // 38
    { "if-match", "" },                      // 39
    { "if-modified-since", "" },             // 40
    { "if-none-match", "" },                 // 41
    { "if-range", "" },                      // 42
    { "if-unmodified-since", "" },           // 43
    { "last-modified", "" },                 // 44
    { "link", "" },                          // 45
    { "location", "" },                      // 46
    { "max-forwards", "" },                  // 47
    { "proxy-authenticate", "" },            // 48
    { "proxy-authorization", "" },           // 49
    { "range", "" },                         // 50
    { "referer", "" },                       // 51
    { "refresh", "" },                       // 52
    { "retry-after", "" },                   // 53
    { "server", "" },                        // 54
    { "set-cookie", "" },                    // 55
    { "strict-transport-security", "" },     // 56
    { "transfer-encoding", "" },             // 57
    { "user-agent", "" },                    // 58
    { "vary", "" },                          // 59
    { "via", "" },                           // 60
    { "www-authenticate", "" },              // 61
};

// Static indices the request encoder refers to by name.
enum {
    ST_AUTHORITY      = 1,
    ST_METHOD_POST    = 3,
    ST_PATH           = 4,
    ST_SCHEME_HTTPS   = 7,
    ST_ACCEPT         = 19,
    ST_CONTENT_LENGTH = 28,
    ST_CONTENT_TYPE   = 31,
};

// ---------------------------------------------------------------------------
// HPACK Huffman code (RFC 7541 Appendix B): code and bit length per symbol,
// index 256 = EOS. The code is canonical: within one length, codes rise
// with the symbol; the unit test checks that and the Kraft equality, so a
// transcription slip in either column is caught against the other.
// ---------------------------------------------------------------------------
static const struct { uint32_t code; uint8_t len; } kHuff[257] = {
    { 0x1ff8, 13 }, { 0x7fffd8, 23 }, { 0xfffffe2, 28 }, { 0xfffffe3, 28 },
    { 0xfffffe4, 28 }, { 0xfffffe5, 28 }, { 0xfffffe6, 28 }, { 0xfffffe7, 28 },
    { 0xfffffe8, 28 }, { 0xffffea, 24 }, { 0x3ffffffc, 30 }, { 0xfffffe9, 28 },
    { 0xfffffea, 28 }, { 0x3ffffffd, 30 }, { 0xfffffeb, 28 }, { 0xfffffec, 28 },
    { 0xfffffed, 28 }, { 0xfffffee, 28 }, { 0xfffffef, 28 }, { 0xffffff0, 28 },
    { 0xffffff1, 28 }, { 0xffffff2, 28 }, { 0x3ffffffe, 30 }, { 0xffffff3, 28 },
    { 0xffffff4, 28 }, { 0xffffff5, 28 }, { 0xffffff6, 28 }, { 0xffffff7, 28 },
    { 0xffffff8, 28 }, { 0xffffff9, 28 }, { 0xffffffa, 28 }, { 0xffffffb, 28 },
    { 0x14, 6 },      { 0x3f8, 10 },    { 0x3f9, 10 },    { 0xffa, 12 },      // SP ! DQ #
    { 0x1ff9, 13 },   { 0x15, 6 },      { 0xf8, 8 },      { 0x7fa, 11 },      // $ % & APOS
    { 0x3fa, 10 },    { 0x3fb, 10 },    { 0xf9, 8 },      { 0x7fb, 11 },      // ( ) * +
    { 0xfa, 8 },      { 0x16, 6 },      { 0x17, 6 },      { 0x18, 6 },        // , - . /
    { 0x0, 5 },       { 0x1, 5 },       { 0x2, 5 },       { 0x19, 6 },        // 0 1 2 3
    { 0x1a, 6 },      { 0x1b, 6 },      { 0x1c, 6 },      { 0x1d, 6 },        // 4 5 6 7
    { 0x1e, 6 },      { 0x1f, 6 },      { 0x5c, 7 },      { 0xfb, 8 },        // 8 9 : ;
    { 0x7ffc, 15 },   { 0x20, 6 },      { 0xffb, 12 },    { 0x3fc, 10 },      // < = > ?
    { 0x1ffa, 13 },   { 0x21, 6 },      { 0x5d, 7 },      { 0x5e, 7 },        // @ A B C
    { 0x5f, 7 },      { 0x60, 7 },      { 0x61, 7 },      { 0x62, 7 },        // D E F G
    { 0x63, 7 },      { 0x64, 7 },      { 0x65, 7 },      { 0x66, 7 },        // H I J K
    { 0x67, 7 },      { 0x68, 7 },      { 0x69, 7 },      { 0x6a, 7 },        // L M N O
    { 0x6b, 7 },      { 0x6c, 7 },      { 0x6d, 7 },      { 0x6e, 7 },        // P Q R S
    { 0x6f, 7 },      { 0x70, 7 },      { 0x71, 7 },      { 0x72, 7 },        // T U V W
    { 0xfc, 8 },      { 0x73, 7 },      { 0xfd, 8 },      { 0x1ffb, 13 },     // X Y Z [
    { 0x7fff0, 19 },  { 0x1ffc, 13 },   { 0x3ffc, 14 },   { 0x22, 6 },        // BSL ] ^ _
    { 0x7ffd, 15 },   { 0x3, 5 },       { 0x23, 6 },      { 0x4, 5 },         // ` a b c
    { 0x24, 6 },      { 0x5, 5 },       { 0x25, 6 },      { 0x26, 6 },        // d e f g
    { 0x27, 6 },      { 0x6, 5 },       { 0x74, 7 },      { 0x75, 7 },        // h i j k
    { 0x28, 6 },      { 0x29, 6 },      { 0x2a, 6 },      { 0x7, 5 },         // l m n o
    { 0x2b, 6 },      { 0x76, 7 },      { 0x2c, 6 },      { 0x8, 5 },         // p q r s
    { 0x9, 5 },       { 0x2d, 6 },      { 0x77, 7 },      { 0x78, 7 },        // t u v w
    { 0x79, 7 },      { 0x7a, 7 },      { 0x7b, 7 },      { 0x7ffe, 15 },     // x y z {
    { 0x7fc, 11 },    { 0x3ffd, 14 },   { 0x1ffd, 13 },   { 0xffffffc, 28 },  // | } ~ DEL
    { 0xfffe6, 20 },  { 0x3fffd2, 22 }, { 0xfffe7, 20 },  { 0xfffe8, 20 },    // 128..131
    { 0x3fffd3, 22 }, { 0x3fffd4, 22 }, { 0x3fffd5, 22 }, { 0x7fffd9, 23 },
    { 0x3fffd6, 22 }, { 0x7fffda, 23 }, { 0x7fffdb, 23 }, { 0x7fffdc, 23 },
    { 0x7fffdd, 23 }, { 0x7fffde, 23 }, { 0xffffeb, 24 }, { 0x7fffdf, 23 },
    { 0xffffec, 24 }, { 0xffffed, 24 }, { 0x3fffd7, 22 }, { 0x7fffe0, 23 },
    { 0xffffee, 24 }, { 0x7fffe1, 23 }, { 0x7fffe2, 23 }, { 0x7fffe3, 23 },
    { 0x7fffe4, 23 }, { 0x1fffdc, 21 }, { 0x3fffd8, 22 }, { 0x7fffe5, 23 },
    { 0x3fffd9, 22 }, { 0x7fffe6, 23 }, { 0x7fffe7, 23 }, { 0xffffef, 24 },
    { 0x3fffda, 22 }, { 0x1fffdd, 21 }, { 0xfffe9, 20 },  { 0x3fffdb, 22 },   // 160..163
    { 0x3fffdc, 22 }, { 0x7fffe8, 23 }, { 0x7fffe9, 23 }, { 0x1fffde, 21 },
    { 0x7fffea, 23 }, { 0x3fffdd, 22 }, { 0x3fffde, 22 }, { 0xfffff0, 24 },
    { 0x1fffdf, 21 }, { 0x3fffdf, 22 }, { 0x7fffeb, 23 }, { 0x7fffec, 23 },
    { 0x1fffe0, 21 }, { 0x1fffe1, 21 }, { 0x3fffe0, 22 }, { 0x1fffe2, 21 },
    { 0x7fffed, 23 }, { 0x3fffe1, 22 }, { 0x7fffee, 23 }, { 0x7fffef, 23 },
    { 0xfffea, 20 },  { 0x3fffe2, 22 }, { 0x3fffe3, 22 }, { 0x3fffe4, 22 },
    { 0x7ffff0, 23 }, { 0x3fffe5, 22 }, { 0x3fffe6, 22 }, { 0x7ffff1, 23 },
    { 0x3ffffe0, 26 },{ 0x3ffffe1, 26 },{ 0xfffeb, 20 },  { 0x7fff1, 19 },    // 192..195
    { 0x3fffe7, 22 }, { 0x7ffff2, 23 }, { 0x3fffe8, 22 }, { 0x1ffffec, 25 },
    { 0x3ffffe2, 26 },{ 0x3ffffe3, 26 },{ 0x3ffffe4, 26 },{ 0x7ffffde, 27 },
    { 0x7ffffdf, 27 },{ 0x3ffffe5, 26 },{ 0xfffff1, 24 }, { 0x1ffffed, 25 },
    { 0x7fff2, 19 },  { 0x1fffe3, 21 }, { 0x3ffffe6, 26 },{ 0x7ffffe0, 27 },
    { 0x7ffffe1, 27 },{ 0x3ffffe7, 26 },{ 0x7ffffe2, 27 },{ 0xfffff2, 24 },
    { 0x1fffe4, 21 }, { 0x1fffe5, 21 }, { 0x3ffffe8, 26 },{ 0x3ffffe9, 26 },
    { 0xffffffd, 28 },{ 0x7ffffe3, 27 },{ 0x7ffffe4, 27 },{ 0x7ffffe5, 27 },
    { 0xfffec, 20 },  { 0xfffff3, 24 }, { 0xfffed, 20 },  { 0x1fffe6, 21 },   // 224..227
    { 0x3fffe9, 22 }, { 0x1fffe7, 21 }, { 0x1fffe8, 21 }, { 0x7ffff3, 23 },
    { 0x3fffea, 22 }, { 0x3fffeb, 22 }, { 0x1ffffee, 25 },{ 0x1ffffef, 25 },
    { 0xfffff4, 24 }, { 0xfffff5, 24 }, { 0x3ffffea, 26 },{ 0x7ffff4, 23 },
    { 0x3ffffeb, 26 },{ 0x7ffffe6, 27 },{ 0x3ffffec, 26 },{ 0x3ffffed, 26 },
    { 0x7ffffe7, 27 },{ 0x7ffffe8, 27 },{ 0x7ffffe9, 27 },{ 0x7ffffea, 27 },
    { 0x7ffffeb, 27 },{ 0xffffffe, 28 },{ 0x7ffffec, 27 },{ 0x7ffffed, 27 },
    { 0x7ffffee, 27 },{ 0x7ffffef, 27 },{ 0x7fffff0, 27 },{ 0x3ffffee, 26 },
    { 0x3fffffff, 30 },                                                        // EOS
};

#ifdef LUNAR_TESTING
void Hpack_TestHuffmanEntry(int sym, uint32_t *code, int *len)
{
    if (sym < 0 || sym > 256) { *code = 0; *len = 0; return; }
    *code = kHuff[sym].code;
    *len  = kHuff[sym].len;
}
#endif

// ---------------------------------------------------------------------------
// Primitive codecs
// ---------------------------------------------------------------------------

int Hpack_DecodeInt(const uint8_t *p, size_t len, int prefix_bits,
                    uint32_t *out, size_t *consumed)
{
    if (!p || len == 0 || prefix_bits < 1 || prefix_bits > 8) return -1;
    uint32_t mask = (1u << prefix_bits) - 1;
    uint32_t v = p[0] & mask;
    size_t i = 1;
    if (v < mask) { *out = v; *consumed = 1; return 0; }
    for (int shift = 0; ; shift += 7) {
        // Header lengths and indices are small; anything past 2^28 is a
        // malformed block, not a legitimate value.
        if (i >= len || shift > 21) return -1;
        uint8_t b = p[i++];
        v += (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
    }
    *out = v; *consumed = i;
    return 0;
}

size_t Hpack_EncodeInt(uint8_t *out, size_t cap, uint8_t first_byte_bits,
                       int prefix_bits, uint32_t value)
{
    if (!out || cap == 0 || prefix_bits < 1 || prefix_bits > 8) return 0;
    uint32_t mask = (1u << prefix_bits) - 1;
    size_t n = 0;
    if (value < mask) { out[n++] = (uint8_t)(first_byte_bits | value); return n; }
    out[n++] = (uint8_t)(first_byte_bits | mask);
    value -= mask;
    while (value >= 128) {
        if (n >= cap) return 0;
        out[n++] = (uint8_t)(0x80 | (value & 0x7F));
        value >>= 7;
    }
    if (n >= cap) return 0;
    out[n++] = (uint8_t)value;
    return n;
}

int Hpack_HuffmanDecode(const uint8_t *in, size_t len,
                        uint8_t *out, size_t cap, size_t *out_len)
{
    uint32_t code = 0;
    int nbits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        for (int b = 7; b >= 0; b--) {
            code = (code << 1) | ((in[i] >> b) & 1u);
            nbits++;
            if (nbits < 5) continue;
            // Header strings are short: a linear scan per bit is plenty.
            int hit = 0;
            for (int s = 0; s < 257; s++) {
                if (kHuff[s].len == nbits && kHuff[s].code == code) {
                    if (s == 256) return -1;        // EOS inside a string
                    if (n >= cap) return -1;
                    out[n++] = (uint8_t)s;
                    code = 0; nbits = 0; hit = 1;
                    break;
                }
            }
            if (!hit && nbits > 30) return -1;
        }
    }
    // Padding: strictly fewer than 8 bits, all ones (a prefix of EOS).
    if (nbits >= 8) return -1;
    if (code != ((1u << nbits) - 1)) return -1;
    *out_len = n;
    return 0;
}

// ---------------------------------------------------------------------------
// Decoder with dynamic table
// ---------------------------------------------------------------------------

void Hpack_Init(HpackDecoder *d)
{
    memset(d, 0, sizeof *d);
    d->max = HPACK_DYN_MAX;
}

static size_t dyn_size(const HpackDecoder *d)
{
    return d->used + 32u * (size_t)d->count;
}

static void dyn_evict_oldest(HpackDecoder *d)
{
    if (d->count == 0) return;
    size_t sz = (size_t)d->ent[0].nlen + d->ent[0].vlen;
    memmove(d->arena, d->arena + sz, d->used - sz);
    d->used -= sz;
    for (int i = 1; i < d->count; i++) {
        d->ent[i - 1] = d->ent[i];
        d->ent[i - 1].off = (uint16_t)(d->ent[i - 1].off - sz);
    }
    d->count--;
}

static void dyn_set_max(HpackDecoder *d, size_t max)
{
    d->max = max;
    while (d->count && dyn_size(d) > d->max) dyn_evict_oldest(d);
}

static int dyn_add(HpackDecoder *d, const uint8_t *n, size_t nl,
                   const uint8_t *v, size_t vl)
{
    size_t need = nl + vl + 32;
    if (need > d->max) {
        // RFC 7541 4.4: an entry larger than the table empties it.
        while (d->count) dyn_evict_oldest(d);
        return 0;
    }
    while (d->count && dyn_size(d) + need > d->max) dyn_evict_oldest(d);
    if (d->count >= HPACK_DYN_ENTRIES || d->used + nl + vl > sizeof d->arena) return -1;
    memcpy(d->arena + d->used, n, nl);
    memcpy(d->arena + d->used + nl, v, vl);
    d->ent[d->count].off  = (uint16_t)d->used;
    d->ent[d->count].nlen = (uint16_t)nl;
    d->ent[d->count].vlen = (uint16_t)vl;
    d->count++;
    d->used += nl + vl;
    return 0;
}

static int table_lookup(const HpackDecoder *d, uint32_t idx,
                        const uint8_t **n, size_t *nl,
                        const uint8_t **v, size_t *vl)
{
    if (idx == 0) return -1;
    if (idx <= 61) {
        *n = (const uint8_t *)kStatic[idx - 1].name;  *nl = strlen(kStatic[idx - 1].name);
        *v = (const uint8_t *)kStatic[idx - 1].value; *vl = strlen(kStatic[idx - 1].value);
        return 0;
    }
    uint32_t k = idx - 62;                 // 0 = most recently added
    if (k >= (uint32_t)d->count) return -1;
    int i = d->count - 1 - (int)k;
    *n = d->arena + d->ent[i].off;                  *nl = d->ent[i].nlen;
    *v = d->arena + d->ent[i].off + d->ent[i].nlen; *vl = d->ent[i].vlen;
    return 0;
}

// A string literal at *pos: raw bytes stay in the block, Huffman strings
// land in `scratch`. Either way the length is bounded by `scratch_cap`.
static int read_string(const uint8_t *p, size_t len, size_t *pos,
                       uint8_t *scratch, size_t scratch_cap,
                       const uint8_t **out, size_t *out_len)
{
    if (*pos >= len) return -1;
    int huff = p[*pos] & 0x80;
    uint32_t slen; size_t used;
    if (Hpack_DecodeInt(p + *pos, len - *pos, 7, &slen, &used) != 0) return -1;
    *pos += used;
    if (slen > len - *pos) return -1;
    if (huff) {
        size_t n;
        if (Hpack_HuffmanDecode(p + *pos, slen, scratch, scratch_cap, &n) != 0) return -1;
        *out = scratch; *out_len = n;
    } else {
        if (slen > scratch_cap) return -1;
        *out = p + *pos; *out_len = slen;
    }
    *pos += slen;
    return 0;
}

int Hpack_Decode(HpackDecoder *d, const uint8_t *p, size_t len,
                 HpackHeaderFn fn, void *ctx)
{
    if (!d || (!p && len) || !fn) return -1;
    size_t pos = 0;
    while (pos < len) {
        uint8_t b = p[pos];
        uint32_t idx; size_t used;
        const uint8_t *n, *v; size_t nl, vl;

        if (b & 0x80) {                                  // indexed field
            if (Hpack_DecodeInt(p + pos, len - pos, 7, &idx, &used)) return -1;
            pos += used;
            if (table_lookup(d, idx, &n, &nl, &v, &vl)) return -1;
            fn(ctx, n, nl, v, vl);
            continue;
        }
        if ((b & 0xE0) == 0x20) {                        // dynamic table size update
            if (Hpack_DecodeInt(p + pos, len - pos, 5, &idx, &used)) return -1;
            pos += used;
            if (idx > HPACK_DYN_MAX) return -1;
            dyn_set_max(d, idx);
            continue;
        }
        // Literal: 01xxxxxx adds to the table, 0000xxxx / 0001xxxx do not.
        int add = (b & 0xC0) == 0x40;
        if (Hpack_DecodeInt(p + pos, len - pos, add ? 6 : 4, &idx, &used)) return -1;
        pos += used;
        if (idx == 0) {
            if (read_string(p, len, &pos, d->nbuf, sizeof d->nbuf, &n, &nl)) return -1;
        } else {
            // Copy the name out: adding the entry below may evict the very
            // table entry it points into (RFC 7541 4.4).
            const uint8_t *tn, *tv; size_t tnl, tvl;
            if (table_lookup(d, idx, &tn, &tnl, &tv, &tvl)) return -1;
            if (tnl > sizeof d->nbuf) return -1;
            memcpy(d->nbuf, tn, tnl);
            n = d->nbuf; nl = tnl;
        }
        if (read_string(p, len, &pos, d->vbuf, sizeof d->vbuf, &v, &vl)) return -1;
        fn(ctx, n, nl, v, vl);
        if (add && dyn_add(d, n, nl, v, vl)) return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Request encoding: static-table references and literals without indexing,
// never Huffman -- the server needs no state to decode it.
// ---------------------------------------------------------------------------

static size_t enc_literal(uint8_t *o, size_t cap, uint32_t name_idx, const char *value)
{
    size_t n = Hpack_EncodeInt(o, cap, 0x00, 4, name_idx);
    if (!n) return 0;
    size_t vl = strlen(value);
    size_t m = Hpack_EncodeInt(o + n, cap - n, 0x00, 7, (uint32_t)vl);
    if (!m) return 0;
    n += m;
    if (vl > cap - n) return 0;
    memcpy(o + n, value, vl);
    return n + vl;
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

enum {
    F_DATA = 0, F_HEADERS = 1, F_PRIORITY = 2, F_RST_STREAM = 3, F_SETTINGS = 4,
    F_PUSH_PROMISE = 5, F_PING = 6, F_GOAWAY = 7, F_WINDOW_UPDATE = 8,
    F_CONTINUATION = 9,
};
enum {
    FL_END_STREAM = 0x1, FL_ACK = 0x1, FL_END_HEADERS = 0x4, FL_PADDED = 0x8,
    FL_PRIORITY = 0x20,
};
#define H2_MAX_FRAME   16384      // SETTINGS_MAX_FRAME_SIZE default; we never raise it
#define H2_MAX_FRAMES  512        // a response is a handful of frames; more is a stall tactic

typedef struct {
    HpackDecoder dec;
    uint8_t      frame[H2_MAX_FRAME];    // current frame payload
    uint8_t      hblock[H2_MAX_FRAME];   // header block under assembly
    uint8_t      req[24 + 9 + 18 + 9 + 1024 + 9 + H2_MAX_FRAME];
} H2Conn;

static int io_write_all(const H2Io *io, const uint8_t *p, size_t n)
{
    while (n) {
        int w = io->write(io->ctx, p, n);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int io_read_full(const H2Io *io, uint8_t *p, size_t n)
{
    while (n) {
        int r = io->read(io->ctx, p, n);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static size_t put_frame_header(uint8_t *h, size_t len, uint8_t type, uint8_t flags, uint32_t sid)
{
    h[0] = (uint8_t)(len >> 16); h[1] = (uint8_t)(len >> 8); h[2] = (uint8_t)len;
    h[3] = type; h[4] = flags;
    h[5] = (uint8_t)((sid >> 24) & 0x7F); h[6] = (uint8_t)(sid >> 16);
    h[7] = (uint8_t)(sid >> 8); h[8] = (uint8_t)sid;
    return 9;
}

static int send_frame(const H2Io *io, uint8_t type, uint8_t flags, uint32_t sid,
                      const uint8_t *payload, size_t len)
{
    uint8_t h[9];
    put_frame_header(h, len, type, flags, sid);
    if (io_write_all(io, h, 9)) return -1;
    return len ? io_write_all(io, payload, len) : 0;
}

typedef struct { int status; int have; } StatusCapture;

static void on_response_header(void *ctx, const uint8_t *n, size_t nl,
                               const uint8_t *v, size_t vl)
{
    StatusCapture *s = (StatusCapture *)ctx;
    if (nl == 7 && memcmp(n, ":status", 7) == 0 && vl == 3 &&
        v[0] >= '0' && v[0] <= '9' && v[1] >= '0' && v[1] <= '9' &&
        v[2] >= '0' && v[2] <= '9') {
        s->status = (v[0] - '0') * 100 + (v[1] - '0') * 10 + (v[2] - '0');
        s->have = 1;
    }
}

int H2_PostOnce(const H2Io *io, const char *authority, const char *path,
                const char *content_type,
                const uint8_t *body, size_t body_len,
                uint8_t *out, size_t out_cap, size_t *out_len,
                int *out_status)
{
    if (!io || !io->read || !io->write || !authority || !path || !content_type ||
        (!body && body_len) || !out || !out_len || !out_status) return -1;
    *out_len = 0;
    *out_status = 0;
    if (body_len > H2_MAX_FRAME) return -1;

    H2Conn *c = (H2Conn *)malloc(sizeof *c);
    if (!c) return -1;
    Hpack_Init(&c->dec);

    // --- Request: preface, SETTINGS, HEADERS, DATA -- one write. ---
    uint8_t hb[1024];
    size_t  hl = 0, n;
    char    clen[16];
    _snprintf(clen, sizeof clen, "%u", (unsigned)body_len);
    clen[sizeof clen - 1] = 0;
#define HB_ADD(expr) do { n = (expr); if (!n) goto fail; hl += n; } while (0)
    HB_ADD(Hpack_EncodeInt(hb + hl, sizeof hb - hl, 0x80, 7, ST_METHOD_POST));
    HB_ADD(Hpack_EncodeInt(hb + hl, sizeof hb - hl, 0x80, 7, ST_SCHEME_HTTPS));
    HB_ADD(enc_literal(hb + hl, sizeof hb - hl, ST_AUTHORITY, authority));
    HB_ADD(enc_literal(hb + hl, sizeof hb - hl, ST_PATH, path));
    HB_ADD(enc_literal(hb + hl, sizeof hb - hl, ST_CONTENT_TYPE, content_type));
    HB_ADD(enc_literal(hb + hl, sizeof hb - hl, ST_ACCEPT, content_type));
    HB_ADD(enc_literal(hb + hl, sizeof hb - hl, ST_CONTENT_LENGTH, clen));
#undef HB_ADD

    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    // ENABLE_PUSH 0, MAX_CONCURRENT_STREAMS 1, MAX_HEADER_LIST_SIZE 16384.
    static const uint8_t settings[18] = {
        0, 2, 0, 0, 0, 0,
        0, 3, 0, 0, 0, 1,
        0, 6, 0, 0, 0x40, 0,
    };
    size_t rl = 0;
    memcpy(c->req + rl, preface, 24);                                  rl += 24;
    rl += put_frame_header(c->req + rl, sizeof settings, F_SETTINGS, 0, 0);
    memcpy(c->req + rl, settings, sizeof settings);                    rl += sizeof settings;
    rl += put_frame_header(c->req + rl, hl, F_HEADERS, FL_END_HEADERS, 1);
    memcpy(c->req + rl, hb, hl);                                       rl += hl;
    rl += put_frame_header(c->req + rl, body_len, F_DATA, FL_END_STREAM, 1);
    if (body_len) memcpy(c->req + rl, body, body_len);
    rl += body_len;
    if (io_write_all(io, c->req, rl)) goto fail;

    // --- Response ---
    StatusCapture st = { 0, 0 };
    size_t hbl = 0;                 // header block bytes assembled so far
    int in_continuation = 0;        // a HEADERS without END_HEADERS is open
    int hb_end_stream = 0;          // that HEADERS carried END_STREAM
    int final_status = 0;           // non-1xx status seen
    int stream_done = 0;
    for (int frames = 0; !stream_done; frames++) {
        if (frames >= H2_MAX_FRAMES) goto fail;
        uint8_t fh[9];
        if (io_read_full(io, fh, 9)) goto fail;
        size_t   flen  = ((size_t)fh[0] << 16) | ((size_t)fh[1] << 8) | fh[2];
        uint8_t  type  = fh[3], flags = fh[4];
        uint32_t sid   = ((uint32_t)(fh[5] & 0x7F) << 24) | ((uint32_t)fh[6] << 16) |
                         ((uint32_t)fh[7] << 8) | fh[8];
        if (flen > H2_MAX_FRAME) goto fail;
        if (flen && io_read_full(io, c->frame, flen)) goto fail;
        const uint8_t *pl = c->frame;
        if (in_continuation && (type != F_CONTINUATION || sid != 1)) goto fail;

        switch (type) {
        case F_SETTINGS:
            if (sid != 0) goto fail;
            if (flags & FL_ACK) { if (flen) goto fail; break; }
            if (flen % 6) goto fail;
            // Nothing the server can set changes what we do: we never index
            // into its table, our whole request is already on the wire, and
            // the response fits the default windows.
            if (send_frame(io, F_SETTINGS, FL_ACK, 0, NULL, 0)) goto fail;
            break;
        case F_PING:
            if (sid != 0 || flen != 8) goto fail;
            if (!(flags & FL_ACK) && send_frame(io, F_PING, FL_ACK, 0, pl, 8)) goto fail;
            break;
        case F_WINDOW_UPDATE:
            if (flen != 4) goto fail;
            break;
        case F_PRIORITY:
            break;
        case F_GOAWAY:
        case F_PUSH_PROMISE:            // push is disabled: a violation
            goto fail;
        case F_RST_STREAM:
            if (sid == 1) goto fail;
            break;
        case F_HEADERS: {
            if (sid != 1) goto fail;
            size_t off = 0, end = flen;
            if (flags & FL_PADDED) {
                if (flen < 1) goto fail;
                size_t pad = pl[0];
                if (pad + 1 > flen) goto fail;
                off = 1; end = flen - pad;
            }
            if (flags & FL_PRIORITY) {
                if (end < off + 5) goto fail;
                off += 5;
            }
            if (end - off > sizeof c->hblock - hbl) goto fail;
            memcpy(c->hblock + hbl, pl + off, end - off);
            hbl += end - off;
            if (flags & FL_END_STREAM) hb_end_stream = 1;
            in_continuation = !(flags & FL_END_HEADERS);
            break;
        }
        case F_CONTINUATION: {
            if (!in_continuation || sid != 1) goto fail;
            if (flen > sizeof c->hblock - hbl) goto fail;
            memcpy(c->hblock + hbl, pl, flen);
            hbl += flen;
            in_continuation = !(flags & FL_END_HEADERS);
            break;
        }
        case F_DATA: {
            if (sid != 1 || !final_status) goto fail;
            size_t off = 0, end = flen;
            if (flags & FL_PADDED) {
                if (flen < 1) goto fail;
                size_t pad = pl[0];
                if (pad + 1 > flen) goto fail;
                off = 1; end = flen - pad;
            }
            size_t dl = end - off;
            if (dl > out_cap - *out_len) goto fail;
            memcpy(out + *out_len, pl + off, dl);
            *out_len += dl;
            if (flags & FL_END_STREAM) {
                stream_done = 1;
            } else if (flen) {
                // Keep both windows topped up; padding counts too.
                uint8_t wu[4] = { (uint8_t)((flen >> 24) & 0x7F), (uint8_t)(flen >> 16),
                                  (uint8_t)(flen >> 8), (uint8_t)flen };
                if (send_frame(io, F_WINDOW_UPDATE, 0, 0, wu, 4) ||
                    send_frame(io, F_WINDOW_UPDATE, 0, 1, wu, 4)) goto fail;
            }
            break;
        }
        default:
            break;                      // unknown frame types are ignored
        }

        // A complete header block: the response status, an interim 1xx to
        // skip, or trailers after the body.
        if ((type == F_HEADERS || type == F_CONTINUATION) && !in_continuation) {
            st.have = 0;
            if (Hpack_Decode(&c->dec, c->hblock, hbl, on_response_header, &st)) goto fail;
            hbl = 0;
            if (!final_status) {
                if (!st.have) goto fail;
                if (st.status >= 100 && st.status < 200) {
                    if (hb_end_stream) goto fail;     // 1xx cannot end the stream
                    continue;
                }
                final_status = st.status;
            }
            if (hb_end_stream) stream_done = 1;
        }
    }

    // Done: tell the server we are leaving (best effort) and report.
    {
        static const uint8_t bye[8] = { 0 };
        (void)send_frame(io, F_GOAWAY, 0, 0, bye, 8);
    }
    *out_status = final_status;
    free(c);
    return 0;

fail:
    *out_len = 0;
    *out_status = 0;
    free(c);
    return -1;
}
