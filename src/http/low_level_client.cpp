#include "../precompiled.hpp"
#include "low_level_client.hpp"
#include "exception.hpp"
#include "status_codes.hpp"
#include "utilities.hpp"
#include "../log.hpp"
#include "../profiler.hpp"
#include "../string.hpp"

namespace Poseidon {

namespace Http {
	namespace {
		const std::string IDENTITY_STRING	= "identity";
		const std::string CHUNKED_STRING	= "chunked";
	}

	LowLevelClient::LowLevelClient(const SockAddr &addr, bool useSsl)
		: TcpClientBase(addr, useSsl)
		, m_expectingNewLine(true), m_sizeExpecting(0), m_state(S_FIRST_HEADER)
	{
	}
	LowLevelClient::LowLevelClient(const IpPort &addr, bool useSsl)
		: TcpClientBase(addr, useSsl)
		, m_expectingNewLine(true), m_sizeExpecting(0), m_state(S_FIRST_HEADER)
	{
	}
	LowLevelClient::~LowLevelClient(){
		if(m_state != S_FIRST_HEADER){
			LOG_POSEIDON_WARNING("Now that this client is to be destroyed, a premature response has to be discarded.");
		}
	}

	void LowLevelClient::onReadAvail(const void *data, std::size_t size){
		PROFILE_ME;

		try {
			m_received.put(data, size);

			for(;;){
				if(m_expectingNewLine){
					struct Helper {
						static bool traverseCallback(void *ctx, const void *data, std::size_t size){
							AUTO_REF(lfOffset, *static_cast<std::size_t *>(ctx));

							const AUTO(pos, std::memchr(data, '\n', size));
							if(!pos){
								lfOffset += size;
								return true;
							}
							lfOffset += static_cast<std::size_t>(static_cast<const char *>(pos) - static_cast<const char *>(data));
							return false;
						}
					};

					std::size_t lfOffset = 0;
					if(m_received.traverse(&Helper::traverseCallback, &lfOffset)){
						// 没找到换行符。
						break;
					}
					// 找到了。
					m_sizeExpecting = lfOffset + 1;
				} else {
					if(m_received.size() < m_sizeExpecting){
						break;
					}
				}

				AUTO(expected, m_received.cut(m_sizeExpecting));
				if(m_expectingNewLine){
					expected.unput(); // '\n'
					if(expected.back() == '\r'){
						expected.unput();
					}
				}

				switch(m_state){
				case S_FIRST_HEADER:
					if(!expected.empty()){
						m_responseHeaders = ResponseHeaders();
						m_contentLength = 0;
						m_contentOffset = 0;

						m_chunkSize = 0;
						m_chunkOffset = 0;
						m_chunkTrailer.clear();

						std::string line;
						expected.dump(line);

						std::size_t pos = line.find(' ');
						if(pos == std::string::npos){
							LOG_POSEIDON_WARNING("Bad response header: expecting HTTP version: line = ", line.c_str());
							DEBUG_THROW(BasicException, SSLIT("No HTTP version in response header"));
						}
						line[pos] = 0;
						long verEnd = 0;
						char verMajorStr[16], verMinorStr[16];
						if(std::sscanf(line.c_str(), "HTTP/%15[0-9].%15[0-9]%ln", verMajorStr, verMinorStr, &verEnd) != 2){
							LOG_POSEIDON_WARNING("Bad response header: expecting HTTP version: line = ", line.c_str());
							DEBUG_THROW(BasicException, SSLIT("Malformed HTTP version in response header"));
						}
						if(static_cast<unsigned long>(verEnd) != pos){
							LOG_POSEIDON_WARNING("Bad response header: junk after HTTP version: line = ", line.c_str());
							DEBUG_THROW(BasicException, SSLIT("Malformed HTTP version in response header"));
						}
						m_responseHeaders.version = std::strtoul(verMajorStr, NULLPTR, 10) * 10000 + std::strtoul(verMinorStr, NULLPTR, 10);
						line.erase(0, pos + 1);

						pos = line.find(' ');
						if(pos == std::string::npos){
							LOG_POSEIDON_WARNING("Bad response header: expecting status code: line = ", line.c_str());
							DEBUG_THROW(BasicException, SSLIT("No status code in response header"));
						}
						line[pos] = 0;
						char *endptr;
						const AUTO(statusCode, std::strtoul(line.c_str(), &endptr, 10));
						if(*endptr){
							LOG_POSEIDON_WARNING("Bad response header: expecting status code: line = ", line.c_str());
							DEBUG_THROW(BasicException, SSLIT("Malformed status code in response header"));
						}
						m_responseHeaders.statusCode = static_cast<StatusCode>(statusCode);
						line.erase(0, pos + 1);

						m_responseHeaders.reason = STD_MOVE(line);

						m_expectingNewLine = true;
						m_state = S_HEADERS;
					} else {
						// m_state = S_FIRST_HEADER;
					}
					break;

				case S_HEADERS:
					if(!expected.empty()){
						std::string line;
						expected.dump(line);

						std::size_t pos = line.find(':');
						if(pos == std::string::npos){
							LOG_POSEIDON_WARNING("Invalid HTTP header: line = ", line);
							DEBUG_THROW(BasicException, SSLIT("Malformed HTTP header in response header"));
						}
						m_responseHeaders.headers.append(SharedNts(line.c_str(), pos),
							line.substr(line.find_first_not_of(' ', pos + 1)));

						m_expectingNewLine = true;
						// m_state = S_HEADERS;
					} else {
						AUTO(transferEncoding, explode<std::string>(',', m_responseHeaders.headers.get("Transfer-Encoding")));
						for(AUTO(it, transferEncoding.begin()); it != transferEncoding.end(); ++it){
							*it = toLowerCase(trim(STD_MOVE(*it)));
						}
						std::sort(transferEncoding.begin(), transferEncoding.end());
						AUTO(range, std::equal_range(transferEncoding.begin(), transferEncoding.end(), IDENTITY_STRING));
						transferEncoding.erase(range.first, range.second);

						boost::uint64_t contentLength;
						if(!transferEncoding.empty()){
							range = std::equal_range(transferEncoding.begin(), transferEncoding.end(), CHUNKED_STRING);
							transferEncoding.erase(range.first, range.second);
							contentLength = CONTENT_CHUNKED;
						} else {
							const AUTO_REF(contentLengthStr, m_responseHeaders.headers.get("Content-Length"));
							if(contentLengthStr.empty()){
								contentLength = CONTENT_TILL_EOF;
							} else {
								char *endptr;
								contentLength = ::strtoull(contentLengthStr.c_str(), &endptr, 10);
								if(*endptr){
									LOG_POSEIDON_WARNING("Bad request header Content-Length: ", contentLengthStr);
									DEBUG_THROW(BasicException, SSLIT("Malformed Content-Length"));
								}
								if(contentLength > CONTENT_LENGTH_MAX){
									LOG_POSEIDON_WARNING("Inacceptable Content-Length: ", contentLengthStr);
									DEBUG_THROW(BasicException, SSLIT("Inacceptable Content-Length"));
								}
							}
						}

						onLowLevelResponseHeaders(STD_MOVE(m_responseHeaders), STD_MOVE(transferEncoding), contentLength);

						m_contentLength = contentLength;

						if(contentLength == CONTENT_CHUNKED){
							m_expectingNewLine = true;
							m_state = S_CHUNK_HEADER;
						} else if(contentLength == CONTENT_TILL_EOF){
							m_expectingNewLine = false;
							m_sizeExpecting = 1024;
							m_state = S_IDENTITY;
						} else {
							m_expectingNewLine = false;
							m_sizeExpecting = std::min<boost::uint64_t>(contentLength, 1024);
							m_state = S_IDENTITY;
						}
					}
					break;

				case S_IDENTITY:
					boost::uint64_t bytesAvail;
					if(m_contentLength == CONTENT_TILL_EOF){
						bytesAvail = expected.size();
					} else {
						bytesAvail = std::min<boost::uint64_t>(expected.size(), m_contentLength - m_contentOffset);
					}
					onLowLevelEntity(m_contentOffset, expected.cut(bytesAvail));
					m_contentOffset += bytesAvail;

					if(m_contentLength == CONTENT_TILL_EOF){
						m_expectingNewLine = false;
						m_sizeExpecting = 1024;
						// m_state = S_IDENTITY;
					} else {
						if(m_contentOffset < m_contentLength){
							m_expectingNewLine = false;
							m_sizeExpecting = std::min<boost::uint64_t>(m_contentLength - m_contentOffset, 1024);
							// m_state = S_IDENTITY;
						} else {
							onLowLevelResponseEof(m_contentOffset, VAL_INIT);

							m_expectingNewLine = true;
							m_state = S_FIRST_HEADER;
						}
					}
					break;

				case S_CHUNK_HEADER:
					if(!expected.empty()){
						std::string line;
						expected.dump(line);

						char *endptr;
						const boost::uint64_t chunkSize = ::strtoull(line.c_str(), &endptr, 16);
						if(*endptr && (*endptr != ' ')){
							LOG_POSEIDON_WARNING("Bad chunk header: ", line);
							DEBUG_THROW(BasicException, SSLIT("Bad chunk header"));
						}
						if(chunkSize == 0){
							onLowLevelResponseEof(m_contentOffset, VAL_INIT);

							m_expectingNewLine = true;
							m_state = S_CHUNKED_TRAILER;
						} else {
							m_chunkSize = chunkSize;
							m_chunkOffset = 0;

							m_expectingNewLine = false;
							m_sizeExpecting = std::min<boost::uint64_t>(chunkSize, 1024);
							m_state = S_CHUNK_DATA;
						}
					} else {
						// chunk-data 后面应该有一对 CRLF。我们在这里处理这种情况。
					}
					break;

				case S_CHUNK_DATA:
					if(!expected.empty()){
						const AUTO(bytesAvail, std::min<boost::uint64_t>(expected.size(), m_chunkSize - m_chunkOffset));
						onLowLevelEntity(m_contentOffset, expected.cut(bytesAvail));
						m_contentOffset += bytesAvail;
						m_chunkOffset += bytesAvail;

						if(m_chunkOffset < m_chunkSize){
							m_expectingNewLine = false;
							m_sizeExpecting = std::min<boost::uint64_t>(m_chunkSize - m_chunkOffset, 1024);
							// m_state = S_CHUNK_DATA;
						} else {
							m_expectingNewLine = true;
							m_state = S_CHUNK_HEADER;
						}
					}
					break;

				case S_CHUNKED_TRAILER:
					if(!expected.empty()){
						std::string line;
						expected.dump(line);

						std::size_t pos = line.find(':');
						if(pos == std::string::npos){
							LOG_POSEIDON_WARNING("Invalid HTTP header: line = ", line);
							DEBUG_THROW(BasicException, SSLIT("Invalid HTTP header"));
						}
						m_chunkTrailer.append(SharedNts(line.c_str(), pos), line.substr(line.find_first_not_of(' ', pos + 1)));

						m_expectingNewLine = true;
						// m_state = S_CHUNKED_TRAILER;
					} else {
						onLowLevelResponseEof(m_contentOffset, STD_MOVE(m_chunkTrailer));

						m_expectingNewLine = true;
						m_state = S_FIRST_HEADER;
					}
					break;

				default:
					LOG_POSEIDON_ERROR("Unknown state: ", static_cast<unsigned>(m_state));
					std::abort();
				}
			}
		} catch(std::exception &e){
			LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO,
				"std::exception thrown while parsing request: what = ", e.what());
			forceShutdown();
		}
	}

