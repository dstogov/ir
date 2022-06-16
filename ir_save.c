#include "ir.h"
#include "ir_private.h"

void ir_save(ir_ctx *ctx, FILE *f)
{
	ir_ref i, j, n, ref, *p;
	ir_insn *insn;
	uint32_t flags;
	bool first;

	fprintf(f, "{\n");
	for (i = IR_UNUSED + 1, insn = ctx->ir_base - i; i < ctx->consts_count; i++, insn--) {
		fprintf(f, "\t%s c_%d = ", ir_type_cname[insn->type], i);
		if (insn->op == IR_FUNC) {
			fprintf(f, "func(%s)", ir_get_str(ctx, insn->val.addr));
		} else {
			ir_print_const(ctx, insn, f);
		}
		fprintf(f, ";\n");
	}

	for (i = IR_UNUSED + 1, insn = ctx->ir_base + i; i < ctx->insns_count;) {
		flags = ir_op_flags[insn->op];
		if (flags & IR_OP_FLAG_CONTROL) {
			if (!(flags & IR_OP_FLAG_MEM) || insn->type == IR_VOID) {
				fprintf(f, "\tl_%d = ", i);
			} else {
				fprintf(f, "\t%s d_%d, l_%d = ", ir_type_cname[insn->type], i, i);
			}
		} else {
			fprintf(f, "\t");
			if (flags & IR_OP_FLAG_DATA) {
				fprintf(f, "%s d_%d = ", ir_type_cname[insn->type], i);
			}
		}
		fprintf(f, "%s", ir_op_name[insn->op]);
		n = ir_operands_count(ctx, insn);
		if ((insn->op == IR_MERGE || insn->op == IR_LOOP_BEGIN) && n != 2) {
			fprintf(f, "/%d", n);
		} else if ((insn->op == IR_CALL || insn->op == IR_TAILCALL) && n != 2) {
			fprintf(f, "/%d", n - 2);
		} else if (insn->op == IR_PHI && n != 3) {
			fprintf(f, "/%d", n - 1);
		}
		first = 1;
		for (j = 1, p = insn->ops + 1; j <= n; j++, p++) {
			uint32_t opnd_kind = IR_OPND_KIND(flags, j);

			ref = *p;
			if (ref) {
				switch (opnd_kind) {
					case IR_OPND_DATA:
					case IR_OPND_VAR:
						if (IR_IS_CONST_REF(ref)) {
							fprintf(f, "%sc_%d", first ? "(" : ", ", -ref);
						} else {
							fprintf(f, "%sd_%d", first ? "(" : ", ", ref);
						}
						first = 0;
						break;
					case IR_OPND_CONTROL:
					case IR_OPND_CONTROL_DEP:
					case IR_OPND_CONTROL_REF:
						fprintf(f, "%sl_%d", first ? "(" : ", ", ref);
						first = 0;
						break;
					case IR_OPND_STR:
						fprintf(f, "%s\"%s\"", first ? "(" : ", ", ir_get_str(ctx, ref));
						first = 0;
						break;
					case IR_OPND_PROB:
						if (ref == 0) {
							break;
						}
					case IR_OPND_NUM:
						fprintf(f, "%s%d", first ? "(" : ", ", ref);
						first = 0;
						break;
				}
			} else if (opnd_kind == IR_OPND_NUM) {
				fprintf(f, "%s%d", first ? "(" : ", ", ref);
				first = 0;
			}
		}
		n = 1 + (n >> 2); // support for multi-word instructions like MERGE and PHI
		i += n;
		insn += n;
		fprintf(f, "%s;\n", first ? "" : ")");
	}
	fprintf(f, "}\n");
}
