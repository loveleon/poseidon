// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2018, LH_Mouse. All wrongs reserved.

#ifndef POSEIDON_SINGLETONS_MONGODB_DAEMON_HPP_
#define POSEIDON_SINGLETONS_MONGODB_DAEMON_HPP_

#include "../cxx_ver.hpp"
#include "../mongodb/fwd.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>

namespace Poseidon {

class Promise;

class MongoDbDaemon {
private:
	MongoDbDaemon();

public:
	typedef boost::function<void (const boost::shared_ptr<MongoDb::Connection> &)> QueryCallback;

	static void start();
	static void stop();

	// 同步接口。
	static boost::shared_ptr<MongoDb::Connection> create_connection(bool from_slave = false);

	static void wait_for_all_async_operations();

	// 异步接口。
	static boost::shared_ptr<const Promise> enqueue_for_saving(boost::shared_ptr<const MongoDb::ObjectBase> object, bool to_replace, bool urgent);
	static boost::shared_ptr<const Promise> enqueue_for_loading(boost::shared_ptr<MongoDb::ObjectBase> object, MongoDb::BsonBuilder query);
	static boost::shared_ptr<const Promise> enqueue_for_deleting(const char *collection, MongoDb::BsonBuilder query);
	static boost::shared_ptr<const Promise> enqueue_for_batch_loading(QueryCallback callback, const char *collection_hint, MongoDb::BsonBuilder query);

	static void enqueue_for_low_level_access(const boost::shared_ptr<Promise> &promise, QueryCallback callback, const char *collection_hint, bool from_slave = false);

	static boost::shared_ptr<const Promise> enqueue_for_waiting_for_all_async_operations();
};

}

#endif
