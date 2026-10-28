#pragma once

#include <string>

// One control frame from the relay: a watcher arrived, a watcher left, or a fresh count.
//
// Engine-free on purpose: tools/observer_notice_test.sh compiles this file with g++ on Linux. That is
// why the signature uses plain int and bool instead of Int and Bool - the caller converts.
//
// Returns false on a frame it cannot read, and then count/who/isJoin are left untouched. On success,
// who is empty for a count-only snapshot and isJoin is only meaningful when who is non-empty. count
// is never negative: a malformed frame that says so is clamped to 0 rather than refused, because a
// count nobody can act on is still less wrong than "-2 watching".
//
// who is safe to show: control characters are stripped and it is at most 64 UTF-8 bytes, so a
// nickname cannot break the line it is formatted into.
bool GencastParseObserverNotice(const char* json, int& count, std::string& who, bool& isJoin);
