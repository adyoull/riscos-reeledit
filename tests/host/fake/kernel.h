/* kernel.h for the Linux tests: as UnixLib's, but the registers are as wide
   as a pointer (on RISC OS they're the same thing), so the code that passes
   pointers in registers runs unchanged on a 64-bit machine. The test
   supplies _kernel_swi: a fake Wimp and the modules ReelEdit calls.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef FAKE_KERNEL_H
#define FAKE_KERNEL_H
#include <stdint.h>
typedef struct { intptr_t r[10]; } _kernel_swi_regs;
typedef struct { int errnum; char errmess[252]; } _kernel_oserror;
_kernel_oserror *_kernel_swi(int no, _kernel_swi_regs *in, _kernel_swi_regs *out);
#endif
