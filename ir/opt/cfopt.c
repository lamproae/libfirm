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
 * @brief   Control flow optimizations.
 * @author  Goetz Lindenmaier, Michael Beck, Sebastian Hack
 * @version $Id$
 *
 * Removes Bad control flow predecessors and empty blocks.  A block is empty
 * if it contains only a Jmp node. Blocks can only be removed if they are not
 * needed for the semantics of Phi nodes. Further, we NEVER remove labeled
 * blocks (even if we could move the label).
 */
#include "config.h"

#include "iroptimize.h"

#include <assert.h>
#include <stdbool.h>

#include "xmalloc.h"
#include "irnode_t.h"
#include "irgraph_t.h"
#include "irprog_t.h"

#include "ircons.h"
#include "iropt_t.h"
#include "irgwalk.h"
#include "irgmod.h"
#include "irdump.h"
#include "irverify.h"
#include "iredges.h"

#include "array_t.h"

#include "irouts.h"
#include "irbackedge_t.h"

#include "irflag_t.h"
#include "firmstat.h"
#include "irpass.h"

#include "iropt_dbg.h"

/** An environment for merge_blocks and collect nodes. */
typedef struct merge_env {
	bool      changed;      /**< Set if the graph was changed. */
	bool      phis_moved;   /**< Set if Phi nodes were moved. */
	ir_node **switch_conds; /**< Helper list for all found Switch Conds. */
} merge_env;

static void set_Block_removable(ir_node *block, bool removable)
{
	set_Block_mark(block, removable);
}

static bool is_Block_removable(ir_node *block)
{
	return get_Block_mark(block);
}

static void clear_link(ir_node *node, void *ctx)
{
	(void) ctx;
	set_irn_link(node, NULL);
	if (is_Block(node))
		set_Block_removable(node, true);
}

/**
 * Collects all Phi nodes in link list of Block.
 * Marks all blocks "non_removable" if they contain a node other
 * than Jmp (and Proj).
 * Links all Proj nodes to their predecessors.
 * Collects all switch-Conds in a list.
 */
static void collect_nodes(ir_node *n, void *ctx)
{
	merge_env *env = (merge_env*)ctx;

	if (is_Phi(n)) {
		/* Collect Phi nodes to compact ins along with block's ins. */
		ir_node *block = get_nodes_block(n);
		set_irn_link(n, get_irn_link(block));
		set_irn_link(block, n);
	} else if (is_Block(n)) {
		if (has_Block_entity(n))
			set_Block_removable(n, false);
		return;
	} else if (!is_Jmp(n)) {  /* Check for non-empty block. */
		ir_node *block = get_nodes_block(n);
		set_Block_removable(block, false);

		if (is_Proj(n)) {
			/* link Proj nodes */
			ir_node *pred = get_Proj_pred(n);
			set_irn_link(n, get_irn_link(pred));
			set_irn_link(pred, n);
		} else if (is_Cond(n)) {
			ir_node *sel = get_Cond_selector(n);
			if (get_irn_mode(sel) != mode_b) {
				/* found a switch-Cond, collect */
				ARR_APP1(ir_node*, env->switch_conds, n);
			}
		}
	}
}

/** Returns true if pred is predecessor of block. */
static bool is_pred_of(ir_node *pred, ir_node *b)
{
	int i;

	for (i = get_Block_n_cfgpreds(b) - 1; i >= 0; --i) {
		ir_node *b_pred = get_Block_cfgpred_block(b, i);
		if (b_pred == pred)
			return true;
	}
	return false;
}

/** Test whether we can optimize away pred block pos of b.
 *
 *  @param  b    A block node.
 *  @param  pos  The position of the predecessor block to judge about.
 *
 *  @returns     The number of predecessors
 *
 *  The test is rather tricky.
 *
 *  The situation is something like the following:
 *  @verbatim
 *                 if-block
 *                  /   \
 *              then-b  else-b
 *                  \   /
 *                    b
 *  @endverbatim
 *
 *  b merges the control flow of an if-then-else.  We may not remove
 *  the 'then' _and_ the 'else' block of an 'if' if there is a Phi
 *  node in b, even if both are empty.  The destruction of this Phi
 *  requires that a copy is added before the merge.  We have to
 *  keep one of the case blocks to place the copies in.
 *
 *  To perform the test for pos, we must regard predecessors before pos
 *  as already removed.
 **/
