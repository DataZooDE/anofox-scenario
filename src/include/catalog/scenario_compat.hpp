//===----------------------------------------------------------------------===//
//                         anofox-scenario
//
// catalog/scenario_compat.hpp
//
// One source tree for DuckDB v1.5.6 and 2.0. Generic API differences are
// handled by duckdb-compat (namespace dc); the helpers here cover the storage
// internals only this extension touches (DataTable DML entry points, index
// entries and ART lookups, table filters, physical operators). Like
// duckdb-compat they detect the API by shape (SFINAE), never by version
// number: call sites carry no #if.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb_compat.hpp"

namespace dc = duckdb_compat;

#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/operator/persistent/physical_merge_into.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/art_operator.hpp"
#include "duckdb/execution/index/art/iterator.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
// 2.0 moved TableFilterSet out of table_filter.hpp
#if __has_include("duckdb/planner/table_filter_set.hpp")
#include "duckdb/planner/table_filter_set.hpp"
#endif
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/table_index_list.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

namespace duckdb {
// 2.0's delta kinds of an index entry (see WithART)
enum class IndexDeltaType : uint8_t;
// the typed table filters of 1.5.6 (see TypedPointFilterValues)
class ConstantFilter;
class InFilter;
class ConjunctionAndFilter;
class OptionalFilter;

namespace scenario_compat {

//===----------------------------------------------------------------------===//
// DataTable
//===----------------------------------------------------------------------===//

//! DataTable::NextParallelScan: the rows of the next morsel (1.5.6, 0 = done)
//! or an optional_idx that is invalid once the scan is done (2.0)
inline bool ParallelScanHasMore(idx_t rows) {
	return rows > 0;
}
inline bool ParallelScanHasMore(const optional_idx &rows) {
	return rows.IsValid();
}

namespace detail {
// 2.0: the DML entry points take the table's catalog entry
template <class DT>
auto Delete(DT &storage, TableDeleteState &state, ClientContext &context, DuckTableEntry &table, Vector &row_ids,
            idx_t count, int) -> decltype(storage.Delete(state, context, table, row_ids, count)) {
	return storage.Delete(state, context, table, row_ids, count);
}
template <class DT>
idx_t Delete(DT &storage, TableDeleteState &state, ClientContext &context, DuckTableEntry &, Vector &row_ids,
             idx_t count, long) {
	return storage.Delete(state, context, row_ids, count);
}

template <class DT>
auto Update(DT &storage, TableUpdateState &state, ClientContext &context, DuckTableEntry &table, Vector &row_ids,
            const vector<PhysicalIndex> &column_ids, DataChunk &data, int)
    -> decltype(storage.Update(state, context, table, row_ids, column_ids, data)) {
	storage.Update(state, context, table, row_ids, column_ids, data);
}
template <class DT>
void Update(DT &storage, TableUpdateState &state, ClientContext &context, DuckTableEntry &, Vector &row_ids,
            const vector<PhysicalIndex> &column_ids, DataChunk &data, long) {
	storage.Update(state, context, row_ids, column_ids, data);
}

template <class DT>
auto LocalAppend(DT &storage, LocalAppendState &state, DuckTableEntry &table, ClientContext &context,
                 DataChunk &chunk, bool unsafe, int)
    -> decltype(storage.LocalAppend(state, table, context, chunk, unsafe)) {
	storage.LocalAppend(state, table, context, chunk, unsafe);
}
template <class DT>
void LocalAppend(DT &storage, LocalAppendState &state, DuckTableEntry &, ClientContext &context, DataChunk &chunk,
                 bool unsafe, long) {
	storage.LocalAppend(state, context, chunk, unsafe);
}
} // namespace detail

//! storage.Delete(state, context, row_ids, count) of `table`'s storage
inline idx_t TableDelete(DataTable &storage, TableDeleteState &state, ClientContext &context, DuckTableEntry &table,
                         Vector &row_ids, idx_t count) {
	return detail::Delete(storage, state, context, table, row_ids, count, 0);
}

//! storage.Update(state, context, row_ids, column_ids, data) of `table`'s storage
inline void TableUpdate(DataTable &storage, TableUpdateState &state, ClientContext &context, DuckTableEntry &table,
                        Vector &row_ids, const vector<PhysicalIndex> &column_ids, DataChunk &data) {
	detail::Update(storage, state, context, table, row_ids, column_ids, data, 0);
}

//! storage.LocalAppend(state, context, chunk, unsafe) of `table`'s storage
inline void TableLocalAppend(DataTable &storage, LocalAppendState &state, DuckTableEntry &table,
                             ClientContext &context, DataChunk &chunk, bool unsafe) {
	detail::LocalAppend(storage, state, table, context, chunk, unsafe, 0);
}

//===----------------------------------------------------------------------===//
// Transactions
//===----------------------------------------------------------------------===//

namespace detail {
// 2.0: the manager tracks the lowest visibility bound, Before(start_time) of
// the oldest active transaction
template <class M>
auto OlderTransactionActive(M &manager, DuckTransaction &transaction, int)
    -> decltype(manager.LowestVisibilityBound(), bool()) {
	using BOUND = decltype(manager.LowestVisibilityBound());
	auto mine = BOUND::Before(transaction.start_time);
	return BOUND::Min(manager.LowestVisibilityBound(), mine) != mine;
}
template <class M>
bool OlderTransactionActive(M &manager, DuckTransaction &transaction, long) {
	return manager.LowestActiveStart() < transaction.start_time;
}
} // namespace detail

//! True when a transaction that started before `transaction` is still active
inline bool OlderTransactionActive(DuckTransactionManager &manager, DuckTransaction &transaction) {
	return detail::OlderTransactionActive(manager, transaction, 0);
}

//===----------------------------------------------------------------------===//
// Index entries and ART lookups
//
// 1.5.6: TableIndexList::IndexEntries() yields IndexEntry& - public `index`,
// `lock` (a mutex) and delta ARTs (deleted_rows_in_use,
// added_data_during_checkpoint) - and ARTs are read through non-const
// methods. 2.0: it yields shared_ptr<IndexEntry>; the physical index and its
// deltas are reached through read/write handles holding the entry's storage
// lock, ARTs are read through const methods, and IndexEntry::VerifyAppend
// checks the index and its checkpoint deltas itself.
//===----------------------------------------------------------------------===//

namespace detail {
inline IndexEntry *HoldIndexEntry(IndexEntry &entry) {
	return &entry;
}
inline shared_ptr<IndexEntry> HoldIndexEntry(shared_ptr<IndexEntry> entry) {
	return entry;
}
} // namespace detail

//! An index entry of a table's index list, kept beyond the iteration: the
//! entry itself (1.5.6, owned by the table) or shared ownership (2.0)
using IndexEntryRef = decltype(detail::HoldIndexEntry(*std::declval<TableIndexList &>().IndexEntries().begin()));

//! Call f(IndexEntryRef) for every index entry of `list`
template <class F>
void ForEachIndexEntry(const TableIndexList &list, F &&f) {
	for (auto &&entry : list.IndexEntries()) {
		f(detail::HoldIndexEntry(entry));
	}
}

namespace detail {
// 2.0
template <class R>
auto IsBoundUniqueART(const R &entry, int) -> decltype(entry->GetBindState(), bool()) {
	return entry->GetBindState() == IndexBindState::BOUND && entry->IsUnique() &&
	       entry->GetIndexType() == ART::TYPE_NAME;
}
template <class R>
bool IsBoundUniqueART(const R &entry, long) {
	auto &index = *entry->index;
	return index.IsBound() && index.IsUnique() && index.GetIndexType() == ART::TYPE_NAME;
}

template <class R>
auto IndexColumns(const R &entry, int) -> decltype(entry->template GetReadHandle<Index>(), unordered_set<column_t>()) {
	auto handle = entry->template GetReadHandle<Index>();
	return handle->GetColumnIdSet();
}
template <class R>
unordered_set<column_t> IndexColumns(const R &entry, long) {
	return entry->index->GetColumnIdSet();
}

template <class R, class F, class DELTA_TYPE = IndexDeltaType>
auto WithART(const R &entry, bool with_deltas, F &f, int) -> decltype(entry->template GetReadHandle<ART>(), void()) {
	auto art = entry->template GetReadHandle<ART>();
	f(*art.operator->());
	if (with_deltas) {
		for (auto type : {DELTA_TYPE::DELETED_ROWS_IN_USE, DELTA_TYPE::ADDED_DATA_DURING_CHECKPOINT}) {
			auto delta = art.FindDelta(type);
			if (delta) {
				f(*delta);
			}
		}
	}
}
template <class R, class F>
void WithART(const R &entry, bool with_deltas, F &f, long) {
	lock_guard<mutex> guard(entry->lock);
	f(entry->index->template Cast<ART>());
	if (with_deltas) {
		for (auto delta : {entry->deleted_rows_in_use.get(), entry->added_data_during_checkpoint.get()}) {
			if (!delta) {
				continue;
			}
			if (delta->GetIndexType() != ART::TYPE_NAME) {
				throw InternalException("anofox_scenario: concurrent changes made to a non-ART delta index");
			}
			f(delta->template Cast<ART>());
		}
	}
}
} // namespace detail

//! True when the entry is a bound, unique ART (a PRIMARY KEY or UNIQUE index)
inline bool IsBoundUniqueART(const IndexEntryRef &entry) {
	return detail::IsBoundUniqueART(entry, 0);
}

//! The physical column ids the entry's index covers
inline unordered_set<column_t> IndexColumns(const IndexEntryRef &entry) {
	return detail::IndexColumns(entry, 0);
}

//! Call f(art) on the entry's ART - `ART &` (1.5.6) or `const ART &` (2.0) -
//! while holding the entry's lock, and with_deltas also on the delta ARTs a
//! committed row id can live in: deleted_rows_in_use (deleted while older
//! transactions still see it) and added_data_during_checkpoint. The entry
//! must be a bound ART (IsBoundUniqueART).
template <class F>
void WithART(const IndexEntryRef &entry, bool with_deltas, F &&f) {
	detail::WithART(entry, with_deltas, f, 0);
}

namespace detail {
template <class P>
auto LeafNode(const P &leaf, int) -> decltype(leaf.Get()) {
	return leaf.Get();
}
template <class P>
auto LeafNode(const P &leaf, long) -> decltype(*leaf) {
	return *leaf;
}
} // namespace detail

//! Every row id the ART stores under `key` (a unique ART can briefly hold two:
//! a row deleted by a running transaction plus its replacement; the delta ARTs
//! are not unique at all). A = ART or const ART.
template <class A>
void CollectARTRowIds(A &art, const ARTKey &key, vector<row_t> &result) {
	if (key.Empty() || !art.tree.HasMetadata()) {
		return;
	}
	auto leaf = ARTOperator::Lookup(art, art.tree, key, 0);
	if (!leaf) {
		return;
	}
	auto &&node = detail::LeafNode(leaf, 0);
	if (node.GetType() == NType::LEAF_INLINED) {
		result.push_back(node.GetRowId());
		return;
	}
	// a nested leaf (gate): iterate its row ids, exactly as ART::SearchEqual
	set<row_t> row_ids;
	Iterator it(art);
	it.FindMinimum(node);
	ARTKey empty_key;
	RowIdSetOutput output(row_ids, NumericLimits<idx_t>::Maximum());
	it.Scan(empty_key, output, false);
	result.insert(result.end(), row_ids.begin(), row_ids.end());
}

//! The row id of an inlined leaf under `key`, if any (a PK point lookup)
template <class A>
bool LookupInlinedRowId(A &art, const ARTKey &key, row_t &row_id) {
	if (key.Empty() || !art.tree.HasMetadata()) {
		return false;
	}
	auto leaf = ARTOperator::Lookup(art, art.tree, key, 0);
	if (!leaf) {
		return false;
	}
	auto &&node = detail::LeafNode(leaf, 0);
	if (node.GetType() != NType::LEAF_INLINED) {
		return false;
	}
	row_id = node.GetRowId();
	return true;
}

namespace detail {
// 2.0: IndexEntry::VerifyAppend locks the entry and covers its checkpoint
// deltas; the mask is a transaction-local delete copy of the index
template <class R>
auto VerifyAppend(const R &entry, DataChunk &rows, int) -> decltype(entry->VerifyAppend(nullptr, rows, nullptr)) {
	entry->VerifyAppend(nullptr, rows, nullptr);
}
template <class R>
void VerifyAppend(const R &entry, DataChunk &rows, long) {
	auto &art = entry->index->template Cast<ART>();
	lock_guard<mutex> guard(entry->lock);
	IndexAppendInfo append_info;
	art.VerifyAppend(rows, append_info, nullptr);
}

template <class R>
struct UniqueMaskV2 {
	R entry;
	TableIndexList delete_indexes;
	TableIndexList append_indexes;
	R mask;
};
template <class R>
struct UniqueMaskV1 {
	R entry;
	unique_ptr<BoundIndex> mask;
};

// 1.5.6's ART::CreateDeltaIndex(DeltaIndexType): the enum is named through the
// member's parameter type so that 2.0 (which has neither) never looks it up
template <class>
struct FirstParameter;
template <class RESULT, class CLASS, class PARAM>
struct FirstParameter<RESULT (CLASS::*)(PARAM) const> {
	using type = PARAM;
};
template <class A, class DELTA_TYPE = typename FirstParameter<decltype(&A::CreateDeltaIndex)>::type>
unique_ptr<BoundIndex> CreateLocalDeleteIndex(A &art) {
	return art.CreateDeltaIndex(DELTA_TYPE::LOCAL_DELETE);
}

template <class R>
auto MakeUniqueMask(const R &entry, int) -> decltype(entry->VerifyAppend(entry, std::declval<DataChunk &>(), nullptr),
                                                      unique_ptr<UniqueMaskV2<R>>()) {
	auto result = make_uniq<UniqueMaskV2<R>>();
	result->entry = entry;
	entry->InitializeLocalIndexes(result->delete_indexes, result->append_indexes);
	result->mask = result->delete_indexes.FindEntry(*entry);
	if (!result->mask) {
		throw InternalException("anofox_scenario: unique index '%s' has no delete mask", dc::Str(entry->GetName()));
	}
	return result;
}
template <class R>
unique_ptr<UniqueMaskV1<R>> MakeUniqueMask(const R &entry, long) {
	auto result = make_uniq<UniqueMaskV1<R>>();
	result->entry = entry;
	result->mask = CreateLocalDeleteIndex(entry->index->template Cast<ART>());
	return result;
}

template <class R>
void MaskAppend(UniqueMaskV2<R> &mask, DataChunk &rows, Vector &row_ids) {
	mask.mask->AppendToDeleteIndexes(rows, row_ids);
}
template <class R>
void MaskAppend(UniqueMaskV1<R> &mask, DataChunk &rows, Vector &row_ids) {
	IndexAppendInfo mask_info(IndexAppendMode::IGNORE_DUPLICATES, nullptr);
	auto error = mask.mask->Append(rows, row_ids, mask_info);
	if (error.HasError()) {
		throw InternalException("anofox_scenario: unexpected error building the unique mask: %s", error.Message());
	}
}

template <class R>
void MaskVerifyAppend(UniqueMaskV2<R> &mask, DataChunk &rows) {
	mask.entry->VerifyAppend(mask.mask, rows, nullptr);
}
template <class R>
void MaskVerifyAppend(UniqueMaskV1<R> &mask, DataChunk &rows) {
	auto &art = mask.entry->index->template Cast<ART>();
	lock_guard<mutex> guard(mask.entry->lock);
	IndexAppendInfo append_info(IndexAppendMode::DEFAULT, mask.mask.get());
	art.VerifyAppend(rows, append_info, nullptr);
}
} // namespace detail

//! Throw a ConstraintException when appending `rows` would violate the
//! entry's unique index (the rows are not appended)
inline void VerifyAppend(const IndexEntryRef &entry, DataChunk &rows) {
	detail::VerifyAppend(entry, rows, 0);
}

//! A delete mask over a unique index: VerifyAppend ignores the index rows
//! added to the mask, as if they were deleted
using UniqueMask = decltype(detail::MakeUniqueMask(std::declval<const IndexEntryRef &>(), 0));

//! An empty mask for the entry's unique ART index
inline UniqueMask MakeUniqueMask(const IndexEntryRef &entry) {
	return detail::MakeUniqueMask(entry, 0);
}

//! Mask the index rows of `rows` (in table layout) stored under `row_ids`
inline void MaskAppend(UniqueMask &mask, DataChunk &rows, Vector &row_ids) {
	detail::MaskAppend(*mask, rows, row_ids);
}

//! VerifyAppend against the index with the masked rows ignored
inline void MaskVerifyAppend(UniqueMask &mask, DataChunk &rows) {
	detail::MaskVerifyAppend(*mask, rows);
}

//===----------------------------------------------------------------------===//
// Pushed-down table filters
//
// 1.5.6: TableFilterSet is a public map<idx_t, unique_ptr<TableFilter>>
// (`filters`) keyed by projection position, and equality-style filters arrive
// as ConstantFilter / InFilter / ConjunctionAndFilter / OptionalFilter. 2.0:
// the map is private behind iterators keyed by ProjectionIndex, and every
// pushed filter is an ExpressionFilter over a column placeholder (the typed
// filter classes only remain for deserializing old plans, renamed LEGACY_*).
//===----------------------------------------------------------------------===//

namespace detail {
template <class S, class F>
auto ForEachTableFilter(const S &set, F &f, int) -> decltype((*set.begin()).Filter(), void()) {
	for (auto &entry : set) {
		f(idx_t(entry.GetIndex()), entry.Filter());
	}
}
template <class S, class F>
void ForEachTableFilter(const S &set, F &f, long) {
	for (auto &entry : set.filters) {
		f(entry.first, *entry.second);
	}
}

template <class>
struct ConstMemberParameter;
template <class RESULT, class CLASS, class PARAM>
struct ConstMemberParameter<RESULT (CLASS::*)(PARAM) const> {
	using type = typename std::remove_cv<typename std::remove_reference<PARAM>::type>::type;
};
// 2.0: keyed by ProjectionIndex (named through the member's parameter type)
template <class S, class INDEX = typename ConstMemberParameter<decltype(&S::GetFilterByColumnIndex)>::type>
const TableFilter &GetTableFilter(const S &set, idx_t position, int) {
	return set.GetFilterByColumnIndex(INDEX(position));
}
template <class S>
const TableFilter &GetTableFilter(const S &set, idx_t position, long) {
	return *set.filters.at(position);
}

template <class>
struct PushFilterIndex;
template <class RESULT, class CLASS, class INDEX, class FILTER>
struct PushFilterIndex<RESULT (CLASS::*)(INDEX, FILTER)> {
	using type = typename std::remove_cv<typename std::remove_reference<INDEX>::type>::type;
};
} // namespace detail

//! Call f(idx_t position, const TableFilter &filter) for every single-column
//! filter of the set (keyed by projection position). 2.0's multi-column
//! filters are only pushed into DuckDB's own tables, and only partially.
template <class F>
void ForEachTableFilter(const TableFilterSet &set, F &&f) {
	detail::ForEachTableFilter(set, f, 0);
}

namespace detail {
template <class S>
auto HasTableFilters(const S &set, int) -> decltype(set.HasFilters()) {
	return set.HasFilters();
}
template <class S>
bool HasTableFilters(const S &set, long) {
	return !set.filters.empty();
}
} // namespace detail

//! True when the set holds any filter
inline bool HasTableFilters(const TableFilterSet &set) {
	return detail::HasTableFilters(set, 0);
}

//! The filter of the column at projection `position` (it must have one)
inline const TableFilter &GetTableFilter(const TableFilterSet &set, idx_t position) {
	return detail::GetTableFilter(set, position, 0);
}

//! set.PushFilter(position, filter): ColumnIndex (1.5.6) or ProjectionIndex (2.0)
template <class S = TableFilterSet>
void PushTableFilter(S &set, idx_t position, unique_ptr<TableFilter> filter) {
	using INDEX = typename detail::PushFilterIndex<decltype(&S::PushFilter)>::type;
	set.PushFilter(INDEX(position), std::move(filter));
}

namespace detail {
bool PointFilterValues(const TableFilter &filter, vector<Value> &values);

// 2.0: the comparison is a BoundFunctionExpression, IN a BoundOperatorExpression
// over the column placeholder, optional filters an internal function wrapper
template <class EF = ExpressionFilter>
auto ExpressionPointValues(const Expression &expr, vector<Value> &values, int)
    -> decltype(EF::GetOptionalFilterChild(expr), EF::IsRootOptionalExpression(expr), bool()) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		if (EF::IsRootOptionalExpression(expr)) {
			// optional filters are re-checked above the scan, so narrowing
			// the delta side to their values never changes the result
			auto child = EF::GetOptionalFilterChild(expr);
			return child && ExpressionPointValues<EF>(*child, values, 0);
		}
		if (expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
			return false;
		}
		ExpressionType comparison_type;
		auto constant = EF::TryGetColumnConstantComparison(expr.Cast<BoundFunctionExpression>(), comparison_type);
		if (!constant || comparison_type != ExpressionType::COMPARE_EQUAL) {
			return false;
		}
		values.push_back(dc::ConstantValue(*constant));
		return true;
	}
	if (expr.GetExpressionType() == ExpressionType::COMPARE_IN &&
	    expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &children = dc::Children(expr.Cast<BoundOperatorExpression>());
		if (children.empty() || !EF::IsSimpleFilterColumnRef(*children[0])) {
			return false;
		}
		vector<Value> in_values;
		for (idx_t i = 1; i < children.size(); i++) {
			if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
				return false;
			}
			in_values.push_back(dc::ConstantValue(children[i]->template Cast<BoundConstantExpression>()));
		}
		values = std::move(in_values);
		return true;
	}
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND &&
	    expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		for (auto &child : dc::Children(expr.Cast<BoundConjunctionExpression>())) {
			if (ExpressionPointValues<EF>(*child, values, 0)) {
				return true;
			}
		}
	}
	return false;
}
template <class EF = ExpressionFilter>
bool ExpressionPointValues(const Expression &, vector<Value> &, long) {
	return false;
}

