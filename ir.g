/*
 * IR - Lightweight JIT Compilation Framework
 * (IR loader)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 *
 * To generate ir_load.c use llk <https://github.com/dstogov/llk>:
 * php llk.php ir.g
 */

%start          ir
%case-sensetive true
%global-vars    false
%output         "ir_load.c"
%language       "c"
%indent         "\t"

%{
/*
 * IR - Lightweight JIT Compilation Framework
 * (IR loader)
 * Copyright (C) 2022 Zend by Perforce.
 * Authors: Dmitry Stogov <dmitry@php.net>
 *
 * This file is generated from "ir.g". Do not edit!
 *
 * To generate ir_load.c use llk <https://github.com/dstogov/llk>:
 * php llk.php ir.g
 */

#include "ir.h"
#include "ir_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifndef _WIN32
# include <unistd.h>
#endif

const unsigned char *yy_buf;
const unsigned char *yy_end;
const unsigned char *yy_pos;
const unsigned char *yy_text;
uint32_t yy_line;

typedef struct _ir_parser_ctx {
	ir_ctx    *ctx;
	uint32_t   undef_count;
	ir_ref     curr_ref;
	ir_strtab  var_tab;
} ir_parser_ctx;

static ir_strtab type_tab;
static ir_strtab op_tab;

#define IR_IS_UNRESOLVED(ref) \
	((ref) < (ir_ref)0xc0000000)
#define IR_ENCODE_UNRESOLVED_REF(ref, op) \
	((ir_ref)0xc0000000 - ((ref) * sizeof(ir_ref) + (op)))
#define IR_DECODE_UNRESOLVED_REF(ref) \
	((ir_ref)0xc0000000 - (ref))

static ir_ref ir_use_var(ir_parser_ctx *p, uint32_t n, const char *str, size_t len) {
	ir_ref ref;
	uint32_t len32;

	IR_ASSERT(len <= 0xffffffff);
	len32 = (uint32_t)len;
	ref = ir_strtab_find(&p->var_tab, str, len32);
	if (!ref) {
		p->undef_count++;
		/* create a linked list of unresolved references with header in "var_tab" */
		ref = IR_UNUSED; /* list terminator */
		ir_strtab_lookup(&p->var_tab, str, len32, IR_ENCODE_UNRESOLVED_REF(p->curr_ref, n));
	} else if (IR_IS_UNRESOLVED(ref)) {
		/* keep the linked list of unresolved references with header in "var_tab" */
		/* "ref" keeps the tail of the list */
		ir_strtab_update(&p->var_tab, str, len32, IR_ENCODE_UNRESOLVED_REF(p->curr_ref, n));
	}
	return ref;
}

static void ir_define_var(ir_parser_ctx *p, const char *str, size_t len, ir_ref ref) {
	ir_ref old_ref;
	uint32_t len32;

	IR_ASSERT(len <= 0xffffffff);
	len32 = (uint32_t)len;
	old_ref = ir_strtab_lookup(&p->var_tab, str, len32, ref);
	if (ref != old_ref) {
		if (IR_IS_UNRESOLVED(old_ref)) {
			p->undef_count--;
			/* update the linked list of unresolved references */
			do {
				ir_ref *ptr = ((ir_ref*)(p->ctx->ir_base)) + IR_DECODE_UNRESOLVED_REF(old_ref);
				old_ref = *ptr;
				*ptr = ref;
			} while (old_ref != IR_UNUSED);
			ir_strtab_update(&p->var_tab, str, len32, ref);
		} else {
			fprintf(stderr, "ERROR: Redefined variable `%.*s` on line %d\n", (int)len32, str, yy_line);
			exit(2);
		}
	}
}

static void report_undefined_var(const char *str, uint32_t len, ir_ref val)
{
	if (IR_IS_UNRESOLVED(val)) {
		fprintf(stderr, "ERROR: Undefined variable `%.*s`\n", (int)len, str);
	}
}

static void ir_check_indefined_vars(ir_parser_ctx *p)
{
	ir_strtab_apply(&p->var_tab, report_undefined_var);
	exit(2);
}

/* forward declarations */
static void yy_error(const char *msg);
static void yy_error_sym(const char *msg, int sym);
static void yy_error_str(const char *msg, const char *str);

static ir_ref ir_make_const_str(ir_ctx *ctx, const char *str, size_t len)
{
	char *buf = alloca(len + 1);
	char *p = buf;

	while (len > 0) {
		if (*str != '\\') {
			*p = *str;
		} else {
			str++;
			len--;
			IR_ASSERT(len != 0);
			switch (*str) {
				case '\\': *p = '\\'; break;
				case '\'': *p = '\''; break;
				case '"':  *p = '"';  break;
				case 'a':  *p = '\a'; break;
				case 'e':  *p = 27;   break; /* '\e'; */
				case 'f':  *p = '\f'; break;
				case 'n':  *p = '\n'; break;
				case 'r':  *p = '\r'; break;
				case 't':  *p = '\t'; break;
				case 'v':  *p = '\v'; break;
				case '?':  *p = 0x3f; break;
				default:
					yy_error("unsupported escape sequence");
			}
		}
		str++;
		p++;
		len--;
	}
	*p = 0;
	return ir_const_str(ctx, ir_strl(ctx, buf, p - buf));
}

%}

