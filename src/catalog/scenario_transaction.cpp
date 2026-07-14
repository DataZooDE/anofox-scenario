#include "catalog/scenario_catalog.hpp"

namespace duckdb {

ScenarioTransaction::ScenarioTransaction(TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context) {
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
