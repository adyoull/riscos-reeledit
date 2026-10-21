/* ro.h - the RISC OS calls Reel Edit's front end makes: SWI numbers and a
 * small helper. Everything goes through _kernel_swi (UnixLib's kernel.h),
 * so the Linux tests can stand in for the Wimp with a fake _kernel_swi.
 *
 * Part of riscos-reeledit. GPL version 2 or later.
 */
#ifndef RE_RO_H
#define RE_RO_H
#include <kernel.h>
#include <stdint.h>

#define OS_Byte                0x06
#define OS_SpriteOp            0x2E
#define OS_ReadModeVariable    0x35
#define OS_SWINumberFromString 0x39
#define OS_ReadMonotonicTime   0x42
#define OS_WriteN              0x46
#define OS_ScreenMode          0x65
#define Wimp_Initialise        0x400C0
#define Wimp_CreateWindow      0x400C1
#define Wimp_CreateIcon        0x400C2
#define Wimp_DeleteWindow      0x400C3
#define Wimp_OpenWindow        0x400C5
#define Wimp_CloseWindow       0x400C6
#define Wimp_Poll              0x400C7
#define Wimp_RedrawWindow      0x400C8
#define Wimp_UpdateWindow      0x400C9
#define Wimp_GetRectangle      0x400CA
#define Wimp_GetWindowState    0x400CB
#define Wimp_SetIconState      0x400CD
#define Wimp_GetPointerInfo    0x400CF
#define Wimp_ForceRedraw       0x400D1
#define Wimp_SetCaretPosition  0x400D2
#define Wimp_CreateMenu        0x400D4
#define Wimp_SetExtent         0x400D7
#define Wimp_ProcessKey        0x400DC
#define Wimp_CloseDown         0x400DD
#define Wimp_ReportError       0x400DF
#define Wimp_PollIdle          0x400E1
#define Wimp_SendMessage       0x400E7
#define Wimp_ResizeIcon        0x400FC

static inline _kernel_oserror *ro_swi(int n, _kernel_swi_regs *r)
{
    return _kernel_swi(n, r, r);
}

static inline unsigned ro_now_cs(void)
{
    _kernel_swi_regs r;
    return ro_swi(OS_ReadMonotonicTime, &r) ? 0 : (unsigned) r.r[0];
}

typedef struct
{
    int x0, y0, x1, y1;
} ro_box;

#endif
