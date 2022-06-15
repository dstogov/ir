#ifndef IR_PHP_H
#define IR_PHP_H

#define IR_PHP_OPS(_) \
	_(PHP_ENTRY,    S0X2, num, ent, ___) /* PHP Code Entry              */ \

#if defined(IR_TARGET_X86)
# define IR_REG_PHP_FP IR_REG_RSI
# define IR_REG_PHP_IP IR_REG_RDI
#elif defined(IR_TARGET_X64)
# define IR_REG_PHP_FP IR_REG_R14
# define IR_REG_PHP_IP IR_REG_R15
#elif defined(IR_TARGET_AARCH64)
# define IR_REG_PHP_FP IR_REG_X27
# define IR_REG_PHP_IP IR_REG_X28
#else
# error "Unknown IR target"
#endif

#define IR_REGSET_PHP_FIXED \
	(IR_REGSET(IR_REG_PHP_FP) | IR_REGSET(IR_REG_PHP_IP))

#endif /* IR_PHP_H */
