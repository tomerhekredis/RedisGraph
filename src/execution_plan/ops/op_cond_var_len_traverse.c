/*
* Copyright 2018-2020 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "op_cond_var_len_traverse.h"
#include "shared/print_functions.h"
#include "../../util/arr.h"
#include "../../ast/ast.h"
#include "../../arithmetic/arithmetic_expression.h"
#include "../../graph/graphcontext.h"
#include "../../algorithms/all_paths.h"
#include "../../query_ctx.h"

/* Forward declarations. */
static Record CondVarLenTraverseConsume(OpBase *opBase);
static OpResult CondVarLenTraverseReset(OpBase *opBase);
static OpBase *CondVarLenTraverseClone(const ExecutionPlan *plan, const OpBase *opBase);
static void CondVarLenTraverseFree(OpBase *opBase);

static void _setupTraversedRelations(CondVarLenTraverse *op) {
	QGEdge *e = QueryGraph_GetEdgeByAlias(op->op.plan->query_graph, AlgebraicExpression_Edge(op->ae));
	ASSERT(e->minHops <= e->maxHops);
	op->minHops = e->minHops;
	op->maxHops = e->maxHops;

	uint reltype_count = array_len(e->reltypeIDs);
	if(reltype_count == 0) {
		op->edgeRelationCount = 1;
		op->edgeRelationTypes = array_new(int, 1);
		op->edgeRelationTypes = array_append(op->edgeRelationTypes, GRAPH_NO_RELATION);
	} else {
		GraphContext *gc = QueryCtx_GetGraphCtx();
		op->edgeRelationCount = 0;
		op->edgeRelationTypes = array_new(int, reltype_count);

		for(int i = 0; i < reltype_count; i++) {
			int rel_id = e->reltypeIDs[i];
			if(rel_id != GRAPH_UNKNOWN_RELATION) {
				op->edgeRelationTypes = array_append(op->edgeRelationTypes, rel_id);
			} else {
				const char *rel_type = e->reltypes[i];
				Schema *s = GraphContext_GetSchema(gc, rel_type, SCHEMA_EDGE);
				if(s) op->edgeRelationTypes = array_append(op->edgeRelationTypes, s->id);
			}
		}

		op->edgeRelationCount = array_len(op->edgeRelationTypes);
	}
}

// Set the traversal direction to match the traversed edge and AlgebraicExpression form.
static inline void _setTraverseDirection(CondVarLenTraverse *op, const QGEdge *e) {
	if(e->bidirectional) {
		op->traverseDir = GRAPH_EDGE_DIR_BOTH;
	} else {
		if(AlgebraicExpression_Transposed(op->ae)) {
			// traverse in the opposite direction, (dest)->(src) incoming edges
			op->traverseDir = GRAPH_EDGE_DIR_INCOMING;
		} else {
			op->traverseDir = GRAPH_EDGE_DIR_OUTGOING;
		}
	}
}

static inline int CondVarLenTraverseToString(const OpBase *ctx, char *buf, uint buf_len) {
	// TODO: tmp, improve TraversalToString
	AlgebraicExpression_Optimize(&((CondVarLenTraverse *)ctx)->ae);
	return TraversalToString(ctx, buf, buf_len, ((const CondVarLenTraverse *)ctx)->ae);
}

void CondVarLenTraverseOp_ExpandInto(CondVarLenTraverse *op) {
	// Expand into doesn't performs any modifications.
	array_clear(op->op.modifies);
	op->expandInto = true;
	op->op.type = OPType_CONDITIONAL_VAR_LEN_TRAVERSE_EXPAND_INTO;
	op->op.name = "Conditional Variable Length Traverse (Expand Into)";
}

inline void CondVarLenTraverseOp_SetFilter(CondVarLenTraverse *op,
										   FT_FilterNode *ft) {
	ASSERT(op != NULL);
	ASSERT(ft != NULL);
	ASSERT(op->ft == NULL);

	op->ft = ft;
}

