// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2018, LH_Mouse. All wrongs reserved.

#include "../precompiled.hpp"
#include "verbs.hpp"

namespace Poseidon {
namespace Http {

namespace {
	CONSTEXPR const char VERB_TABLE[][16] = {
		"INVALID_VERB",
		"GET",
		"POST",
		"HEAD",
		"PUT",
		"DELETE",
		"TRACE",
		"CONNECT",
		"OPTIONS",
	};
}

Verb get_verb_from_string(const char *str){
	const std::size_t len = std::strlen(str);
	if(len == 0){
		return V_INVALID_VERB;
	}
	const char *const begin = VERB_TABLE[0];
	const AUTO(pos, static_cast<const char *>(::memmem(begin, sizeof(VERB_TABLE), str, len + 1)));
	if(!pos){
		return V_INVALID_VERB;
	}
	std::size_t index = static_cast<std::size_t>(pos - begin) / sizeof(VERB_TABLE[0]);
	if(pos != VERB_TABLE[index]){
		return V_INVALID_VERB;
	}
	return static_cast<Verb>(index);
}
const char *get_string_from_verb(Verb verb){
	std::size_t index = static_cast<std::size_t>(verb);
	if(index >= COUNT_OF(VERB_TABLE)){
		index = static_cast<unsigned>(V_INVALID_VERB);
	}
	return VERB_TABLE[index];
}

}
}