ir(ir_loader *loader):
	{ir_parser_ctx p;}
	{ir_ctx ctx;}
	{uint8_t ret_type;}
	{char name[256];}
	{uint32_t flags = 0;}
	{size_t size;}
	{uint32_t params_count;}
	{uint8_t param_types[256];}
	{p.ctx = &ctx;}
	(
		(
			"extern"
			{flags = 0;}
			(
				ir_sym(name, &flags)
				";"
				{
					if (loader->external_sym_dcl
					 && !loader->external_sym_dcl(loader, name, flags)) {
						yy_error("extenral_sym_dcl error");
					}
				}
			|
				{flags = 0;}
				ir_func_name(name)
				ir_func_proto(&p, &flags, &ret_type, &params_count, param_types)
				";"
				{
					if (loader->external_func_dcl
					 && !loader->external_func_dcl(loader, name, flags, ret_type, params_count, param_types)) {
						yy_error("extenral_func_dcl error");
					}
				}
			)
		|
			{flags = 0;}
			(
				"static"
				{flags |= IR_STATIC;}
			)?
			(
				ir_sym(name, &flags) ir_sym_size(&size)
				(
					{
						if (loader->sym_dcl && !loader->sym_dcl(loader, name, flags, size, 0)) {
							yy_error("sym_dcl error");
						}
					}
					";"
				|
					{
						if (loader->sym_dcl && !loader->sym_dcl(loader, name, flags, size, 1)) {
							yy_error("sym_dcl error");
						}
					}
					"=" "{" ir_sym_data(loader) ("," ir_sym_data(loader))* ","? "}" ";"
					{
						if (loader->sym_data_end && !loader->sym_data_end(loader)) {
							yy_error("sym_data_end error");
						}
					}
				)
			|
				ir_func_name(name)
				ir_func_proto(&p, &flags, &ret_type, &params_count, param_types)
				(
					";"
					{
						if (loader->forward_func_dcl
						 && !loader->forward_func_dcl(loader, name, flags, ret_type, params_count, param_types)) {
							yy_error("forward_func_decl error");
						}
					}
				|
					{if (!loader->func_init(loader, &ctx, name)) yy_error("init_func error");}
					{ctx.flags |= flags;}
					{ctx.ret_type = ret_type;}
					ir_func(&p)
					{if (!loader->func_process(loader, &ctx, name)) yy_error("process_func error");}
					{ir_free(&ctx);}
				)
			)
		)+
	|
		{if (!loader->func_init(loader, &ctx, NULL)) yy_error("ini_func error");}
		{ctx.ret_type = -1;}
		ir_func(&p)
		{if (!loader->func_process(loader, &ctx, NULL)) yy_error("process_func error");}
		{ir_free(&ctx);}
	)
;

ir_sym(char *buf, uint32_t *flags):
	{const char *name;}
	{size_t len;}
	(
		"var"
	|
		"const"
		{*flags |= IR_CONST;}
	)
	ID(&name, &len)
	{if (len > 255) yy_error("name too long");}
	{memcpy(buf, name, len);}
	{buf[len] = 0;}
;

ir_sym_size(size_t *size):
	{ir_val val;}
	"[" DECNUMBER(IR_U64, &val) "]"
	{*size = val.u64;}
;