// 1.5.6: the typed filter classes (2.0 only keeps them as Legacy*, for old
// plans). They are reached through template parameters defaulting to the
// forward declarations below: on 2.0 the classes are incomplete and
// TypedPointFilterValues drops out of overload resolution.
template <class CF = ConstantFilter, class IF = InFilter, class AF = ConjunctionAndFilter, class OF = OptionalFilter>
auto TypedPointFilterValues(const TableFilter &filter, vector<Value> &values, int)
    -> decltype(sizeof(CF) + sizeof(IF) + sizeof(AF) + sizeof(OF), bool()) {
	if (filter.filter_type == CF::TYPE) {
		auto &constant = filter.Cast<CF>();
		if (constant.comparison_type != ExpressionType::COMPARE_EQUAL) {
			return false;
		}
		values.push_back(constant.constant);
		return true;
	}
	if (filter.filter_type == IF::TYPE) {
		values = filter.Cast<IF>().values;
		return true;
	}
	if (filter.filter_type == AF::TYPE) {
		for (auto &child : filter.Cast<AF>().child_filters) {
			if (PointFilterValues(*child, values)) {
				return true;
			}
		}
		return false;
	}
	if (filter.filter_type == OF::TYPE) {
		// optional filters are re-checked above the scan, so narrowing the
		// delta side to their values never changes the result
		auto &optional = filter.Cast<OF>();
		return optional.child_filter && PointFilterValues(*optional.child_filter, values);
	}
	return false;
}
inline bool TypedPointFilterValues(const TableFilter &, vector<Value> &, long) {
	return false;
}

