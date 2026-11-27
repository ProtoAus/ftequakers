#!/usr/bin/env python3
"""Exercise Schannel's stream write path with a deterministic short-write sink.

The framing shim stands in for EncryptMessage; the buffering and write path
are the production functions. This does not test the Windows TLS provider.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[1] / "engine/common/net_ssl_winsspi.c"

SHIM = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int qboolean, neterr_t, SECURITY_STATUS;
typedef unsigned long ULONG;
enum {false, true};
enum {NETERR_SENT, NETERR_CLOGGED, NETERR_DISCONNECTED};
enum {VFS_ERROR_TRYLATER = 0, VFS_ERROR_EOF = -4};
enum {SECBUFFER_VERSION, SECBUFFER_STREAM_HEADER, SECBUFFER_DATA,
      SECBUFFER_STREAM_TRAILER, SECBUFFER_EMPTY, SEC_E_ENCRYPT_FAILURE = -1};
#define QDECL
#define VFS_WRITE(f, p, n) sink_write(f, p, n)
#define Z_ReallocElements(p, len, count, size) do { \
    *(p) = realloc(*(p), (count) * (size)); assert(*(p)); *(len) = (count); \
} while (0)

typedef struct { int BufferType; unsigned cbBuffer; void *pvBuffer; } SecBuffer;
typedef struct { int ulVersion, cBuffers; SecBuffer *pBuffers; } SecBufferDesc;
struct sslbuf { size_t datasize; char *data; size_t avail; };
enum { CHUNK = 8192, BODY = 2 * CHUNK + 17, FRAMING = 16 };
typedef struct vfsfile_s {
    unsigned char bytes[BODY + 3 * FRAMING];
    size_t len, budget;
    int closed;
} vfsfile_t;
typedef struct {
    vfsfile_t funcs, *stream;
    struct sslbuf outraw, outcrypt;
    int handshaking, datagram, sechnd;
    char headerdata[FRAMING / 2], footerdata[FRAMING / 2];
    int headersize, footersize;
} sslfile_t;

static int sink_write(vfsfile_t *sink, const void *data, size_t len)
{
    if (sink->closed) return VFS_ERROR_EOF;
    if (len > sink->budget) len = sink->budget;
    assert(sink->len + len <= sizeof(sink->bytes));
    memcpy(sink->bytes + sink->len, data, len);
    sink->len += len;
    sink->budget -= len;
    return (int)len;
}

static SECURITY_STATUS frame(int *ctx, ULONG qop, SecBufferDesc *d, int seq)
{
    (void)ctx; (void)qop; (void)seq;
    assert(d->cBuffers == 4 && d->pBuffers[1].cbBuffer <= CHUNK);
    memset(d->pBuffers[0].pvBuffer, 0xC1, d->pBuffers[0].cbBuffer);
    memset(d->pBuffers[2].pvBuffer, 0xC2, d->pBuffers[2].cbBuffer);
    return 0;
}
static const struct { SECURITY_STATUS (*pEncryptMessage)(int *, ULONG, SecBufferDesc *, int); }
    secur = { frame };
static void SSPI_Error(sslfile_t *f, const char *why, ...) { (void)f; (void)why; abort(); }
static void SSPI_Handshake(sslfile_t *f) { (void)f; abort(); }
static void SSPI_CheckNewInCrypt(sslfile_t *f) { (void)f; abort(); }
"""

