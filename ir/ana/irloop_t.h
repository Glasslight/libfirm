/*
 * Project:     libFIRM
 * File name:   ir/ana/irloop_t.h
 * Purpose:     Loop datastructure and access functions -- private stuff.
 * Author:      Goetz Lindenmaier
 * Modified by:
 * Created:     7.2002
 * CVS-ID:      $Id$
 * Copyright:   (c) 2002-2003 Universitšt Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.
 */

/**
* @file irloop_t.h
*
* @author Goetz Lindenmaier
*/

/* $Id$ */

#include "firm_common.h"
#include "irloop.h"

#ifndef _IRLOOP_T_H_
#define _IRLOOP_T_H_

/** The loops datastructure. */
struct ir_loop {
  firm_kind kind;		    /**< A type tag, set to k_ir_loop. */

  struct ir_loop *outer_loop;       /**< The outer loop */
  loop_element   *children;         /**< Mixed array: Contains sons and loop_nodes */
  int depth;                        /**< Nesting depth */
  int n_sons;                       /**< Number of ir_nodes in array "children" */
  int n_nodes;                      /**< Number of loop_nodes in array "childern" */

  /*
  struct state_entry *mem_phis;
  struct state_entry *states;

  struct obset **oval;
  struct loop_node *link;
  */
#ifdef DEBUG_libfirm
  int loop_nr;             /**< a unique node number for each loop node to make output
			      readable. */
  void *link;              /**< GL @@@ For debuging the analyses. */
#endif

};

void
add_loop_son(ir_loop *loop, ir_loop *son);

void
add_loop_node(ir_loop *loop, ir_node *n);

/* The following functions are needed in ircfloop.c */
void set_irn_loop (ir_node *n, ir_loop* loop);
#endif /* _IRLOOP_T_H_ */
