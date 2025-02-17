/*
* Copyright 2018-2020 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#pragma once

#include "op.h"
#include "../execution_plan.h"
#include "../../graph/graph.h"
#include "../../algorithms/algorithms.h"
#include "../../arithmetic/algebraic_expression.h"

/* OP Traverse */
typedef struct {
	OpBase op;
	Graph *g;
	Record r;
	FT_FilterNode *ft;              /* If not NULL, FilterTree applied to the traversed edge. */
	AlgebraicExpression *ae;        /* ArithmeticExpression describing the op's traversal pattern. */
	int srcNodeIdx;                 /* Node set by operation. */
	int edgesIdx;                   /* Edges set by operation. */
	int destNodeIdx;                /* Node set by operation. */
	bool expandInto;                /* Both src and dest already resolved. */
	unsigned int minHops;           /* Maximum number of hops to perform. */
	unsigned int maxHops;           /* Maximum number of hops to perform. */
	int edgeRelationCount;          /* Length of edgeRelationTypes. */
	int *edgeRelationTypes;         /* Relation(s) we're traversing. */
	AllPathsCtx *allPathsCtx;
	GRAPH_EDGE_DIR traverseDir;     /* Traverse direction. */
} CondVarLenTraverse;

OpBase *NewCondVarLenTraverseOp(const ExecutionPlan *plan, Graph *g, AlgebraicExpression *ae);

/* Transform operation from Conditional Variable Length Traverse
 * to Expand Into Conditional Variable Length Traverse */
void CondVarLenTraverseOp_ExpandInto(CondVarLenTraverse *op);

// Set the FilterTree pointer of a CondVarLenTraverse operation.
void CondVarLenTraverseOp_SetFilter(CondVarLenTraverse *op, FT_FilterNode *ft);

