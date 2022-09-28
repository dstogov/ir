#ifndef IR_PHP_H
#define IR_PHP_H

#define IR_PHP_OPS(_)

#ifndef IR_PHP_MM
# define IR_PHP_MM 1
#endif

#if IR_PHP_MM
# include "zend.h"

# define ir_mem_malloc  emalloc
# define ir_mem_calloc  ecalloc
# define ir_mem_realloc erealloc
# define ir_mem_free    efree
#endif

#define IR_EXTERNAL_GDB_ENTRY

#endif /* IR_PHP_H */
