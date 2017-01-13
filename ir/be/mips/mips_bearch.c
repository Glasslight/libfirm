/*
 * This file is part of libFirm.
 * Copyright (C) 2017 University of Karlsruhe.
 */

#include "be_t.h"
#include "beirg.h"
#include "bemodule.h"
#include "bera.h"
#include "besched.h"
#include "bespillslots.h"
#include "bestack.h"
#include "betranshlp.h"
#include "gen_mips_new_nodes.h"
#include "gen_mips_regalloc_if.h"
#include "irarch_t.h"
#include "iredges.h"
#include "irgwalk.h"
#include "irprog_t.h"
#include "lower_dw.h"
#include "lowering.h"
#include "mips_bearch_t.h"
#include "mips_emitter.h"
#include "mips_transform.h"

static int mips_is_mux_allowed(ir_node *const sel, ir_node *const mux_false, ir_node *const mux_true)
{
	(void)sel;
	(void)mux_false;
	(void)mux_true;
	return false;
}

static ir_settings_arch_dep_t const mips_arch_dep = {
	.also_use_subs        = true,
	.maximum_shifts       = 4,
	.highest_shift_amount = MIPS_MACHINE_SIZE - 1,
	.evaluate             = NULL,
	.allow_mulhs          = true,
	.allow_mulhu          = true,
	.max_bits_for_mulh    = MIPS_MACHINE_SIZE,
};

static backend_params mips_backend_params = {
	.experimental                  = "the MIPS backend is highly experimental and unfinished",
	.byte_order_big_endian         = true,
	.pic_supported                 = false,
	.unaligned_memaccess_supported = false,
	.modulo_shift                  = MIPS_MACHINE_SIZE,
	.dep_param                     = &mips_arch_dep,
	.allow_ifconv                  = &mips_is_mux_allowed,
	.machine_size                  = MIPS_MACHINE_SIZE,
	.mode_float_arithmetic         = NULL,  /* will be set later */ // TODO
	.type_long_double              = NULL,  /* will be set later */ // TODO
	.stack_param_align             = 4,
	.float_int_overflow            = ir_overflow_indefinite,
};

static void mips_init_asm_constraints(void)
{
	be_set_constraint_support(ASM_CONSTRAINT_FLAG_SUPPORTS_MEMOP,     "Rm");
	be_set_constraint_support(ASM_CONSTRAINT_FLAG_SUPPORTS_REGISTER,  "cdrvy");
	be_set_constraint_support(ASM_CONSTRAINT_FLAG_SUPPORTS_ANY,       "g");
	be_set_constraint_support(ASM_CONSTRAINT_FLAG_SUPPORTS_IMMEDIATE, "IJKLMNOPin");
}

static void mips_init(void)
{
	ir_mode *const ptr_mode = new_reference_mode("p32", MIPS_MACHINE_SIZE, MIPS_MACHINE_SIZE);
	set_modeP(ptr_mode);

	mips_init_asm_constraints();
	mips_create_opcodes();
	mips_register_init();
}

static void mips_finish(void)
{
	mips_free_opcodes();
}

static const backend_params *mips_get_libfirm_params(void)
{
	return &mips_backend_params;
}

static void mips_select_instructions(ir_graph *const irg)
{
	be_timer_push(T_CODEGEN);
	mips_transform_graph(irg);
	be_timer_pop(T_CODEGEN);
	be_dump(DUMP_BE, irg, "code-selection");

	place_code(irg);
	be_dump(DUMP_BE, irg, "place");
}

static ir_node *mips_new_spill(ir_node *const value, ir_node *const after)
{
	ir_mode *const mode = get_irn_mode(value);
	if (be_mode_needs_gp_reg(mode)) {
		ir_node  *const block = get_block(after);
		ir_graph *const irg   = get_irn_irg(after);
		ir_node  *const nomem = get_irg_no_mem(irg);
		ir_node  *const frame = get_irg_frame(irg);
		ir_node  *const store = new_bd_mips_sw(NULL, block, nomem, frame, value, NULL, 0);
		sched_add_after(after, store);
		return store;
	}
	panic("TODO");
}

static ir_node *mips_new_reload(ir_node *const value, ir_node *const spill, ir_node *const before)
{
	ir_mode *const mode = get_irn_mode(value);
	if (be_mode_needs_gp_reg(mode)) {
		ir_node  *const block = get_block(before);
		ir_graph *const irg   = get_irn_irg(before);
		ir_node  *const frame = get_irg_frame(irg);
		ir_node  *const load  = new_bd_mips_lw(NULL, block, spill, frame, NULL, 0);
		sched_add_before(before, load);
		return be_new_Proj(load, pn_mips_lw_res);
	}
	panic("TODO");
}

