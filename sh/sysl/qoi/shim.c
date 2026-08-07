/* The C a QOI binding cannot be written without, and almost all of it is about
 * one question: who owns the memory.
 *
 * `qoi_encode` and `qoi_decode` both `malloc` their result and hand the caller a
 * pointer to free. No other binding in this organisation has that shape, and
 * sysl has neither a way to adopt a raw pointer as owned storage nor a `free` to
 * release one -- so a transcription of that API would have had nowhere to put
 * the ownership.
 *
 * The escape is that QOI lets you replace its allocator. `QOI_MALLOC` and
 * `QOI_FREE` are macros, and `qoi_encode` works out how much it needs up front
 * -- a number the caller can work out too. So this file points the allocator at
 * a buffer the caller already owns, and the one allocation each call makes is
 * satisfied by handing that buffer back. Ownership transfer becomes
 * caller-supplies-the-buffer, which is the pattern every other package here
 * uses, and the package needs no `alloc` capability at all.
 *
 * The rest of the file is the ordinary business of a shim (`15 §7`):
 *
 *   - **`qoi_desc` is a struct passed by pointer, and a layout only the header
 *     knows.** Its four fields cross as four scalars instead.
 *   - **`QOI_HEADER_SIZE`, `QOI_MAGIC` and `QOI_PIXELS_MAX` are `#define`s.** A
 *     macro has no symbol, so there is nothing for a linker to resolve and
 *     nothing for `extern` to name. Everything below uses the header's own
 *     macros rather than transcribing their values, which is the whole reason
 *     this file is compiled together with the implementation.
 *   - **`qoi_encode` reports its length through an `int *`.** That comes back as
 *     a `size_t` out-param, alongside a status the sysl side turns into a
 *     `Result`.
 */

#include <limits.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * The arena.
 *
 * One buffer, handed out once per call and never freed. `g_active` says a call
 * of ours is in flight, which is what stops a stray allocation from anywhere
 * else being answered with the caller's pixels; `g_taken` refuses a second
 * allocation within one call, since the design rests on there being exactly one
 * and a silent second would alias the first.
 *
 * `g_short` is how a buffer that is too small stays distinguishable from input
 * that QOI refused: both come back from QOI as a null pointer, and only the
 * allocator knows which happened. It records the size that was asked for, so the
 * caller can be told what would have been enough rather than merely that it was
 * not.
 *
 * **Thread safety.** These are file-scope, so two threads encoding at once would
 * share one arena. `_Thread_local` gives each its own where the compiler is C11
 * or later, which is every toolchain sysl builds with today. A freestanding
 * target whose runtime has no thread-local storage can define `SYSL_QOI_NO_TLS`,
 * and then the constraint is the honest one: one encode or decode at a time.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(SYSL_QOI_NO_TLS)
#define SYSL_QOI_LOCAL _Thread_local
#else
#define SYSL_QOI_LOCAL
#endif

static SYSL_QOI_LOCAL unsigned char *g_buf;
static SYSL_QOI_LOCAL size_t g_cap;
static SYSL_QOI_LOCAL int g_active;
static SYSL_QOI_LOCAL int g_taken;
static SYSL_QOI_LOCAL size_t g_short;

static void *sysl_qoi_alloc(size_t sz) {
    if (!g_active || g_taken) return NULL;

    if (sz > g_cap) {
        g_short = sz;
        return NULL;
    }

    g_taken = 1;

    return g_buf;
}

#define QOI_MALLOC(sz) sysl_qoi_alloc((size_t)(sz))
#define QOI_FREE(p)    ((void)(p))

/* No `fopen`, and this is what keeps the package free of `posix`. `qoi_write`
 * and `qoi_read` are the two functions that touch a file system; leaving them
 * out costs nothing, because reading a file is `sysl.fs`'s job and the bytes it
 * hands back are what `decode` wants anyway.
 */
#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#include "qoi.h"

/* What the sysl side matches on. */
#define SYSL_QOI_OK      0
#define SYSL_QOI_SHORT   1
#define SYSL_QOI_INVALID 2

static void arena_open(unsigned char *buf, size_t cap) {
    g_buf = buf;
    g_cap = cap;
    g_active = 1;
    g_taken = 0;
    g_short = 0;
}

/* `g_short` deliberately survives this: it is read after the call returns. */
static void arena_close(void) {
    g_buf = NULL;
    g_cap = 0;
    g_active = 0;
    g_taken = 0;
}

