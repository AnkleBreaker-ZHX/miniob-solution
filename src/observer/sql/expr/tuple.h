/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2021/5/14.
//

#pragma once

#include <memory>
#include <vector>
#include <string>

#include "common/log/log.h"
#include "sql/expr/tuple_cell.h"
#include "sql/parser/parse.h"
#include "sql/parser/value.h"
#include "sql/expr/expression.h"
#include "storage/record/record.h"
#include "common/lang/bitmap.h"

class Table;

/**
 * @defgroup Tuple
 * @brief Tuple 元组，表示一行数据，当前返回客户端时使用
 * @details 
 * tuple是一种可以嵌套的数据结构。
 * 比如select t1.a+t2.b from t1, t2;
 * 需要使用下面的结构表示：
 * @code {.cpp}
 *  Project(t1.a+t2.b)
 *        |
 *      Joined
 *      /     \
 *   Row(t1) Row(t2)
 * @endcode
 * 
 */

/**
 * @brief 元组的结构，包含哪些字段(这里成为Cell)，每个字段的说明
 * @ingroup Tuple
 */
class TupleSchema 
{
public:
  void append_cell(const TupleCellSpec &cell)
  {
    cells_.push_back(cell);
  }
  void append_cell(const char *table, const char *field)
  {
    append_cell(TupleCellSpec(table, field));
  }
  void append_cell(const char *alias)
  {
    append_cell(TupleCellSpec(alias));
  }
  int cell_num() const
  {
    return static_cast<int>(cells_.size());
  }
  const TupleCellSpec &cell_at(int i) const
  {
    return cells_[i];
  }

private:
  std::vector<TupleCellSpec> cells_;
};

/**
 * @brief 元组的抽象描述
 * @ingroup Tuple
 */
class Tuple 
{
public:
  Tuple() = default;
  virtual ~Tuple() = default;

  /**
   * @brief 获取元组中的Cell的个数
   * @details 个数应该与tuple_schema一致
   */
  virtual int cell_num() const = 0;

  /**
   * @brief 获取指定位置的Cell
   * 
   * @param index 位置
   * @param[out] cell  返回的Cell
   */
  virtual RC cell_at(int index, Value &cell) const = 0;

  /**
   * @brief 根据cell的描述，获取cell的值
   * 
   * @param spec cell的描述
   * @param[out] cell 返回的cell
   */
  virtual RC find_cell(const TupleCellSpec &spec, Value &cell) const = 0;

  virtual std::string to_string() const
  {
    std::string str;
    const int cell_num = this->cell_num();
    for (int i = 0; i < cell_num - 1; i++) {
      Value cell;
      cell_at(i, cell);
      str += cell.to_string();
      str += ", ";
    }

    if (cell_num > 0) {
      Value cell;
      cell_at(cell_num - 1, cell);
      str += cell.to_string();
    }
    return str;
  }
};

/**
 * @brief 一行数据的元组
 * @ingroup Tuple
 * @details 直接就是获取表中的一条记录
 */
class RowTuple : public Tuple 
{
public:
  RowTuple() = default;
  virtual ~RowTuple()
  {
    for (FieldExpr *spec : speces_) {
      delete spec;
    }
    speces_.clear();
  }

  void set_record(Record *record)
  {
    this->record_ = record;
    ASSERT(!this->speces_.empty(), "RowTuple speces empty!");
    const FieldMeta* null_field = this->speces_.front()->field().meta();
    ASSERT(nullptr != null_field && CHARS == null_field->type(), "RowTuple get null field failed!");
    bitmap_.init(record->data() + null_field->offset(), this->speces_.size());
  }

  void set_schema(const Table *table)
  {
    ASSERT(nullptr != table, "RowTuple set_schema with a null table");
    table_ = table;
    const std::vector<FieldMeta> *fields = table_->table_meta().field_metas();
    this->speces_.reserve(fields->size());
    for (const FieldMeta &field : *fields) {
      speces_.push_back(new FieldExpr(table, &field));
    }
  }

  int cell_num() const override
  {
    return speces_.size();
  }

  RC cell_at(int index, Value &cell) const override
  {
    RC rc = RC::SUCCESS;
    if (index < 0 || index >= static_cast<int>(speces_.size())) {
      LOG_WARN("invalid argument. index=%d", index);
      return RC::INVALID_ARGUMENT;
    }

    if (bitmap_.get_bit(index)) {
      cell.set_null();
    } else {
      FieldExpr *field_expr = speces_[index];
      const FieldMeta *field_meta = field_expr->field().meta();
      if (TEXTS == field_meta->type()) {
        cell.set_type(CHARS);
        int64_t offset = *(int64_t*)(record_->data() + field_meta->offset());
        int64_t length = *(int64_t*)(record_->data() + field_meta->offset() + sizeof(int64_t));
        char *text = (char*)malloc(length);
        rc = table_->read_text(offset, length, text);
        if (RC::SUCCESS != rc) {
          LOG_WARN("Failed to read text from table, rc=%s", strrc(rc));
          return rc;
        }
        cell.set_data(text, length);
        free(text);
      } else {
        cell.set_type(field_meta->type());
        cell.set_data(this->record_->data() + field_meta->offset(), field_meta->len());
      }
    }
    return RC::SUCCESS;
  }

