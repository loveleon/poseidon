// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2018, LH_Mouse. All wrongs reserved.

#ifndef POSEIDON_ZLIB_HPP_
#define POSEIDON_ZLIB_HPP_

#include "cxx_util.hpp"
#include "stream_buffer.hpp"
#include <string>
#include <cstddef>
#include <cstring>
#include <boost/scoped_ptr.hpp>

#undef ZLIB_CONST
#define ZLIB_CONST 1
#include <zlib.h>

namespace Poseidon {

class Deflator : NONCOPYABLE {
private:
	::z_stream m_stream;
	unsigned char m_temp[16384];
	StreamBuffer m_buffer;

public:
	explicit Deflator(bool gzip = false, int level = 8);
	~Deflator();

public:
	const StreamBuffer &get_buffer() const {
		return m_buffer;
	}
	StreamBuffer &get_buffer(){
		return m_buffer;
	}

	void clear();
	void put(const void *data, std::size_t size);
	void put(char ch){
		put(&ch, 1);
	}
	void put(const char *str){
		put(str, std::strlen(str));
	}
	void put(const std::string &str){
		put(str.data(), str.size());
	}
	void put(const StreamBuffer &buffer);
	void flush();
	StreamBuffer finalize();
};

class Inflator : NONCOPYABLE {
private:
	::z_stream m_stream;
	unsigned char m_temp[16384];
	StreamBuffer m_buffer;

public:
	explicit Inflator(bool gzip = false);
	~Inflator();

public:
	const StreamBuffer &get_buffer() const {
		return m_buffer;
	}
	StreamBuffer &get_buffer(){
		return m_buffer;
	}

	void clear();
	void put(const void *data, std::size_t size);
	void put(char ch){
		put(&ch, 1);
	}
	void put(const char *str){
		put(str, std::strlen(str));
	}
	void put(const std::string &str){
		put(str.data(), str.size());
	}
	StreamBuffer finalize();
	void flush();
	void put(const StreamBuffer &buffer);
};

}

#endif
