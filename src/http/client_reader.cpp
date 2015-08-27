// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2015, LH_Mouse. All wrongs reserved.

#include "../precompiled.hpp"
#include "client_reader.hpp"
#include "const_strings.hpp"
#include "exception.hpp"
#include "utilities.hpp"
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "../log.hpp"
#include "../profiler.hpp"
#include "../string.hpp"

namespace Poseidon {

namespace Http {
	ClientReader::ClientReader()
		: m_sizeExpecting(EXPECTING_NEW_LINE), m_state(S_FIRST_HEADER)
	{
	}
	ClientReader::~ClientReader(){
		if(m_state != S_FIRST_HEADER){
			LOG_POSEIDON_DEBUG("Now that this reader is to be destroyed, a premature response has to be discarded.");
		}
	}

	bool ClientReader::putEncodedData(StreamBuffer encoded){
		PROFILE_ME;

		m_queue.splice(encoded);

		bool hasNextResponse = true;
		do {
			const bool expectingNewLine = (m_sizeExpecting == EXPECTING_NEW_LINE);

			if(expectingNewLine){
				std::size_t lfOffset = 0;
				AUTO(ce, m_queue.getConstChunkEnumerator());
				while(ce){
					const AUTO(pos, std::find(ce.begin(), ce.end(), '\n'));
					if(pos != ce.end()){
						lfOffset += static_cast<std::size_t>(pos - ce.begin());
						goto _found;
					}
					lfOffset += ce.size();
					++ce;
				}
				// 没找到换行符。
				break;
			_found:
				m_sizeExpecting = lfOffset + 1;
			} else {
				if(m_queue.size() < m_sizeExpecting){
					break;
				}
			}

			AUTO(expected, m_queue.cutOff(m_sizeExpecting));
			if(expectingNewLine){
				expected.unput(); // '\n'
				if(expected.back() == '\r'){
					expected.unput();
				}
			}

			switch(m_state){
				boost::uint64_t temp64;

			case S_FIRST_HEADER:
				if(!expected.empty()){
					m_responseHeaders = ResponseHeaders();
					m_contentLength = 0;
					m_contentOffset = 0;

					std::string line;
					expected.dump(line);

					AUTO(pos, line.find(' '));
					if(pos == std::string::npos){
						LOG_POSEIDON_WARNING("Bad request header: expecting verb, line = ", line);
						DEBUG_THROW(BasicException, sslit("No HTTP version in response headers"));
					}
					line[pos] = 0;
					long verEnd = 0;
					char verMajorStr[16], verMinorStr[16];
					if(std::sscanf(line.c_str(), "HTTP/%15[0-9].%15[0-9]%ln", verMajorStr, verMinorStr, &verEnd) != 2){
						LOG_POSEIDON_WARNING("Bad response header: expecting HTTP version: line = ", line.c_str());
						DEBUG_THROW(BasicException, sslit("Malformed HTTP version in response headers"));
					}
					if(static_cast<unsigned long>(verEnd) != pos){
						LOG_POSEIDON_WARNING("Bad response header: junk after HTTP version: line = ", line.c_str());
						DEBUG_THROW(BasicException, sslit("Malformed HTTP version in response headers"));
					}
					m_responseHeaders.version = std::strtoul(verMajorStr, NULLPTR, 10) * 10000 + std::strtoul(verMinorStr, NULLPTR, 10);
					line.erase(0, pos + 1);

					pos = line.find(' ');
					if(pos == std::string::npos){
						LOG_POSEIDON_WARNING("Bad response header: expecting status code: line = ", line.c_str());
						DEBUG_THROW(BasicException, sslit("No status code in response headers"));
					}
					line[pos] = 0;
					char *endptr;
					const AUTO(statusCode, std::strtoul(line.c_str(), &endptr, 10));
					if(*endptr){
						LOG_POSEIDON_WARNING("Bad response header: expecting status code: line = ", line.c_str());
						DEBUG_THROW(BasicException, sslit("Malformed status code in response headers"));
					}
					m_responseHeaders.statusCode = statusCode;
					line.erase(0, pos + 1);

					m_responseHeaders.reason = STD_MOVE(line);

					m_sizeExpecting = EXPECTING_NEW_LINE;
					m_state = S_HEADERS;
				} else {
					m_sizeExpecting = EXPECTING_NEW_LINE;
					// m_state = S_FIRST_HEADER;
				}
				break;

			case S_HEADERS:
				if(!expected.empty()){
					std::string line;
					expected.dump(line);

					AUTO(pos, line.find(':'));
					if(pos == std::string::npos){
						LOG_POSEIDON_WARNING("Invalid HTTP header: line = ", line);
						DEBUG_THROW(BasicException, sslit("Malformed HTTP header in response headers"));
					}
					m_responseHeaders.headers.append(SharedNts(line.data(), pos), ltrim(line.substr(pos + 1)));

					m_sizeExpecting = EXPECTING_NEW_LINE;
					// m_state = S_HEADERS;
				} else {
					AUTO(transferEncoding, m_responseHeaders.headers.get("Transfer-Encoding"));
					AUTO(pos, transferEncoding.find(';'));
					if(pos != std::string::npos){
						transferEncoding.erase(pos);
					}
					transferEncoding = toLowerCase(trim(STD_MOVE(transferEncoding)));

					if(transferEncoding.empty() || (transferEncoding == STR_IDENTITY)){
						const AUTO_REF(contentLength, m_responseHeaders.headers.get("Content-Length"));
						if(contentLength.empty()){
							m_contentLength = CONTENT_TILL_EOF;
						} else {
							char *endptr;
							m_contentLength = ::strtoull(contentLength.c_str(), &endptr, 10);
							if(*endptr){
								LOG_POSEIDON_WARNING("Bad request header Content-Length: ", contentLength);
								DEBUG_THROW(BasicException, sslit("Malformed Content-Length header"));
							}
							if(m_contentLength > CONTENT_LENGTH_MAX){
								LOG_POSEIDON_WARNING("Inacceptable Content-Length: ", contentLength);
								DEBUG_THROW(BasicException, sslit("Inacceptable Content-Length"));
							}
						}
					} else {
						m_contentLength = CONTENT_CHUNKED;
					}

					onResponseHeaders(STD_MOVE(m_responseHeaders), STD_MOVE(transferEncoding), m_contentLength);

					if(m_contentLength == CONTENT_CHUNKED){
						m_sizeExpecting = EXPECTING_NEW_LINE;
						m_state = S_CHUNK_HEADER;
					} else if(m_contentLength == CONTENT_TILL_EOF){
						m_sizeExpecting = 4096;
						m_state = S_IDENTITY;
					} else {
						m_sizeExpecting = std::min<boost::uint64_t>(m_contentLength, 4096);
						m_state = S_IDENTITY;
					}
				}
				break;

			case S_IDENTITY:
				temp64 = std::min<boost::uint64_t>(expected.size(), m_contentLength - m_contentOffset);
				onResponseEntity(m_contentOffset, false, expected.cutOff(temp64));
				m_contentOffset += temp64;

				if(m_contentLength == CONTENT_TILL_EOF){
					m_sizeExpecting = 4096;
					// m_state = S_IDENTITY;
				} else if(m_contentOffset < m_contentLength){
					m_sizeExpecting = std::min<boost::uint64_t>(m_contentLength - m_contentOffset, 4096);
					// m_state = S_IDENTITY;
				} else {
					hasNextResponse = onResponseEnd(m_contentOffset, false, VAL_INIT);

					m_sizeExpecting = EXPECTING_NEW_LINE;
					m_state = S_FIRST_HEADER;
				}
				break;

			case S_CHUNK_HEADER:
				if(!expected.empty()){
					m_chunkSize = 0;
					m_chunkOffset = 0;
					m_chunkedTrailer.clear();

					std::string line;
					expected.dump(line);

					char *endptr;
					m_chunkSize = ::strtoull(line.c_str(), &endptr, 16);
					if(*endptr && (*endptr != ' ')){
						LOG_POSEIDON_WARNING("Bad chunk header: ", line);
						DEBUG_THROW(BasicException, sslit("Malformed chunk header"));
					}
					if(m_chunkSize > CONTENT_LENGTH_MAX){
						LOG_POSEIDON_WARNING("Inacceptable chunk size in header: ", line);
						DEBUG_THROW(BasicException, sslit("Inacceptable chunk length"));
					}
					if(m_chunkSize == 0){
						m_sizeExpecting = EXPECTING_NEW_LINE;
						m_state = S_CHUNKED_TRAILER;
					} else {
						m_sizeExpecting = std::min<boost::uint64_t>(m_chunkSize, 4096);
						m_state = S_CHUNK_DATA;
					}
				} else {
					// chunk-data 后面应该有一对 CRLF。我们在这里处理这种情况。
					m_sizeExpecting = EXPECTING_NEW_LINE;
					// m_state = S_CHUNK_HEADER;
				}
				break;

			case S_CHUNK_DATA:
				temp64 = std::min<boost::uint64_t>(expected.size(), m_chunkSize - m_chunkOffset);
				onResponseEntity(m_contentOffset, true, expected.cutOff(temp64));
				m_contentOffset += temp64;
				m_chunkOffset += temp64;

				if(m_chunkOffset < m_chunkSize){
					m_sizeExpecting = std::min<boost::uint64_t>(m_chunkSize - m_chunkOffset, 4096);
					// m_state = S_CHUNK_DATA;
				} else {
					m_sizeExpecting = EXPECTING_NEW_LINE;
					m_state = S_CHUNK_HEADER;
				}
				break;

			case S_CHUNKED_TRAILER:
				if(!expected.empty()){
					std::string line;
					expected.dump(line);

					AUTO(pos, line.find(':'));
					if(pos == std::string::npos){
						LOG_POSEIDON_WARNING("Invalid chunk trailer: line = ", line);
						DEBUG_THROW(BasicException, sslit("Invalid HTTP header in chunk trailer"));
					}
					m_chunkedTrailer.append(SharedNts(line.data(), pos), ltrim(line.substr(pos + 1)));

					m_sizeExpecting = EXPECTING_NEW_LINE;
					// m_state = S_CHUNKED_TRAILER;
				} else {
					hasNextResponse = onResponseEnd(m_contentOffset, true, STD_MOVE(m_chunkedTrailer));

					m_sizeExpecting = EXPECTING_NEW_LINE;
					m_state = S_FIRST_HEADER;
				}
				break;

			default:
				LOG_POSEIDON_ERROR("Unknown state: ", static_cast<unsigned>(m_state));
				std::abort();
			}
		} while(hasNextResponse);

		return hasNextResponse;
	}

	bool ClientReader::isContentTillEof() const {
		return m_contentLength == CONTENT_TILL_EOF;
	}
	bool ClientReader::terminateContent(){
		PROFILE_ME;

		if(!isContentTillEof()){
			DEBUG_THROW(BasicException, sslit("Terminating a non-until-EOF HTTP response"));
		}

		const AUTO(bytesRemaining, m_queue.size());
		if(bytesRemaining != 0){
			onResponseEntity(m_contentOffset, false, STD_MOVE(m_queue));
			m_contentOffset += bytesRemaining;
			m_queue.clear();
		}

		const bool ret = onResponseEnd(m_contentOffset, false, STD_MOVE(m_chunkedTrailer));

		m_sizeExpecting = EXPECTING_NEW_LINE;
		m_state = S_FIRST_HEADER;

		return ret;
	}
}

}
