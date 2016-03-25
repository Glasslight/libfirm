/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief   This file implements functions to finalize the irg for emit.
 * @author  Christian Wuerdig
 */
#include "ia32_finish.h"

#include "be2addr.h"
#include "bearch.h"
#include "besched.h"
#include "debug.h"
#include "gen_ia32_regalloc_if.h"
#include "ia32_bearch_t.h"
#include "ia32_new_nodes.h"
#include "ia32_transform.h"
#include "iredges_t.h"
#include "irgmod.h"
#include "irgwalk.h"
#include "irnode_t.h"
#include "panic.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)

static bool reads_carry(x86_condition_code_t code)
{
	x86_condition_code_t c2 = code & ~x86_cc_negated;
	return c2 == x86_cc_below || c2 == x86_cc_below_equal
	    || c2 == x86_cc_float_below || c2 == x86_cc_float_below_equal
	    || c2 == x86_cc_float_unordered_below_equal
	    || c2 == x86_cc_float_unordered_below;
}

/**
 * Transforms a Sub or xSub into Neg--Add iff OUT_REG != SRC1_REG && OUT_REG == SRC2_REG.
 * THIS FUNCTIONS MUST BE CALLED AFTER REGISTER ALLOCATION.
 */
static bool ia32_transform_sub_to_neg_add(ir_node *const irn, arch_register_t const *const out_reg)
{
	/* in case of sub and OUT == SRC2 we can transform the sequence into neg src2 -- add */
	ir_node *const in2 = get_irn_n(irn, n_ia32_binary_right);
	if (out_reg != arch_get_irn_register(in2))
		return false;

	dbg_info *const dbgi  = get_irn_dbg_info(irn);
	ir_node  *const block = get_nodes_block(irn);
	ir_graph *const irg   = get_irn_irg(irn);
	ir_node  *const noreg = ia32_new_NoReg_gp(irg);
	ir_node  *const nomem = get_irg_no_mem(irg);
	ir_node  *const in1   = get_irn_n(irn, n_ia32_binary_left);

	/* generate the neg src2 */
	ir_node *res;
	if (is_ia32_xSub(irn)) {
		ir_mode *op_mode = get_ia32_ls_mode(irn);
		assert(get_irn_mode(irn) != mode_T);

		ir_node  *const noreg_fp = ia32_new_NoReg_xmm(irg);
		res = new_bd_ia32_xXor(dbgi, block, noreg, noreg, nomem, in2, noreg_fp);
		int        size   = get_mode_size_bits(op_mode);
		ir_entity *entity = ia32_gen_fp_known_const(size == 32 ? ia32_SSIGN : ia32_DSIGN);
		set_ia32_am_ent(res, entity);
		set_ia32_op_type(res, ia32_AddrModeS);
		set_ia32_ls_mode(res, op_mode);

		arch_set_irn_register(res, out_reg);

		/* add to schedule */
		sched_add_before(irn, res);

		/* generate the add */
		res = new_bd_ia32_xAdd(dbgi, block, noreg, noreg, nomem, res, in1);
		set_ia32_ls_mode(res, get_ia32_ls_mode(irn));
	} else {
		ir_node *flags_proj  = NULL;
		bool     needs_carry = false;
		/** See if someone is interested in a correctly set carry flag */
		if (get_irn_mode(irn) == mode_T) {
			flags_proj = get_Proj_for_pn(irn, pn_ia32_flags);
			if (flags_proj) {
				foreach_out_edge(flags_proj, edge) {
					ir_node *user = get_edge_src_irn(edge);
					x86_condition_code_t cc = get_ia32_condcode(user);
					if (reads_carry(cc)) {
						needs_carry = true;
						break;
					}
				}
			}
		}

		ir_node *carry;
		if (is_ia32_Sbb(irn)) {
			/* Feed borrow (in CF) as carry (via CMC) into NOT+ADC. */
			carry = get_irn_n(irn, n_ia32_Sbb_eflags);
			carry = new_bd_ia32_Cmc(dbgi, block, carry);
			goto carry;
		} else if (flags_proj != NULL && needs_carry) {
			/*
			 * ARG, the above technique does NOT set the flags right.
			 * So, we must produce the following code:
			 * t1 = ~b
			 * t2 = a + ~b + Carry
			 * Complement Carry
			 *
			 * a + -b = a + (~b + 1)  would set the carry flag wrong IFF both a and b are zero.
			 */
			carry = new_bd_ia32_Stc(dbgi, block);

carry:;
			ir_node *nnot = new_bd_ia32_Not(dbgi, block, in2);
			arch_set_irn_register(nnot, out_reg);
			sched_add_before(irn, nnot);

			arch_set_irn_register(carry, &ia32_registers[REG_EFLAGS]);
			sched_add_before(irn, carry);

			ir_node *adc = new_bd_ia32_Adc(dbgi, block, noreg, noreg, nomem, nnot, in1, carry);
			arch_set_irn_register(adc, out_reg);
			set_ia32_commutative(adc);

			if (flags_proj != NULL) {
				set_irn_mode(adc, mode_T);
				arch_register_t const *const reg_flags = &ia32_registers[REG_EFLAGS];
				ir_node               *const adc_flags = be_new_Proj_reg(adc, pn_ia32_Adc_flags, reg_flags);

				ir_node *cmc = new_bd_ia32_Cmc(dbgi, block, adc_flags);
				arch_set_irn_register(cmc, reg_flags);
				sched_add_after(irn, cmc);
				exchange(flags_proj, cmc);
			}

			res = adc;
		} else {
			res = new_bd_ia32_Neg(dbgi, block, in2);
			arch_set_irn_register(res, out_reg);

			/* add to schedule */
			sched_add_before(irn, res);

			/* generate the add */
			res = new_bd_ia32_Add(dbgi, block, noreg, noreg, nomem, res, in1);
			arch_set_irn_register_out(res, pn_ia32_res,   out_reg);
			arch_set_irn_register_out(res, pn_ia32_flags, &ia32_registers[REG_EFLAGS]);
			set_ia32_commutative(res);
		}
	}

	set_irn_mode(res, get_irn_mode(irn));
	SET_IA32_ORIG_NODE(res, irn);

	/* exchange the add and the sub */
	sched_replace(irn, res);
	exchange(irn, res);
	return true;
}

