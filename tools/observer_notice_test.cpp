// The relay's watcher notice, parsed outside the engine.
//
//   ./tools/observer_notice_test.sh
//
// The frames the relay actually sends: someone joins, someone leaves, and a snapshot with no
// names at all (the client asked and nobody moved). The rest are frames we must not trust: an
// empty object, a name that spells a field name, a string that never closes, and a negative count.
#include "Common/GencastObserverNotice.h"

#include <cstdio>
#include <string>

static int g_checked = 0;
static int g_failed = 0;

static void expect(bool condition, const char* what)
{
	++g_checked;

	if (!condition)
	{
		++g_failed;
		std::printf("FAIL %s\n", what);
	}
}

int main()
{
	int cases = 0;
	int casesFailed = 0;

	// Someone starts watching: the count moves and the name is the newcomer.
	{
		++cases;

		int count = -1;
		std::string who = "stale";
		bool isJoin = false;

		expect(GencastParseObserverNotice("{\"count\":2,\"joined\":\"Dennis\",\"left\":null}",
		                                  count, who, isJoin), "a join parses");
		expect(count == 2, "a join carries the new count");
		expect(who == "Dennis", "a join carries the name");
		expect(isJoin, "a join says so");

		if (g_failed)
		{
			++casesFailed;
		}
	}

	// Someone stops watching: the name is escaped by the relay, so the quote must not end the string.
	{
		++cases;

		const int before = g_failed;
		int count = -1;
		std::string who;
		bool isJoin = true;

		expect(GencastParseObserverNotice("{\"count\":1,\"left\":\"Dennis \\\"Doc\\\"\",\"joined\":null}",
		                                  count, who, isJoin), "a leave parses");
		expect(count == 1, "a leave carries the new count");
		expect(who == "Dennis \"Doc\"", "a leave carries the unescaped name");
		expect(!isJoin, "a leave says so");

		if (g_failed != before)
		{
			++casesFailed;
		}
	}

	// A snapshot: both names null, so only the count is news and no name may survive from before.
	{
		++cases;

		const int before = g_failed;
		int count = -1;
		std::string who = "stale";
		bool isJoin = true;

		expect(GencastParseObserverNotice("{\"count\":3,\"joined\":null,\"left\":null}",
		                                  count, who, isJoin), "a snapshot parses");
		expect(count == 3, "a snapshot carries the count");
		expect(who.empty(), "a snapshot clears the name");
		expect(!isJoin, "a snapshot is not a join");

		if (g_failed != before)
		{
			++casesFailed;
		}
	}

	// No count means we cannot say anything: the frame is rejected and the caller's values stay put.
	{
		++cases;

		const int before = g_failed;
		int count = 7;
		std::string who = "untouched";
		bool isJoin = true;

		expect(!GencastParseObserverNotice("{}", count, who, isJoin), "an empty frame is rejected");
		expect(count == 7, "a rejected frame leaves the count alone");
		expect(who == "untouched", "a rejected frame leaves the name alone");
		expect(isJoin, "a rejected frame leaves the direction alone");

		if (g_failed != before)
		{
			++casesFailed;
		}
	}

	// A name is free text: it may spell the field name, carry quotes, and hold a character above
	// the BMP that the relay escapes as a surrogate pair. None of that may be mistaken for the
	// frame's own fields, and the pair must come back as one character.
	{
		++cases;

		const int before = g_failed;
		int count = -1;
		std::string who;
		bool isJoin = false;

		expect(GencastParseObserverNotice(
		           "{\"joined\":\"\\uD83D\\uDE00 \\\"count\\\":99\",\"count\":4,\"left\":null}",
		           count, who, isJoin), "a name that spells a field parses");
		expect(count == 4, "the count in the name is not the count");
		expect(who == "\xF0\x9F\x98\x80 \"count\":99", "the name keeps its text, emoji and all");
		expect(isJoin, "the join direction survives a hostile name");

		if (g_failed != before)
		{
			++casesFailed;
		}
	}

	// A quote that never closes: half a frame is not a frame, and the caller must not see the
	// count that had already been read before the parser gave up.
	{
		++cases;

		const int before = g_failed;
		int count = 7;
		std::string who = "untouched";
		bool isJoin = true;

		expect(!GencastParseObserverNotice("{\"count\":5,\"joined\":\"Dennis", count, who, isJoin),
		       "an unterminated string is rejected");
		expect(count == 7, "an unterminated string leaves the count alone");
		expect(who == "untouched", "an unterminated string leaves the name alone");
		expect(isJoin, "an unterminated string leaves the direction alone");

		if (g_failed != before)
		{
			++casesFailed;
		}
	}

	// A negative count: the relay has no state that says -2 watching, so this is a broken frame -
	// but a readable one. Clamped to zero rather than refused, because the HUD must never print
	// "-2 watching" and the notice is otherwise harmless.
	{
		++cases;

		const int before = g_failed;
		int count = 7;
		std::string who = "stale";
		bool isJoin = true;

		expect(GencastParseObserverNotice("{\"count\":-2,\"joined\":null,\"left\":null}",
		                                  count, who, isJoin), "a negative count still parses");
		expect(count == 0, "a negative count is clamped to zero");
		expect(who.empty(), "a negative count names nobody");
		expect(!isJoin, "a negative count is not a join");

		if (g_failed != before)
		{
			++casesFailed;
		}
	}

	// A name that tries to break the line it lands in: the relay's \n escape decodes to a real
	// newline, and a 200-byte name would otherwise be formatted whole into lobby chat. The control
	// character must be gone and the text must fit the cap - and the cap must fall on a character
	// boundary, or an emoji that straddles it would leave the HUD invalid UTF-8.
	{
		++cases;

		const int before = g_failed;
		const std::string huge(200, 'A');
		const std::string frame = "{\"count\":5,\"joined\":\"Dennis\\n" + huge + "\",\"left\":null}";

		int count = -1;
		std::string who;
		bool isJoin = false;

		expect(GencastParseObserverNotice(frame.c_str(), count, who, isJoin),
		       "a newline and a 200-byte name still parse");
		expect(count == 5, "a hostile name leaves the count alone");
		expect(who.find('\n') == std::string::npos, "the newline is stripped from the name");
		expect(who.size() <= 64, "the name is capped at 64 bytes");
		expect(who == "Dennis" + std::string(58, 'A'), "the cap keeps the start of the name");

		// 62 bytes plus a four-byte emoji: the emoji does not fit, and half of it would be worse
		// than none of it.
		const std::string emoji = "\xF0\x9F\x98\x80";
		const std::string boundaryFrame =
			"{\"count\":1,\"joined\":\"" + std::string(62, 'A') + emoji + "\",\"left\":null}";

		expect(GencastParseObserverNotice(boundaryFrame.c_str(), count, who, isJoin),
		       "a name with an emoji at the cap parses");
		expect(who == std::string(62, 'A'), "the cap does not cut an emoji in half");

		if (g_failed != before)
		{
			++casesFailed;
		}
	}

	if (casesFailed == 0)
	{
		std::printf("PASS: %d cases\n", cases);
	}
	else
	{
		std::printf("FAIL: %d of %d cases (%d checks failed)\n", casesFailed, cases, g_failed);
	}

	return casesFailed == 0 ? 0 : 1;
}
