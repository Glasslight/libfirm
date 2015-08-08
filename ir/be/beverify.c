/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief       Various verify routines that check a scheduled graph for correctness.
 * @author      Matthias Braun
 * @date        05.05.2006
 */
#include "beverify.h"

#include "array.h"
#include "beirg.h"
#include "belistsched.h"
#include "belive.h"
#include "benode.h"
#include "besched.h"
#include "bitset.h"
#include "irdump_t.h"
#include "iredges.h"
#include "irgwalk.h"
#include "irnode.h"
#include "irprintf.h"
#include "irverify_t.h"
#include "set.h"

static bool check_value_constraint(const ir_node *node)
{
	const arch_register_req_t   *req  = arch_get_irn_register_req(node);
	const arch_register_class_t *cls  = req->cls;
	ir_mode                     *mode = get_irn_mode(node);

	bool fine = true;
	if (!cls->mode) {
		if (is_Proj(node)) {
			verify_warn(node, "Value with class %s must not have a Proj", cls->name);
			fine = false;
		} else if (mode != mode_ANY) {
			verify_warn(node, "Value with class %s must have mode %+F", cls->name, mode_ANY);
			fine = false;
		}
	} else if (mode != cls->mode) {
		verify_warn(node, "Value with register class %s should have mode %+F but has %+F",
					cls->name, cls->mode, mode);
		fine = false;
	}

	return fine;
}

