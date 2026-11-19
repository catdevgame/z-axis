#pragma once
// Declarations only (README.txt). sse2neon.h (ARM) names the rounding modes in its _MM_SET_ROUNDING_MODE; these are
// the AArch64 FPCR.RMode values of macOS's, glibc's and bionic's fenv.h.
#ifdef __cplusplus
extern "C" {
#endif
#define FE_TONEAREST 0x00000000
#define FE_UPWARD 0x00400000
#define FE_DOWNWARD 0x00800000
#define FE_TOWARDZERO 0x00C00000
int fegetround(void);
int fesetround(int mode);
#ifdef __cplusplus
}
#endif