inline bool PointFilterValues(const TableFilter &filter, vector<Value> &values) {
	if (filter.filter_type == ExpressionFilter::TYPE) {
		auto &expression = filter.Cast<ExpressionFilter>();
		return expression.expr && ExpressionPointValues(*expression.expr, values, 0);
	}
	return TypedPointFilterValues(filter, values, 0);
}

template <class F>
auto CopyTableFilter(const F &filter, int) -> decltype(filter.Copy()) {
	return filter.Copy();
}
template <class F, class EF = ExpressionFilter>
unique_ptr<TableFilter> CopyTableFilter(const F &filter, long) {
	return EF::GetExpressionFilter(filter, "anofox_scenario").Copy();
}
} // namespace detail

//! A copy of a pushed filter (2.0: every pushed filter is an ExpressionFilter)
inline unique_ptr<TableFilter> CopyTableFilter(const TableFilter &filter) {
	return detail::CopyTableFilter(filter, 0);
}

//! The values an equality-style pushed filter pins a column to: `= c`,
//! `IN (...)`, or such a filter inside an AND / optional filter. False when
//! the filter does not pin the column to a finite set.
inline bool PointFilterValues(const TableFilter &filter, vector<Value> &values) {
	return detail::PointFilterValues(filter, values);
}

//===----------------------------------------------------------------------===//
// Table functions scanning a catalog table
//
// The planner learns which table a scan reads (LogicalGet::GetTable) from
// get_bind_info returning BindInfo(table) on 1.5.6, and from get_table_entry
// on 2.0 (BindInfo no longer carries a table).
//===----------------------------------------------------------------------===//