TEST = r"""
static void send_body(int backpressure)
{
    unsigned char body[BODY], expected[sizeof(((vfsfile_t *)0)->bytes)];
    vfsfile_t sink = {0};
    sslfile_t f = {0};
    size_t sent = 0, expected_len = 0;
    f.stream = &sink;
    f.headersize = sizeof(f.headerdata);
    f.footersize = sizeof(f.footerdata);
    SSPI_ExpandBuffer(&f.outraw, CHUNK);
    SSPI_ExpandBuffer(&f.outcrypt, CHUNK);
    for (size_t i = 0; i < BODY; i++) body[i] = (unsigned char)(i ^ (i >> 8));
    for (size_t i = 0; i < BODY; i += CHUNK)
    {
        size_t n = BODY - i < CHUNK ? BODY - i : CHUNK;
        memset(expected + expected_len, 0xC1, f.headersize);
        memcpy(expected + expected_len + f.headersize, body + i, n);
        memset(expected + expected_len + f.headersize + n, 0xC2, f.footersize);
        expected_len += f.headersize + n + f.footersize;
    }

    if (backpressure)
    {
        sink.budget = 1;
        assert(SSPI_WriteBytes(&f.funcs, body, BODY) == CHUNK);
        sent = CHUNK;
        assert(f.outcrypt.avail > CHUNK && sink.len == 1);
        assert(SSPI_WriteBytes(&f.funcs, body + sent, BODY - sent) == 0);
        assert(sink.len == 1 && f.outraw.avail == 0);
        sink.budget = 1;
        assert(SSPI_WriteBytes(&f.funcs, body + sent, BODY - sent) == 0);
        assert(sink.len == 2 && f.outcrypt.avail > CHUNK && f.outraw.avail == 0);
    }

    // The HTTP writer will not call ReadBytes until its whole request is accepted.
    sink.budget = sizeof(sink.bytes) - sink.len;
    for (int poll = 0; poll < 8 && sent < BODY; poll++)
    {
        int n = SSPI_WriteBytes(&f.funcs, body + sent, BODY - sent);
        assert(n >= 0);
        sent += n;
    }
    assert(sent == BODY && f.outcrypt.avail == 0 && f.outraw.avail == 0);
    assert(sink.len == expected_len && memcmp(sink.bytes, expected, expected_len) == 0);
    free(f.outraw.data);
    free(f.outcrypt.data);
    puts(backpressure ? "short-write upload: exact body" : "uninterrupted upload: exact body");
    fflush(stdout);
}

static void disconnect(int budget)
{
    unsigned char body[2 * CHUNK] = {0};
    vfsfile_t sink = {0};
    sslfile_t f = {.stream = &sink};
    f.headersize = sizeof(f.headerdata);
    f.footersize = sizeof(f.footerdata);
    SSPI_ExpandBuffer(&f.outraw, CHUNK);
    SSPI_ExpandBuffer(&f.outcrypt, CHUNK);
    if (budget >= 0)
    {
        sink.budget = (size_t)budget;
        assert(SSPI_WriteBytes(&f.funcs, body, sizeof(body)) == CHUNK);
        assert(f.outcrypt.avail == CHUNK + FRAMING - budget);
    }
    sink.closed = 1;
    assert(SSPI_WriteBytes(&f.funcs, body + CHUNK, CHUNK) == VFS_ERROR_EOF);
    free(f.outraw.data);
    free(f.outcrypt.data);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    switch (argv[1][0])
    {
    case '0': send_body(0); break;
    case '1': send_body(1); break;
    case '2': disconnect(1); break;       // queued ciphertext exceeds the high-water mark
    case '3': disconnect(FRAMING); break; // queued ciphertext is at the high-water mark
    case '4': disconnect(-1); break;     // failure sending the first record
    default: abort();
    }
    return 0;
}
"""


def main():
    text = (Path(sys.argv[1]) if len(sys.argv) == 2 else SOURCE).read_text()
    production = []
    for signature in (
        "static int SSPI_ExpandBuffer(", "static int SSPI_CopyIntoBuffer(",
        "static neterr_t SSPI_TryFlushCryptOut(", "static neterr_t SSPI_Encode(",
        "static int QDECL SSPI_WriteBytes (",
    ):
        start = text.index(signature)
        production.append(text[start:text.index("\n}", start) + 2])

    with tempfile.TemporaryDirectory(prefix="sspi-") as tmp:
        test = Path(tmp) / "sspi.c"
        binary = Path(tmp) / "sspi"
        test.write_text("\n".join([SHIM, *production, TEST]))
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run([*cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-Wno-sign-compare", "-Wno-implicit-fallthrough", str(test), "-o", str(binary)], check=True)
        failed = False
        for i, label in enumerate(("normal sink", "short-write recovery", "closed while throttled",
                                   "closed at the high-water mark", "closed before the first record")):
            result = subprocess.run([str(binary), str(i)])
            print(f"{label}: {'FAIL' if result.returncode else 'PASS'}", flush=True)
            failed |= bool(result.returncode)
        return failed


if __name__ == "__main__":
    sys.exit(main())
