/*
* Copyright 2018-2021 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "../../config.h"
#include "../../util/arr.h"
#include "../../util/strcmp.h"
#include "../../util/rmalloc.h"
#include "../../filter_tree/filter_tree.h"
#include "../../arithmetic/algebraic_expression.h"

#define T 1           // Transpose penalty.
#define L 2 * T       // Label score.
#define F 4 * T       // Filter score.
#define B 8 * F       // Bound variable bonus.

typedef AlgebraicExpression **Arrangement;

// create arrangement
static inline Arrangement _Arrangement_New(uint size) {
	return rm_malloc(sizeof(AlgebraicExpression *) * size);
}

// clone arrangement
static inline Arrangement _Arrangement_Clone(const Arrangement arrangement, uint size) {
	ASSERT(arrangement != NULL);
	Arrangement clone = _Arrangement_New(size);
	memcpy(clone, arrangement, sizeof(AlgebraicExpression *) * size);
	return clone;
}

// print arrangement
static inline void _Arrangement_Print(Arrangement arrangement, uint size) {
	printf("Arrangement_Print\n");
	for(uint i = 0; i < size; i++) {
		AlgebraicExpression *exp = arrangement[i];
		printf("%d, src: %s, dest: %s\n", i, AlgebraicExpression_Source(exp),
			   AlgebraicExpression_Destination(exp));
	}
}

// free arrangement
static inline void _Arrangement_Free(Arrangement arrangement) {
	ASSERT(arrangement != NULL);
	rm_free(arrangement);
}

// computes x!
static inline unsigned long _factorial(uint x) {
	unsigned long res = 1;
	for(int i = 2; i <= x; i++) res *= i;
	return res;
}

// swaps exps[i] with exps[j]
static inline void _swap(Arrangement exps, uint i, uint  j) {
	AlgebraicExpression *temp;
	temp = exps[i];
	exps[i] = exps[j];
	exps[j] = temp;
}

// computes all permutations of set exps
static inline void _permute(Arrangement set, int l, int r, Arrangement **permutations) {
	int i;
	if(l == r) {
		Arrangement permutation = _Arrangement_Clone(set, r + 1);
		*permutations = array_append(*permutations, permutation);
	} else {
		for(i = l; i <= r; i++) {
			_swap(set, l, i);
			_permute(set, l + 1, r, permutations);
			_swap(set, l, i);   // backtrack.
		}
	}
}

// computes all possible permutations of exps
static Arrangement *_permutations(const Arrangement exps, uint exps_count) {
	// Number of permutations of a set S is |S|!.
	unsigned long permutation_count = _factorial(exps_count);
	Arrangement *permutations = array_new(Arrangement, permutation_count);

	// Compute permutations.
	_permute(exps, 0, exps_count - 1, &permutations);
	ASSERT(array_len(permutations) == permutation_count);

	return permutations;
}

/* A valid arrangement of expressions is one in which the ith expression
 * source or destination nodes appear in a previous expression k where k < i. */
static bool _valid_arrangement(const Arrangement arrangement, uint exps_count, QueryGraph *qg) {
	AlgebraicExpression *exp = arrangement[0];
	/* A 1 hop traversals where either the source node
	 * or destination node is labeled, can't be the opening expression
	 * in an arrangement.
	 * Consider: MATCH (a:L0)-[:R*]->(b:L1)
	 * [L0] * [R] * [L1] but because R is a variable length traversal
	 * we're dealing with 3 different expressions:
	 * exp0: [L0]
	 * exp1: [R]
	 * exp2: [L1]
	 * the arrangement where [R] is the first expression:
	 * exp0: [R]
	 * exp1: [L0]
	 * exp2: [L1]
	 * Isn't valid, as currently the first expression is converted
	 * into a scan operation. */
	QGNode *src = QueryGraph_GetNodeByAlias(qg,
											AlgebraicExpression_Source(exp)); // TODO unwisely expensive
	QGNode *dest = QueryGraph_GetNodeByAlias(qg,
											 AlgebraicExpression_Destination(exp)); // TODO unwisely expensive
	if((src->label || dest->label) &&
	   AlgebraicExpression_Edge(exp) &&
	   AlgebraicExpression_OperandCount(exp) == 1) return false;

	for(int i = 1; i < exps_count; i++) {
		exp = arrangement[i];
		int j = i - 1;

		// Scan previous expressions.
		for(; j >= 0; j--) {
			AlgebraicExpression *prev_exp = arrangement[j];
			const char *exp_src = AlgebraicExpression_Source(exp);
			const char *exp_dest = AlgebraicExpression_Destination(exp);
			const char *prev_exp_src = AlgebraicExpression_Source(prev_exp);
			const char *prev_exp_dest = AlgebraicExpression_Destination(prev_exp);

			if(!RG_STRCMP(prev_exp_src, exp_src)     ||
			   !RG_STRCMP(prev_exp_dest, exp_src)    ||
			   !RG_STRCMP(prev_exp_src, exp_dest)    ||
			   !RG_STRCMP(prev_exp_dest, exp_dest)) break;
		}
		/* Nither src or dest nodes are mentioned in previous expressions
		 * as such the arrangement is invalid. */
		if(j < 0) return false;
	}
	return true;
}

