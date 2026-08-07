# qoi

Lossless image compression for [sysl](https://github.com/sysl-lang/sysl), bound to
[QOI](https://qoiformat.org) — the Quite OK Image format.

QOI is PNG-comparable compression in six hundred lines of C: an order of magnitude faster to encode,
several times faster to decode, and about as small. The whole format is four operations over a
rolling sixty-four-entry colour index, which is why the reference implementation fits in one header
and why carrying it here costs a consumer nothing.

**This package asks the machine for nothing.** There is no `requires` block — no allocator, no
operating system, no file system. For a library whose entire C API hands back `malloc`'d storage that
takes some doing, and how it is done is the interesting part of this repository.

```
sh/sysl/qoi/
    qoi.sysl            the binding
    qoi.h               vendored QOI, unmodified — phoboslab/qoi, MIT
    shim.c              the arena, and the C that only C can reach
    tests.sysl          10 tests, most of them about the arena rather than the codec
package.hocon           who this package is, and what it needs of the machine
```

The module is **`sh.sysl.qoi`**, and the three directories are that name: a dotted module name
mirrors its path from the library root. The prefix is the reverse-DNS of `sysl.sh`, so that a package
claims a name nobody else will mint rather than the top-level word `qoi`.

## Using it

Name it in your project's `package.hocon` and `sysl build` fetches it:

```hocon
dependencies {
  qoi { git = "github.com/sysl-lang/qoi", version = "0.1.0" }
}
```

The coordinate is an identity rather than a URL, so it carries no `https://`, and `version` is the
tag `v0.1.0` here.

Or point at it directly, which needs no fetching and is what this repository's own tests do. Either a
built artifact or the source tree works, and they are the same road:

```
sysl build-lib . -o /tmp/qoi.syslib
sysl run yourprogram.sysl --lib /tmp/qoi.syslib

sysl run yourprogram.sysl --lib /path/to/this/repo
```

## The surface

```sysl
enum Colorspace
    Srgb                // gamma-scaled colour, linear alpha — almost every image
    Linear              // every channel linear

struct Header
    width: u32
    height: u32
    channels: u8        // 3 for RGB, 4 for RGBA
    colorspace: Colorspace

    pixel_bytes(self) -> usize
    pixel_bytes_as(self, channels: u8) -> usize
    max_encoded(self) -> usize

enum Error
    Short(needed: usize)        // the output slice is too small
    Truncated(needed: usize)    // `encode` only: fewer pixels than the header describes
    Invalid                     // not an image, or not one QOI encodes

header(bytes: []const u8) -> Option[Header]

encode(pixels: []const u8, h: Header, out: []u8) -> Result[usize, Error]
decode(bytes: []const u8, out: []u8) -> Result[usize, Error]
decode_as(bytes: []const u8, channels: u8, out: []u8) -> Result[usize, Error]
```

**Every call is the same shape: ask how big the answer can be, hand over a slice that size, and get
back how much of it was written.** Nothing here allocates, so nothing here has to be freed.

`pixels` is `width * height * channels` bytes, row-major, with no padding between rows.
`max_encoded()` is a ceiling and usually a generous one — an image that compresses at all needs far
less, and `encode` answers how much it really used. A smaller slice is worth trying: `Short` says
exactly what it would have taken, so a caller that guessed low can resize once rather than doubling
until it fits.

`decode_as` forces three channels or four whatever the file holds — filling alpha with 255 or
dropping it — which is worth having because a caller usually wants one layout for everything it has
loaded rather than a branch per image.

**Reading a file is `sysl.fs`'s job.** `qoi_write` and `qoi_read` are compiled out; the bytes
`read_bytes` hands back are what `decode` wants anyway, and leaving them out is half of why this
package needs no `posix`.

## An example

A package has no way to carry a program of its own — everything under the package root is compiled
*into* the library — so the demo lives here (`design/packages.md § Open h`):

```sysl
import sh.sysl.qoi.*
import sysl.slices.equal

// A 4x4 image in RGB: alternating rows of two colours, so forty-eight bytes of pixels.
val image = Header(4, 4, 3, Srgb)

var pixels: []u8 = [0; 48]

for i in 0..<pixels.len / 3
    val dark = i / 4 % 2 == 0
    val at = i * 3

    pixels[at] = if dark then 30 else 220
    pixels[at + 1] = if dark then 60 else 180
    pixels[at + 2] = if dark then 90 else 140

// The bound is what a caller has to be ready for; the length is what it actually took.
var out: []u8 = [0; 96]

val n = encode(pixels, image, out).expect("a 4x4 image encodes")

print(f"${pixels.len} bytes of pixels became ${n}, in a slice sized for ${image.max_encoded()}")

// Reading an encoded image back begins with its header, because that is what says how much room the
// pixels will need.
val read = header(out[0..<n]).expect("what was just written is a QOI image")

print(f"${read.width}x${read.height}, ${read.channels} channels")

var back: []u8 = [0; 48]

val p = decode(out[0..<n], back).expect("and it decodes")

print(f"${p} bytes of pixels came back, identical: ${equal(back, pixels)}")

// A slice too small is refused rather than overrun, and says what would have been enough.
var tiny: []u8 = [0; 16]

encode(pixels, image, tiny) match
    Ok(_) -> print("sixteen bytes was enough, which it should not have been")
    Err(Short(needed)) -> print(f"sixteen bytes is not enough; ${needed} would always be")
    Err(_) -> print("refused for some other reason")
```

```
48 bytes of pixels became 36, in a slice sized for 86
4x4, 3 channels
48 bytes of pixels came back, identical: true
sixteen bytes is not enough; 86 would always be
```

## How a library that allocates gets bound to a language with no `free`

This is the part the package exists to demonstrate.

**The problem.** `qoi_encode`, `qoi_decode` and `qoi_read` all `malloc` their result and hand the
caller a pointer to release. No other binding in this organisation has that shape, and sysl cannot
express it: there is no way to adopt a raw pointer as owned storage, and no `free` to release one.
Transcribed straight across, the ownership would have had nowhere to live. A `Qoi` struct holding the
pointer and carrying a `free` method — which is what
[regex](https://github.com/sysl-lang/regex) does with `regex_t` — would have worked, and would have
made every program using this package one that allocates.

**The escape is that QOI lets you replace its allocator.** `QOI_MALLOC` and `QOI_FREE` are macros,
and `qoi_encode` works out how much it needs before it asks:

```c
max_size = desc->width * desc->height * (desc->channels + 1) +
           QOI_HEADER_SIZE + sizeof(qoi_padding);
```

That is a number the *caller* can work out too. So `shim.c` points the allocator at a buffer the
caller already owns, and answers the one allocation each call makes by handing that buffer straight
back:

```c
static void *sysl_qoi_alloc(size_t sz) {
    if (!g_active || g_taken) return NULL;

    if (sz > g_cap) {
        g_short = sz;
        return NULL;
    }

    g_taken = 1;

    return g_buf;
}
```

Ownership transfer becomes caller-supplies-the-buffer, which is the pattern the rest of these
packages already use. There is no heap underneath this package at all — a stronger statement than
`alloc = false` would have been, and the reason `package.hocon` has no `requires` block rather than
an empty one.

**`g_short` is what makes the refusals honest.** QOI answers a null pointer to a buffer that was too
small and to input it would not encode, and says nothing about which. The allocator knows: if it was
asked for more than it had, it records the size, and the caller is told `Short(86)` rather than
merely that something went wrong. That number is what QOI actually asked for, not an estimate.

**The rest is the ordinary business of a shim** (`15 §7`) — `qoi_desc` is a layout only the header
knows, so its four fields cross as four scalars; `QOI_MAGIC`, `QOI_HEADER_SIZE` and `QOI_PIXELS_MAX`
are `#define`s with no symbols to link against, so everything that needs them is computed on the C
side rather than having its value transcribed into sysl.

**The one check that is the binding's own** is `Truncated`. QOI is handed a pointer and the
dimensions and never a length, so a `pixels` slice shorter than the header describes would be read
past the end and nothing in C would notice. `encode` compares the two in sysl before the call.

### Thread safety

The arena is file-scope state, so two threads encoding at once would share one buffer.
`_Thread_local` gives each its own where the compiler is C11 or later, which is every toolchain sysl
builds with today. A freestanding target whose runtime has no thread-local storage can be built with
`SYSL_QOI_NO_TLS` defined, and then the constraint is the honest one: **one encode or decode at a
time.**

### What is still hosted about it

`qoi.h` includes `<stdlib.h>` and `<string.h>`, which a freestanding toolchain may not carry even
though nothing in the compiled result calls `malloc` or `fopen`. `requires {}` is a claim about what
this package asks of *sysl* — no allocator, no operating system — and that claim is exact. Whether
those two headers exist is the C toolchain's business.

## Testing

```
sysl test .
```

**Most of the tests are about the arena rather than about QOI.** The codec is exercised upstream
against a corpus of real images, and repeating that here would test the vendored file rather than the
binding. What is untested anywhere else is the decision this package rests on, and two cases carry
most of the weight:

- **a slice too small is refused** — the only direct evidence that nothing reached the heap. A shim
  that quietly fell back to `malloc` passes a round trip and fails this;
- **encoding twice works** — a latch left set after the first allocation passes everything else and
  fails this, which is what a program encoding more than one image would meet immediately.

The pixels are picked so that only real QOI produces the answer: four pixels of which the third
repeats the first, so the encoding runs through the colour index rather than emitting literals, and
channel values that are all distinct and none of them 0 or 255, so a dropped or swapped channel is
visible rather than hidden by symmetry.

## Licence

Two things under two licences — `qoi.h` is MIT and upstream's, everything else is ISC and this
repository's. See [LICENSE](LICENSE).