	void LowLevelClient::onReadHup() NOEXCEPT {
		PROFILE_ME;

		try {
			LOG_POSEIDON_DEBUG("HTTP client read hang up");

			if((m_state == S_IDENTITY) && (m_contentLength == CONTENT_TILL_EOF)){
				onLowLevelResponseEof(m_contentOffset, VAL_INIT);

				m_expectingNewLine = true;
				m_state = S_FIRST_HEADER;
			}
		} catch(std::exception &e){
			LOG_POSEIDON_ERROR("std::exception thrown when processing read hang up event: what = ", e.what());
			forceShutdown();
		}

		return TcpSessionBase::onReadHup();
	}

	bool LowLevelClient::send(RequestHeaders requestHeaders, StreamBuffer entity){
		PROFILE_ME;

		StreamBuffer data;

		data.put(getStringFromVerb(requestHeaders.verb));
		data.put(' ');
		data.put(requestHeaders.uri);
		if(!requestHeaders.getParams.empty()){
			data.put('?');
			data.put(urlEncodedFromOptionalMap(requestHeaders.getParams));
		}
		char temp[64];
		const unsigned verMajor = requestHeaders.version / 10000, verMinor = requestHeaders.version % 10000;
		unsigned len = (unsigned)std::sprintf(temp, " HTTP/%u.%u\r\n", verMajor, verMinor);
		data.put(temp, len);

		AUTO_REF(headers, requestHeaders.headers);
		if(!entity.empty() && !headers.has("Content-Type")){
			headers.set("Content-Type", "application/x-www-form-urlencoded; charset=utf-8");
		}
		const AUTO_REF(transferEncodingStr, headers.get("Transfer-Encoding"));
		if(transferEncodingStr.empty()){
			if(!entity.empty()){
				headers.set("Content-Length", boost::lexical_cast<std::string>(entity.size()));
			}
		} else {
			// 只有一个 chunk。
			char str[256];
			unsigned len = (unsigned)std::sprintf(str, "%llx\r\n", (unsigned long long)entity.size());

			StreamBuffer temp;
			temp.swap(entity);
			entity.put(str, len);
			entity.splice(temp);
			entity.put("\r\n0\r\n\r\n");
		}
		for(AUTO(it, headers.begin()); it != headers.end(); ++it){
			data.put(it->first.get());
			data.put(": ");
			data.put(it->second.data(), it->second.size());
			data.put("\r\n");
		}
		data.put("\r\n");

		data.splice(entity);
		return TcpClientBase::send(STD_MOVE(data));
	}

	bool LowLevelClient::send(Verb verb, std::string uri, OptionalMap getParams, OptionalMap headers, StreamBuffer entity){
		PROFILE_ME;

		RequestHeaders requestHeaders;
		requestHeaders.verb = verb;
		requestHeaders.uri = STD_MOVE(uri);
		requestHeaders.version = 10001;
		requestHeaders.getParams = STD_MOVE(getParams);
		requestHeaders.headers = STD_MOVE(headers);
		return send(STD_MOVE(requestHeaders), STD_MOVE(entity));
	}
}

}
