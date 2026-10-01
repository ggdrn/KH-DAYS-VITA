/* Compile-time checks that the decomp's structs keep their DS layout under GCC (built with the
 * game's flags and headers). Code compiled from the ROM's functions reaches these fields at
 * fixed offsets, so a struct that grows on the Vita breaks it silently.
 *
 * OSThread: 0.0.12 hung at the first thread switch because CPContext's u64 fields were 8-byte
 * aligned (mwcc: 4), which moved OSThread.state from +0x64 to +0x68. The SDK's own threads are
 * 0xc0 apart in the ROM (OSi_IdleThread, OSi_LauncherThread, OSi_IdleThreadStack). */
#include "nitro/types.h"
#include "nitro/cp.h"
#include "nitro/hw.h"
#include "../../build/decomp/libs/mobiclip/video/portable/mobiclip_frame_core.h"
#include "nitro/os.h"

#include <stddef.h>

_Static_assert(sizeof(u64) == 8 && _Alignof(u64) == 4, "u64: 4-byte aligned as with mwcc");
_Static_assert(sizeof(CPContext) == 0x1c, "CPContext");
_Static_assert(sizeof(OSContext) == 0x64, "OSContext");
_Static_assert(offsetof(OSThread, state) == 0x64, "OSThread.state");
_Static_assert(offsetof(OSThread, queue) == 0x78, "OSThread.queue");
_Static_assert(sizeof(OSThread) == 0xc0, "OSThread");

/* Register fields stay plain numbers: tools/rewrite_decomp.py once wrapped this mask in KH_HW
 * because its value is the GBA slot's base, and the geometry engine read as busy forever
 * (0.0.19 hung in G3X_ResetMtxStack on the first frame). */
_Static_assert(REG_G3X_GXSTAT_GE_MASK == 0x08000000, "REG_G3X_GXSTAT_GE_MASK");

/* The portable MobiClip decoder works on the state ov024 keeps for the ARM payload it replaces:
 * the ARM9's layout (the same asserts as the decomp's tools/tests/mobiclip_frame_core_test.cpp). */
_Static_assert(sizeof(MobiClipDecoderState) == 0x454, "MobiClipDecoderState");
_Static_assert(offsetof(MobiClipDecoderState, apLuma) == 0x0c, "apLuma");
_Static_assert(offsetof(MobiClipDecoderState, apChroma) == 0x24, "apChroma");
_Static_assert(offsetof(MobiClipDecoderState, apCoefficientTables) == 0x3c, "apCoefficientTables");
_Static_assert(offsetof(MobiClipDecoderState, bFormatVariant) == 0x48, "bFormatVariant");
_Static_assert(offsetof(MobiClipDecoderState, aQuantScan8x8) == 0x74, "aQuantScan8x8");
_Static_assert(offsetof(MobiClipDecoderState, aQuantScan4x4) == 0x174, "aQuantScan4x4");
_Static_assert(offsetof(MobiClipDecoderState, nQuantizer) == 0x3b4, "nQuantizer");
_Static_assert(offsetof(MobiClipDecoderState, aMotion) == 0x3c4, "aMotion");
