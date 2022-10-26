#include "ir.h"

#define DASM_M_GROW(ctx, t, p, sz, need) \
  do { \
    size_t _sz = (sz), _need = (need); \
    if (_sz < _need) { \
      if (_sz < 16) _sz = 16; \
      while (_sz < _need) _sz += _sz; \
      (p) = (t *)ir_mem_realloc((p), _sz); \
      (sz) = _sz; \
    } \
  } while(0)

#define DASM_M_FREE(ctx, p, sz) ir_mem_free(p)

#if IR_DEBUG
# define DASM_CHECKS
#endif

#if defined(__GNUC__)
# pragma GCC diagnostic ignored "-Warray-bounds"
#endif

#if defined(IR_TARGET_X86) || defined(IR_TARGET_X64)
# include "ir_x86.h"
# include "ir_private.h"
# include "dynasm/dasm_proto.h"
# include "dynasm/dasm_x86.h"
# include "ir_emit_x86.h"
#elif defined(IR_TARGET_AARCH64)
# include "ir_aarch64.h"
# include "ir_private.h"
# include "dynasm/dasm_proto.h"
# include "dynasm/dasm_arm64.h"
# include "ir_emit_aarch64.h"
#else
# error "Unknown IR target"
#endif