template <TableCatalogEntry &(*GET_TABLE)(const FunctionData &)>
struct ScanTableCallbacks {
	static optional_ptr<TableCatalogEntry> GetTableEntry(optional_ptr<const FunctionData> bind_data) {
		return &GET_TABLE(*bind_data);
	}
	template <class BIND_INFO = BindInfo>
	static BIND_INFO GetBindInfo(const optional_ptr<FunctionData> bind_data) {
		return BIND_INFO(GET_TABLE(*bind_data));
	}
};

namespace detail {
template <class CALLBACKS, class TF>
auto SetScanTable(TF &function, int) -> decltype(function.get_table_entry = &CALLBACKS::GetTableEntry, void()) {
	function.get_table_entry = &CALLBACKS::GetTableEntry;
}
template <class CALLBACKS, class TF>
void SetScanTable(TF &function, long) {
	function.get_bind_info = &CALLBACKS::template GetBindInfo<>;
}
} // namespace detail

//! Make `function` report GET_TABLE(bind_data) as the table it scans
template <TableCatalogEntry &(*GET_TABLE)(const FunctionData &)>
void SetScanTable(TableFunction &function) {
	detail::SetScanTable<ScanTableCallbacks<GET_TABLE>>(function, 0);
}

//===----------------------------------------------------------------------===//
// Physical operators
//===----------------------------------------------------------------------===//

