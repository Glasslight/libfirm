/*
 * This file is part of libFirm.
 * Copyright (C) 2014 University of Karlsruhe.
 */

/**
 * @file
 * @brief   arm graph touchups before emitting
 * @author  Matthias Braun
 */
#include "arm_bearch_t.h"

#include "arm_new_nodes.h"
#include "arm_optimize.h"
#include "be2addr.h"
#include "beirg.h"
#include "benode.h"
#include "besched.h"
#include "bespillslots.h"
#include "bestack.h"
#include "be_types.h"
#include "firm_types.h"
#include "gen_arm_regalloc_if.h"
#include "iredges_t.h"
#include "irgwalk.h"
#include "panic.h"

static bool is_frame_load(const ir_node *node)
{
	return is_arm_Ldr(node) || is_arm_Ldf(node);
}

static void arm_collect_frame_entity_nodes(ir_node *node, void *data)
{
	if (!is_frame_load(node))
		return;

	const arm_load_store_attr_t *attr = get_arm_load_store_attr_const(node);
	if (!attr->is_frame_entity)
		return;
	const ir_entity *entity = attr->entity;
	if (entity != NULL)
		return;
	const ir_mode *mode = attr->load_store_mode;
	const ir_type *type = get_type_for_mode(mode);

	be_fec_env_t *env = (be_fec_env_t*)data;
	be_load_needs_frame_entity(env, node, type);
}

static void arm_set_frame_entity(ir_node *node, ir_entity *entity,
                                 const ir_type *type)
{
	(void)type;
	arm_load_store_attr_t *attr = get_arm_load_store_attr(node);
	attr->entity = entity;
}

static void introduce_epilog(ir_node *ret)
{
	arch_register_t const *const sp_reg = &arm_registers[REG_SP];
	assert(arch_get_irn_register_req_in(ret, n_arm_Return_sp) == sp_reg->single_req);

	ir_node  *const sp         = get_irn_n(ret, n_arm_Return_sp);
	ir_node  *const block      = get_nodes_block(ret);
	ir_graph *const irg        = get_irn_irg(ret);
	ir_type  *const frame_type = get_irg_frame_type(irg);
	unsigned  const frame_size = get_type_size(frame_type);
	ir_node  *const incsp      = be_new_IncSP(sp_reg, block, sp, -frame_size, 0);
	set_irn_n(ret, n_arm_Return_sp, incsp);
	sched_add_before(ret, incsp);
}

static void introduce_prolog_epilog(ir_graph *irg)
{
	/* introduce epilog for every return node */
	foreach_irn_in(get_irg_end_block(irg), i, ret) {
		assert(is_arm_Return(ret));
		introduce_epilog(ret);
	}

	const arch_register_t *sp_reg     = &arm_registers[REG_SP];
	ir_node               *start      = get_irg_start(irg);
	ir_node               *block      = get_nodes_block(start);
	ir_node               *initial_sp = be_get_Start_proj(irg, sp_reg);
	ir_type               *frame_type = get_irg_frame_type(irg);
	unsigned               frame_size = get_type_size(frame_type);

	ir_node *const incsp = be_new_IncSP(sp_reg, block, initial_sp, frame_size, 0);
	edges_reroute_except(initial_sp, incsp, incsp);
	sched_add_after(start, incsp);
}

/**
 * This function is called by the generic backend to correct offsets for
 * nodes accessing the stack.
 */
static void arm_set_frame_offset(ir_node *irn, int bias)
{
	if (be_is_MemPerm(irn)) {
		be_set_MemPerm_offset(irn, bias);
	} else if (is_arm_FrameAddr(irn)) {
		arm_Address_attr_t *attr = get_arm_Address_attr(irn);
		attr->fp_offset += bias;
	} else {
		arm_load_store_attr_t *attr = get_arm_load_store_attr(irn);
		assert(attr->base.is_load_store);
		attr->offset += bias;
	}
}

static int arm_get_sp_bias(const ir_node *node)
{
	(void)node;
	return 0;
}

static ir_entity *arm_get_frame_entity(const ir_node *irn)
{
	if (be_is_MemPerm(irn))
		return be_get_MemPerm_in_entity(irn, 0);
	if (!is_arm_irn(irn))
		return NULL;
	const arm_attr_t *attr = get_arm_attr_const(irn);
	if (is_arm_FrameAddr(irn)) {
		const arm_Address_attr_t *frame_attr = get_arm_Address_attr_const(irn);
		return frame_attr->entity;
	}
	if (attr->is_load_store) {
		const arm_load_store_attr_t *load_store_attr
			= get_arm_load_store_attr_const(irn);
		if (load_store_attr->is_frame_entity) {
			return load_store_attr->entity;
		}
	}
	return NULL;
}

void arm_finish_graph(ir_graph *irg)
{
	bool          omit_fp = arm_get_irg_data(irg)->omit_fp;
	be_fec_env_t *fec_env = be_new_frame_entity_coalescer(irg);

	irg_walk_graph(irg, NULL, arm_collect_frame_entity_nodes, fec_env);
	be_assign_entities(fec_env, arm_set_frame_entity, omit_fp);
	be_free_frame_entity_coalescer(fec_env);

	introduce_prolog_epilog(irg);

	/* fix stack entity offsets */
	be_fix_stack_nodes(irg, &arm_registers[REG_SP]);
	be_birg_from_irg(irg)->non_ssa_regs = NULL;
	be_abi_fix_stack_bias(irg, arm_get_sp_bias, arm_set_frame_offset,
	                      arm_get_frame_entity);

	/* do peephole optimizations and fix stack offsets */
	arm_peephole_optimization(irg);

	be_handle_2addr(irg, NULL);
}