static void warn_constr(const ir_node *node, const char *type, unsigned n,
                        const char *format, ...)
{
	verify_warn_prefix(node);
	fprintf(stderr, "reqs %s %u: ", type, n);
	va_list ap;
	va_start(ap, format);
	ir_vfprintf(stderr, format, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static bool check_reg_constraint(const ir_node *node,
                                 const arch_register_req_t *req,
                                 const arch_register_t *reg, const char *type,
                                 unsigned n)
{
	bool fine = true;
	const arch_register_class_t *cls = req->cls;
	if (req->width > cls->n_regs || (req->width == 0 && cls->n_regs != 0)) {
		warn_constr(node, type, n, "invalid width %u", (unsigned)req->width);
		fine = false;
	}

	if (reg != NULL && reg->cls != cls) {
		warn_constr(node, type, n, "register %s does not match class %s",
		            reg->name, cls->name);
		fine = false;
	} else if (reg != NULL) {
		unsigned reg_index = reg->index;
		if (!arch_reg_is_allocatable(req, reg)) {
			warn_constr(node, type, n, "register %s not allowed (limited)",
						reg->name);
			fine = false;
		}
		if (req->must_be_different) {
			foreach_irn_in(node, i, in) {
				(void)in;
				if (!rbitset_is_set(&req->must_be_different, i))
					continue;

				const arch_register_req_t *in_req
					= arch_get_irn_register_req_in(node, i);
				if (in_req->cls != cls) {
					warn_constr(node, type, n, "must_be_different input %u has class %s should be %s",
								i, in_req->cls!=NULL ? in_req->cls->name:"NULL",
								cls!=NULL?cls->name:"NULL");
					fine = false;
				}
				const arch_register_t *in_reg
					= arch_get_irn_register_in(node, i);
				if (in_reg != NULL && reg == in_reg) {
					warn_constr(node, type, n, "register %s not different from input %u",
								reg->name, i);
					fine = false;
				}
			}
		}
		if (reg_index + req->width - 1 > cls->n_regs) {
			warn_constr(node, type, n,
			            "register width constraint not fulfilled");
			fine = false;
		}
		if (req->aligned && reg->index % req->width != 0) {
			warn_constr(node, type, n,
			            "register alignment constraint not fulfilled");
			fine = false;
		}
	}
	return fine;
}

bool be_verify_node(const ir_node *node)
{
	if (is_Proj(node))
		return check_value_constraint(node);
	/** Only schedulable nodes are real instructions and require constraints */
	if (arch_is_irn_not_scheduled(node))
		return true;

	bool fine = true;
	if (get_irn_mode(node) != mode_T)
		fine &= check_value_constraint(node);

	be_foreach_out(node, o) {
		const arch_register_req_t *req
			= arch_get_irn_register_req_out(node, o);
		const arch_register_t *reg = arch_get_irn_register_out(node, o);
		fine &= check_reg_constraint(node, req, reg, "output", o);
	}
	foreach_irn_in(node, i, in) {
		if (is_Dummy(in))
			continue;

		const arch_register_req_t *req
			 = arch_get_irn_register_req_in(node, i);
		const arch_register_t *reg = arch_get_irn_register_in(node, i);
		fine &= check_reg_constraint(node, req, reg, "input", i);

		const arch_register_req_t *in_req = arch_get_irn_register_req(in);
		if (in_req->cls != req->cls) {
			warn_constr(node, "input", i,
						"input class %s does not match value class %s (%+F)",
						in_req->cls!=NULL?in_req->cls->name:"NULL",
						req->cls!=NULL?req->cls->name:"NULL", in);
			fine = false;
		}
		if (in_req->width < req->width) {
			warn_constr(node, "input", i,
			            "register width is too small: %u need at least %u",
			            in_req->width, req->width);
			fine = false;
		}
	}
	return fine;
}

static bool my_values_interfere(const ir_node *a, const ir_node *b);

typedef struct be_verify_register_pressure_env_t_ {
	be_lv_t                     *lv;                  /**< Liveness information. */
	const arch_register_class_t *cls;                 /**< the register class to check for */
	unsigned                    registers_available;  /**< number of available registers */
	bool                        problem_found;        /**< flag indicating if a problem was found */
} be_verify_register_pressure_env_t;

/**
 * Print all nodes of a pset into a file.
 */
static void print_living_values(FILE *F, const ir_nodeset_t *live_nodes)
{
	ir_fprintf(F, "\t");
	foreach_ir_nodeset(live_nodes, node, iter) {
		ir_fprintf(F, "%+F ", node);
	}
	ir_fprintf(F, "\n");
}

static void verify_warnf(ir_node const *const node, char const *const fmt, ...)
{
	FILE* const f = stderr;

	ir_node const *const block    = get_block_const(node);
	ir_graph      *const irg      = get_irn_irg(node);
	ir_entity     *const irg_ent  = get_irg_entity(irg);
	char    const *const irg_name = get_entity_ld_name(irg_ent);
	ir_fprintf(f, "%+F(%s): verify warning: ", block, irg_name);
	va_list ap;
	va_start(ap, fmt);
	ir_vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
}

/**
 * Check if number of live nodes never exceeds the number of available registers.
 */
static void verify_liveness_walker(ir_node *block, void *data)
{
	be_verify_register_pressure_env_t *env = (be_verify_register_pressure_env_t *)data;
	ir_nodeset_t live_nodes;

	/* collect register pressure info, start with end of a block */
	ir_nodeset_init(&live_nodes);
	be_liveness_end_of_block(env->lv, env->cls, block,
	                         &live_nodes);

	unsigned pressure = ir_nodeset_size(&live_nodes);
	if (pressure > env->registers_available) {
		verify_warnf(block, "register pressure too high at end of block (%d/%d):",
			pressure, env->registers_available);
		print_living_values(stderr, &live_nodes);
		env->problem_found = true;
	}

	sched_foreach_non_phi_reverse(block, irn) {
		// print_living_values(stderr, &live_nodes);
		be_liveness_transfer(env->cls, irn, &live_nodes);

		pressure = ir_nodeset_size(&live_nodes);

		if (pressure > env->registers_available) {
			verify_warnf(block, "register pressure too high before %+F (%d/%d):",
				irn, pressure, env->registers_available);
			print_living_values(stderr, &live_nodes);
			env->problem_found = true;
		}
	}
	ir_nodeset_destroy(&live_nodes);
}

bool be_verify_register_pressure(ir_graph *irg, const arch_register_class_t *cls)
{
	be_verify_register_pressure_env_t env;
	env.lv                  = be_liveness_new(irg);
	env.cls                 = cls;
	env.registers_available = be_get_n_allocatable_regs(irg, cls);
	env.problem_found       = false;

	be_liveness_compute_sets(env.lv);
	irg_block_walk_graph(irg, verify_liveness_walker, NULL, &env);
	be_liveness_free(env.lv);

	return ! env.problem_found;
}

/*--------------------------------------------------------------------------- */

typedef struct be_verify_schedule_env_t_ {
	bool      problem_found; /**< flags indicating a problem */
	bitset_t *scheduled;     /**< bitset of scheduled nodes */
} be_verify_schedule_env_t;

static void verify_schedule_walker(ir_node *block, void *data)
{
	be_verify_schedule_env_t *env = (be_verify_schedule_env_t*) data;

	/*
	 * Tests for the following things:
	 *   1. Make sure that all phi nodes are scheduled at the beginning of the
	 *      block
	 *   2. No value is defined after it has been used
	 */
	ir_node         *non_phi_found  = NULL;
	ir_node         *cfchange_found = NULL;
	sched_timestep_t last_timestep  = 0;
	sched_foreach(block, node) {
		/* this node is scheduled */
		if (bitset_is_set(env->scheduled, get_irn_idx(node))) {
			verify_warnf(block, "%+F appears to be schedule twice");
			env->problem_found = true;
		}
		bitset_set(env->scheduled, get_irn_idx(node));

		/* Check that scheduled nodes are in the correct block */
		if (get_nodes_block(node) != block) {
			verify_warnf(block, "%+F is in wrong %+F", node, get_nodes_block(node));
			env->problem_found = true;
		}

		/* Check that timesteps are increasing */
		sched_timestep_t timestep = sched_get_time_step(node);
		if (timestep <= last_timestep) {
			verify_warnf(block, "schedule timestep did not increase at %+F", node);
			env->problem_found = true;
		}
		last_timestep = timestep;

		if (arch_get_irn_flags(node) & arch_irn_flag_not_scheduled) {
			verify_warnf(block, "flag_not_scheduled node %+F scheduled anyway", node);
			env->problem_found = true;
		}

		/* Check that phis come before any other node */
		if (!is_Phi(node)) {
			non_phi_found = node;
		} else if (non_phi_found) {
			verify_warnf(block, "%+F scheduled after non-Phi %+F", node, non_phi_found);
			env->problem_found = true;
		}

		/* Check for control flow changing nodes */
		if (is_cfop(node)) {
			/* check, that only one CF operation is scheduled */
			if (cfchange_found != NULL) {
				verify_warnf(block, "additional control flow changing node %+F scheduled after %+F", node, cfchange_found);
				env->problem_found = true;
			} else {
				cfchange_found = node;
			}
		} else if (cfchange_found != NULL) {
			/* keepany isn't a real instruction. */
			if (!be_is_Keep(node)) {
				verify_warnf(block, "%+F scheduled after control flow changing node", node);
				env->problem_found = true;
			}
		}

		/* Check that all uses come before their definitions */
		if (!is_Phi(node)) {
			sched_timestep_t nodetime = sched_get_time_step(node);
			foreach_irn_in(node, i, arg) {
				if (get_nodes_block(arg) != block || !sched_is_scheduled(arg))
					continue;

				if (sched_get_time_step(arg) >= nodetime) {
					verify_warnf(block, "%+F used by %+F before it was defined", arg, node);
					env->problem_found = true;
				}
			}
		}

		/* Check that no dead nodes are scheduled */
		if (get_irn_n_edges(node) == 0) {
			verify_warnf(block, "%+F is dead but scheduled", node);
			env->problem_found = true;
		}

		if (be_is_Keep(node) || be_is_CopyKeep(node)) {
			/* at least 1 of the keep arguments has to be its schedule
			 * predecessor */
			ir_node *prev = sched_prev(node);
			while (be_is_Keep(prev) || be_is_CopyKeep(prev))
				prev = sched_prev(prev);

			do {
				foreach_irn_in(node, i, in) {
					if (skip_Proj(in) == prev)
						goto ok;
				}
				prev = sched_prev(prev);
			} while (is_Phi(prev));
			verify_warnf(block, "%+F not scheduled after its pred node", node);
			env->problem_found = true;
ok:;
		}
	}
}

static void check_schedule(ir_node *node, void *data)
{
	be_verify_schedule_env_t *env = (be_verify_schedule_env_t*)data;
	bool const should_be = !arch_is_irn_not_scheduled(node);
	bool const scheduled = bitset_is_set(env->scheduled, get_irn_idx(node));

	if (should_be != scheduled) {
		verify_warnf(node, "%+F should%s be scheduled", node, should_be ? "" : " not");
		env->problem_found = true;
	}
}

bool be_verify_schedule(ir_graph *irg)
{
	be_verify_schedule_env_t env;
	env.problem_found = false;
	env.scheduled     = bitset_alloca(get_irg_last_idx(irg));

	irg_block_walk_graph(irg, verify_schedule_walker, NULL, &env);
	/* check if all nodes are scheduled */
	irg_walk_graph(irg, check_schedule, NULL, &env);

	return ! env.problem_found;
}

/*--------------------------------------------------------------------------- */

typedef struct spill_t {
	ir_node *spill;
	ir_entity *ent;
} spill_t;

typedef struct {
	set                  *spills;
	ir_node             **reloads;
	bool                  problem_found;
	get_frame_entity_func get_frame_entity;
} be_verify_spillslots_env_t;

static int cmp_spill(const void* d1, const void* d2, size_t size)
{
	(void) size;
	const spill_t* s1 = (const spill_t*)d1;
	const spill_t* s2 = (const spill_t*)d2;
	return s1->spill != s2->spill;
}

static spill_t *find_spill(be_verify_spillslots_env_t *env, ir_node *node)
{
	spill_t spill;
	spill.spill = node;
	return set_find(spill_t, env->spills, &spill, sizeof(spill), hash_ptr(node));
}

static spill_t *get_spill(be_verify_spillslots_env_t *env, ir_node *node, ir_entity *ent)
{
	int hash = hash_ptr(node);
	spill_t spill;
	spill.spill = node;
	spill_t *res = set_find(spill_t, env->spills, &spill, sizeof(spill), hash);

	if (res == NULL) {
		spill.ent = ent;
		res = set_insert(spill_t, env->spills, &spill, sizeof(spill), hash);
	}

	return res;
}

static ir_node *get_memory_edge(const ir_node *node)
{
	ir_node *result = NULL;
	foreach_irn_in_r(node, i, arg) {
		if (get_irn_mode(arg) == mode_M) {
			assert(result == NULL);
			result = arg;
		}
	}

	return result;
}

static void collect(be_verify_spillslots_env_t *env, ir_node *node, ir_node *reload, ir_entity* ent);

static void be_check_entity(ir_node *node, ir_entity *ent)
{
	if (ent == NULL)
		verify_warnf(node, "%+F should have an entity assigned", node);
}

static void collect_spill(be_verify_spillslots_env_t *env, ir_node *node, ir_node *reload, ir_entity* ent)
{
	ir_entity *spillent = env->get_frame_entity(node);
	be_check_entity(node, spillent);
	get_spill(env, node, ent);

	if (spillent != ent) {
		verify_warnf(node, "spill %+F has different entity than reload %+F", node, reload);
		env->problem_found = true;
	}
}

static void collect_memperm(be_verify_spillslots_env_t *env, ir_node *node, ir_node *reload, ir_entity* ent)
{
	ir_node *memperm = get_Proj_pred(node);
	unsigned out     = get_Proj_num(node);

	ir_entity *spillent = be_get_MemPerm_out_entity(memperm, out);
	be_check_entity(memperm, spillent);
	if (spillent != ent) {
		verify_warnf(node, "MemPerm %+F has different entity than reload %+F", node, reload);
		env->problem_found = true;
	}

	int hash = hash_ptr(node);
	spill_t spill;
	spill.spill = node;
	spill_t *res = set_find(spill_t, env->spills, &spill, sizeof(spill), hash);
	if (res != NULL) {
		return;
	}

	spill.ent = spillent;
	(void)set_insert(spill_t, env->spills, &spill, sizeof(spill), hash);

	int arity = be_get_MemPerm_entity_arity(memperm);
	for (int i = 0; i < arity; ++i) {
		ir_node   *const arg    = get_irn_n(memperm, i);
		ir_entity *const argent = be_get_MemPerm_in_entity(memperm, i);

		collect(env, arg, memperm, argent);
	}
}

static void collect_memphi(be_verify_spillslots_env_t *env, ir_node *node, ir_node *reload, ir_entity *ent)
{
	assert(is_Phi(node));

	int hash = hash_ptr(node);
	spill_t spill;
	spill.spill = node;
	spill_t *res = set_find(spill_t, env->spills, &spill, sizeof(spill), hash);
	if (res != NULL) {
		return;
	}

	spill.ent = ent;
	(void)set_insert(spill_t, env->spills, &spill, sizeof(spill), hash);

	/* is 1 of the arguments a spill? */
	foreach_irn_in(node, i, arg) {
		collect(env, arg, reload, ent);
	}
}

static void collect(be_verify_spillslots_env_t *env, ir_node *node, ir_node *reload, ir_entity* ent)
{
	if (arch_irn_is(node, spill)) {
		collect_spill(env, node, reload, ent);
	} else if (is_Proj(node)) {
		collect_memperm(env, node, reload, ent);
	} else if (is_Phi(node) && get_irn_mode(node) == mode_M) {
		collect_memphi(env, node, reload, ent);
	}
}

/**
 * This walker function searches for reloads and collects all the spills
 * and memphis attached to them.
 */
static void collect_spills_walker(ir_node *node, void *data)
{
	be_verify_spillslots_env_t *env = (be_verify_spillslots_env_t*)data;

	if (arch_irn_is(node, reload)) {
		ir_node *spill = get_memory_edge(node);
		if (spill == NULL) {
			verify_warnf(node, "no spill attached to reload %+F", node);
			env->problem_found = true;
			return;
		}
		ir_entity *ent = env->get_frame_entity(node);
		be_check_entity(node, ent);

		collect(env, spill, node, ent);
		ARR_APP1(ir_node*, env->reloads, node);
	}
}

static void check_spillslot_interference(be_verify_spillslots_env_t *env)
{
	int       spillcount = set_count(env->spills);
	spill_t **spills     = ALLOCAN(spill_t*, spillcount);

	int s = 0;
	foreach_set(env->spills, spill_t, spill) {
		spills[s++] = spill;
	}
	assert(s == spillcount);

	for (int i = 0; i < spillcount; ++i) {
		spill_t *sp1 = spills[i];

		for (int i2 = i+1; i2 < spillcount; ++i2) {
			spill_t *sp2 = spills[i2];

			if (sp1->ent != sp2->ent)
				continue;

			if (my_values_interfere(sp1->spill, sp2->spill)) {
				verify_warnf(sp1->spill, "spillslots for %+F and %+F (in %+F) interfere",
						sp1->spill, sp2->spill, get_nodes_block(sp2->spill));
				env->problem_found = true;
			}
		}
	}
}

static void check_lonely_spills(ir_node *node, void *data)
{
	be_verify_spillslots_env_t *env = (be_verify_spillslots_env_t*)data;

	if (arch_irn_is(node, spill)
	    || (is_Proj(node) && be_is_MemPerm(get_Proj_pred(node)))) {
		spill_t *spill = find_spill(env, node);
		if (arch_irn_is(node, spill)) {
			ir_entity *ent = env->get_frame_entity(node);
			be_check_entity(node, ent);
		}

		if (spill == NULL)
			verify_warnf(node, "%+F not connected to a reload", node);
	}
}

bool be_verify_spillslots(ir_graph *irg, get_frame_entity_func get_frame_entity)
{
	be_verify_spillslots_env_t env;
	env.spills           = new_set(cmp_spill, 10);
	env.reloads          = NEW_ARR_F(ir_node*, 0);
	env.problem_found    = false;
	env.get_frame_entity = get_frame_entity;

	irg_walk_graph(irg, collect_spills_walker, NULL, &env);
	irg_walk_graph(irg, check_lonely_spills, NULL, &env);

	check_spillslot_interference(&env);

	DEL_ARR_F(env.reloads);
	del_set(env.spills);

	return ! env.problem_found;
}

/*--------------------------------------------------------------------------- */

/**
 * Check, if two values interfere.
 * @param a The first value.
 * @param b The second value.
 * @return 1, if a and b interfere, 0 if not.
 */
static bool my_values_interfere(const ir_node *a, const ir_node *b)
{
	assert(a != b);
	int a2b = value_strictly_dominates(a, b);
	int b2a = value_strictly_dominates(b, a);

	/* If there is no dominance relation, they do not interfere. */
	if (!a2b && !b2a)
		return false;

	/*
	 * Adjust a and b so, that a dominates b if
	 * a dominates b or vice versa.
	 */
	if (b2a) {
		const ir_node *t = a;
		a = b;
		b = t;
	}

	ir_node *bb = get_nodes_block(b);

	/*
	 * Look at all usages of a.
	 * If there's one usage of a in the block of b, then
	 * we check, if this use is dominated by b, if that's true
	 * a and b interfere. Note that b must strictly dominate the user,
	 * since if b is the last user of in the block, b and a do not
	 * interfere.
	 * Uses of a not in b's block can be disobeyed, because the
	 * check for a being live at the end of b's block is already
	 * performed.
	 */
	foreach_out_edge(a, edge) {
		const ir_node *user = get_edge_src_irn(edge);
		if (b == user)
			continue;

		if (is_End(user))
			continue;

		/* in case of phi arguments we compare with the block the value comes from */
		if (is_Phi(user)) {
			ir_node *phiblock = get_nodes_block(user);
			if (phiblock == bb)
				continue;
			user = get_irn_n(phiblock, get_edge_src_pos(edge));
		}

		if (value_strictly_dominates(b, user))
			return true;
	}

	return false;
}

/*--------------------------------------------------------------------------- */

typedef struct be_verify_reg_alloc_env_t {
	be_lv_t *lv;
	bool     problem_found;
} be_verify_reg_alloc_env_t;

static void check_output_constraints(be_verify_reg_alloc_env_t *const env, const ir_node *node)
{
	arch_register_req_t const *const req = arch_get_irn_register_req(node);
	if (!req->cls->regs)
		return;

	/* verify output register */
	arch_register_t const *const reg = arch_get_irn_register(node);
	if (reg == NULL) {
		verify_warnf(node, "%+F should have a register assigned", node);
		env->problem_found = true;
	} else if (!arch_reg_is_allocatable(req, reg)) {
		verify_warnf(node, "register %s assigned as output of %+F not allowed (register constraint)", reg->name, node);
		env->problem_found = true;
	}
}

static void check_input_constraints(be_verify_reg_alloc_env_t *const env, ir_node *const node)
{
	arch_register_req_t const **const in_reqs = arch_get_irn_register_reqs_in(node);
	if (!in_reqs && get_irn_arity(node) != 0) {
		verify_warnf(node, "%+F has no input requirements", node);
		env->problem_found = true;
		return;
	}

	/* verify input register */
	foreach_irn_in(node, i, pred) {
		if (is_Bad(pred)) {
			verify_warnf(node, "%+F has Bad as input %d", node, i);
			env->problem_found = true;
			continue;
		}

		const arch_register_req_t *req      = arch_get_irn_register_req_in(node, i);
		const arch_register_req_t *pred_req = arch_get_irn_register_req(pred);
		if (req->cls != pred_req->cls) {
			verify_warnf(node, "%+F register class of requirement at input %d and operand differ", node, i);
			env->problem_found = true;
		}

		if (!req->cls->regs)
			continue;

		if (req->width > pred_req->width) {
			verify_warnf(node, "%+F register width of value at input %d too small", node, i);
			env->problem_found = true;
		}

		const arch_register_t *reg = arch_get_irn_register(pred);
		if (reg == NULL) {
			verify_warnf(pred, "%+F should have a register assigned (%+F input constraint)", pred, node);
			env->problem_found = true;
		} else if (!arch_reg_is_allocatable(req, reg)) {
			verify_warnf(node, "register %s as input %d of %+F not allowed (register constraint)", reg->name, i, node);
			env->problem_found = true;
		}
	}

	/* phis should be NOPs at this point, which means all input regs
	 * must be the same as the output reg */
	if (is_Phi(node)) {
		const arch_register_t *reg = arch_get_irn_register(node);
		foreach_irn_in(node, i, pred) {
			const arch_register_t *pred_reg = arch_get_irn_register(pred);

			if (reg != pred_reg && !(pred_reg->is_virtual)) {
				const char *pred_name = pred_reg != NULL ? pred_reg->name : "(null)";
				const char *reg_name  = reg != NULL ? reg->name : "(null)";
				verify_warnf(node, "input %d of %+F uses register %s instead of %s", i, node, pred_name, reg_name);
				env->problem_found = true;
			}
		}
	}
}

static bool ignore_error_for_reg(ir_graph *irg, const arch_register_t *reg)
{
	be_irg_t *birg = be_birg_from_irg(irg);
	if (birg->non_ssa_regs == NULL)
		return false;
	return rbitset_is_set(birg->non_ssa_regs, reg->global_index);
}

static void value_used(be_verify_reg_alloc_env_t *const env, ir_node const **const registers, ir_node const *const block, ir_node const *const node)
{
	const arch_register_t *reg = arch_get_irn_register(node);
	if (reg == NULL || reg->is_virtual)
		return;

	const arch_register_req_t *req = arch_get_irn_register_req(node);
	assert(req->width > 0);
	unsigned idx = reg->global_index;
	for (unsigned i = 0; i < req->width; ++i) {
		ir_node const *const reg_node = registers[idx + i];
		if (reg_node != NULL && reg_node != node
			&& !ignore_error_for_reg(get_irn_irg(block), reg)) {
			verify_warnf(block, "register %s assigned more than once (nodes %+F and %+F)", reg->name, node, reg_node);
			env->problem_found = true;
		}
		registers[idx + i] = node;
	}
}

static void value_def(be_verify_reg_alloc_env_t *const env, ir_node const **const registers, ir_node const *const node)
{
	const arch_register_t *reg = arch_get_irn_register(node);

	if (reg == NULL || reg->is_virtual)
		return;

	const arch_register_req_t *req = arch_get_irn_register_req(node);
	assert(req->width > 0);
	unsigned idx = reg->global_index;
	for (unsigned i = 0; i < req->width; ++i) {
		ir_node const *const reg_node = registers[idx + i];

		/* a little cheat, since its so hard to remove all outedges to dead code
		 * in the backend. This particular case should never be a problem. */
		if (reg_node == NULL && get_irn_n_edges(node) == 0)
			return;

		if (reg_node != node && !ignore_error_for_reg(get_irn_irg(node), reg)) {
			verify_warnf(node, "%+F not registered as value for register %s (but %+F)", node, reg->name, reg_node);
			env->problem_found = true;
		}
		registers[idx + i] = NULL;
	}
}

static void verify_block_register_allocation(ir_node *block, void *data)
{
	be_verify_reg_alloc_env_t *const env = (be_verify_reg_alloc_env_t*)data;

	unsigned        const n_regs    = isa_if->n_registers;
	ir_node const **const registers = ALLOCANZ(ir_node const*, n_regs);

	be_lv_foreach(env->lv, block, be_lv_state_end, lv_node) {
		value_used(env, registers, block, lv_node);
	}

	sched_foreach_reverse(block, node) {
		be_foreach_value(node, value,
			value_def(env, registers, value);
			check_output_constraints(env, value);
		);

		check_input_constraints(env, node);

		/* process uses. (Phi inputs are no real uses) */
		if (!is_Phi(node)) {
			foreach_irn_in(node, i, use) {
				value_used(env, registers, block, use);
			}
		}
	}

	be_lv_foreach(env->lv, block, be_lv_state_in, lv_node) {
		value_def(env, registers, lv_node);
	}

	/* set must be empty now */
	for (unsigned i = 0; i < n_regs; ++i) {
		if (registers[i]) {
			verify_warnf(block, "%+F not live-in and no def found", registers[i]);
			env->problem_found = true;
		}
	}
}

bool be_verify_register_allocation(ir_graph *const irg)
{
	be_verify_reg_alloc_env_t env = {
		.lv                 = be_liveness_new(irg),
		.problem_found      = false,
	};

	be_liveness_compute_sets(env.lv);
	irg_block_walk_graph(irg, verify_block_register_allocation, NULL, &env);
	be_liveness_free(env.lv);

	return !env.problem_found;
}

/*--------------------------------------------------------------------------- */

typedef struct lv_walker_t {
	be_lv_t *given;
	be_lv_t *fresh;
} lv_walker_t;

static const char *lv_flags_to_str(unsigned flags)
{
	static const char *states[] = {
		"---",
		"i--",
		"-e-",
		"ie-",
		"--o",
		"i-o",
		"-eo",
		"ieo"
	};

	return states[flags & 7];
}

static void lv_check_walker(ir_node *bl, void *data)
{
	lv_walker_t  *const w    = (lv_walker_t*)data;
	be_lv_info_t *const curr = ir_nodehashmap_get(be_lv_info_t, &w->given->map, bl);
	be_lv_info_t *const fr   = ir_nodehashmap_get(be_lv_info_t, &w->fresh->map, bl);

	if (!fr && curr && curr->n_members > 0) {
		ir_fprintf(stderr, "%+F liveness should be empty but current liveness contains:\n", bl);
		for (unsigned i = 0; i < curr->n_members; ++i) {
			ir_fprintf(stderr, "\t%+F\n", curr->nodes[i].node);
		}
	} else if (curr) {
		unsigned const n_curr  = curr->n_members;
		unsigned const n_fresh = fr->n_members;
		if (n_curr != n_fresh) {
			ir_fprintf(stderr, "%+F: liveness set sizes differ. curr %d, correct %d\n", bl, n_curr, n_fresh);

			ir_fprintf(stderr, "current:\n");
			for (unsigned i = 0; i < n_curr; ++i) {
				be_lv_info_node_t *const n = &curr->nodes[i];
				ir_fprintf(stderr, "%+F %u %+F %s\n", bl, i, n->node, lv_flags_to_str(n->flags));
			}

			ir_fprintf(stderr, "correct:\n");
			for (unsigned i = 0; i < n_fresh; ++i) {
				be_lv_info_node_t *const n = &fr->nodes[i];
				ir_fprintf(stderr, "%+F %u %+F %s\n", bl, i, n->node, lv_flags_to_str(n->flags));
			}
		}
	}
}

void be_liveness_check(be_lv_t *lv)
{
	be_lv_t *const fresh = be_liveness_new(lv->irg);
	be_liveness_compute_sets(fresh);
	lv_walker_t w = {
		.given = lv,
		.fresh = fresh,
	};
	irg_block_walk_graph(lv->irg, lv_check_walker, NULL, &w);
	be_liveness_free(fresh);
}
