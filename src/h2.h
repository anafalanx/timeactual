// h2.h -- minimal HTTP/2 client: one POST per connection (RFC 9113) with
// the HPACK codec (RFC 7541) a DoH exchange needs.
//
// Scope, deliberately narrow: the client opens stream 1, sends one request
// with a small body, reads one response and is done. Server push is
// refused, only one stream is ever open, flow-control windows are simply
// topped up as DATA arrives, and every protocol violation is a hard
// failure -- the caller treats it like a transport failure (dns.c then
// stays on HTTP/1.1 for that resolver). The transport sits behind two
// callbacks so the framing and HPACK paths are unit-testable without a
// socket.

#ifndef LUNAR_H2_H
#define LUNAR_H2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Transport callbacks. read: >0 bytes read (may be short), 0 = orderly
// close, <0 = error. write: >0 bytes written (may be short), <0 = error.
typedef int (*H2ReadFn)(void *ctx, uint8_t *buf, size_t len);
typedef int (*H2WriteFn)(void *ctx, const uint8_t *buf, size_t len);
typedef struct {
    H2ReadFn  read;
    H2WriteFn write;
    void     *ctx;
} H2Io;

// One POST on a fresh connection whose TLS layer negotiated ALPN "h2".
// Sends the connection preface, SETTINGS, the request HEADERS and DATA on
// stream 1, then collects the response. Returns 0 when a complete response
// arrived: *out_status is the HTTP status and out/out_len the body (a body
// larger than out_cap is a failure). Returns -1 on any transport or
// protocol failure; *out_status is then 0.
int H2_PostOnce(const H2Io *io, const char *authority, const char *path,
                const char *content_type,
                const uint8_t *body, size_t body_len,
                uint8_t *out, size_t out_cap, size_t *out_len,
                int *out_status);

// --- HPACK decoder (exposed for the unit tests) ----------------------------

#define HPACK_DYN_MAX      4096                  // SETTINGS_HEADER_TABLE_SIZE default
#define HPACK_DYN_ENTRIES  (HPACK_DYN_MAX / 32)  // every entry costs >= 32 octets
#define HPACK_STR_MAX      HPACK_DYN_MAX         // longest decoded name or value

typedef struct {
    uint8_t  arena[HPACK_DYN_MAX];   // entries back to back, oldest first
    struct { uint16_t off, nlen, vlen; } ent[HPACK_DYN_ENTRIES];
    int      count;
    size_t   used;                   // arena octets in use
    size_t   max;                    // current limit (<= HPACK_DYN_MAX)
    uint8_t  nbuf[HPACK_STR_MAX];    // scratch: decoded literal name
    uint8_t  vbuf[HPACK_STR_MAX];    // scratch: decoded literal value
} HpackDecoder;

typedef void (*HpackHeaderFn)(void *ctx,
                              const uint8_t *name, size_t name_len,
                              const uint8_t *value, size_t value_len);

void Hpack_Init(HpackDecoder *d);

// Decode one complete header block, calling fn per header field in order.
// Pointers handed to fn are valid only during the call. 0 ok, -1 malformed.
int  Hpack_Decode(HpackDecoder *d, const uint8_t *block, size_t len,
                  HpackHeaderFn fn, void *ctx);

// Primitive codecs (RFC 7541 5.1 / 5.2). Hpack_EncodeInt returns the bytes
// written, 0 when `cap` is too small.
int    Hpack_DecodeInt(const uint8_t *p, size_t len, int prefix_bits,
                       uint32_t *out, size_t *consumed);
size_t Hpack_EncodeInt(uint8_t *out, size_t cap, uint8_t first_byte_bits,
                       int prefix_bits, uint32_t value);
int    Hpack_HuffmanDecode(const uint8_t *in, size_t len,
                           uint8_t *out, size_t cap, size_t *out_len);

#ifdef LUNAR_TESTING
// The Huffman code of symbol `sym` (0..256, 256 = EOS): code and bit length.
void   Hpack_TestHuffmanEntry(int sym, uint32_t *code, int *len);
#endif

#ifdef __cplusplus
}
#endif

#endif