static regalloc_if_t const mips_regalloc_if = {
	.spill_cost  = 7,
	.reload_cost = 5,
	.new_spill   = mips_new_spill,
	.new_reload  = mips_new_reload,
};

static void mips_collect_frame_entity_nodes(ir_node *const node, void *const data)
{
	be_fec_env_t *const env = (be_fec_env_t*)data;

	if (is_mips_lw(node)) {
		ir_node  *const base  = get_irn_n(node, n_mips_lw_base);
		ir_graph *const irg   = get_irn_irg(node);
		ir_node  *const frame = get_irg_frame(irg);
		if (base == frame) {
			mips_immediate_attr_t const *const attr = get_mips_immediate_attr_const(node);
			if (!attr->ent) {
				unsigned const size     = MIPS_MACHINE_SIZE / 8; // TODO
				unsigned const po2align = log2_floor(size);
				be_load_needs_frame_entity(env, node, size, po2align);
			}
		}
	}
}

static void mips_set_frame_entity(ir_node *const node, ir_entity *const entity, unsigned const size, unsigned const po2align)
{
	(void)size, (void)po2align;

	mips_immediate_attr_t *const imm = get_mips_immediate_attr(node);
	imm->ent = entity;
}

static void mips_assign_spill_slots(ir_graph *const irg)
{
	be_fec_env_t *const fec_env = be_new_frame_entity_coalescer(irg);
	irg_walk_graph(irg, NULL, mips_collect_frame_entity_nodes, fec_env);
	be_assign_entities(fec_env, mips_set_frame_entity, true);
	be_free_frame_entity_coalescer(fec_env);
}

static ir_node *mips_new_IncSP(ir_node *const block, ir_node *const sp, int const offset, unsigned const align)
{
	return be_new_IncSP(&mips_registers[REG_SP], block, sp, offset, align);
}

static void mips_introduce_prologue(ir_graph *const irg, unsigned const size)
{
	ir_node *const start    = get_irg_start(irg);
	ir_node *const block    = get_nodes_block(start);
	ir_node *const start_sp = be_get_Start_proj(irg, &mips_registers[REG_SP]);
	ir_node *const inc_sp   = mips_new_IncSP(block, start_sp, size, 0);
	sched_add_after(start, inc_sp);
	edges_reroute_except(start_sp, inc_sp, inc_sp);
}

static void mips_introduce_epilogue(ir_node *const ret, unsigned const size)
{
	ir_node *const block  = get_nodes_block(ret);
	ir_node *const ret_sp = get_irn_n(ret, n_mips_ret_stack);
	ir_node *const inc_sp = mips_new_IncSP(block, ret_sp, -(int)size, 0);
	sched_add_before(ret, inc_sp);
	set_irn_n(ret, n_mips_ret_stack, inc_sp);
}

static void mips_introduce_prologue_epilogue(ir_graph *const irg)
{
	ir_type *const frame = get_irg_frame_type(irg);
	unsigned const size  = get_type_size(frame);
	if (size == 0)
		return;

	foreach_irn_in(get_irg_end_block(irg), i, ret) {
		assert(is_mips_ret(ret));
		mips_introduce_epilogue(ret, size);
	}

	mips_introduce_prologue(irg, size);
}

static void mips_sp_sim(ir_node *const node, stack_pointer_state_t *const state)
{
	if (is_mips_irn(node)) {
		switch ((mips_opcodes)get_mips_irn_opcode(node)) {
		case iro_mips_addiu:
		case iro_mips_lb:
		case iro_mips_lbu:
		case iro_mips_lh:
		case iro_mips_lhu:
		case iro_mips_lw:
		case iro_mips_sb:
		case iro_mips_sh:
		case iro_mips_sw: {
			mips_immediate_attr_t *const imm = get_mips_immediate_attr(node);
			ir_entity             *const ent = imm->ent;
			if (ent && is_frame_type(get_entity_owner(ent))) {
				imm->ent  = NULL;
				imm->val += state->offset + get_entity_offset(ent);
			}
			break;
		}

		default:
			break;
		}
	}
}

