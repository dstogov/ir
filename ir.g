/*
To generate ir_load.c use llk <https://github.com/dstogov/llk>:
php llk.php ir.g
*/

%start          ir
%case-sensetive true
%global-vars    false
%output         "ir_load.c"
%language       "c"
%indent         "\t"

%{
#include "ir.h"
#include "ir_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

const unsigned char *yy_buf;
const unsigned char *yy_end;
const unsigned char *yy_pos;
const unsigned char *yy_text;
uint32_t yy_line;

typedef struct _ir_parser_ctx {
	ir_ctx    *ctx;
	uint32_t   undef_count;
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
	ir_ref ref = ir_strtab_find(&p->var_tab, str, len);
	if (!ref) {
		p->undef_count++;
		/* create a linked list of unresolved references with header in "var_tab" */
		ref = IR_UNUSED; /* list terminator */
		ir_strtab_lookup(&p->var_tab, str, len, IR_ENCODE_UNRESOLVED_REF(p->ctx->insns_count, n));
	} else if (IR_IS_UNRESOLVED(ref)) {
		/* keep the linked list of unresolved references with header in "var_tab" */
		/* "ref" keeps the tail of the list */
		ir_strtab_update(&p->var_tab, str, len, IR_ENCODE_UNRESOLVED_REF(p->ctx->insns_count, n));
	}
	return ref;
}

static void ir_define_var(ir_parser_ctx *p, const char *str, size_t len, ir_ref ref) {
	ir_ref old_ref = ir_strtab_lookup(&p->var_tab, str, len, ref);
	if (ref != old_ref) {
		if (IR_IS_UNRESOLVED(old_ref)) {
			p->undef_count--;
			/* update the linked list of unresolved references */
			do {
				ir_ref *ptr = ((ir_ref*)(p->ctx->ir_base)) + IR_DECODE_UNRESOLVED_REF(old_ref);
				old_ref = *ptr;
				*ptr = ref;
			} while (old_ref != IR_UNUSED);
			ir_strtab_update(&p->var_tab, str, len, ref);
		} else {
			fprintf(stderr, "ERROR: Redefined variable `%*s` on line %d\n", (int)len, str, yy_line);
			exit(2);
		}
	}
}

static void report_undefined_var(const char *str, uint32_t len, ir_ref val)
{
	if (IR_IS_UNRESOLVED(val)) {
		fprintf(stderr, "ERROR: Undefined variable `%*s`\n", (int)len, str);
	}
}

void ir_check_indefined_vars(ir_parser_ctx *p)
{
	ir_strtab_apply(&p->var_tab, report_undefined_var);
	exit(2);
}

/* forward declarations */
static void yy_error(const char *msg);
static void yy_error_sym(const char *msg, int sym);

%}

ir:
	{ir_parser_ctx p;}
	(
		ir_func_prototype(&p)
		ir_func(&p)
	)+
;

ir_func(ir_parser_ctx *p):
	"{" (ir_insn(p) ";")* "}"
;

ir_func_prototype(ir_parser_ctx *p):
	{const char *name;}
	{size_t len;}
	{uint8_t t = 0;}
	"func" ID(&name, &len)
	"("
	(
		type(&t)
		(
			","
			type(&t)
		)*
	)?
	")"
	":"
	type(&t)
;

