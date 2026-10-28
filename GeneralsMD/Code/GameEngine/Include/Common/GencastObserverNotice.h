#pragma once

#include <string>

// One control frame from the relay: a watcher arrived, a watcher left, or a fresh count.
//
// Engine-free on purpose: tools/observer_notice_test.sh compiles this file with g++ on Linux. That is
// why the signature uses plain int and bool instead of Int and Bool - the caller converts.
//
// Returns false on a frame it cannot read, and then count/who/isJoin are left untouched. On success,
// who is empty for a count-only snapshot and isJoin is only meaningful when who is non-empty.
bool GencastParseObserverNotice(const char* json, int& count, std::string& who, bool& isJoin);