using MergeIntoActions = map<MergeActionCondition, vector<unique_ptr<MergeIntoOperator>>>;

namespace detail {
// 2.0: the child is a constructor argument (and added by the constructor)
template <class OP = PhysicalMergeInto>
auto MakeMergeInto(PhysicalPlanGenerator &planner, vector<LogicalType> &types, PhysicalOperator &child,
                   MergeIntoActions &actions, idx_t row_id_index, optional_idx source_marker, bool parallel,
                   bool return_chunk, int)
    -> decltype(OP(std::declval<PhysicalPlan &>(), types, child, std::move(actions), row_id_index, source_marker,
                   parallel, return_chunk),
                std::declval<PhysicalOperator &>()) {
	return planner.Make<OP>(types, child, std::move(actions), row_id_index, source_marker, parallel, return_chunk);
}
template <class OP = PhysicalMergeInto>
PhysicalOperator &MakeMergeInto(PhysicalPlanGenerator &planner, vector<LogicalType> &types, PhysicalOperator &child,
                                MergeIntoActions &actions, idx_t row_id_index, optional_idx source_marker,
                                bool parallel, bool return_chunk, long) {
	auto &result =
	    planner.Make<OP>(types, std::move(actions), row_id_index, source_marker, parallel, return_chunk);
	result.children.push_back(child);
	return result;
}
} // namespace detail

//! A PhysicalMergeInto over `child`
inline PhysicalOperator &MakeMergeInto(PhysicalPlanGenerator &planner, vector<LogicalType> &types,
                                       PhysicalOperator &child, MergeIntoActions actions, idx_t row_id_index,
                                       optional_idx source_marker, bool parallel, bool return_chunk) {
	return detail::MakeMergeInto(planner, types, child, actions, row_id_index, source_marker, parallel, return_chunk,
	                             0);
}

} // namespace scenario_compat
} // namespace duckdb
