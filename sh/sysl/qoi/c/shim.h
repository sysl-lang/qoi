/* What `shim.c` answers, and what the sysl side matches on.
 *
 * These three are in a header rather than in `shim.c` for one reason: the
 * `c const` block in `c.sysl` is measured from a probe translation unit of the
 * compiler's own making (`15 §7`), which includes what `@include` names. A number
 * defined only in the `.c` is a number the probe cannot see, and the sysl side
 * would be back to writing `0`, `1` and `2` down -- which is exactly the
 * agreement-in-two-places this arrangement exists to remove.
 *
 * They are the shim's own numbers rather than QOI's, and that is the point: QOI
 * answers one null pointer to a buffer that was too small and to input it will not
 * encode, and says no more about it. Only the allocator knows which happened, so
 * the shim is what can tell them apart and these are how it says so.
 */
#ifndef SYSL_QOI_SHIM_H
#define SYSL_QOI_SHIM_H

#define SYSL_QOI_OK      0
#define SYSL_QOI_SHORT   1
#define SYSL_QOI_INVALID 2

#endif