static unsigned test_whether_dispensable(ir_node *b, int pos)
{
	ir_node *pred  = get_Block_cfgpred(b, pos);
	ir_node *predb = get_nodes_block(pred);

	if (is_Bad(pred) || !is_Block_removable(predb))
		return 1;

	/* can't remove self-loops */
	if (predb == b)
		goto non_dispensable;
	if (is_unknown_jump(pred))
		goto non_dispensable;

	/* Seems to be empty. At least we detected this in collect_nodes. */
	if (get_irn_link(b) != NULL) {
		int n_cfgpreds = get_Block_n_cfgpreds(b);
		int i;
		/* there are Phi nodes */

		/* b's pred blocks and pred's pred blocks must be pairwise disjunct.
		 * Handle all pred blocks with preds < pos as if they were already
		 * removed. */
		for (i = 0; i < pos; i++) {
			ir_node *other_pred  = get_Block_cfgpred(b, i);
			ir_node *other_predb = get_nodes_block(other_pred);
			if (is_Bad(other_pred))
				continue;
			if (is_Block_removable(other_predb)
			    && !Block_block_visited(other_predb)) {
				int j;
				for (j = get_Block_n_cfgpreds(other_predb) - 1; j >= 0; --j) {
					ir_node *other_predpred
						= get_Block_cfgpred_block(other_predb, j);
					if (is_pred_of(other_predpred, predb))
						goto non_dispensable;
				}
			} else if (is_pred_of(other_predb, predb)) {
				goto non_dispensable;
			}
		}
		for (i = pos+1; i < n_cfgpreds; i++) {
			ir_node *other_predb = get_Block_cfgpred_block(b, i);
			if (is_pred_of(other_predb, predb))
				goto non_dispensable;
		}
	}
	/* we will not dispense already visited blocks */
	if (Block_block_visited(predb))
		return 1;
	/* if we get here, the block is dispensable, count useful preds */
	return get_irn_arity(predb);

non_dispensable:
	set_Block_removable(predb, false);
	return 1;
}

/**
 * This method removes empty blocks.  A block is empty if it only contains Phi
 * and Jmp nodes.
 *
 * We first adapt Phi nodes, then Block nodes, as we need the old ins
 * of the Block to adapt the Phi nodes.  We do this by computing new
 * in arrays, and then replacing the old ones.  So far we compute new in arrays
 * for all nodes, not regarding whether there is a possibility for optimization.
 *
 * For each predecessor p of a Block b there are three cases:
 *  - The predecessor p is a Bad node: just skip it. The in array of b shrinks
 *    by one.
 *  - The predecessor p is empty. Remove p. All predecessors of p are now
 *    predecessors of b.
 *  - The predecessor p is a block containing useful code. Just keep p as is.
 *
 * For Phi nodes f we have to check the conditions at the Block of f.
 * For cases 1 and 3 we proceed as for Blocks.  For case 2 we can have two
 * cases:
 *  -2a: The old predecessor of the Phi f is a Phi pred_f IN THE BLOCK REMOVED.
 *       In this case we proceed as for blocks. We remove pred_f.  All
 *       predecessors of pred_f now are predecessors of f.
 *  -2b: The old predecessor of f is NOT in the block removed. It might be a Phi
 *       too. We have to replicate f for each predecessor of the removed block.
 *       Or, with other words, the removed predecessor block has exactly one
 *       predecessor.
 *
 * Further there is a special case for self referencing blocks:
 * @verbatim
 *
 *    then_b     else_b                              then_b  else_b
 *       \      /                                      \      /
 *        \    /                                        |    /
 *        pred_b                                        |   /
 *         |   ____                                     |  /  ____
 *         |  |    |                                    |  | |    |
 *         |  |    |       === optimized to ===>        \  | |    |
 *        loop_b   |                                     loop_b   |
 *         |  |    |                                      |  |    |
 *         |  |____|                                      |  |____|
 *         |                                              |
 * @endverbatim
 *
 * If there is a Phi in pred_b, but we remove pred_b, we have to generate a
 * Phi in loop_b, that has the ins of the Phi in pred_b and a self referencing
 * backedge.
 */
