#include "Common/GencastObserverNotice.h"

#include <cstring>

namespace {

	// The frame is three flat fields, so it is scanned by hand instead of pulling in a library. It has
	// to respect string boundaries: a player name is free text and may itself contain "count" or a quote.
	struct Scanner
	{
		const char* p;
		const char* end;

		bool atEnd() const { return p >= end; }
		char peek() const { return p < end ? *p : '\0'; }

		void skipSpace()
		{
			while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
			{
				++p;
			}
		}

		bool consume(char c)
		{
			skipSpace();

			if (p < end && *p == c)
			{
				++p;
				return true;
			}

			return false;
		}

		// Four hex digits. One decoder serves both the single escape and the low half of a
		// surrogate pair, so the two cannot disagree about what counts as a digit.
		bool readHex4(unsigned& out)
		{
			unsigned value = 0;

			for (int i = 0; i < 4; ++i)
			{
				if (p >= end)
				{
					return false;
				}

				const char h = *p++;
				int digit = 0;

				if (h >= '0' && h <= '9')
				{
					digit = h - '0';
				}
				else if (h >= 'a' && h <= 'f')
				{
					digit = h - 'a' + 10;
				}
				else if (h >= 'A' && h <= 'F')
				{
					digit = h - 'A' + 10;
				}
				else
				{
					return false;
				}

				value = value * 16 + (unsigned)digit;
			}

			out = value;

			return true;
		}

		// The HUD wants bytes, so a code point becomes UTF-8 here. Anything above the BMP needs
		// four of them - an emoji nickname is exactly that case.
		static void appendUtf8(std::string& out, unsigned code)
		{
			if (code < 0x80)
			{
				out.push_back((char)code);
			}
			else if (code < 0x800)
			{
				out.push_back((char)(0xC0 | (code >> 6)));
				out.push_back((char)(0x80 | (code & 0x3F)));
			}
			else if (code < 0x10000)
			{
				out.push_back((char)(0xE0 | (code >> 12)));
				out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
				out.push_back((char)(0x80 | (code & 0x3F)));
			}
			else
			{
				out.push_back((char)(0xF0 | (code >> 18)));
				out.push_back((char)(0x80 | ((code >> 12) & 0x3F)));
				out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
				out.push_back((char)(0x80 | (code & 0x3F)));
			}
		}

		// Escapes are resolved because the relay escapes quotes and backslashes, and a raw \" must not
		// be mistaken for the closing quote.
		bool readString(std::string& out)
		{
			out.clear();
			skipSpace();

			if (p >= end || *p != '"')
			{
				return false;
			}

			++p;

			while (p < end)
			{
				const char c = *p++;

				if (c == '"')
				{
					return true;
				}

				if (c != '\\')
				{
					out.push_back(c);
					continue;
				}

				if (p >= end)
				{
					return false;
				}

				switch (*p++)
				{
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					case '/': out.push_back('/'); break;
					case 'b': out.push_back('\b'); break;
					case 'f': out.push_back('\f'); break;
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;

					case 'u':
					{
						// System.Text.Json escapes anything outside ASCII, so a name like "Ren\u00e9"
						// arrives here and must not come out as a question mark.
						unsigned code = 0;

						if (!readHex4(code))
						{
							return false;
						}

						// A character above the BMP arrives as a UTF-16 surrogate pair. Decoded one
						// half at a time it becomes two replacement marks, so an emoji nickname
						// would reach the HUD garbled; the pair is one character and is combined.
						if (code >= 0xD800 && code <= 0xDBFF)
						{
							const char* const afterHigh = p;
							unsigned low = 0;
							bool haveLow = false;

							if (end - p >= 6 && p[0] == '\\' && p[1] == 'u')
							{
								p += 2;
								haveLow = readHex4(low);
							}

							if (haveLow && low >= 0xDC00 && low <= 0xDFFF)
							{
								code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
							}
							else
							{
								// Not a pair after all: the high half is unshowable, but whatever
								// follows it is an ordinary escape that must still be read.
								code = 0xFFFD;
								p = afterHigh;
							}
						}
						else if (code >= 0xDC00 && code <= 0xDFFF)
						{
							code = 0xFFFD;	// a low half with nothing before it
						}

						appendUtf8(out, code);

						break;
					}

					default:
						return false;
				}
			}

			return false;	// unterminated
		}

		// Advances over anything numeric so an unknown field of any shape can be stepped over.
		bool skipNumber()
		{
			skipSpace();
			const char* start = p;

			while (p < end && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' ||
			                   *p == 'e' || *p == 'E'))
			{
				++p;
			}