ir_sym_data(ir_loader *loader):
	{uint8_t t = 0;}
	{ir_val val;}
	{void *p;}
	{const char *name;}
	{size_t name_len;}
	{char buf[256];}
	{uintptr_t offset = 0;}
	type(&t)
	(
		"sym" "(" ID(&name, &name_len) ")"
		(
			"+" const(t, &val)
			{offset = (uintptr_t)val.addr;}
		)?
		{
			if (loader->sym_data_ref) {
				if (name_len > 255) yy_error("name too long");
				memcpy(buf, name, name_len);
				buf[name_len] = 0;
				if (!loader->sym_data_ref(loader, IR_SYM, buf, offset)) {
					yy_error("sym_data_ref error");
				}
			}
		}
	|	"func" "(" ID(&name, &name_len) ")"
		{
			if (loader->sym_data_ref) {
				if (name_len > 255) yy_error("name too long");
				memcpy(buf, name, name_len);
				buf[name_len] = 0;
				if (!loader->sym_data_ref(loader, IR_FUNC, buf, 0)) {
					yy_error("sym_data_ref error");
				}
			}
		}
	|
		const(t, &val)
		{
			if (loader->sym_data) {
				switch (ir_type_size[t]) {
					case 1: p = &val.i8;  break;
					case 2: p = &val.i16; break;
					case 4: p = &val.i32; break;
					case 8: p = &val.i64; break;
					default:
						IR_ASSERT(0);
						break;
				}
				if (!loader->sym_data(loader, t, 1, p)) {
					yy_error("sym_data error");
				}
			}
		}
	)
;

ir_func(ir_parser_ctx *p):
	{p->undef_count = 0;}
	{ir_strtab_init(&p->var_tab, 256, 4096);}
	"{" ( ("NOP" | ir_insn(p) ) ";")* "}"
	{if (p->undef_count) ir_check_indefined_vars(p);}
	{ir_strtab_free(&p->var_tab);}
;

ir_func_name(char *buf):
	{const char *name;}
	{size_t len;}
	"func" ID(&name, &len)
	{if (len > 255) yy_error("name too long");}
	{memcpy(buf, name, len);}
	{buf[len] = 0;}
;

ir_func_proto(ir_parser_ctx *p, uint32_t *flags, uint8_t *ret_type, uint32_t *params_count, uint8_t *param_types):
	{uint8_t t = 0;}
	{uint32_t n = 0;}
	"("
	(
		"void"
	|
		"..."
		{*flags |= IR_VARARG_FUNC;}
	|
		(
			type(&t)
			{param_types[n++] = t;}
			(
				","
				type(&t)
				{param_types[n++] = t;}
				{if (n > 256) yy_error("name too params");}
			)*
			(
				","
				"..."
				{*flags |= IR_VARARG_FUNC;}
			)?
		)
	)?
	")"
	":"
	(
		type(ret_type)
	|
		"void"
		{*ret_type = IR_VOID;}
	)
	(
		"__fastcall"
		{*flags |= IR_FASTCALL_FUNC;}
	|
		"__builtin"
		{*flags |= IR_BUILTIN_FUNC;}
	)?
	{*params_count = n;}
;