static void optimize_blocks(ir_node *b, void *ctx)
{
	int i, j, k, n, max_preds, n_preds, p_preds = -1;
	ir_node *pred, *phi, *next;
	ir_node **in;
	merge_env *env = (merge_env*)ctx;

	/* Count the number of predecessor if this block is merged with pred blocks
	   that are empty. */
	max_preds = 0;
	for (i = 0, k = get_Block_n_cfgpreds(b); i < k; ++i) {
		max_preds += test_whether_dispensable(b, i);
	}
	in = XMALLOCN(ir_node*, max_preds);

	/*- Fix the Phi nodes of the current block -*/
	for (phi = (ir_node*)get_irn_link(b); phi != NULL; phi = (ir_node*)next) {
		assert(is_Phi(phi));
		next = (ir_node*)get_irn_link(phi);

		/* Find the new predecessors for the Phi */
		p_preds = 0;
		for (i = 0, n = get_Block_n_cfgpreds(b); i < n; ++i) {
			ir_graph *irg = get_irn_irg(b);
			pred = get_Block_cfgpred_block(b, i);

			if (is_Bad(pred)) {
				/* case Phi 1: maintain Bads, as somebody else is responsible to remove them */
				in[p_preds++] = new_r_Bad(irg, get_irn_mode(phi));
			} else if (is_Block_removable(pred) && !Block_block_visited(pred)) {
				/* case Phi 2: It's an empty block and not yet visited. */
				ir_node *phi_pred = get_Phi_pred(phi, i);

				for (j = 0, k = get_Block_n_cfgpreds(pred); j < k; j++) {
					ir_node *pred_pred = get_Block_cfgpred(pred, j);

					if (is_Bad(pred_pred)) {
						in[p_preds++] = new_r_Bad(irg, get_irn_mode(phi));
						continue;
					}

					if (get_nodes_block(phi_pred) == pred) {
						/* case Phi 2a: */
						assert(is_Phi(phi_pred));  /* Block is empty!! */

						in[p_preds++] = get_Phi_pred(phi_pred, j);
					} else {
						/* case Phi 2b: */
						in[p_preds++] = phi_pred;
					}
				}
			} else {
				/* case Phi 3: */
				in[p_preds++] = get_Phi_pred(phi, i);
			}
		}
		assert(p_preds == max_preds);

		/* Fix the node */
		if (p_preds == 1)
			exchange(phi, in[0]);
		else
			set_irn_in(phi, p_preds, in);
		env->changed = true;
	}

	/*- This happens only if merge between loop backedge and single loop entry.
	    Moreover, it is only needed if predb is the direct dominator of b,
	    else there can be no uses of the Phi's in predb ... -*/
	for (k = 0, n = get_Block_n_cfgpreds(b); k < n; ++k) {
		ir_node *pred  = get_Block_cfgpred(b, k);
		ir_node *predb = get_nodes_block(pred);
		if (is_Bad(pred))
			continue;

		if (is_Block_removable(predb) && !Block_block_visited(predb)) {
			ir_node *next_phi;

			/* we found a predecessor block at position k that will be removed */
			for (phi = (ir_node*)get_irn_link(predb); phi; phi = next_phi) {
				int q_preds = 0;
				next_phi = (ir_node*)get_irn_link(phi);

				assert(is_Phi(phi));

				if (get_Block_idom(b) != predb) {
					/* predb is not the dominator. There can't be uses of pred's Phi nodes, kill them .*/
					ir_graph *irg  = get_irn_irg(b);
					ir_mode  *mode = get_irn_mode(phi);
					exchange(phi, new_r_Bad(irg, mode));
				} else {
					/* predb is the direct dominator of b. There might be uses of the Phi nodes from
					   predb in further block, so move this phi from the predecessor into the block b */
					set_nodes_block(phi, b);
					set_irn_link(phi, get_irn_link(b));
					set_irn_link(b, phi);
					env->phis_moved = true;

					/* first, copy all 0..k-1 predecessors */
					for (i = 0; i < k; i++) {
						pred = get_Block_cfgpred_block(b, i);

						if (is_Bad(pred)) {
							in[q_preds++] = pred;
						} else if (is_Block_removable(pred) && !Block_block_visited(pred)) {
							/* It's an empty block and not yet visited. */
							for (j = 0; j < get_Block_n_cfgpreds(pred); j++) {
								if (! is_Bad(get_Block_cfgpred(pred, j)))
									in[q_preds++] = phi;
							}
						} else {
							in[q_preds++] = phi;
						}
					}

					/* now we are at k, copy the phi predecessors */
					pred = get_nodes_block(get_Block_cfgpred(b, k));
					for (i = 0; i < get_Phi_n_preds(phi); i++) {
						in[q_preds++] = get_Phi_pred(phi, i);
					}

					/* and now all the rest */
					for (i = k+1; i < get_Block_n_cfgpreds(b); i++) {
						pred = get_Block_cfgpred_block(b, i);

						if (is_Bad(pred)) {
							ir_graph *irg  = get_irn_irg(b);
							ir_mode  *mode = get_irn_mode(phi);
							in[q_preds++] = new_r_Bad(irg, mode);
						} else if (is_Block_removable(pred) && !Block_block_visited(pred)) {
							/* It's an empty block and not yet visited. */
							for (j = 0; j < get_Block_n_cfgpreds(pred); j++) {
								if (! is_Bad(get_Block_cfgpred(pred, j)))
									in[q_preds++] = phi;
							}
						} else {
							in[q_preds++] = phi;
						}
					}

					/* Fix the node */
					if (q_preds == 1)
						exchange(phi, in[0]);
					else
						set_irn_in(phi, q_preds, in);
					env->changed = true;

					assert(q_preds <= max_preds);
					// assert(p_preds == q_preds && "Wrong Phi Fix");
				}
			}
		}
	}

	/*- Fix the block -*/
	n_preds = 0;
	for (i = 0; i < get_Block_n_cfgpreds(b); i++) {
		ir_node *pred  = get_Block_cfgpred(b, i);
		ir_node *predb = get_nodes_block(pred);
		ir_graph *irg  = get_irn_irg(pred);

		/* case 1: Bad predecessor */
		if (is_Bad(pred)) {
			in[n_preds++] = new_r_Bad(irg, mode_X);
			continue;
		}
		if (is_Block_removable(predb) && !Block_block_visited(predb)) {
			/* case 2: It's an empty block and not yet visited. */
			for (j = 0; j < get_Block_n_cfgpreds(predb); j++) {
				ir_node *predpred = get_Block_cfgpred(predb, j);

				if (is_Bad(predpred)) {
					in[n_preds++] = new_r_Bad(irg, mode_X);
					continue;
				}

				in[n_preds++] = predpred;
			}
			/* Remove block+jump as it might be kept alive. */
			exchange(pred, new_r_Bad(get_irn_irg(b), mode_X));
			exchange(predb, new_r_Bad(get_irn_irg(b), mode_BB));
		} else {
			/* case 3: */
			in[n_preds++] = pred;
		}
	}
	assert(n_preds == max_preds);

	set_irn_in(b, n_preds, in);
	env->changed = true;

	/* see if phi-fix was correct */
	assert(get_irn_link(b) == NULL || p_preds == -1 || (n_preds == p_preds));
	xfree(in);
}