			return p > start;
		}

		// The count is a whole number: a fraction or an exponent means this is not the frame we know.
		bool readCount(int& out)
		{
			skipSpace();
			const char* start = p;

			if (!skipNumber())
			{
				return false;
			}

			const char* q = start;
			bool negative = false;

			if (*q == '-' || *q == '+')
			{
				negative = (*q == '-');
				++q;
			}

			if (q == p)
			{
				return false;
			}

			long long value = 0;

			for (; q < p; ++q)
			{
				if (*q < '0' || *q > '9')
				{
					return false;
				}

				value = value * 10 + (*q - '0');

				if (value > 1000000000LL)
				{
					return false;	// a watcher count past a billion is a broken frame
				}
			}

			out = (int)(negative ? -value : value);

			return true;
		}

		bool skipLiteral(const char* literal)
		{
			skipSpace();
			const size_t length = std::strlen(literal);

			if ((size_t)(end - p) < length || std::strncmp(p, literal, length) != 0)
			{
				return false;
			}

			p += length;

			return true;
		}

		// An unknown field is stepped over instead of refused: the relay may add one and the notice
		// would still be readable.
		bool skipValue()
		{
			skipSpace();

			if (p >= end)
			{
				return false;
			}

			if (*p == '"')
			{
				std::string ignored;

				return readString(ignored);
			}

			if (*p == '{' || *p == '[')
			{
				const char open = *p;
				const char close = (open == '{') ? '}' : ']';
				int depth = 0;

				while (p < end)
				{
					if (*p == '"')
					{
						std::string ignored;

						if (!readString(ignored))
						{
							return false;
						}

						continue;
					}

					const char c = *p++;

					if (c == open)
					{
						++depth;
					}
					else if (c == close && --depth == 0)
					{
						return true;
					}
				}

				return false;
			}

			if (*p == 't')
			{
				return skipLiteral("true");
			}

			if (*p == 'f')
			{
				return skipLiteral("false");
			}

			if (*p == 'n')
			{
				return skipLiteral("null");
			}

			return skipNumber();
		}
	};

	// Reads a whole frame into locals: nothing the caller passes in is written until every field is in.
	bool readFrame(const char* json, int& count, std::string& who, bool& isJoin)
	{
		Scanner scanner = { json, json + std::strlen(json) };

		if (!scanner.consume('{'))
		{
			return false;
		}

		bool haveCount = false;
		bool haveName = false;
		int parsedCount = 0;
		std::string parsedWho;
		bool parsedIsJoin = false;

		for (;;)
		{
			scanner.skipSpace();

			if (scanner.atEnd())
			{
				return false;
			}

			if (scanner.peek() == '}')
			{
				++scanner.p;
				break;
			}

			std::string key;

			if (!scanner.readString(key) || !scanner.consume(':'))
			{
				return false;
			}

			if (key == "count")
			{
				if (!scanner.readCount(parsedCount))
				{
					return false;
				}

				haveCount = true;
			}
			else if (key == "joined" || key == "left")
			{
				scanner.skipSpace();

				if (scanner.peek() == 'n')
				{
					if (!scanner.skipLiteral("null"))
					{
						return false;
					}
				}
				else if (!scanner.readString(parsedWho))
				{
					return false;
				}
				else
				{
					parsedIsJoin = (key == "joined");
					haveName = true;
				}
			}
			else if (!scanner.skipValue())
			{
				return false;
			}

			scanner.skipSpace();

			if (!scanner.atEnd() && scanner.peek() == ',')
			{
				++scanner.p;
				continue;
			}

			if (!scanner.atEnd() && scanner.peek() == '}')
			{
				++scanner.p;
				break;
			}

			return false;
		}

		scanner.skipSpace();

		if (!scanner.atEnd() || !haveCount)
		{
			return false;	// trailing bytes mean this is not the frame we think it is
		}

		count = parsedCount;
		who = haveName ? parsedWho : std::string();
		isJoin = haveName && parsedIsJoin;

		return true;
	}

}	// namespace

bool GencastParseObserverNotice(const char* json, int& count, std::string& who, bool& isJoin)
{
	int parsedCount = 0;
	std::string parsedWho;
	bool parsedIsJoin = false;

	if (json == nullptr || !readFrame(json, parsedCount, parsedWho, parsedIsJoin))
	{
		return false;
	}

	count = parsedCount;
	who = parsedWho;
	isJoin = parsedIsJoin;

	return true;
}