ir_insn(ir_parser_ctx *p):
	{const char *str, *str2 = NULL, *func;}
	{size_t len, len2 = 0, func_len;}
	{uint8_t op;}
	{uint8_t t = 0;}
	{ir_ref op1 = IR_UNUSED;}
	{ir_ref op2 = IR_UNUSED;}
	{ir_ref op3 = IR_UNUSED;}
	{ir_ref ref = IR_UNUSED;}
	{ir_val val;}
	{ir_val count;}
	{int32_t n;}
	{uint8_t ret_type;}
	{uint32_t flags;}
	{uint32_t params_count;}
	{uint8_t param_types[256];}
	(
		type(&t)
		ID(&str, &len)
		(
			","
			ID(&str2, &len2)
		)?
	|
		ID(&str, &len)
	)
	"="
	(	{val.u64 = 0;}
		const(t, &val)
		{ref = ir_const(p->ctx, val, t);}
	|	"func"
		(	ID(&func, &func_len)
			{flags = 0;}
			ir_func_proto(p, &flags, &ret_type, &params_count, param_types)
			{ref = ir_proto(p->ctx, flags, ret_type, params_count, param_types);}
			{ref = ir_const_func(p->ctx, ir_strl(p->ctx, func, func_len), ref);}
		|	"*"
			(	DECNUMBER(IR_ADDR, &val)
			|	HEXNUMBER(IR_ADDR, &val)
			)
			{flags = 0;}
			ir_func_proto(p, &flags, &ret_type, &params_count, param_types)
			{ref = ir_proto(p->ctx, flags, ret_type, params_count, param_types);}
			{ref = ir_const_func_addr(p->ctx, val.addr, ref);}
		)
	|	"sym" "(" ID(&func, &func_len) ")"
		{ref = ir_const_sym(p->ctx, ir_strl(p->ctx, func, func_len));}
	|   STRING(&func, &func_len)
		{ref = ir_make_const_str(p->ctx, func, func_len);}
	|	func(&op)
		(
			"/"
			DECNUMBER(IR_I32, &count)
			{if (op == IR_PHI || op == IR_SNAPSHOT) count.i32++;}
			{if (op == IR_CALL || op == IR_TAILCALL) count.i32+=2;}
			{if (count.i32 < 0 || count.i32 > 255) yy_error("bad number of operands");}
			{ref = ir_emit_N(p->ctx, IR_OPT(op, t), count.i32);}
			(	"("
				(
					{p->curr_ref = ref;}
					val(p, op, 1, &op1)
					{n = 1;}
					{if (n > count.i32) yy_error("too many operands");}
					{ir_set_op(p->ctx, ref, n, op1);}
					(	","
						val(p, op, n + 1, &op1)
						{n++;}
						{if (n > count.i32) yy_error("too many operands");}
						{ir_set_op(p->ctx, ref, n, op1);}
					)*
				)?
				")"
			)?
		|
			{n = 0;}
			(	"("
				(
					{p->curr_ref = p->ctx->insns_count;}
					val(p, op, 1, &op1)
					{n = 1;}
					(	","
						val(p, op, 2, &op2)
						{n = 2;}
						(	","
							val(p, op, 3, &op3)
							{n = 3;}
						)?
					)?
				)?
				")"
			)?
			{
				if (IR_IS_FOLDABLE_OP(op)
				 && !IR_IS_UNRESOLVED(op1)
				 && !IR_IS_UNRESOLVED(op2)
				 && !IR_IS_UNRESOLVED(op3)) {
					ref = ir_fold(p->ctx, IR_OPT(op, t), op1, op2, op3);
				} else {
					uint32_t opt;

					if (!IR_OP_HAS_VAR_INPUTS(ir_op_flags[op])) {
						opt = IR_OPT(op, t);
					} else {
						opt = IR_OPTX(op, t, n);
					}
					ref = ir_emit(p->ctx, opt, op1, op2, op3);
				}
			}
		)
	)
	{ir_define_var(p, str, len, ref);}
	{if (str2) ir_define_var(p, str2, len2, ref);}
;

type(uint8_t *t):
	{const char *str;}
	{size_t len;}
	{ir_ref ref;}
	ID(&str, &len)
	{IR_ASSERT(len <= 0xffffffff);}
	{ref = ir_strtab_find(&type_tab, str, (uint32_t)len);}
	{if (!ref) yy_error("invalid type");}
	{*t = ref;}
;

func(uint8_t *op):
	{const char *str;}
	{size_t len;}
	{ir_ref ref;}
	ID(&str, &len)
	{IR_ASSERT(len <= 0xffffffff);}
	{ref = ir_strtab_find(&op_tab, str, (uint32_t)len);}
	{if (!ref) yy_error("invalid op");}
	{*op = ref - 1;}
;

val(ir_parser_ctx *p, uint8_t op, uint32_t n, ir_ref *ref):
	{const char *str;}
	{size_t len;}
	{ir_val val;}
	{uint32_t kind = IR_OPND_KIND(ir_op_flags[op], n);}
	{uint8_t ret_type;}
	{uint32_t flags;}
	{uint32_t params_count;}
	{uint8_t param_types[256];}
	(	ID(&str, &len)
		{if (!IR_IS_REF_OPND_KIND(kind)) yy_error("unexpected reference");}
		{*ref = ir_use_var(p, n, str, len);}
	|   STRING(&str, &len)
		{if (kind != IR_OPND_STR) yy_error("unexpected string");}
		{*ref = ir_strl(p->ctx, str, len);}
	|	DECNUMBER(IR_I32, &val)
		{if (kind != IR_OPND_NUM && kind != IR_OPND_PROB) yy_error("unexpected number");}
		{if (val.i64 < 0 || val.i64 > 0x7fffffff) yy_error("number out of range");}
		{*ref = val.i32;}
	|	"null"
		{*ref = IR_UNUSED;}
	|   "func"
		{if (kind != IR_OPND_PROTO) yy_error("unexpected function prototype");}
		{flags = 0;}
		ir_func_proto(p, &flags, &ret_type, &params_count, param_types)
		{*ref = ir_proto(p->ctx, flags, ret_type, params_count, param_types);}
	)
;

const(uint8_t t, ir_val *val):
		DECNUMBER(t, val)
	|	HEXNUMBER(t, val)
	|	FLOATNUMBER(t, val)
	|	CHARACTER(val)
	|	"inf"
		{if (t == IR_DOUBLE) val->d = INFINITY; else val->f = INFINITY;}
	|	"nan"
		{if (t == IR_DOUBLE) val->d = NAN; else val->f = NAN;}
	|	"-" "inf"
		{if (t == IR_DOUBLE) val->d = -INFINITY; else val->f = -INFINITY;}
