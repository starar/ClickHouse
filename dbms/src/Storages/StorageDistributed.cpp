#include <DB/Parsers/formatAST.h>

#include <DB/DataStreams/RemoteBlockInputStream.h>
#include <DB/DataStreams/RemoveColumnsBlockInputStream.h>

#include <DB/Storages/StorageDistributed.h>

#include <Poco/Net/NetworkInterface.h>
#include <DB/Client/ConnectionPool.h>

#include <DB/Interpreters/InterpreterSelectQuery.h>
#include <boost/bind.hpp>
#include <DB/Core/Field.h>

namespace DB
{

StorageDistributed::StorageDistributed(
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const String & remote_database_,
	const String & remote_table_,
	Cluster & cluster_,
	const Context & context_,
	const String & sign_column_name_)
	: name(name_), columns(columns_),
	remote_database(remote_database_), remote_table(remote_table_),
	sign_column_name(sign_column_name_),
	context(context_),
	cluster(cluster_)
{
}

StoragePtr StorageDistributed::create(
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const String & remote_database_,
	const String & remote_table_,
	const String & cluster_name,
	Context & context_,
	const String & sign_column_name_)
{
	context_.initClusters();
	return (new StorageDistributed(name_, columns_, remote_database_, remote_table_, context_.getCluster(cluster_name), context_, sign_column_name_))->thisPtr();
}


StoragePtr StorageDistributed::create(
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const String & remote_database_,
	const String & remote_table_,
	SharedPtr<Cluster> & owned_cluster_,
	Context & context_,
	const String & sign_column_name_)
{
	auto res = new StorageDistributed(name_, columns_, remote_database_, remote_table_, *owned_cluster_, context_, sign_column_name_);

	/// Захватываем владение объектом-кластером.
	res->owned_cluster = owned_cluster_;

	return res->thisPtr();
}

ASTPtr StorageDistributed::rewriteQuery(ASTPtr query)
{
	/// Создаем копию запроса.
	ASTPtr modified_query_ast = query->clone();

	/// Меняем имена таблицы и базы данных
	ASTSelectQuery & select = dynamic_cast<ASTSelectQuery &>(*modified_query_ast);
	select.database = new ASTIdentifier(StringRange(), remote_database, ASTIdentifier::Database);
	select.table 	= new ASTIdentifier(StringRange(), remote_table, 	ASTIdentifier::Table);

	return modified_query_ast;
}

static String selectToString(ASTPtr query)
{
	ASTSelectQuery & select = dynamic_cast<ASTSelectQuery &>(*query);
	std::stringstream s;
	formatAST(select, s, 0, false, true);
	return s.str();
}

BlockInputStreams StorageDistributed::read(
	const Names & column_names,
	ASTPtr query,
	const Settings & settings,
	QueryProcessingStage::Enum & processed_stage,
	size_t max_block_size,
	unsigned threads)
{
	/// Установим sign_rewrite = 0, чтобы второй раз не переписывать запрос
	Settings new_settings = settings;
	new_settings.sign_rewrite = false;
	new_settings.queue_max_wait_ms = Cluster::saturate(new_settings.queue_max_wait_ms, settings.limits.max_execution_time);

	size_t result_size = cluster.pools.size() + cluster.getLocalNodesNum();

	processed_stage = result_size == 1
		? QueryProcessingStage::Complete
		: QueryProcessingStage::WithMergeableState;

	BlockInputStreams res;
	ASTPtr modified_query_ast = rewriteQuery(query);

	/// Цикл по шардам.
	for (auto & conn_pool : cluster.pools)
	{
		String modified_query = selectToString(modified_query_ast);

		res.push_back(new RemoteBlockInputStream(
			conn_pool,
			modified_query,
			&new_settings,
			external_tables,
			processed_stage));
	}

	/// Добавляем запросы к локальному ClickHouse.
	if (cluster.getLocalNodesNum() > 0)
	{
		DB::Context new_context = context;
		new_context.setSettings(new_settings);
		for (auto & it : external_tables)
			if (!new_context.tryGetExternalTable(it.first))
				new_context.addExternalTable(it.first, it.second);

		for(size_t i = 0; i < cluster.getLocalNodesNum(); ++i)
		{
			InterpreterSelectQuery interpreter(modified_query_ast, new_context, processed_stage);
				res.push_back(interpreter.execute());
		}
	}

	external_tables.clear();
	return res;
}

void StorageDistributed::alter(const ASTAlterQuery::Parameters &params)
{
	alterColumns(params, columns, context);
}

}