static void ia32_transform_ShlD_to_ShrD_imm(ir_node *const irn, arch_register_t const *const out_reg)
{
	/* a = ShlD(b, a, c) -> a = ShrD(a, b, 32 - c) */
	ir_node                     *const lcount = get_irn_n(irn, n_ia32_ShlD_count);
	ia32_immediate_attr_t const *const attr   = get_ia32_immediate_attr_const(lcount);
	ir_graph                    *const irg    = get_irn_irg(irn);
	ir_node                     *const count  = ia32_create_Immediate(irg, 32 - attr->imm.offset);
	dbg_info                    *const dbgi   = get_irn_dbg_info(irn);
	ir_node                     *const block  = get_nodes_block(irn);
	ir_node                     *const in1    = get_irn_n(irn, n_ia32_ShlD_val_low);
	ir_node                     *const in0    = get_irn_n(irn, n_ia32_ShlD_val_high);
	ir_node                     *const res    = new_bd_ia32_ShrD_imm(dbgi, block, in1, in0, count);
	arch_set_irn_register_out(res, pn_ia32_ShrD_res, out_reg);
	sched_replace(irn, res);
	exchange(irn, res);
}

static inline int need_constraint_copy(ir_node *irn)
{
	/* TODO this should be determined from the node specification */
	if (is_ia32_irn(irn)) {
		switch (get_ia32_irn_opcode(irn)) {
		case iro_ia32_Lea:
		case iro_ia32_Minus64:
			return 0;

		default:
			return 1;
		}
	}
	return be_is_Asm(irn);
}

/**
 * Following Problem:
 * We have a source address mode node with base or index register equal to
 * result register and unfulfilled should_be_same requirement. The constraint
 * handler will insert a copy from the remaining input operand to the result
 * register -> base or index is broken then.
 * Solution: Turn back this address mode into explicit Load + Operation.
 */
static void fix_am_source(ir_node *const irn, arch_register_t const *const out_reg)
{
	/* check only nodes with source address mode */
	if (get_ia32_op_type(irn) != ia32_AddrModeS)
		return;
	/* only need to fix binary operations */
	if (get_ia32_am_support(irn) != ia32_am_binary)
		return;
	/* we only need to do something if the out reg is the same as base or index
	 * register */
	if (out_reg != arch_get_irn_register_in(irn, n_ia32_base) &&
			out_reg != arch_get_irn_register_in(irn, n_ia32_index))
		return;

	ir_node *load_res = ia32_turn_back_am(irn);
	arch_set_irn_register(load_res, out_reg);

	DBG((dbg, LEVEL_3,
		"irg %+F: build back AM source for node %+F, inserted load %+F\n",
		get_irn_irg(irn), irn, get_Proj_pred(load_res)));
}

static bool ia32_handle_2addr(ir_node *const node, arch_register_req_t const *const req, arch_register_t const *const reg)
{
	/* Some nodes are just a bit less efficient, but need no fixing if the
	 * same_as requirement is not fulfilled. */
	if (!need_constraint_copy(node))
		return true;
	fix_am_source(node, reg);
	if (req->same_as_next) {
		arch_register_t const *const next_reg = arch_get_irn_register_in(node, req->same_as + 1);
		if (reg == next_reg) {
			if (is_ia32_ShlD(node)) {
				ia32_transform_ShlD_to_ShrD_imm(node, reg);
			} else {
				assert(req->same_as == n_ia32_binary_left);
				assert(is_ia32_commutative(node));
				DBG((dbg, LEVEL_1, "swap left/right input of %+F to resolve should be same constraint\n", node));
				ia32_swap_left_right(node);
			}
			return true;
		}
	} else if (is_ia32_Sub(node) || is_ia32_Sbb(node) || is_ia32_xSub(node)) {
		return ia32_transform_sub_to_neg_add(node, reg);
	}
	DBG((dbg, LEVEL_1, "create copy for should be same argument at input %d of %+F\n", req->same_as, node));
	return false;
}

/**
 * Add Copy nodes for not fulfilled should_be_equal constraints
 */
void ia32_finish_irg(ir_graph *irg)
{
	be_handle_2addr(irg, &ia32_handle_2addr);
}

void ia32_init_finish(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.be.ia32.finish");
}