;

/* scanner rules */
ID(const char **str, size_t *len):
	/[A-Za-z_][A-Za-z_0-9]*/
	{*str = (const char*)yy_text; *len = yy_pos - yy_text;}
;

DECNUMBER(uint32_t t, ir_val *val):
	/[\-]?[0-9]+/
	{if (t == IR_DOUBLE) val->d = atof((const char*)yy_text);
	else if (t == IR_FLOAT) val->f = strtof((const char*)yy_text, NULL);
	else if (IR_IS_TYPE_SIGNED(t)) val->i64 = atoll((const char*)yy_text);
	else val->u64 = strtoull((const char*)yy_text, NULL, 10);}
;

HEXNUMBER(uint32_t t, ir_val *val):
	/0x[0-9A-Fa-f]+/
	{val->u64 = strtoull((const char*)yy_text + 2, NULL, 16);}
;

FLOATNUMBER(uint32_t t, ir_val *val):
	/[\-]?([0-9]*\.[0-9]+([Ee][\+\-]?[0-9]+)?|[0-9]+\.([Ee][\+\-]?[0-9]+)?|[0-9]+[Ee][\+\-]?[0-9]+)/
	{if (t == IR_DOUBLE) val->d = atof((const char*)yy_text); else val->f = strtof((const char*)yy_text, NULL);}
;

CHARACTER(ir_val *val):
	/'([^'\\]|\\.)*'/
	{
		if ((char)yy_text[1] != '\\') {
			val->i64 = (char)yy_text[1];
		} else if ((char)yy_text[2] == '\\') {
			val->i64 = '\\';
		} else if ((char)yy_text[2] == 'r') {
			val->i64 = '\r';
		} else if ((char)yy_text[2] == 'n') {
			val->i64 = '\n';
		} else if ((char)yy_text[2] == 't') {
			val->i64 = '\t';
		} else if ((char)yy_text[2] == '0') {
			val->i64 = '\0';
		} else {
			IR_ASSERT(0);
		}
    }
;

STRING(const char **str, size_t *len):
	/"([^"\\]|\\.)*"/
	{*str = (const char*)yy_text + 1; *len = yy_pos - yy_text - 2;}
;

EOL: /\r\n|\r|\n/;
WS: /[ \t\f\v]+/;
ONE_LINE_COMMENT: /(\/\/|#)[^\r\n]*(\r\n|\r|\n)/;
COMMENT: /\/\*([^\*]|\*+[^\*\/])*\*+\//;

SKIP: ( EOL | WS | ONE_LINE_COMMENT | COMMENT )*;

%%

static void yy_error(const char *msg) {
	fprintf(stderr, "ERROR: %s at line %d\n", msg, yy_line);
	exit(2);
}

static void yy_error_sym(const char *msg, int sym) {
	fprintf(stderr, "ERROR: %s '%s' at line %d\n", msg, sym_name[sym], yy_line);
	exit(2);
}

static void yy_error_str(const char *msg, const char *str) {
	fprintf(stderr, "ERROR: %s '%s' at line %d\n", msg, str, yy_line);
	exit(2);
}

int ir_load(ir_loader *loader, FILE *f) {
	long pos, end;

	pos = ftell(f);
	fseek(f, 0, SEEK_END);
	end = ftell(f);
	fseek(f, pos, SEEK_SET);
	yy_buf = alloca(end - pos + 1);
	yy_end = yy_buf + (end - pos);
	fread((void*)yy_buf, (end - pos), 1, f);
	*(unsigned char*)yy_end = 0;

	parse(loader);

	return 1;
}

void ir_loader_init(void)
{
	ir_ref i;

	ir_strtab_init(&type_tab, IR_LAST_OP, 0);
	for (i = 1; i < IR_LAST_TYPE; i++) {
		ir_strtab_lookup(&type_tab, ir_type_cname[i], (uint32_t)strlen(ir_type_cname[i]), i);
	}

	ir_strtab_init(&op_tab, IR_LAST_OP, 0);
	for (i = 0; i < IR_LAST_OP; i++) {
		ir_strtab_lookup(&op_tab, ir_op_name[i], (uint32_t)strlen(ir_op_name[i]), i + 1);
	}
}

void ir_loader_free(void)
{
	ir_strtab_free(&type_tab);
	ir_strtab_free(&op_tab);
}
