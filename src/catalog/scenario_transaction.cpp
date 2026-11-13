#include "catalog/scenario_catalog.hpp"

namespace duckdb {

static atomic<idx_t> transaction_serial {0};

ScenarioTransaction::ScenarioTransaction(TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), serial(++transaction_serial) {
}

optional_ptr<CatalogEntry> ScenarioTransaction::FindEntry(const string &name, bool enumeration) {
	lock_guard<mutex> guard(table_entries_lock);
	auto &entries = enumeration ? enumeration_entries : table_entries;
	auto entry = entries.find(name);
	if (entry == entries.end()) {
		return nullptr;
	}
	return entry->second.get();
}

CatalogEntry &ScenarioTransaction::PublishEntry(const string &name, unique_ptr<CatalogEntry> entry,
                                                bool enumeration) {
	lock_guard<mutex> guard(table_entries_lock);
	auto &entries = enumeration ? enumeration_entries : table_entries;
	// a concurrent pipeline may have published it meanwhile: keep that one,
	// references to it may already be in use
	auto inserted = entries.emplace(name, std::move(entry));
	return *inserted.first->second;
}

CatalogEntry &ScenarioTransaction::ReplaceEntry(const string &name, CatalogEntry &stale,
                                                unique_ptr<CatalogEntry> entry, bool enumeration) {
	lock_guard<mutex> guard(table_entries_lock);
	auto &entries = enumeration ? enumeration_entries : table_entries;
	auto existing = entries.find(name);
	if (existing == entries.end()) {
		return *entries.emplace(name, std::move(entry)).first->second;
	}
	if (existing->second.get() != &stale) {
		return *existing->second; // a sibling pipeline replaced it already
	}
	retired_entries.push_back(std::move(existing->second));
	existing->second = std::move(entry);
	return *existing->second;
}

ScenarioTransactionManager::ScenarioTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
}

Transaction &ScenarioTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<ScenarioTransaction>(*this, context);
	auto &result = *transaction;
	lock_guard<mutex> guard(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData ScenarioTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	// Deliberately empty: all physical state lives in the host catalog, whose
	// DuckTransactionManager commits inside the same MetaTransaction.
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
	return ErrorData();
}

void ScenarioTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
}

void ScenarioTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// Nothing to checkpoint: the host database owns all storage.
}

} // namespace duckdb