  RC find_cell(const TupleCellSpec &spec, Value &cell) const override
  {
    const char *table_name = spec.table_name();
    const char *field_name = spec.field_name();
    if (0 != strcmp(table_name, table_->name())) {
      return RC::NOTFOUND;
    }

    for (size_t i = 0; i < speces_.size(); ++i) {
      const FieldExpr *field_expr = speces_[i];
      const Field &field = field_expr->field();
      if (0 == strcmp(field_name, field.field_name())) {
        return cell_at(i, cell);
      }
    }
    return RC::NOTFOUND;
  }

#if 0
  RC cell_spec_at(int index, const TupleCellSpec *&spec) const override
  {
    if (index < 0 || index >= static_cast<int>(speces_.size())) {
      LOG_WARN("invalid argument. index=%d", index);
      return RC::INVALID_ARGUMENT;
    }
    spec = speces_[index];
    return RC::SUCCESS;
  }
#endif

  Record &record()
  {
    return *record_;
  }

  const Record &record() const
  {
    return *record_;
  }

private:
  Record *record_ = nullptr;
  common::Bitmap bitmap_;
  const Table *table_ = nullptr;
  std::vector<FieldExpr *> speces_;
};

/**
 * @brief 从一行数据中，选择部分字段组成的元组，也就是投影操作
 * @ingroup Tuple
 * @details 一般在select语句中使用。
 * 投影也可以是很复杂的操作，比如某些字段需要做类型转换、重命名、表达式运算、函数计算等。
 * 当前的实现是比较简单的，只是选择部分字段，不做任何其他操作。
 */
class ProjectTuple : public Tuple 
{
public:
  ProjectTuple() = default;
  virtual ~ProjectTuple() { exprs_.clear(); }

  void set_tuple(Tuple *tuple)
  {
    this->tuple_ = tuple;
  }

  void add_expr(std::unique_ptr<Expression> expr)
  {
    exprs_.emplace_back(std::move(expr));
  }
  int cell_num() const override
  {
    return exprs_.size();
  }

  RC cell_at(int index, Value &cell) const override
  {
    if (index < 0 || index >= static_cast<int>(exprs_.size())) {
      return RC::INTERNAL;
    }
    if (tuple_ == nullptr) {
      return RC::INTERNAL;
    }

    // 原本这里会根据 tuple cell spec 去 tuple_ 里 find cell
    // 现在这个逻辑是在 FieldExpr 的 get_value 里面
    return exprs_[index]->get_value(*tuple_, cell);
  }

  RC find_cell(const TupleCellSpec &spec, Value &cell) const override
  {
    return tuple_->find_cell(spec, cell);
  }

  const std::vector<std::unique_ptr<Expression>>& expressions() const
  {
    return exprs_;
  }
#if 0
  RC cell_spec_at(int index, const TupleCellSpec *&spec) const override
  {
    if (index < 0 || index >= static_cast<int>(speces_.size())) {
      return RC::NOTFOUND;
    }
    spec = speces_[index];
    return RC::SUCCESS;
  }
private:
  std::vector<TupleCellSpec *> speces_;
#endif
private:
  std::vector<std::unique_ptr<Expression>> exprs_;
  Tuple *tuple_ = nullptr;
};

class ExpressionTuple : public Tuple 
{
public:
  ExpressionTuple(std::vector<std::unique_ptr<Expression>> &expressions)
    : expressions_(expressions)
  {
  }
  
  virtual ~ExpressionTuple()
  {
  }

  int cell_num() const override
  {
    return expressions_.size();
  }

  RC cell_at(int index, Value &cell) const override
  {
    if (index < 0 || index >= static_cast<int>(expressions_.size())) {
      return RC::INTERNAL;
    }

    const Expression *expr = expressions_[index].get();
    return expr->try_get_value(cell);
  }

  RC find_cell(const TupleCellSpec &spec, Value &cell) const override
  {
    for (const std::unique_ptr<Expression> &expr : expressions_) {
      if (0 == strcmp(spec.alias(), expr->name().c_str())) {
        return expr->try_get_value(cell);
      }
    }
    return RC::NOTFOUND;
  }


private:
  const std::vector<std::unique_ptr<Expression>> &expressions_;
};

/**
 * @brief 一些常量值组成的Tuple
 * @ingroup Tuple
 */
class ValueListTuple : public Tuple 
{
public:
  ValueListTuple() = default;
  virtual ~ValueListTuple() = default;

  void set_cells(const std::vector<Value> &cells)
  {
    cells_ = cells;
  }

  virtual int cell_num() const override
  {
    return static_cast<int>(cells_.size());
  }

  virtual RC cell_at(int index, Value &cell) const override
  {
    if (index < 0 || index >= cell_num()) {
      return RC::NOTFOUND;
    }

    cell = cells_[index];
    return RC::SUCCESS;
  }