OpBase *NewCondVarLenTraverseOp(const ExecutionPlan *plan, Graph *g, AlgebraicExpression *ae) {
	ASSERT(g != NULL);
	ASSERT(ae != NULL);

	CondVarLenTraverse *op = rm_malloc(sizeof(CondVarLenTraverse));
	op->g = g;
	op->ae = ae;
	op->r = NULL;
	op->ft = NULL;
	op->expandInto = false;
	op->allPathsCtx = NULL;
	op->edgeRelationTypes = NULL;

	OpBase_Init((OpBase *)op, OPType_CONDITIONAL_VAR_LEN_TRAVERSE,
				"Conditional Variable Length Traverse", NULL, CondVarLenTraverseConsume, CondVarLenTraverseReset,
				CondVarLenTraverseToString, CondVarLenTraverseClone, CondVarLenTraverseFree, false, plan);

	bool aware = OpBase_Aware((OpBase *)op, AlgebraicExpression_Source(ae), &op->srcNodeIdx);
	ASSERT(aware);
	op->destNodeIdx = OpBase_Modifies((OpBase *)op, AlgebraicExpression_Destination(ae));

	// populate edge value in record only if it is referenced
	AST *ast = QueryCtx_GetAST();
	QGEdge *e = QueryGraph_GetEdgeByAlias(plan->query_graph, AlgebraicExpression_Edge(op->ae));
	op->edgesIdx = AST_AliasIsReferenced(ast, e->alias) ? OpBase_Modifies((OpBase *)op, e->alias) : -1;
	_setTraverseDirection(op, e);

	return (OpBase *)op;
}

static Record CondVarLenTraverseConsume(OpBase *opBase) {
	CondVarLenTraverse  *op     =  (CondVarLenTraverse *)opBase;
	Path                *p      =  NULL;
	OpBase              *child  =  op->op.children[0];

	while(!(p = AllPathsCtx_NextPath(op->allPathsCtx))) {
		Record childRecord = OpBase_Consume(child);
		if(!childRecord) return NULL;

		if(op->r) OpBase_DeleteRecord(op->r);
		op->r = childRecord;

		Node *srcNode = Record_GetNode(op->r, op->srcNodeIdx);
		if(srcNode == NULL) {
			/* The child Record may not contain the source node in scenarios like
			 * a failed OPTIONAL MATCH. In this case, delete the Record and try again. */
			OpBase_DeleteRecord(op->r);
			op->r = NULL;
			continue;
		}

		// Create edge relation type array on first call to consume.
		if(!op->edgeRelationTypes) {
			_setupTraversedRelations(op);
			/* Incase we don't have any relations to traverse and minimal traversal is at least one hop
			 * we can return quickly.
			 * Consider: MATCH (S)-[:L*]->(M) RETURN M
			 * where label L does not exists. */
			if(op->edgeRelationCount == 0 && op->minHops > 0) return NULL;
		}

		Node *destNode = NULL;
		// The destination node is known in advance if we're performing an ExpandInto.
		if(op->expandInto) destNode = Record_GetNode(op->r, op->destNodeIdx);

		AllPathsCtx_Free(op->allPathsCtx);
		op->allPathsCtx = AllPathsCtx_New(srcNode, destNode, op->g, op->edgeRelationTypes,
										  op->edgeRelationCount, op->traverseDir, op->minHops,
										  op->maxHops, op->r, op->ft, op->edgesIdx);

	}


	//--------------------------------------------------------------------------
	// populate output record
	//--------------------------------------------------------------------------

	Record r = OpBase_CloneRecord(op->r);

	// add destination node to record
	if(!op->expandInto) Record_AddNode(r, op->destNodeIdx, Path_Head(p));

	// add new path to record
	if(op->edgesIdx >= 0) Record_AddScalar(r, op->edgesIdx, SI_Path(p));

	return r;
}

static OpResult CondVarLenTraverseReset(OpBase *ctx) {
	CondVarLenTraverse *op = (CondVarLenTraverse *)ctx;
	if(op->r) {
		OpBase_DeleteRecord(op->r);
		op->r = NULL;
	}
	AllPathsCtx_Free(op->allPathsCtx);
	op->allPathsCtx = NULL;
	return OP_OK;
}

static OpBase *CondVarLenTraverseClone(const ExecutionPlan *plan, const OpBase *opBase) {
	ASSERT(opBase->type == OPType_CONDITIONAL_VAR_LEN_TRAVERSE);
	CondVarLenTraverse *op = (CondVarLenTraverse *) opBase;
	OpBase *op_clone = NewCondVarLenTraverseOp(plan, QueryCtx_GetGraph(),
											   AlgebraicExpression_Clone(op->ae));
	return op_clone;
}

static void CondVarLenTraverseFree(OpBase *ctx) {
	CondVarLenTraverse *op = (CondVarLenTraverse *)ctx;

	if(op->edgeRelationTypes) {
		array_free(op->edgeRelationTypes);
		op->edgeRelationTypes = NULL;
	}

	if(op->ae) {
		AlgebraicExpression_Free(op->ae);
		op->ae = NULL;
	}

	if(op->r) {
		OpBase_DeleteRecord(op->r);
		op->r = NULL;
	}

	if(op->allPathsCtx) {
		AllPathsCtx_Free(op->allPathsCtx);
		op->allPathsCtx = NULL;
	}

	if(op->ft) {
		FilterTree_Free(op->ft);
		op->ft = NULL;
	}
}

