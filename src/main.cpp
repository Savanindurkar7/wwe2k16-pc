// wwe2k16 - WWE 2K16 (Xbox 360) static recompilation with the ReXGlue SDK.

// ReXGlue 0.7 writes generated/wwe2k16_config.h + wwe2k16_init.h; 0.10 (WWE 2K14 SDK) writes only the init header, into
// generated_k14/ (an include folder of that build).
#if __has_include("generated/wwe2k16_config.h")
#include "generated/wwe2k16_config.h"
#include "generated/wwe2k16_init.h"
#else
#include "wwe2k16_init.h"
#endif

#include "wwe2k16_app.h"

REX_DEFINE_APP(wwe2k16, Wwe2k16App::Create)