ir_insn(ir_parser_ctx *p):
	{const char *str, *str2 = NULL, *func;}
	{size_t len, len2 = 0, func_len;}
	{uint8_t op;}
	{uint8_t t = 0;}
	{ir_ref op1 = IR_UNUSED;}
	{ir_ref op2 = IR_UNUSED;}
	{ir_ref op3 = IR_UNUSED;}
	{ir_ref ref;}
	{ir_val val;}
	{ir_val count;}
	{ir_val flags;}
	{uint32_t n;}
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
	|	"func" "(" ID(&func, &func_len)
		{flags.u64 = 0;}
		(	","
			DECNUMBER(IR_U16, &flags)
		)?
		")"
		{ref = ir_const_func(p->ctx, ir_strl(p->ctx, func, func_len), flags.u16);}
	|	"func_addr" "("
		(	DECNUMBER(IR_ADDR, &val)
		|	HEXNUMBER(IR_ADDR, &val)
		)
		{flags.u64 = 0;}
		(	","
			DECNUMBER(IR_U16, &flags)
		)?
		")"
		{ref = ir_const_func_addr(p->ctx, val.addr, flags.u16);}
	|   STRING(&func, &func_len)
		{ref = ir_const_str(p->ctx, ir_strl(p->ctx, func, func_len));}
	|	func(&op)
		(
			"/"
			DECNUMBER(IR_I32, &count)
			{if (op == IR_PHI) count.i32++;}
			{if (op == IR_CALL || op == IR_TAILCALL) count.i32+=2;}
			{if (count.i32 < 0 || count.i32 > 255) yy_error("bad bumber of operands");}
			{ref = ir_emit_N(p->ctx, IR_OPT(op, t), count.i32);}
			(	"("
				(	val(p, op, 1, &op1)
					{n = 1;}
					{if (n > count.i32) yy_error("too many operands");}
					{ir_set_op(p->ctx, ref, n, op1);}
					(	","
						val(p, op, n,  &op1)
						{n++;}
						{if (n > count.i32) yy_error("too many operands");}
						{ir_set_op(p->ctx, ref, n, op1);}
					)*
				)?
				")"
			)?
		|
			(	"("
				(	val(p, op, 1, &op1)
					(	","
						val(p, op, 2, &op2)
						(	","
							val(p, op, 3, &op3)
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
					ref = ir_emit(p->ctx, IR_OPT(op, t), op1, op2, op3);
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
	{ref = ir_strtab_find(&type_tab, str, len);}
	{if (!ref) yy_error("invalid type");}
	{*t = ref;}
;

func(uint8_t *op):
	{const char *str;}
	{size_t len;}
	{ir_ref ref;}
	ID(&str, &len)
	{ref = ir_strtab_find(&op_tab, str, len);}
	{if (!ref) yy_error("invalid op");}
	{*op = ref - 1;}
;

val(ir_parser_ctx *p, uint8_t op, uint32_t n, ir_ref *ref):
	{const char *str;}
	{size_t len;}
	{ir_val val;}
	{uint32_t kind = IR_OPND_KIND(ir_op_flags[op], n);}
	(	ID(&str, &len)
		{if (kind < IR_OPND_DATA || kind > IR_OPND_VAR) yy_error("unexpected reference");}
		{*ref = ir_use_var(p, n, str, len);}
	|   STRING(&str, &len)
		{if (kind != IR_OPND_STR) yy_error("unexpected string");}
		{*ref = ir_strl(p->ctx, str, len);}
	|	DECNUMBER(IR_I32, &val)
		{if (kind != IR_OPND_NUM && kind != IR_OPND_PROB) yy_error("unexpected number");}
		{if (val.u64 < 0 && val.u64 >= 0x7ffffff) yy_error("number out of range");}
		{*ref = val.u64;}
	)
;

const(uint8_t t, ir_val *val):
		DECNUMBER(t, val)
	|	HEXNUMBER(t, val)
	|	FLOATNUMBER(t, val)
	|	CHARACTER(val)
;

/* scanner rules */
ID(const char **str, size_t *len):
	/[A-Za-z_][A-Za-z_0-9]*/
	{*str = (const char*)yy_text; *len = yy_pos - yy_text;}
;

DECNUMBER(uint32_t t, ir_val *val):
	/[\-]?[0-9]+/
	{if (t >= IR_DOUBLE) val->d = atof((const char*)yy_text); else val->i64 = atoll((const char*)yy_text);}
;

HEXNUMBER(uint32_t t, ir_val *val):
	/0x[0-9A-Fa-f]+/
	{val->u64 = strtoull((const char*)yy_text + 2, NULL, 16);}
;

FLOATNUMBER(uint32_t t, ir_val *val):
	/[\-]?([0-9]*\.[0-9]+([Ee][\+\-]?[0-9]+)?|[0-9]+\.([Ee][\+\-]?[0-9]+)?|[0-9]+[Ee][\+\-]?[0-9]+)/
	{val->d = atof((const char*)yy_text);}
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

int ir_load(ir_ctx *ctx, FILE *f) {
	ir_parser_ctx p;
	int sym;
	long pos, end;

	p.ctx = ctx;
	p.undef_count = 0;
	ir_strtab_init(&p.var_tab, 256, 4096);

	pos = ftell(f);
	fseek(f, 0, SEEK_END);
	end = ftell(f);
	fseek(f, pos, SEEK_SET);
	yy_buf = alloca(end - pos + 1);
	yy_end = yy_buf + (end - pos);
	fread((void*)yy_buf, (end - pos), 1, f);
	*(unsigned char*)yy_end = 0;

	yy_pos = yy_text = yy_buf;
	yy_line = 1;
	sym = parse_ir_func(get_sym(), &p);
	if (sym != YY_EOF) {
		yy_error_sym("<EOF> expected, got", sym);
	}
	if (p.undef_count) {
		ir_check_indefined_vars(&p);
	}

	ir_strtab_free(&p.var_tab);

	return 1;
}

void ir_loader_init(void)
{
	ir_ref i;

	ir_strtab_init(&type_tab, IR_LAST_OP, 0);
	for (i = 1; i < IR_LAST_TYPE; i++) {
		ir_strtab_lookup(&type_tab, ir_type_cname[i], strlen(ir_type_cname[i]), i);
	}

	ir_strtab_init(&op_tab, IR_LAST_OP, 0);
	for (i = 0; i < IR_LAST_OP; i++) {
		ir_strtab_lookup(&op_tab, ir_op_name[i], strlen(ir_op_name[i]), i + 1);
	}
}

void ir_loader_free(void)
{
	ir_strtab_free(&type_tab);
	ir_strtab_free(&op_tab);
}