/**
 * Block walker: optimize all blocks using the default optimizations.
 * This removes Blocks with only a Jmp predecessor.
 */
static void remove_simple_blocks(ir_node *block, void *ctx)
{
	merge_env *env = (merge_env*)ctx;
	ir_node   *new_blk = equivalent_node(block);

	if (new_blk != block) {
		exchange(block, new_blk);
		env->changed = true;
	}
}

/**
 * Optimize table-switch Conds.
 *
 * @param cond the switch-Cond
 * @return true if the switch-Cond was optimized
 */
static bool handle_switch_cond(ir_node *cond)
{
	ir_node *sel   = get_Cond_selector(cond);
	ir_node *proj1 = (ir_node*)get_irn_link(cond);
	ir_node *proj2 = (ir_node*)get_irn_link(proj1);
	ir_node *blk   = get_nodes_block(cond);

	/* exactly 1 Proj on the Cond node: must be the defaultProj */
	if (proj2 == NULL) {
		ir_node *jmp = new_r_Jmp(blk);
		assert(get_Cond_default_proj(cond) == get_Proj_proj(proj1));
		/* convert it into a Jmp */
		exchange(proj1, jmp);
		return true;
	}

	/* handle Cond nodes with constant argument. In this case the localopt rules
	 * should have killed all obviously impossible cases.
	 * So the only case left to handle here is 1 defaultProj + 1case
	 * (this one case should be the one taken) */
	if (get_irn_link(proj2) == NULL) {
		ir_tarval *tv = value_of(sel);

		if (tv != tarval_bad) {
			/* we have a constant switch */
			long      num     = get_tarval_long(tv);
			long      def_num = get_Cond_default_proj(cond);
			ir_graph *irg     = get_irn_irg(cond);
			ir_node  *bad     = new_r_Bad(irg, mode_X);

			if (def_num == get_Proj_proj(proj1)) {
				/* first one is the defProj */
				if (num == get_Proj_proj(proj2)) {
					ir_node *jmp = new_r_Jmp(blk);
					exchange(proj2, jmp);
					exchange(proj1, bad);
					return true;
				}
			} else if (def_num == get_Proj_proj(proj2)) {
				/* second one is the defProj */
				if (num == get_Proj_proj(proj1)) {
					ir_node *jmp = new_r_Jmp(blk);
					exchange(proj1, jmp);
					exchange(proj2, bad);
					return true;
				}
			} else {
				/* neither: strange, Cond was not optimized so far */
				if (num == get_Proj_proj(proj1)) {
					ir_node *jmp = new_r_Jmp(blk);
					exchange(proj1, jmp);
					exchange(proj2, bad);
					return true;
				} else if (num == get_Proj_proj(proj2)) {
					ir_node *jmp = new_r_Jmp(blk);
					exchange(proj2, jmp);
					exchange(proj1, bad);
					return true;
				}
			}
		}
	}
	return false;
}

