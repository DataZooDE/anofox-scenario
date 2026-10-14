#include "catalog/scenario_catalog.hpp"

namespace duckdb {

static atomic<idx_t> transaction_serial {0};

ScenarioTransaction::ScenarioTransaction(TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), serial(++transaction_serial) {
}

optional_ptr<CatalogEntry> ScenarioTransaction::FindEntry(const string &name) {
	lock_guard<mutex> guard(table_entries_lock);
	auto entry = table_entries.find(name);
	if (entry == table_entries.end()) {
		return nullptr;
	}
	return entry->second.get();
}

CatalogEntry &ScenarioTransaction::PublishEntry(const string &name, unique_ptr<CatalogEntry> entry) {
	lock_guard<mutex> guard(table_entries_lock);
	// a concurrent pipeline may have published it meanwhile: keep that one,
	// references to it may already be in use
	auto inserted = table_entries.emplace(name, std::move(entry));
	return *inserted.first->second;
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
