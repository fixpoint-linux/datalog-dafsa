/*
 * topdown.h — Top-down / QSQ (query-subquery) demand-driven evaluator
 *
 * An OPT-IN 4th per-query path (dl_query_topdown / dl_query_topdown_adorn)
 * that reuses the magic-sets ADORNED PROGRAM (magic_transform_adorn) but
 * schedules it DEMAND-DRIVEN instead of as a forward semi-naive fixpoint.
 *
 * The driver (td_eval) runs an SLG-style worklist of subqueries (variant +
 * bound tuple).  Each subquery's answers are materialized into a per-variant
 * memo tuple_set; child subqueries are discovered by running the MAGIC rules
 * with the parent subquery's bound guard.  Cyclic (recursive) programs are
 * resolved by monotone least-fixpoint iteration over the reachable subquery
 * set — no C recursion, so an N=10000 chain cannot overflow the stack.
 *
 * Correctness contract: td_eval streams byte-for-byte the same tuples that
 * filtering the full materialization of the goal on the bound positions
 * would produce (== dl_query_magic_adorn's result for the same program).
 */
#ifndef TOPDOWN_H
#define TOPDOWN_H

#include "dl.h"
#include "magic.h"
#include "compiler.h"

typedef struct td_ctx td_ctx;   /* opaque: variants[], subquery hash, worklist */

/*
 * Run the goal subquery (goal variant + bound columns) top-down, streaming
 * the full-arity answer tuples via cb.  `edb` is the eval clone (EDB aliased,
 * magic/adorned relations pre-declared empty, dir==NULL); `prog` is the
 * transform output; `crules`/`n_crules` are the rules compiled with
 * g_reorder==0 and g_bushy==0 (so compiled body_idx == AST body position and
 * recursive atoms stay override-compatible).  `goal_variant_id` indexes the
 * variant whose adorned_name == prog->adorned_goal; `bound` holds nbound u32
 * values in left-to-right bound-position order.
 *
 * Returns the number of tuples emitted, or -1 on error.
 */
long td_eval(dl_db *edb, const magic_program *prog,
             compiled_rule **crules, int n_crules,
             int goal_variant_id, const uint32_t *bound,
             dl_tuple_cb cb, void *user);

#endif /* TOPDOWN_H */