static int _penalty_arrangement(Arrangement arrangement, uint exp_count) {
	int penalty = 0;

	/* see if graph maintains transpose matrices
	 * if it does, there's no penalty */
	bool maintain_transpose = false;
	Config_Option_get(Config_MAINTAIN_TRANSPOSE, &maintain_transpose);
	if(maintain_transpose) return 0;

	AlgebraicExpression *exp;

	// account for first expression transposes
	exp = arrangement[0];
	uint transpose_count = AlgebraicExpression_OperationCount(exp, AL_EXP_TRANSPOSE);
	penalty += transpose_count * T;

	for(uint i = 1; i < exp_count; i++) {
		exp = arrangement[i];
		bool src_resolved = false;

		// see if source is already resolved
		for(int j = i - 1; j >= 0; j--) {
			AlgebraicExpression *prev_exp = arrangement[j];
			if(!RG_STRCMP(AlgebraicExpression_Source(prev_exp), AlgebraicExpression_Source(exp)) ||
			   !RG_STRCMP(AlgebraicExpression_Destination(prev_exp), AlgebraicExpression_Source(exp))) {
				src_resolved = true;
				break;
			}
		}

		// dest must be resolved as we're working with valid arrangemet
		if(src_resolved) {
			// count how many transposes are performed
			transpose_count = AlgebraicExpression_OperationCount(exp, AL_EXP_TRANSPOSE);
			penalty += transpose_count * T;
		} else {
			// count how many transposes we require to perform
			transpose_count = AlgebraicExpression_OperationCount(exp, AL_EXP_TRANSPOSE);
			uint operand_count = AlgebraicExpression_OperandCount(exp);
			penalty += (operand_count - transpose_count) * T;
		}
	}

	return penalty;
}

static int _reward_expression(AlgebraicExpression *exp, QueryGraph *qg,
							  rax *filtered_entities, rax *bound_vars, uint reward_factor) {

	// A bit naive at the moment.
	void *res                = NULL;
	int reward               = 0;
	const char *src          = AlgebraicExpression_Source(exp);
	const char *dest         = AlgebraicExpression_Destination(exp);
	size_t src_len           = strlen(src);
	size_t dest_len          = strlen(dest);

	// Reward bound variables such that any expression with a bound variable
	// will be preferred over any expression without.
	if(bound_vars) {
		res = raxFind(bound_vars, (unsigned char *)src, src_len);
		if(res != raxNotFound) reward += B * reward_factor;

		res = raxFind(bound_vars, (unsigned char *)dest, dest_len);
		if(res != raxNotFound) reward += B * reward_factor;
	}

	// Reward filters in expression.
	res = raxFind(filtered_entities, (unsigned char *)src, src_len);
	if(res != raxNotFound) reward += F * reward_factor;

	res = raxFind(filtered_entities, (unsigned char *)dest, dest_len);
	if(res != raxNotFound) reward += F * reward_factor;

	// TODO unwisely expensive
	QGNode *src_node = QueryGraph_GetNodeByAlias(qg, src);
	if(src_node->label) reward += L * reward_factor;

	return reward;
}

static int _reward_arrangement(Arrangement arrangement, uint exp_count, QueryGraph *qg,
							   rax *filtered_entities, rax *bound_vars) {
	int reward = 0;

	// A bit naive at the moment.
	for(uint i = 0; i < exp_count; i++) {
		uint reward_factor = exp_count - i;
		AlgebraicExpression *exp = arrangement[i];
		reward += _reward_expression(exp, qg, filtered_entities, bound_vars,
									 reward_factor);
	}

	return reward;
}

static int _score_arrangement(Arrangement arrangement, uint exp_count, QueryGraph *qg,
							  rax *filtered_entities, rax *bound_vars) {
	int score = 0;
	score -= _penalty_arrangement(arrangement, exp_count);
	score += _reward_arrangement(arrangement, exp_count, qg, filtered_entities, bound_vars);
	return score;
}

// Transpose out-of-order expressions
// such that each expresson's source is resolved by a previous expression.
static void _resolve_winning_sequence(AlgebraicExpression **exps, uint exp_count) {
	for(uint i = 1; i < exp_count; i ++) {
		AlgebraicExpression *exp = exps[i];
		bool src_resolved        = false;
		const char *src          = AlgebraicExpression_Source(exp);

		// see if source is already resolved
		for(int j = i - 1; j >= 0; j--) {
			AlgebraicExpression *prev_exp = exps[j];
			if(!RG_STRCMP(AlgebraicExpression_Source(prev_exp), src) ||
			   !RG_STRCMP(AlgebraicExpression_Destination(prev_exp), src)) {
				src_resolved = true;
				break;
			}
		}

		if(!src_resolved) AlgebraicExpression_Transpose(exps + i);
	}
}

