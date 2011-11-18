/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief   Compute loop depth information for PEG graphs.
 * @author  Olaf Liebe
 * @version $Id: $
 */

#include "config.h"

#include "vf_depth.h"
#include "irnodemap.h"
#include "plist.h"
#include "obstack.h"
#include "irtools.h"
#include "irgmod.h"
#include "util.h"

#define VLD_DEBUG_DEPTHS 0

typedef struct obstack obstack;

typedef struct vl_node {
	int depth;
} vl_node;

struct vl_info {
	obstack     obst;
	ir_graph   *irg;
	ir_nodemap  nodemap;
};

int vl_node_get_depth(vl_info *vli, ir_node *irn)
{
	vl_node *vln = ir_nodemap_get(&vli->nodemap, irn);
	assert(vln && "No depth information for the given node.");
	assert(vln->depth >= 0);
	return vln->depth;
}

static vl_node *vl_init_node(vl_info *info, const ir_node *irn)
{
	vl_node *vln = OALLOCZ(&info->obst, vl_node);
	vln->depth = -1;

	ir_nodemap_insert(&info->nodemap, irn, vln);

	return vln;
}

void vl_node_copy_depth(vl_info *vli, ir_node *src, ir_node *dst)
{
	vl_node *vl_src = ir_nodemap_get(&vli->nodemap, src);
	vl_node *vl_dst;

	if (!vl_src || (vl_src->depth < 0)) return;

	vl_dst = ir_nodemap_get(&vli->nodemap, dst);
	if (vl_dst == NULL) {
		vl_dst = vl_init_node(vli, dst);
	}
	assert(vl_dst->depth < 0);
	vl_dst->depth = vl_src->depth;
}

ir_node *vl_exact_copy(vl_info *vli, ir_node *irn)
{
	ir_node *copy = exact_copy(irn);
	if (vli) vl_node_copy_depth(vli, irn, copy);
	return copy;
}

void vl_exchange(vl_info *vli, ir_node *ir_old, ir_node *ir_new)
{
	if (vli) vl_node_copy_depth(vli, ir_old, ir_new);
	exchange(ir_old, ir_new);
}

static void vl_compute_depth(vl_info *vli, ir_node *irn, plist_t *todo)
{
	int i;
	vl_node *vln;

	if (irn_visited(irn)) return;
	mark_irn_visited(irn);

	if (is_Theta(irn)) {
		/* The next dep may recurse back to one of the nodes we have already
		 * visited, but not processed. Prevent that, by queueing the dep for
		 * later processing, so that post-order processing can finish first.
		 * The depth of the theta itself is known after all. */
		vl_compute_depth(vli, get_Theta_init(irn), todo);
		plist_insert_back(todo, get_Theta_next(irn));

		/* Use the known theta depth. */
		vln = ir_nodemap_get(&vli->nodemap, irn);
		if (vln == NULL) {
			vln = vl_init_node(vli, irn);
		}
		vln->depth = get_Theta_depth(irn);
	} else {
		/* Recurse first and calculate post-order. */
		for (i = 0; i < get_irn_arity(irn); i++) {
			ir_node *ir_dep = get_irn_n(irn, i);
			vl_compute_depth(vli, ir_dep, todo);
		}

		/* Use this when no deps are present (can't be in a loop). */
		vln = ir_nodemap_get(&vli->nodemap, irn);
		if (vln == NULL) {
			vln = vl_init_node(vli, irn);
		}
		vln->depth = 0;

		/* Calculate minimal and maximal depth of the deps. */
		for (i = 0; i < get_irn_arity(irn); i++) {
			ir_node *ir_dep = get_irn_n(irn, i);
			vl_node *vl_dep = ir_nodemap_get(&vli->nodemap, ir_dep);
			assert(vl_dep);

			/* Just take the nodes depth on the first try. */
			vln->depth = i ? MAX(vln->depth, vl_dep->depth) : vl_dep->depth;
		}

		/* Leave nodes on eta. */
		if (is_Eta(irn)) {
			vln->depth--;
			assert(vln->depth >= 0);
		}
	}
}

vl_info *vl_init(ir_graph *irg)
{
	plist_t *todo;
	ir_node *end = get_irg_end_block(irg);
	ir_node *ret = get_Block_cfgpred(end, 0);
	vl_info *vli = XMALLOC(vl_info);
	assert(is_Return(ret) && "Invalid PEG graph.");

	obstack_init(&vli->obst);
	ir_nodemap_init(&vli->nodemap, irg);
	vli->irg = irg;

	/* Do the depth analysis by processing acyclic fragments of the graph.
	 * On every theta node, analysis stops and the fragment on the thetas
	 * next dependency is added to the queue for later processing. */

	todo = plist_obstack_new(&vli->obst);
	plist_insert_back(todo, ret);
	inc_irg_visited(irg); /* Only reset once. */

	while (plist_count(todo) > 0) {
		ir_node *next = plist_first(todo)->data;
		plist_erase(todo, plist_first(todo));
		vl_compute_depth(vli, next, todo);
	}

#if VLD_DEBUG_DEPTHS
	printf("+------------------------------------------------+\n");
	printf("| Loop Depths                                    |\n");
	printf("+------------------------------------------------+\n");
	vl_dump(vli, stdout);
#endif

	return vli;
}

void vl_free(vl_info *vli)
{
	ir_nodemap_destroy(&vli->nodemap);
	obstack_free(&vli->obst, NULL);
	xfree(vli);
}

ir_graph *vl_get_irg(vl_info *vli)
{
	return vli->irg;
}

static void vl_dump_walk(vl_info *vli, ir_node *irn, FILE* f)
{
	int i;
	vl_node *vln;

	if (irn_visited(irn)) return;
	mark_irn_visited(irn);

	/* Recurse deeper. */
	for (i = 0; i < get_irn_arity(irn); i++) {
		ir_node *ir_dep = get_irn_n(irn, i);
		vl_dump_walk(vli, ir_dep, f);
	}

	vln = ir_nodemap_get_fast(&vli->nodemap, irn);
	fprintf(f, "%3ld: %d\n", get_irn_node_nr(irn), vln->depth);
}

void vl_dump(vl_info *vli, FILE* f)
{
	ir_graph *irg = vli->irg;
	ir_node  *end = get_irg_end_block(irg);
	ir_node  *ret = get_Block_cfgpred(end, 0);
	assert(ret);

	/* Walk the tree and dump every node. */
	inc_irg_visited(irg);
	vl_dump_walk(vli, ret, f);
}