/* The size of the largest encoding an image of these dimensions can have --
 * `qoi_encode`'s own `max_size`, computed here from the same macros rather than
 * copied into the caller's language. A buffer this big is one QOI cannot
 * outgrow, which is what makes a single non-growing arena enough.
 */
size_t sysl_qoi_max_encoded(unsigned int w, unsigned int h, unsigned char channels) {
    return (size_t)w * (size_t)h * (size_t)(channels + 1) +
           QOI_HEADER_SIZE + sizeof(qoi_padding);
}

/* The fourteen header bytes, read here rather than by letting a decode that then
 * fails leave its findings behind in a `qoi_desc`. `qoi_decode` does fill the
 * struct before it validates, but that is an implementation detail and a caller
 * that relied on it would be reading a struct the function has told it not to
 * trust.
 *
 * The checks are `qoi_decode`'s own, in its order, so a byte string this accepts
 * is one a decode will not refuse for a reason the header could have shown.
 * `qoi_read_32` is the file's own reader, which is what removes the last place a
 * big-endian field could have been transcribed wrongly.
 *
 * Answers 1 and fills the four out-params, or answers 0 and touches nothing.
 */
int sysl_qoi_header(const unsigned char *data, size_t size,
                    unsigned int *w, unsigned int *h,
                    unsigned char *ch, unsigned char *cs) {
    int p = 0;
    unsigned int magic, width, height;
    unsigned char channels, colorspace;

    if (data == NULL || size < (size_t)(QOI_HEADER_SIZE + sizeof(qoi_padding))) return 0;

    magic = qoi_read_32(data, &p);
    width = qoi_read_32(data, &p);
    height = qoi_read_32(data, &p);
    channels = data[p++];
    colorspace = data[p++];

    if (magic != QOI_MAGIC ||
        width == 0 || height == 0 ||
        channels < 3 || channels > 4 ||
        colorspace > 1 ||
        height >= QOI_PIXELS_MAX / width) {
        return 0;
    }

    *w = width;
    *h = height;
    *ch = channels;
    *cs = colorspace;

    return 1;
}

/* Encode into `out`, which the caller owns and which QOI's allocator is pointed
 * at for the duration of the call.
 *
 * On `SYSL_QOI_OK` the length written lands in `*len`; on `SYSL_QOI_SHORT` the
 * length that *would* have been enough lands there instead, which is the one
 * number a caller who guessed too small actually wants.
 */
int sysl_qoi_encode(const unsigned char *pixels, unsigned int w, unsigned int h,
                    unsigned char channels, unsigned char colorspace,
                    unsigned char *out, size_t cap, size_t *len) {
    qoi_desc desc;
    int n = 0;
    void *p;

    desc.width = w;
    desc.height = h;
    desc.channels = channels;
    desc.colorspace = colorspace;

    arena_open(out, cap);
    p = qoi_encode(pixels, &desc, &n);
    arena_close();

    if (p != NULL) {
        *len = (size_t)n;
        return SYSL_QOI_OK;
    }

    if (g_short != 0) {
        *len = g_short;
        g_short = 0;
        return SYSL_QOI_SHORT;
    }

    return SYSL_QOI_INVALID;
}

/* Decode into `out`, the same way round.
 *
 * `channels` is 0 for whatever the file holds, or 3 or 4 to force it. QOI takes
 * the input length as an `int`, so anything larger than one can hold is refused
 * here rather than truncated into a length that would read past the end.
 */
int sysl_qoi_decode(const unsigned char *data, size_t size, int channels,
                    unsigned char *out, size_t cap, size_t *len) {
    qoi_desc desc;
    void *p;

    if (size > (size_t)INT_MAX) return SYSL_QOI_INVALID;

    arena_open(out, cap);
    p = qoi_decode(data, (int)size, &desc, channels);
    arena_close();

    if (p != NULL) {
        unsigned int got = channels == 0 ? (unsigned int)desc.channels : (unsigned int)channels;

        *len = (size_t)desc.width * (size_t)desc.height * (size_t)got;
        return SYSL_QOI_OK;
    }

    if (g_short != 0) {
        *len = g_short;
        g_short = 0;
        return SYSL_QOI_SHORT;
    }

    return SYSL_QOI_INVALID;
}