/* Having chosen which algebraic expression will be evaluated first,
 * determine whether it is worthwhile to transpose it
 * and thus swap the source and destination.
 *
 * If the source is bounded, we will not transpose,
 * if only the destination is bounded, we will.
 *
 * If neither are bounded, we fall back to label and filter heuristics.
 * Filters are considered more valuable than labels in selecting a starting point,
 * so we'll select the starting point with the best combination available of filters and labels. */
static void _select_entry_point(QueryGraph *qg, AlgebraicExpression **ae, rax *filtered_entities,
								rax *bound_vars) {

	AlgebraicExpression *exp = *ae;
	const char *src = AlgebraicExpression_Source(exp);
	const char *dest = AlgebraicExpression_Destination(exp);
	size_t src_len = strlen(src);
	size_t dest_len = strlen(dest);

	// MATCH (a)-[]->(a)
	if(AlgebraicExpression_OperandCount(exp) == 1 && !RG_STRCMP(src, dest)) {
		return;
	}

	// always start at a bound variable if one is present
	void *lookup = NULL;
	if(bound_vars) {
		lookup = raxFind(bound_vars, (unsigned char *)src, src_len);
		if(lookup != raxNotFound) {
			return;
		}

		lookup = raxFind(bound_vars, (unsigned char *)dest, dest_len);
		if(lookup != raxNotFound) {
			AlgebraicExpression_Transpose(ae);
			return;
		}
	}

	int src_score  = 0;
	int dest_score = 0;

	// see if either source or destination nodes are filtered
	lookup = raxFind(filtered_entities, (unsigned char *)src, src_len);
	src_score += (lookup != raxNotFound) ? F : 0;

	lookup = raxFind(filtered_entities, (unsigned char *)dest, dest_len);
	dest_score += (lookup != raxNotFound) ? F : 0;

	// see if either source or destination nodes are labeled
	QGNode *src_node  = QueryGraph_GetNodeByAlias(qg, src);
	src_score += (src_node->label != NULL) ? L : 0;

	QGNode *dest_node = QueryGraph_GetNodeByAlias(qg, dest);
	dest_score += (dest_node->label != NULL) ? L : 0;

	// if the destination is a superior starting point, transpose expression
	if(dest_score > src_score) AlgebraicExpression_Transpose(ae);
}

/* Given a set of algebraic expressions representing a graph traversal
 * we pick the order in which the expressions will be evaluated
 * taking into account filters and transposes.
 * exps will reordered. */
void orderExpressions(QueryGraph *qg, AlgebraicExpression **exps, uint exp_count,
					  const FT_FilterNode *filters, rax *bound_vars) {
	ASSERT(exps && exp_count > 0);

	/* Return early if we only have one expression that represents a scan rather than a traversal.
	 * e.g. MATCH (n:L) RETURN n */
	if(exp_count == 1 && AlgebraicExpression_OperandCount(exps[0]) == 1 &&
	   !RG_STRCMP(AlgebraicExpression_Source(exps[0]), AlgebraicExpression_Destination(exps[0]))) return;

	// Collect all filtered aliases.
	rax *filtered_entities = FilterTree_CollectModified(filters);
	// Compute all possible permutations of algebraic expressions.
	Arrangement *arrangements = _permutations(exps, exp_count);
	uint arrangement_count = array_len(arrangements);

	/* If we only have one arrangement, we still want to select the optimal entry point
	 * but have no other work to do. */
	if(arrangement_count == 1) goto select_entry_point;

	// Remove invalid arrangements.
	Arrangement *valid_arrangements = array_new(Arrangement, arrangement_count);
	for(int i = 0; i < arrangement_count; i++) {
		if(_valid_arrangement(arrangements[i], exp_count, qg)) {
			valid_arrangements = array_append(valid_arrangements, arrangements[i]);
		}
	}
	uint valid_arrangement_count = array_len(valid_arrangements);
	ASSERT(valid_arrangement_count > 0);

	/* Score each arrangement,
	 * keep track after arrangement with highest score. */
	int max_score = INT_MIN;
	Arrangement top_arrangement = valid_arrangements[0];

	for(uint i = 0; i < valid_arrangement_count; i++) {
		Arrangement arrangement = valid_arrangements[i];
		int score = _score_arrangement(arrangement, exp_count, qg, filtered_entities, bound_vars);
		// printf("score: %d\n", score);
		// _Arrangement_Print(arrangement, exp_count);
		if(max_score < score) {
			max_score = score;
			top_arrangement = arrangement;
		}
	}

	array_free(valid_arrangements);

	// Update input.
	for(uint i = 0; i < exp_count; i++) exps[i] = top_arrangement[i];

	// Depending on how the expressions have been ordered, we may have to transpose expressions
	// so that their source nodes have already been resolved by previous expressions.
	_resolve_winning_sequence(exps, exp_count);

select_entry_point:
	// Transpose the winning expression if the destination node is a more efficient starting place.
	_select_entry_point(qg, exps + 0, filtered_entities, bound_vars);

	raxFree(filtered_entities);
	for(uint i = 0; i < arrangement_count; i++) _Arrangement_Free(arrangements[i]);
	array_free(arrangements);
}

