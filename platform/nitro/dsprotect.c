/* DS Protect 1.10 (ov028) on the port: every check reports a genuine cartridge.
 *
 * On the DS these six functions are encrypted in the ROM and decrypt themselves at run time
 * (decomp docs/ov028_encrypted_code.md), so the decomp keeps them as data. They come in pairs
 * whose answers must agree, "Detect X" and "Detect not X", so a single patched branch would
 * betray a tampered game. In address order (taxicat1/dsprot, branch 1.10):
 *
 *   0208b040 DSProt_DetectFlashcart     0208b120 DSProt_DetectNotFlashcart
 *   0208b200 DSProt_DetectEmulator      0208b2e0 DSProt_DetectNotEmulator
 *   0208b3c0 DSProt_DetectDummy         0208b490 DSProt_DetectNotDummy
 *
 * The callback is what the library would run later on a positive detection; a genuine card
 * never triggers it. */

typedef void (*DSProtCallback)(void *arg);

int func_ov028_0208b040(DSProtCallback cb) { (void)cb; return 0; }
int func_ov028_0208b120(DSProtCallback cb) { (void)cb; return 1; }
int func_ov028_0208b200(DSProtCallback cb) { (void)cb; return 0; }
int func_ov028_0208b2e0(DSProtCallback cb) { (void)cb; return 1; }
int func_ov028_0208b3c0(DSProtCallback cb) { (void)cb; return 0; }
int func_ov028_0208b490(DSProtCallback cb) { (void)cb; return 1; }