  virtual RC find_cell(const TupleCellSpec &spec, Value &cell) const override
  {
    return RC::INTERNAL;
  }

private:
  std::vector<Value> cells_;
};

/**
 * @brief 将两个tuple合并为一个tuple
 * @ingroup Tuple
 * @details 在join算子中使用
 */
class JoinedTuple : public Tuple 
{
public:
  JoinedTuple() = default;
  virtual ~JoinedTuple() = default;

  void set_left(Tuple *left)
  {
    left_ = left;
  }
  void set_right(Tuple *right)
  {
    right_ = right;
  }

  int cell_num() const override
  {
    return left_->cell_num() + right_->cell_num();
  }

  RC cell_at(int index, Value &value) const override
  {
    const int left_cell_num = left_->cell_num();
    if (index > 0 && index < left_cell_num) {
      return left_->cell_at(index, value);
    }

    if (index >= left_cell_num && index < left_cell_num + right_->cell_num()) {
      return right_->cell_at(index - left_cell_num, value);
    }

    return RC::NOTFOUND;
  }

  RC find_cell(const TupleCellSpec &spec, Value &value) const override
  {
    RC rc = left_->find_cell(spec, value);
    if (rc == RC::SUCCESS || rc != RC::NOTFOUND) {
      return rc;
    }

    return right_->find_cell(spec, value);
  }

private:
  Tuple *left_ = nullptr;
  Tuple *right_ = nullptr;
};


class GroupTuple : public Tuple {
public:
  GroupTuple() = default;
  virtual ~GroupTuple()
  {
    // TODO(wbj) manage memory
    // for (AggrFuncExpr *expr : aggr_exprs_)
    //   delete expr;
    // aggr_exprs_.clear();
  }

  void set_tuple(Tuple *tuple)
  {
    this->tuple_ = tuple;
  }

  int cell_num() const override
  {
    return tuple_->cell_num();
  }

  RC cell_at(int index, Value &cell) const override
  {
    if (tuple_ == nullptr) {
      return RC::INTERNAL;
    }
    return tuple_->cell_at(index, cell);
  }

  RC find_cell(const TupleCellSpec &spec, Value &cell) const override
  {
    if (tuple_ == nullptr) {
      return RC::INTERNAL;
    }
    // if (field.with_aggr()) {
    //   for (size_t i = 0; i < aggr_exprs_.size(); ++i) {
    //     AggrFuncExpression &expr = *aggr_exprs_[i];
    //     if (field.equal(expr.field()) && expr.get_aggr_func_type() == field.get_aggr_type()) {
    //       cell = aggr_results_[i];//其中存储的是计算好的结果
    //       LOG_INFO("Field is found in aggr_exprs");
    //       return RC::SUCCESS;
    //     }
    //   }
    // }
    // for (size_t i = 0; i < field_exprs_.size(); ++i) {
    //   FieldExpr &expr = *field_exprs_[i];
    //   if (field.equal(expr.field())) {
    //     cell = field_results_[i];
    //     LOG_INFO("Field is found in field_exprs");
    //     return RC::SUCCESS;
    //   }
    // }
    // return RC::NOTFOUND;
  }

  // void get_record(CompoundRecord &record) const override
  // {
  //   tuple_->get_record(record);
  // }

  // void set_record(CompoundRecord &record) override
  // {
  //   tuple_->set_record(record);
  // }

  // void set_right_record(CompoundRecord &record) override
  // {
  //   tuple_->set_right_record(record);
  // }

  // RC cell_spec_at(int index, const TupleCellSpec *&spec) const
  // {
  //   if (index < 0 || index >= cell_num()) {
  //     return RC::INVALID_ARGUMENT;
  //   }
  //   return tuple_->cell_spec_at(index, spec);
  // }

  const std::vector<AggrFuncExpression *> &get_aggr_exprs() const
  {
    return aggr_exprs_;
  }

  const std::vector<FieldExpr *> &get_field_exprs() const
  {
    return field_exprs_;
  }

  void do_aggregate_first()
  {
    
  }

  void do_aggregate();

  void do_aggregate_done();

  void init(const std::vector<AggrFuncExpression *> &aggr_exprs, const std::vector<FieldExpr *> &field_exprs)
  {
    counts_.resize(aggr_exprs.size());
    all_null_.resize(aggr_exprs.size());
    aggr_results_.resize(aggr_exprs.size());
    aggr_exprs_ = aggr_exprs;
    field_results_.resize(field_exprs.size());
    field_exprs_ = field_exprs;
  }

private:
  int count_ = 0;
  std::vector<bool> all_null_;           // for every aggr expr
  std::vector<int> counts_;              // for every aggr expr
  std::vector<Value> aggr_results_;  // for every aggr expr
  std::vector<Value> field_results_;

  // not own these below
  std::vector<FieldExpr *> field_exprs_;
  std::vector<AggrFuncExpression *> aggr_exprs_;  // only use these AggrFuncExpr's type and field info
  Tuple *tuple_ = nullptr;
};