static void mips_generate_code(FILE *const output, char const *const cup_name)
{
	be_begin(output, cup_name);

	unsigned *const sp_is_non_ssa = rbitset_alloca(N_MIPS_REGISTERS);
	rbitset_set(sp_is_non_ssa, REG_SP);

	foreach_irp_irg(i, irg) {
		if (!be_step_first(irg))
			continue;

		be_irg_t *const birg = be_birg_from_irg(irg);
		birg->non_ssa_regs = sp_is_non_ssa;

		mips_select_instructions(irg);
		be_step_schedule(irg);
		be_step_regalloc(irg, &mips_regalloc_if);

		mips_assign_spill_slots(irg);

		ir_type *const frame = get_irg_frame_type(irg);
		be_sort_frame_entities(frame, true);
		be_layout_frame_type(frame, 0, 0);

		mips_introduce_prologue_epilogue(irg);
		be_fix_stack_nodes(irg, &mips_registers[REG_SP]);
		birg->non_ssa_regs = NULL;
		be_sim_stack_pointer(irg, 0, 3, &mips_sp_sim);

		mips_emit_function(irg);
		be_step_last(irg);
	}

	be_finish();
}

static ir_entity *mips_create_64_intrinsic_fkt(ir_type *const method, ir_op const *const op, ir_mode const *const imode, ir_mode const *const omode, void *const context)
{
	(void)method, (void)op, (void)imode, (void)omode, (void)context;
	panic("TODO");
}

static void mips_lower_add64(ir_node *const node, ir_mode *const mode)
{
	dbg_info *const dbg        = get_irn_dbg_info(node);
	ir_node  *const block      = get_nodes_block(node);
	ir_node  *const left       = get_Add_left(node);
	ir_node  *const right      = get_Add_right(node);
	ir_node  *const left_low   = get_lowered_low(left);
	ir_node  *const left_high  = get_lowered_high(left);
	ir_node  *const right_low  = get_lowered_low(right);
	ir_node  *const right_high = get_lowered_high(right);

	ir_node  *const res_low   = new_rd_Add(dbg, block, left_low,  right_low);
	ir_node  *const cmp_carry = new_rd_Cmp(dbg, block, res_low,   right_low, ir_relation_less);
	ir_graph *const irg       = get_irn_irg(node);
	ir_node  *const one       = new_r_Const(irg, get_mode_one(mode));
	ir_node  *const zero      = new_r_Const(irg, get_mode_null(mode));
	ir_node  *const carry     = new_rd_Mux(dbg, block, cmp_carry, zero, one);
	ir_node  *const sum_high  = new_rd_Add(dbg, block, left_high, right_high);
	ir_node  *const res_high  = new_rd_Add(dbg, block, sum_high,  carry);
	ir_set_dw_lowered(node, res_low, res_high);
}

static void mips_lower64(void)
{
	ir_mode *const word_unsigned = mips_reg_classes[CLASS_mips_gp].mode;
	ir_mode *const word_signed   = find_signed_mode(word_unsigned);
	lwrdw_param_t lower_dw_params = {
		.create_intrinsic = &mips_create_64_intrinsic_fkt,
		.word_unsigned    = word_unsigned,
		.word_signed      = word_signed,
		.doubleword_size  = 64,
		.big_endian       = be_is_big_endian(),
	};

	ir_prepare_dw_lowering(&lower_dw_params);
	ir_register_dw_lower_function(op_Add, mips_lower_add64);
	ir_lower_dw_ops();
}

static void mips_lower_for_target(void)
{
	ir_mode *const mode_gp = mips_reg_classes[CLASS_mips_gp].mode;
	foreach_irp_irg(i, irg) {
		lower_switch(irg, 4, 256, mode_gp);
		be_after_transform(irg, "lower-switch");
	}

	mips_lower64();
	be_after_irp_transform("lower-64");
}

static unsigned mips_get_op_estimated_cost(ir_node const *const node)
{
	(void)node; // TODO
	return 1;
}

static arch_isa_if_t const mips_isa_if = {
	.n_registers           = N_MIPS_REGISTERS,
	.registers             = mips_registers,
	.n_register_classes    = N_MIPS_CLASSES,
	.register_classes      = mips_reg_classes,
	.init                  = mips_init,
	.finish                = mips_finish,
	.get_params            = mips_get_libfirm_params,
	.generate_code         = mips_generate_code,
	.lower_for_target      = mips_lower_for_target,
	.is_valid_clobber      = NULL, // TODO
	.get_op_estimated_cost = mips_get_op_estimated_cost,
};

BE_REGISTER_MODULE_CONSTRUCTOR(be_init_arch_mips)
void be_init_arch_mips(void)
{
	be_register_isa_if("mips", &mips_isa_if);
}
