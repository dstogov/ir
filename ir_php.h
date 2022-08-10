#ifndef IR_PHP_H
#define IR_PHP_H

#include "zend.h"

#define IR_PHP_OPS(_)

#define ir_mem_malloc  emalloc
#define ir_mem_calloc  ecalloc
#define ir_mem_realloc erealloc
#define ir_mem_free    efree

#endif /* IR_PHP_H */