/* Optimizations of the control flow that also require changes of Phi nodes.
 *
 * This optimization performs two passes over the graph.
 *
 * The first pass collects all Phi nodes in a link list in the block
 * nodes.  Further it performs simple control flow optimizations.
 * Finally it marks all blocks that do not contain useful
 * computations, i.e., these blocks might be removed.
 *
 * The second pass performs the optimizations intended by this algorithm.
 * It walks only over block nodes and adapts these and the Phi nodes in these
 * blocks, which it finds in a linked list computed by the first pass.
 *
 * We use the mark flag to mark removable blocks in the first phase.
 */
void optimize_cf(ir_graph *irg)
{
	int i, j, n;
	ir_node **in = NULL;
	ir_node *end = get_irg_end(irg);
	ir_node *new_end;
	merge_env env;

	assert(get_irg_phase_state(irg) != phase_building);

	/* if the graph is not pinned, we cannot determine empty blocks */
	assert(get_irg_pinned(irg) != op_pin_state_floats &&
	       "Control flow optimization need a pinned graph");

	/* FIXME: control flow opt destroys block edges. So edges are deactivated
	 * here. Fix the edges! */
	edges_deactivate(irg);

	/* we use the mark flag to mark removable blocks */
	ir_reserve_resources(irg, IR_RESOURCE_BLOCK_MARK | IR_RESOURCE_IRN_LINK);

	/* The Cond optimization might expose unreachable code, so we loop */
	for (;;) {
		int length;
		env.changed    = false;
		env.phis_moved = false;

		assure_doms(irg);

		env.switch_conds = NEW_ARR_F(ir_node*, 0);
		irg_walk(end, clear_link, collect_nodes, &env);

		/* handle all collected switch-Conds */
		length = ARR_LEN(env.switch_conds);
		for (i = 0; i < length; ++i) {
			ir_node *cond = env.switch_conds[i];
			env.changed |= handle_switch_cond(cond);
		}
		DEL_ARR_F(env.switch_conds);

		if (!env.changed) break;

		set_irg_doms_inconsistent(irg);
		set_irg_extblk_inconsistent(irg);
		set_irg_entity_usage_state(irg, ir_entity_usage_not_computed);
	}

	/* assert due to collect_nodes:
	 * 1. removable blocks are now marked as such
	 * 2. phi lists are up to date
	 */

	/* Optimize the standard code. */
	assure_doms(irg);
	irg_block_walk_graph(irg, optimize_blocks, remove_simple_blocks, &env);

	new_end = optimize_in_place(end);
	if (new_end != end) {
		set_irg_end(irg, new_end);
		end = new_end;
	}
	remove_End_Bads_and_doublets(end);

	ir_free_resources(irg, IR_RESOURCE_BLOCK_MARK | IR_RESOURCE_IRN_LINK);

	if (env.phis_moved) {
		/* Bad: when we moved Phi's, we might produce dead Phi nodes
		   that are kept-alive.
		   Some other phases cannot copy with this, so will them.
		 */
		n = get_End_n_keepalives(end);
		if (n > 0) {
			NEW_ARR_A(ir_node *, in, n);
			assure_irg_outs(irg);

			for (i = j = 0; i < n; ++i) {
				ir_node *ka = get_End_keepalive(end, i);

				if (is_Phi(ka)) {
					int k;

					for (k = get_irn_n_outs(ka) - 1; k >= 0; --k) {
						ir_node *user = get_irn_out(ka, k);

						if (user != ka && user != end) {
							/* Is it a real user or just a self loop ? */
							break;
						}
					}
					if (k >= 0)
						in[j++] = ka;
				} else
					in[j++] = ka;
			}
			if (j != n) {
				set_End_keepalives(end, j, in);
				env.changed = true;
			}
		}
	}

	if (env.changed) {
		/* Handle graph state if was changed. */
		set_irg_doms_inconsistent(irg);
		set_irg_extblk_inconsistent(irg);
		set_irg_entity_usage_state(irg, ir_entity_usage_not_computed);
	}
}

/* Creates an ir_graph pass for optimize_cf. */
ir_graph_pass_t *optimize_cf_pass(const char *name)
{
	return def_graph_pass(name ? name : "optimize_cf", optimize_cf);
}
