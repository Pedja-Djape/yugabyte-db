//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//--------------------------------------------------------------------------------------------------

#include "yb/common/yql_expression.h"
#include "yb/ql/exec/executor.h"
#include "yb/util/yb_partition.h"

namespace yb {
namespace ql {

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS Executor::WhereClauseToPB(QLWriteRequestPB *req,
                                         const MCVector<ColumnOp>& key_where_ops,
                                         const MCList<ColumnOp>& where_ops,
                                         const MCList<SubscriptedColumnOp>& subcol_where_ops) {
  // Setup the key columns.
  for (const auto& op : key_where_ops) {
    const ColumnDesc *col_desc = op.desc();
    QLExpressionPB *col_expr_pb;
    if (col_desc->is_hash()) {
      col_expr_pb = req->add_hashed_column_values();
    } else if (col_desc->is_primary()) {
      col_expr_pb = req->add_range_column_values();
    } else {
      LOG(FATAL) << "Unexpected non primary key column in this context";
    }
    RETURN_NOT_OK(PTExprToPB(op.expr(), col_expr_pb));
  }

  // Setup the rest of the columns.
  CHECK(where_ops.empty()) << "Server doesn't support range operation yet";

  for (const auto& op : subcol_where_ops) {
    const ColumnDesc *col_desc = op.desc();
    QLColumnValuePB *col_pb;
    col_pb = req->add_column_values();
    col_pb->set_column_id(col_desc->id());
    for (auto& arg : op.args()->node_list()) {
      RETURN_NOT_OK(PTExprToPB(arg, col_pb->add_subscript_args()));
    }
    RETURN_NOT_OK(PTExprToPB(op.expr(), col_pb->mutable_expr()));
  }

  return Status::OK();
}

CHECKED_STATUS Executor::WhereClauseToPB(QLReadRequestPB *req,
                                         const MCVector<ColumnOp>& key_where_ops,
                                         const MCList<ColumnOp>& where_ops,
                                         const MCList<SubscriptedColumnOp>& subcol_where_ops,
                                         const MCList<PartitionKeyOp>& partition_key_ops,
                                         const MCList<FuncOp>& func_ops,
                                         bool *no_results) {
  // If where clause restrictions guarantee no results can be found this will be set to true below.
  *no_results = false;

  // Setup the lower/upper bounds on the partition key -- if any
  for (const auto& op : partition_key_ops) {
    QLExpressionPB expr_pb;
    RETURN_NOT_OK(PTExprToPB(op.expr(), &expr_pb));
    uint16_t hash_code;
    QLValueWithPB result;
    WriteAction write_action = WriteAction::REPLACE; // default
    RETURN_NOT_OK(YQLExpression::Evaluate(expr_pb, QLTableRow{}, &result, &write_action));
    hash_code = YBPartition::CqlToYBHashCode(result.int64_value());

    // internally we use [start, end) intervals -- start-inclusive, end-exclusive
    switch (op.yb_op()) {
      case QL_OP_GREATER_THAN:
        if (hash_code != YBPartition::kMaxHashCode) {
          req->set_hash_code(hash_code + 1);
        } else {
          // Token hash greater than max implies no results.
          *no_results = true;
          return Status::OK();
        }
        break;
      case QL_OP_GREATER_THAN_EQUAL:
        req->set_hash_code(hash_code);
        break;
      case QL_OP_LESS_THAN:
        req->set_max_hash_code(hash_code);
        break;
      case QL_OP_LESS_THAN_EQUAL:
        if (hash_code != YBPartition::kMaxHashCode) {
          req->set_max_hash_code(hash_code + 1);
        } // Token hash less or equal than max adds no real restriction.
        break;
      case QL_OP_EQUAL:
        req->set_hash_code(hash_code);
        if (hash_code != YBPartition::kMaxHashCode) {
          req->set_max_hash_code(hash_code + 1);
        }  // Token hash equality restriction with max value needs no upper bound.
        break;

      default:
        LOG(FATAL) << "Unsupported operator for token-based partition key condition";
    }
  }

  // Try to set up key_where_ops as the requests hash key columns. This may be empty.
  bool key_ops_are_set = true;
  for (const auto& op : key_where_ops) {
    const ColumnDesc *col_desc = op.desc();
    QLExpressionPB *col_pb;
    CHECK(col_desc->is_hash()) << "Unexpected non partition column in this context";
    col_pb = req->add_hashed_column_values();
    VLOG(3) << "READ request, column id = " << col_desc->id();
    RETURN_NOT_OK(PTExprToPB(op.expr(), col_pb));
    if (op.yb_op() == QL_OP_IN) {
      int in_size = col_pb->value().list_value().elems_size();
      if (in_size == 0) {
        // Empty 'IN' condition guarantees no results.
        *no_results = true;
        return Status::OK();
      } else if (in_size == 1) {
        // 'IN' condition with one element is treated as equality for efficiency.
        QLValuePB* value_pb = col_pb->mutable_value()->mutable_list_value()->mutable_elems(0);
        col_pb->mutable_value()->Swap(value_pb);
      } else {
        // For now doing filtering in this case TODO(Mihnea) optimize this later.
        key_ops_are_set = false;
        req->clear_hashed_column_values();
        break;
      }
    }
  }

  // Skip generation of query condition if where clause is empty.
  if (key_ops_are_set && where_ops.empty() && subcol_where_ops.empty() && func_ops.empty()) {
    return Status::OK();
  }

  // Setup the where clause.
  QLConditionPB *where_pb = req->mutable_where_expr()->mutable_condition();
  where_pb->set_op(QL_OP_AND);
  if (!key_ops_are_set) {
    for (const auto& col_op : key_where_ops) {
      RETURN_NOT_OK(WhereOpToPB(where_pb->add_operands()->mutable_condition(), col_op));
    }
  }
  for (const auto& col_op : where_ops) {
    RETURN_NOT_OK(WhereOpToPB(where_pb->add_operands()->mutable_condition(), col_op));
  }
  for (const auto& col_op : subcol_where_ops) {
    RETURN_NOT_OK(WhereSubColOpToPB(where_pb->add_operands()->mutable_condition(), col_op));
  }
  for (const auto& func_op : func_ops) {
    RETURN_NOT_OK(FuncOpToPB(where_pb->add_operands()->mutable_condition(), func_op));
  }

  return Status::OK();
}

CHECKED_STATUS Executor::WhereOpToPB(QLConditionPB *condition, const ColumnOp& col_op) {
  // Set the operator.
  condition->set_op(col_op.yb_op());

  // Operand 1: The column.
  const ColumnDesc *col_desc = col_op.desc();
  QLExpressionPB *expr_pb = condition->add_operands();
  VLOG(3) << "WHERE condition, column id = " << col_desc->id();
  expr_pb->set_column_id(col_desc->id());

  // Operand 2: The expression.
  expr_pb = condition->add_operands();
  return PTExprToPB(col_op.expr(), expr_pb);
}

CHECKED_STATUS Executor::WhereSubColOpToPB(QLConditionPB *condition,
                                           const SubscriptedColumnOp& col_op) {
  // Set the operator.
  condition->set_op(col_op.yb_op());

  // Operand 1: The column.
  const ColumnDesc *col_desc = col_op.desc();
  QLExpressionPB *expr_pb = condition->add_operands();
  VLOG(3) << "WHERE condition, sub-column with id = " << col_desc->id();
  auto col_pb = expr_pb->mutable_subscripted_col();
  col_pb->set_column_id(col_desc->id());
  for (auto& arg : col_op.args()->node_list()) {
    RETURN_NOT_OK(PTExprToPB(arg, col_pb->add_subscript_args()));
  }
  // Operand 2: The expression.
  expr_pb = condition->add_operands();
  return PTExprToPB(col_op.expr(), expr_pb);
}

CHECKED_STATUS Executor::FuncOpToPB(QLConditionPB *condition, const FuncOp& func_op) {
  // Set the operator.
  condition->set_op(func_op.yb_op());

  // Operand 1: The function call.
  PTBcall::SharedPtr ptr = func_op.func_expr();
  QLExpressionPB *expr_pb = condition->add_operands();
  RETURN_NOT_OK(PTExprToPB(static_cast<const PTBcall*>(ptr.get()), expr_pb));

  // Operand 2: The expression.
  expr_pb = condition->add_operands();
  return PTExprToPB(func_op.value_expr(), expr_pb);
}

}  // namespace ql
}  // namespace yb
