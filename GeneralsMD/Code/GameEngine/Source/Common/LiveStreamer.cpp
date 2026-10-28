/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "PreRTS.h"

#include "Common/GencastTransport.h"
#include "Common/GencastClientLog.h"
#include "Common/GlobalData.h"
#include "Common/PlayerList.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
#include "Common/LiveStreamer.h"
#include "Common/GencastTls.h"
#include "Common/LiveObserver.h"	// LIVE_OBSERVER_LOGGING gate + LIVE_OBSERVER_BUILD_TAG
#include "Common/GlobalData.h"
#include "Common/GameCommon.h"		// LIVE_DELAY_SECONDS_DEFAULT / _MAX
#include "Common/FramePacer.h"		// the logic rate reported by MSG_STATS; see publishStats
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
// the announce reports the lobby's map, not the last loaded one
#include "GameNetwork/GeneralsOnline/OnlineServices_LobbyInterface.h"
#include "GameNetwork/NetworkInterface.h"	// TheNetwork->getRunAhead(), this client's latency
#include "GameClient/ClientInstance.h"
#include "GameClient/InGameUI.h"
#include "GameNetwork/GameInfo.h"	// PLAYERTEMPLATE_OBSERVER, for the REGISTER is_observer flag

#include "GameNetwork/GeneralsOnline/json.hpp"	// parses GO's register reply

#include <curl/curl.h>
#include <curl/multi.h>
#include <curl/websockets.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <windows.h>

// ============================================================================
// liveStreamLog - write diagnostic messages to live_streamer_debug.log
// ============================================================================
// LIVE_OBSERVER_BUILD_TAG and the LIVE_OBSERVER_LOGGING gate both come from LiveObserver.h,
// included above; without that include streamer logging silently stays off in a DEFAULT build.

void liveStreamLog(const char* fmt, ...) {
#if !defined(LIVE_OBSERVER_LOGGING)
	(void)fmt;
#else
	static FILE* logFile = NULL;
	if (!logFile) {
		// Per-instance name: several clients run side by side during testing and would
		// otherwise truncate each other's log.
		AsciiString path;
		path.format("live_streamer_debug_Instance%.2u.log", rts::ClientInstance::getInstanceId());
		logFile = fopen(path.str(), "w");
		if (logFile)
			fprintf(logFile, "LIVE_OBSERVER_BUILD_TAG=%s\n", LIVE_OBSERVER_BUILD_TAG);
	}
	if (logFile) {
		// Wall-clock prefix, so several instances' logs (and the relay's) can be aligned.
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(logFile, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
		va_list args;
		va_start(args, fmt);
		vfprintf(logFile, fmt, args);
		va_end(args);
		fflush(logFile);
	}

	// Mirror to the client log the relay collects, so a remote tester's lines arrive without them
	// having to find and send a file. Never throws, never blocks on the relay.
	char mirrored[768] = {};
	va_list mirrorArgs;
	va_start(mirrorArgs, fmt);
	vsnprintf(mirrored, sizeof(mirrored) - 1, fmt, mirrorArgs);
	va_end(mirrorArgs);
	GencastClientLogLine(mirrored);
#endif // LIVE_OBSERVER_LOGGING
}

// ============================================================================
// UTF-8 helpers (chat payloads travel as UTF-8, like the rest of the GO wire format)
//
// Mirrored in LiveObserver.cpp as chatWideToUtf8/chatUtf8ToWide/chatAppendU32LE - keep both
// copies in sync.
// ============================================================================

static std::string wideToUtf8(const UnicodeString& text)
{
	const wchar_t* src = text.str();
	const int len = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1)       // nothing but the terminator, or a failure
		return std::string();
	std::string out(static_cast<size_t>(len - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, src, -1, &out[0], len, nullptr, nullptr);
	return out;
}

static UnicodeString utf8ToWide(const std::string& utf8)
{
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
	if (len <= 0)
		return UnicodeString::TheEmptyString;
	std::wstring tmp(static_cast<size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &tmp[0], len);
	return UnicodeString(tmp.c_str());
}

static void appendU32LE(std::vector<char>& out, unsigned int value)
{
	out.push_back((char)(value & 0xFF));
	out.push_back((char)((value >> 8) & 0xFF));
	out.push_back((char)((value >> 16) & 0xFF));
	out.push_back((char)((value >> 24) & 0xFF));
}

// ============================================================================
// LiveStreamer
// ============================================================================
LiveStreamer* TheLiveStreamer = nullptr;

LiveStreamer::LiveStreamer()
	: m_lastSentLogicFps(-1)
	, m_lastSentPingMs(-1)
	, m_lastStatsSentMs(0)
	, m_statsLastFrame(0)
	, m_statsLastFrameMs(0)
	, m_statsLogMs(0)
	, m_statsLogFrame(0)
	, m_statsLogAtMs(0)
	, m_isStreaming(false)
	, m_isBackup(false)
	, m_connected(false)
	, m_queuedBytes(0)
	, m_queueOverflowed(false)
	, m_isHost(FALSE)
	, m_delaySeconds(-1)
	, m_shouldRun(false)
	, m_curlEasy(nullptr)
	, m_curlMulti(nullptr)
	, m_bodySentOffset(0)
	, m_sentBytes(0)
	, m_sentFrames(0)
	, m_endReason("shutdown")
{
	m_headerBuffer.reserve(4096);
	m_bodyBuffer.reserve(64 * 1024);
}

LiveStreamer::~LiveStreamer()
{
	close();
}

LiveStreamer* createLiveStreamer()
{
	return new LiveStreamer();
}

// ============================================================================
// Pending registration - the pre-game lobby - Recorder handover
// ============================================================================

// Only ever touched from the main thread: the lobby fills it in from a UI callback, the Recorder
// consumes it from MSG_NEW_GAME. The network thread never sees it - by the time any of this
// reaches the wire it has been copied into the REGISTER payload.
static LiveStreamRegistration s_pendingRegistration;

void liveStreamSetPendingRegistration(const LiveStreamRegistration& registration)
{
	s_pendingRegistration = registration;
	liveStreamLog("liveStreamSetPendingRegistration lobbyId=%s player='%s' canStream=%d lobbyJsonLen=%u\n",
		registration.lobbyId.str(), registration.playerName.str(),
		(int)registration.canStream, (unsigned int)registration.lobbyJson.length());
}

void liveStreamClearPendingRegistration()
{
	if (s_pendingRegistration.isValid())
		liveStreamLog("liveStreamClearPendingRegistration dropping lobbyId=%s\n",
			s_pendingRegistration.lobbyId.str());

	s_pendingRegistration = LiveStreamRegistration();
}

// Ceiling on replay bytes held while waiting for a relay connection. Roughly a few minutes of
// a busy match: enough that a slow registration costs nothing, small enough that a refused one
// cannot grow without bound for the rest of the game.
static const size_t LIVE_STREAM_MAX_QUEUED_BYTES = 8u * 1024u * 1024u;

LiveStreamer* liveStreamStartPendingSession()
{
	if (TheGlobalData == nullptr || !TheGlobalData->m_liveStreamEnabled)
		return nullptr;

	if (!s_pendingRegistration.isValid())
	{
		// Normal for skirmish, replays and LAN - there is no lobby to have registered one.
		liveStreamLog("liveStreamStartPendingSession: nothing pending, not streaming this game\n");
		return nullptr;
	}

	if (TheLiveStreamer == nullptr)
		TheLiveStreamer = createLiveStreamer();

	if (TheLiveStreamer == nullptr)
		return nullptr;

	// Register first, then start the thread: registerForGame only fills in fields and queues the
	// REGISTER frame, and the network thread needs those fields to ask GO for a token. The relay
	// address is not chosen here - GO returns the connect URL (see requestStreamUrl).
	TheLiveStreamer->registerForGame(s_pendingRegistration);
	TheLiveStreamer->init();

	// Consumed. A second recording without a fresh lobby visit must not re-register this one
	// under the same lobby id - that would merge two unrelated matches into one relay session.
	liveStreamClearPendingRegistration();

	return TheLiveStreamer;
}

// ============================================================================
// IReplayStreamSink implementation
// ============================================================================

void LiveStreamer::onHeaderBytes(const void* data, Int size)
{
	if (size <= 0)
		return;

	const char* p = static_cast<const char*>(data);
	m_headerBuffer.insert(m_headerBuffer.end(), p, p + size);
}

void LiveStreamer::onHeaderComplete()
{
	if (m_headerBuffer.empty())
		return;

	// Demoted (backup) streamers do not send the header - the relay already has the session's
	// canonical one. Defensive: the header normally goes out at match start, before any demotion.
	if (m_isBackup.load())
	{
		m_headerBuffer.clear();
		return;
	}

	queueFrame(LIVE_MSG_HEADER, m_headerBuffer.data(), m_headerBuffer.size());
	m_headerBuffer.clear();
}

void LiveStreamer::onHeaderPatch(Int offset, const void* data, Int size)
{
	if (size <= 0)
		return;

	// Demoted: header mutations are not sent (see onHeaderComplete).
	if (m_isBackup.load())
		return;

	// Encode: 4 bytes offset (LE) + 4 bytes length (LE) + data
	unsigned char patchBuf[8];
	patchBuf[0] = (unsigned char)(offset & 0xFF);
	patchBuf[1] = (unsigned char)((offset >> 8) & 0xFF);
	patchBuf[2] = (unsigned char)((offset >> 16) & 0xFF);
	patchBuf[3] = (unsigned char)((offset >> 24) & 0xFF);
	patchBuf[4] = (unsigned char)(size & 0xFF);
	patchBuf[5] = (unsigned char)((size >> 8) & 0xFF);
	patchBuf[6] = (unsigned char)((size >> 16) & 0xFF);
	patchBuf[7] = (unsigned char)((size >> 24) & 0xFF);

	std::vector<char> payload;
	payload.reserve(8 + size);
	payload.insert(payload.end(), patchBuf, patchBuf + 8);
	payload.insert(payload.end(), static_cast<const char*>(data), static_cast<const char*>(data) + size);

	queueFrame(LIVE_MSG_PATCH, payload.data(), payload.size());
}

void LiveStreamer::onBodyBytes(const void* data, Int size)
{
	if (size <= 0)
		return;

	const char* p = static_cast<const char*>(data);

	// One buffer, both roles: while streaming it is flushed to the wire by onBodyFlush; while
	// backup onBodyFlush does nothing, so the same buffer accumulates the body from the demotion
	// point onward - the backfill source a later takeover needs. Its first byte sits at absolute
	// offset m_bodySentOffset (frozen while backup), so takeover offsets stay correct. Guarded by
	// m_sendMutex because onTakeover (network thread) reads it while this (game thread) appends.
	std::lock_guard<std::mutex> lock(m_sendMutex);
	if (m_bodyBuffer.size() + size > BODY_BUFFER_MAX)
	{
		// Drop the oldest bytes and advance the buffer's start offset, so the m_bodySentOffset
		// invariant holds. Only a long backup session grows this large.
		size_t drop = m_bodyBuffer.size() + size - BODY_BUFFER_MAX;
		m_bodyBuffer.erase(m_bodyBuffer.begin(), m_bodyBuffer.begin() + drop);
		m_bodySentOffset += drop;
	}
	m_bodyBuffer.insert(m_bodyBuffer.end(), p, p + size);
}

void LiveStreamer::onBodyFlush()
{
	// Backup: keep the bytes but send nothing; the buffer becomes the backfill source on
	// takeover. END still goes out via onRecordingEnded, so the relay knows we are done.
	if (m_isBackup.load())
		return;

	std::vector<char> framed;
	{
		std::lock_guard<std::mutex> lock(m_sendMutex);
		if (m_bodyBuffer.empty())
			return;

		// Build framed BODY: [8B offset LE][data]
		uint64_t off = m_bodySentOffset;
		unsigned char offBuf[8];
		offBuf[0] = (unsigned char)(off & 0xFF);
		offBuf[1] = (unsigned char)((off >> 8) & 0xFF);
		offBuf[2] = (unsigned char)((off >> 16) & 0xFF);
		offBuf[3] = (unsigned char)((off >> 24) & 0xFF);
		offBuf[4] = (unsigned char)((off >> 32) & 0xFF);
		offBuf[5] = (unsigned char)((off >> 40) & 0xFF);
		offBuf[6] = (unsigned char)((off >> 48) & 0xFF);
		offBuf[7] = (unsigned char)((off >> 56) & 0xFF);

		framed.reserve(8 + m_bodyBuffer.size());
		framed.insert(framed.end(), offBuf, offBuf + 8);
		framed.insert(framed.end(), m_bodyBuffer.begin(), m_bodyBuffer.end());

		m_bodySentOffset += m_bodyBuffer.size();
		m_bodyBuffer.clear();
	}

	queueFrame(LIVE_MSG_BODY, framed.data(), framed.size());
}

void LiveStreamer::onRecordingEnded()
{
	onBodyFlush();
	queueFrame(LIVE_MSG_END, nullptr, 0);
}

// ============================================================================
// Network setup
// ============================================================================

void LiveStreamer::init()
{
	m_shouldRun.store(true);

	liveStreamLog("LiveStreamer::init lobby=%s, asking GO for a stream URL\n", m_lobbyId.str());

	m_networkThread = std::thread(&LiveStreamer::networkThreadFunc, this);
}

void LiveStreamer::close()
{
	// The streamer stopping is the end of the stream, whichever way it stopped: the recorder's own
	// finish, the player leaving the match, or a restart. The recorder posts its own end on a normal
	// finish and clears the announce cache, so this only fires when nothing else did - which is
	// exactly the leave-match case, where the relay used to wait out its whole reconnect grace.
	//
	// A crash or a lost connection never reaches here; that is what the relay's grace is for.
	if (m_isStreaming.load() || m_isBackup.load())
	{
		GencastEndCurrentMatch("streamer-left");
	}

	m_shouldRun.store(false);

	if (m_networkThread.joinable())
		m_networkThread.join();

	m_connected.store(false);
	m_isStreaming.store(false);
	m_isBackup.store(false);

	// Release the body buffer (the streaming/backup accumulation).
	std::lock_guard<std::mutex> lock(m_sendMutex);
	m_bodyBuffer.clear();
	m_bodyBuffer.shrink_to_fit();
	m_bodySentOffset = 0;
}

// ============================================================================
// Registration
// ============================================================================

std::string liveStreamJsonEscape(const char* str)
{
	std::string out;
	if (str == nullptr)
		return out;

	for (const unsigned char* pc = (const unsigned char*)str; *pc; ++pc)
	{
		switch (*pc)
		{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b";  break;
			case '\f': out += "\\f";  break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (*pc < 0x20)
				{
					char esc[8];
					snprintf(esc, sizeof(esc), "\\u%04x", (unsigned int)*pc);
					out += esc;
				}
				else
				{
					// Includes every byte >= 0x80: a UTF-8 sequence is already legal JSON.
					out += (char)*pc;
				}
				break;
		}
	}

	return out;
}

void LiveStreamer::registerForGame(const LiveStreamRegistration& registration)
{
	m_lobbyId = registration.lobbyId;
	m_playerName = registration.playerName;
	m_isHost = registration.isHost;
	m_delaySeconds = registration.delaySeconds;

	liveStreamLog("LiveStreamer::registerForGame lobbyId=%s player='%s' isHost=%d canStream=%d lobbyJsonLen=%u\n",
		registration.lobbyId.str(), registration.playerName.str(), (int)registration.isHost,
		(int)registration.canStream, (unsigned int)registration.lobbyJson.length());

	// Built into a std::string rather than a fixed buffer: the GO-shaped lobby block carries a
	// lobby name, two map paths and up to eight members, and a truncated payload is unparseable
	// rather than merely lossy.
	char scratch[64];
	std::string regJson = "{\"type\":\"register\"";

	regJson += ",\"lobbyid\":\"" + liveStreamJsonEscape(registration.lobbyId.str()) + "\"";
	regJson += ",\"player_name\":\"" + liveStreamJsonEscape(registration.playerName.str()) + "\"";

	// can_stream is no longer this client's claim: the relay derives the right from its own player
	// record and refuses a stream the player was not granted. What goes out here is the relay's own
	// last answer - the field is kept because the frame carries it, not because it grants anything,
	// so a client can no longer give itself permission by asserting it. Fail closed: an unanswered
	// question is a false, never the local preference.
	const Bool relayGrantsStreaming = GencastMayStreamCurrentLobby();

	if (relayGrantsStreaming != registration.canStream)
	{
		liveStreamLog("LiveStreamer::registerForGame lobbyId=%s local canStream=%d but the relay "
			"says %d - sending the relay's answer\n",
			registration.lobbyId.str(), (int)registration.canStream, (int)relayGrantsStreaming);
	}

	regJson += relayGrantsStreaming ? ",\"can_stream\":true" : ",\"can_stream\":false";

	// is_host is sent for the relay's logs only. It no longer grants anything: the relay
	// compares our stream token's user against the owner GO recorded for the session, so a
	// client cannot claim host authority by asserting it here.
	regJson += registration.isHost ? ",\"is_host\":true" : ",\"is_host\":false";
	// In-game observer (side = PLAYERTEMPLATE_OBSERVER)? The relay delivers spectator chat to
	// observer-mode sources only. Read from the GameInfo slot list, because at MSG_NEW_GAME time
	// the player list has not been rebuilt yet while the slots are already populated.
	Bool isObserver = FALSE;
	if (TheGameInfo)
	{
		const Int localSlot = TheGameInfo->getLocalSlotNum();
		if (localSlot >= 0)
		{
			const GameSlot* slot = TheGameInfo->getConstSlot(localSlot);
			if (slot && slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
				isObserver = TRUE;
		}
	}
	regJson += isObserver ? ",\"is_observer\":true" : ",\"is_observer\":false";

	// Both host-only. The relay ignores them from anyone else, but not sending them at all from
	// a non-host keeps the payload honest about who is claiming to describe the game.
	if (registration.isHost)
	{
		if (registration.delaySeconds >= 0)
		{
			snprintf(scratch, sizeof(scratch), ",\"delay_seconds\":%d", registration.delaySeconds);
			regJson += scratch;
		}

		if (!registration.lobbyJson.empty())
		{
			regJson += ",\"lobby\":";
			regJson += registration.lobbyJson;
		}
	}

	regJson += "}";

	// Queued, not sent: m_connected may still be false, so the network thread sends this once
	// the relay accepts the connection.
	queueFrame(LIVE_MSG_REGISTER, regJson.c_str(), regJson.length());
}

void LiveStreamer::onRoleAssigned(const AsciiString& role, const AsciiString& lobbyId, uint64_t bodyOffset)
{
	m_lobbyId = lobbyId;

	// Only the initial streamer ROLE (fresh connect or mid-match reconnect) may establish the
	// send offset from the relay's value. While backup, m_bodySentOffset is frozen at the
	// absolute offset of the accumulating body buffer and the relay's body length is ahead of it,
	// so clobbering it would mislabel the buffered bytes; a takeover ROLE must not either,
	// because onTakeover computes its backfill slice against that same frozen offset.
	Bool wasBackup = m_isBackup.load();
	m_isStreaming.store(role == "streamer");
	m_isBackup.store(role == "backup");
	if (!m_isBackup.load() && !wasBackup)
		m_bodySentOffset = bodyOffset;

	liveStreamLog("LiveStreamer::onRoleAssigned role=%s lobbyId=%s streaming=%d bodyOff=%llu\n",
		role.str(), lobbyId.str(), (int)m_isStreaming.load(), (unsigned long long)bodyOffset);
}

void LiveStreamer::onTakeover(uint64_t bodyOffset)
{
	liveStreamLog("LiveStreamer::onTakeover promoted to streamer, bodyOff=%llu\n",
		(unsigned long long)bodyOffset);

	// Resume live buffering FIRST: any body bytes that arrive while the backfill below is
	// being sent must land in m_bodyBuffer (framed at m_bodySentOffset after the snapshot),
	// not be silently dropped by the backup gate.
	m_isStreaming.store(true);
	m_isBackup.store(false);

	// Backfill the relay's gap from the same body buffer that accumulated while backup:
	// m_bodyBuffer holds every body byte from the demotion point onward, with m_bodySentOffset
	// frozen at the absolute offset of buffer[0]. The relay is missing [bodyOffset..buffer_end];
	// live data continues from buffer_end. Snapshot under the lock, against onBodyBytes.
	uint64_t backfillStart = bodyOffset;
	std::vector<char> backfill;
	{
		std::lock_guard<std::mutex> lock(m_sendMutex);
		if (bodyOffset < m_bodySentOffset)
		{
			// The cap trimmed past the requested offset, so the hole cannot be filled. Degrade
			// to skip-forward; the relay logs a gap, as it does for any missing chunk.
			liveStreamLog("LiveStreamer::onTakeover cannot backfill from %llu "
				"(buffer starts at %llu) - skipping forward\n",
				(unsigned long long)bodyOffset, (unsigned long long)m_bodySentOffset);
			backfillStart = m_bodySentOffset;
		}
		size_t rel = (size_t)(backfillStart - m_bodySentOffset);
		if (rel < m_bodyBuffer.size())
		{
			backfill.assign(m_bodyBuffer.begin() + rel, m_bodyBuffer.end());
		}
		// Live data continues from the end of what was just taken; onBodyFlush frames the next
		// flush at this offset.
		m_bodySentOffset = m_bodySentOffset + m_bodyBuffer.size();
		m_bodyBuffer.clear();
	}

	// Send the backfill in bounded chunks. This runs on the network thread, the same one that
	// drains the send queue, so a direct send here cannot interleave with it.
	if (!backfill.empty())
	{
		const size_t BACKFILL_CHUNK = 64 * 1024;
		uint64_t absOff = backfillStart;
		for (size_t i = 0; i < backfill.size(); i += BACKFILL_CHUNK)
		{
			size_t n = backfill.size() - i;
			if (n > BACKFILL_CHUNK)
				n = BACKFILL_CHUNK;

			std::vector<char> framed;
			framed.reserve(8 + n);
			unsigned char offBuf[8];
			offBuf[0] = (unsigned char)(absOff & 0xFF);
			offBuf[1] = (unsigned char)((absOff >> 8) & 0xFF);
			offBuf[2] = (unsigned char)((absOff >> 16) & 0xFF);
			offBuf[3] = (unsigned char)((absOff >> 24) & 0xFF);
			offBuf[4] = (unsigned char)((absOff >> 32) & 0xFF);
			offBuf[5] = (unsigned char)((absOff >> 40) & 0xFF);
			offBuf[6] = (unsigned char)((absOff >> 48) & 0xFF);
			offBuf[7] = (unsigned char)((absOff >> 56) & 0xFF);
			framed.insert(framed.end(), offBuf, offBuf + 8);
			framed.insert(framed.end(), backfill.data() + i, backfill.data() + i + n);

			WsSendResult sendRes = sendBinaryFrame(LIVE_MSG_BODY, framed.data(), framed.size());
			if (sendRes != WsSendResult::Sent)
			{
				liveStreamLog("LiveStreamer::onTakeover backfill send failed at offset %llu (result %d)\n",
					(unsigned long long)absOff, (int)sendRes);
				break;
			}
			absOff += n;
		}
		liveStreamLog("LiveStreamer::onTakeover backfilled %zu bytes from offset %llu\n",
			backfill.size(), (unsigned long long)backfillStart);
	}
}

// ============================================================================
// Binary frame helpers
// ============================================================================

void LiveStreamer::onChat(UnsignedInt frame, const UnicodeString& text, UnsignedInt colorArgb)
{
	// Payload: [frame u32 LE][textLen u32 LE][UTF-8 text][color u32 LE] - opaque to the
	// relay; the observer frame-gates on `frame` and recolors from `colorArgb`.
	std::string utf8 = wideToUtf8(text);
	std::vector<char> payload;
	payload.reserve(12 + utf8.size());
	appendU32LE(payload, frame);
	appendU32LE(payload, (unsigned int)utf8.size());
	payload.insert(payload.end(), utf8.begin(), utf8.end());
	appendU32LE(payload, colorArgb);

	// CHATDIAG: onChat reached on the streaming side, logged BEFORE queueFrame so a frame the
	// queue then refuses is still visible here. backup=1 means this client is not the source the
	// relay treats as canonical for the match.
	liveStreamLog("CHATDIAG onChat frame=%u textLen=%u bytes=%u color=0x%08X backup=%d connected=%d\n",
		frame, (unsigned int)utf8.size(), (unsigned int)payload.size(), colorArgb,
		(int)m_isBackup.load(), (int)m_connected.load());

	queueFrame(LIVE_MSG_CHAT, payload.data(), payload.size());
}

void LiveStreamer::onTick(UnsignedInt frame)
{
	// Backup: say nothing. A demoted source has stopped pushing body data (see onBodyFlush),
	// so a tick from it would assert an edge for bytes it is not sending - the observer would
	// be told the game is at frame N while nothing behind N is arriving.
	if (m_isBackup.load())
		return;

	// Payload: [frame u32 LE]. Opaque to the relay, which only forwards it and remembers the
	// latest value for observers joining later.
	std::vector<char> payload;
	payload.reserve(4);
	appendU32LE(payload, frame);
	queueFrame(LIVE_MSG_TICK, payload.data(), payload.size());

	// Sampled here rather than off its own timer: onTick already runs on a fixed frame cadence,
	// and the telemetry then describes the same moment as the tick it travels with.
	publishStats(frame);
}

void LiveStreamer::publishStats(UnsignedInt frame)
{
	// Same reasoning as onTick: a demoted source is not the one describing this match.
	if (m_isBackup.load())
		return;

	const UnsignedInt nowMs = timeGetTime();

	// Frames actually advanced per wall-clock second - the achieved rate, not the negotiated one.
	// TheFramePacer->getActualLogicTimeScaleFps() resolves to TheNetwork->getFrameRate() in a
	// network match, which is what the mesh agreed to run at and stays at 60 on a host that is
	// really stepping 11. See m_statsLastFrame.
	Int logicFps = m_lastSentLogicFps;   // hold the last reading until a fresh one can be measured
	if (m_statsLastFrameMs != 0 && nowMs > m_statsLastFrameMs && frame > m_statsLastFrame)
	{
		const UnsignedInt elapsedMs = nowMs - m_statsLastFrameMs;
		logicFps = (Int)((frame - m_statsLastFrame) * 1000 / elapsedMs);
	}
	m_statsLastFrame = frame;
	m_statsLastFrameMs = nowMs;

	// Host-side truth, once a second: what this client renders, what its logic counter advances,
	// and what it publishes. The HUD's plain counter is the render rate; only the frame counter is
	// the match, and the two are not the same number.
	if (nowMs - m_statsLogMs >= 1000 && TheGameLogic != nullptr)
	{
		const UnsignedInt curFrame = TheGameLogic->getFrame();
		Int achieved = 0;
		if (m_statsLogAtMs != 0 && nowMs > m_statsLogAtMs && curFrame > m_statsLogFrame)
			achieved = (Int)((curFrame - m_statsLogFrame) * 1000 / (nowMs - m_statsLogAtMs));
		const Int renderFps = (TheFramePacer != nullptr) ? (Int)(TheFramePacer->getUpdateFps() + 0.5f) : 0;
		const Int negotiated = (TheNetwork != nullptr) ? TheNetwork->getFrameRate() : 0;
		liveStreamLog("STATS: frame=%u logic=%d publish=%d render=%d negotiated=%d paused=%d\n",
			curFrame, achieved, logicFps, renderFps, negotiated,
			TheGameLogic->isGamePaused() ? 1 : 0);
		m_statsLogMs = nowMs;
		m_statsLogFrame = curFrame;
		m_statsLogAtMs = nowMs;
	}

	if (logicFps < 0)
		return;   // nothing measured yet, and nothing worth sending
	if (logicFps > 255)
		logicFps = 255;

	// This client's own latency, derived exactly as the in-game counter derives the number it
	// shows the player: the run-ahead window in milliseconds (InGameUI::drawNetworkLatency).
	//
	// Deliberately not NetworkMesh::getMaximumLatency(), which was tried first and reports 0
	// during a match (2026-08-15) - its latency table is not maintained in-game. Matching the
	// counter's own derivation also means an observer and a player quote the same number.
	Int pingMs = 0;
	if (TheNetwork != nullptr)
		pingMs = (Int)(TheNetwork->getRunAhead() * (1000 / GENERALS_ONLINE_HIGH_FPS_LIMIT));
	if (pingMs < 0)
		pingMs = 0;
	if (pingMs > LIVE_STATS_PING_MAX_MS)
		pingMs = LIVE_STATS_PING_MAX_MS;

	// Quantise before comparing, or "changed" is true on essentially every sample and sending on
	// change degenerates into sending every tick.
	pingMs = ((pingMs + LIVE_STATS_PING_QUANTUM_MS / 2) / LIVE_STATS_PING_QUANTUM_MS)
		* LIVE_STATS_PING_QUANTUM_MS;

	const Bool neverSent = (m_lastStatsSentMs == 0);
	const Bool changed = (logicFps != m_lastSentLogicFps) || (pingMs != m_lastSentPingMs);
	const Bool heartbeatDue = neverSent || (nowMs - m_lastStatsSentMs) >= (UnsignedInt)LIVE_STATS_HEARTBEAT_MS;

	if (!changed && !heartbeatDue)
		return;

	// A change that arrives too soon after the last send is not dropped, only deferred - the next
	// tick re-evaluates against the same still-current value and sends it then.
	if (changed && !heartbeatDue && !neverSent
		&& (nowMs - m_lastStatsSentMs) < (UnsignedInt)LIVE_STATS_MIN_INTERVAL_MS)
	{
		return;
	}

	m_lastSentLogicFps = logicFps;
	m_lastSentPingMs = pingMs;
	m_lastStatsSentMs = nowMs;

	// Payload: [logicFps u32 LE][pingMs u32 LE]. Opaque to the relay, which forwards it and - for
	// observers held behind the broadcast delay - releases it on the same delayed boundary as
	// body bytes, because a live stats frame would otherwise state something about the match now.
	std::vector<char> statsPayload;
	statsPayload.reserve(8);
	appendU32LE(statsPayload, (UnsignedInt)logicFps);
	appendU32LE(statsPayload, (UnsignedInt)pingMs);
	queueFrame(LIVE_MSG_STATS, statsPayload.data(), statsPayload.size());
}

void LiveStreamer::pumpSpectatorChat()
{
	if (!TheInGameUI)
		return;

	std::deque<SpectatorChatEntry> batch;
	{
		std::lock_guard<std::mutex> lock(m_spectatorChatMutex);
		if (m_spectatorChatQueue.empty())
			return;
		batch.swap(m_spectatorChatQueue);
	}

	// Distinct fixed style so spectator chat is never confused with player chat.
	static const RGBColor spectatorColor = { 0.45f, 0.68f, 0.95f };
	for (auto& entry : batch)
	{
		UnicodeString line;
		line.format(L"[%ls] %ls", entry.displayName.str(), entry.text.str());
		TheInGameUI->messageColor(true, &spectatorColor, UnicodeString(L"%ls"), line.str());
	}
}

void LiveStreamer::queueFrame(LiveMsgType type, const void* data, size_t len)
{
	QueuedFrame frame;
	frame.type = (unsigned char)type;
	if (data && len > 0)
	{
		frame.data.assign(static_cast<const char*>(data), static_cast<const char*>(data) + len);
	}
	{
		std::lock_guard<std::mutex> lock(m_sendMutex);

		// Everything queued before the connection exists is held in memory, which is what lets
		// the replay sink attach at match start and stream the header the moment the relay
		// accepts us. Bounded, because a registration GO refuses means nothing ever drains this.
		// REGISTER itself is always kept: dropping it would waste a connection that succeeds.
		if (type != LIVE_MSG_REGISTER &&
			m_queuedBytes + frame.data.size() > LIVE_STREAM_MAX_QUEUED_BYTES)
		{
			if (!m_queueOverflowed)
			{
				m_queueOverflowed = true;
				liveStreamLog("LiveStreamer::queueFrame queue exceeded %u bytes with no relay "
					"connection - dropping stream data from here on\n",
					(unsigned int)LIVE_STREAM_MAX_QUEUED_BYTES);
			}
			return;
		}

		m_queuedBytes += frame.data.size();
		m_outgoingQueue.push_back(std::move(frame));
	}
}

LiveStreamer::WsSendResult LiveStreamer::sendBinaryFrame(LiveMsgType type, const void* payload, size_t payloadLen)
{
	if (!m_connected.load())
		return WsSendResult::Error;

	// Envelope: 1 byte type + 4 bytes length (LE) + payload
	// Must be sent as ONE WebSocket frame - curl_ws_send writes a frame per call.
	unsigned int len = (unsigned int)payloadLen;
	size_t totalSize = 5 + (payload ? len : 0);
	std::vector<unsigned char> buf(totalSize);
	buf[0] = (unsigned char)type;
	buf[1] = (unsigned char)(len & 0xFF);
	buf[2] = (unsigned char)((len >> 8) & 0xFF);
	buf[3] = (unsigned char)((len >> 16) & 0xFF);
	buf[4] = (unsigned char)((len >> 24) & 0xFF);
	if (payload && len > 0)
		memcpy(buf.data() + 5, payload, len);

	return wsSendBinary(buf.data(), totalSize);
}

LiveStreamer::WsSendResult LiveStreamer::sendBinaryFrame(const QueuedFrame& frame)
{
	return sendBinaryFrame((LiveMsgType)frame.type,
		frame.data.empty() ? nullptr : frame.data.data(),
		frame.data.size());
}

// ============================================================================
// WebSocket I/O (libcurl, background thread)
// ============================================================================

LiveStreamer::WsSendResult LiveStreamer::wsSendBinary(const unsigned char* data, size_t len)
{
	if (!m_curlEasy)
		return WsSendResult::Error;

	size_t sent = 0;
	CURLcode rc = curl_ws_send(m_curlEasy, data, len, &sent, 0, CURLWS_BINARY);
	if (rc == CURLE_AGAIN)
	{
		// Backpressure, not failure: the socket buffer is full and nothing was sent, so the
		// caller keeps the frame and retries on the next loop pass.
		return WsSendResult::WouldBlock;
	}
	if (rc != CURLE_OK)
	{
		liveStreamLog("LiveStreamer::wsSendBinary failed: %d\n", (int)rc);
		return WsSendResult::Error;
	}
	if (sent != len)
	{
		// curl_ws_send sends whole frames - a short send would leave the relay holding a
		// truncated frame, which is worse than ending the session cleanly.
		liveStreamLog("LiveStreamer::wsSendBinary short send: %zu of %zu bytes\n", sent, len);
		return WsSendResult::Error;
	}
	return WsSendResult::Sent;
}

bool LiveStreamer::wsRecv(std::vector<char>& outBuffer)
{
	if (!m_curlEasy)
		return false;

	outBuffer.clear();
	outBuffer.resize(65536);

	const struct curl_ws_frame* meta = nullptr;
	size_t nread = 0;
	CURLcode rc = curl_ws_recv(m_curlEasy, outBuffer.data(), outBuffer.size(), &nread, &meta);
	if (rc == CURLE_AGAIN)
	{
		outBuffer.clear();
		return false;
	}
	if (rc != CURLE_OK)
	{
		// The connection itself is gone (relay crash/restart, socket closed without an ERROR
		// frame). Wind down instead of spinning on a dead socket until the watchdog catches up.
		liveStreamLog("STREAM DEAD: wsRecv error: %d - connection lost, winding down the session\n", (int)rc);
		m_endReason = "recv-failed";
		m_connected.store(false);
		outBuffer.clear();
		return false;
	}

	// Any frame - stream data or the relay's protocol-level keepalive pings - proves the relay
	// is alive; the watchdog in networkThreadFunc keys off this timestamp.
	m_lastFrameReceivedMs.store(timeGetTime());

	// Only binary payloads belong in the reassembly buffer: a PING/PONG/TEXT/CLOSE payload
	// appended into the byte stream misparses everything after it.
	if (meta != nullptr && (meta->flags & CURLWS_BINARY) == 0)
	{
		outBuffer.clear();
		return false;
	}

	outBuffer.resize(nread);
	return nread > 0;
}

bool LiveStreamer::requestStreamUrl(AsciiString& outUrl)
{
	// Direct mode: announce to the relay and take the stream key it hands back. The relay binds
	// this player's password to the lobby on the first announce, so a wrong identity here means
	// the wrong password is handed out later.
	//
	// The lobby was already announced as pregame when it was created or joined (see
	// GencastAnnounceCurrentLobby); this is the phase change to live, and the announce that has
	// to come back with a stream key.
	if (NGMP_OnlineServicesManager::Settings.Relay_Direct())
	{
		GencastTransport* transport = GencastTransport::Create();

		if (transport == nullptr)
		{
			liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s no direct transport\n",
				m_lobbyId.str());
			return false;
		}

		// The relay keys player identities from 1 and rejects 0 outright, while Generals' player
		// index is 0-based (the host is index 0). Sending the index unchanged means the most
		// common streamer - the host - is refused with 400 and never announces at all.
		long playerId = 0;
		std::string displayName;

		if (ThePlayerList != nullptr && ThePlayerList->getLocalPlayer() != nullptr)
		{
			Player* localPlayer = ThePlayerList->getLocalPlayer();
			playerId = (long)localPlayer->getPlayerIndex() + 1;
			// The relay stores this name with the player record and shows it in its logs.
			displayName = wideToUtf8(localPlayer->getPlayerDisplayName());
		}

		if (playerId <= 0)
		{
			// No local player means no identity to announce under, and anything invented here
			// would bind a stranger's password to this lobby.
			liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s no local player, cannot announce\n",
				m_lobbyId.str());
			delete transport;
			return false;
		}

		// The wire slot is only an alias: the relay prefers the Services user id, and 0 means
		// "not signed in" so the announce falls back to the slot id.
		long userId = 0;
		NGMP_OnlineServices_AuthInterface* auth =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();

		if (auth != nullptr)
		{
			userId = (long)auth->GetUserID();
		}

		// The map the lobby shows, not TheGlobalData->m_mapName: at match start that global
		// still holds the map that was last *loaded* - the shell map - which is how
		// "ShellMapMD" ended up on the relay while the players were on Defcon.
		std::string mapName;
		
		NGMP_OnlineServices_LobbyInterface* lobbyInterface =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
		
		if (lobbyInterface != nullptr && lobbyInterface->IsInLobby())
		{
			mapName = lobbyInterface->GetCurrentLobby().map_name;
		}
		
		if (mapName.empty() && TheGlobalData != nullptr)
		{
			mapName = TheGlobalData->m_mapName.str();
		}


		transport->SetAnnounceContext(playerId, userId, displayName, mapName);

		// Before announcing anything: the relay decides who may stream, and this is the last place
		// where "not granted" can still keep the request off the wire. Fail closed - an unasked,
		// unanswered or refused question all mean no announce.
		if (!GencastQueryStreamPermissionNow())
		{
			liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s refused: the relay has not "
				"enabled streaming for this account on this match (reason=%s)\n",
				m_lobbyId.str(), GencastStreamPermissionReason().c_str());

			NetworkLog(ELogVerbosity::LOG_RELEASE,
				"[GENCAST] stream not started lobby=%s: no stream permission (%s)",
				m_lobbyId.str(), GencastStreamPermissionReason().c_str());

			delete transport;
			return false;
		}

		std::string streamUrl;
		bool ok = false;

		transport->RegisterStream(m_lobbyId.str(), EGencastStreamState::Live,
			[&](const GencastStreamRegistration& registration)
			{
				ok = registration.success;

				if (registration.success)
				{
					streamUrl = registration.streamUrl;
				}
				else
				{
					liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s direct failed: %s\n",
						m_lobbyId.str(), registration.errorMessage.c_str());
				}
			});

		delete transport;

		if (!ok || streamUrl.empty())
		{
			// Ask again: a refusal is exactly the moment the reason for it changed - the admin
			// revoked the right, or the match's pool closed - and the cached answer is what the
			// lobby's switch reads next time.
			GencastRefreshStreamPermission();

			return false;
		}

		liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s direct stream ready: %s\n",
			m_lobbyId.str(), streamUrl.c_str());

		outUrl = streamUrl.c_str();
		return true;
	}

	// The host reports the broadcast delay here rather than in the REGISTER frame: GO forwards it
	// to the relay when the session is created, before any source connects, so every observer is
	// held behind the same number. A non-host sends no delay at all, so a second source cannot
	// redefine the host's spoiler window.
	std::string postBody = "{}";
	if (m_isHost && m_delaySeconds >= 0)
	{
		char scratch[64];
		snprintf(scratch, sizeof(scratch), "{\"delay_seconds\":%d}", m_delaySeconds);
		postBody = scratch;
	}

	AsciiString url;
	url.format("%s/register", liveServicesEndpoint("Livestreams").str());

	AsciiString body;
	Int statusCode = 0;
	if (!liveServicesRequest(url, TRUE, postBody.c_str(), body, statusCode))
	{
		liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s failed (request not sent)\n",
			m_lobbyId.str());
		return false;
	}

	if (statusCode != 200)
	{
		// 404 means GO does not think we are in an in-progress match, 503 that the deployment
		// has no relay configured. Neither is retryable from here: the match simply records
		// locally, as it would with streaming switched off.
		liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s refused (status=%d) %s\n",
			m_lobbyId.str(), statusCode, body.str());
		return false;
	}

	try
	{
		nlohmann::json response = nlohmann::json::parse(body.str());
		if (response.is_object() && response.contains("url") && response["url"].is_string())
		{
			const std::string streamUrl = response["url"].get<std::string>();
			if (!streamUrl.empty())
			{
				outUrl = streamUrl.c_str();
				liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s got a stream URL\n",
					m_lobbyId.str());
				return true;
			}
		}
	}
	catch (const nlohmann::json::exception&)
	{
	}

	liveStreamLog("LiveStreamer::requestStreamUrl lobby=%s failed (no url in reply)\n",
		m_lobbyId.str());
	return false;
}

bool LiveStreamer::connectToRelay()
{
	if (m_curlEasy)
	{
		curl_easy_cleanup((CURL*)m_curlEasy);
		m_curlEasy = nullptr;
	}
	if (m_curlMulti)
	{
		curl_multi_cleanup((CURLM*)m_curlMulti);
		m_curlMulti = nullptr;
	}

	CURL* easy = curl_easy_init();
	if (!easy)
	{
		liveStreamLog("LiveStreamer::connectToRelay curl_easy_init failed\n");
		return false;
	}

	// The relay does not accept an unauthenticated /register. GO registers the livestream, mints
	// a single-use stream token for this player, and hands back the complete connect URL, so the
	// relay's address is GO's to decide rather than ours to assemble.
	AsciiString url;
	if (!requestStreamUrl(url))
	{
		curl_easy_cleanup(easy);
		return false;
	}

	curl_easy_setopt(easy, CURLOPT_URL, url.str());
	curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L);

	GencastApplyTlsTrust(easy, "stream-wss");

	CURLM* multi = curl_multi_init();
	if (!multi)
	{
		curl_easy_cleanup(easy);
		liveStreamLog("LiveStreamer::connectToRelay curl_multi_init failed\n");
		return false;
	}

	curl_multi_add_handle(multi, easy);

	int stillRunning = 0;
	CURLMcode mc = curl_multi_perform(multi, &stillRunning);
	while (mc == CURLM_OK && stillRunning > 0)
	{
		mc = curl_multi_poll(multi, NULL, 0, 1000, NULL);
		if (mc == CURLM_OK)
			mc = curl_multi_perform(multi, &stillRunning);
	}

	if (mc != CURLM_OK)
	{
		liveStreamLog("LiveStreamer::connectToRelay curl_multi_perform failed: %d\n", (int)mc);
		curl_multi_remove_handle(multi, easy);
		curl_multi_cleanup(multi);
		curl_easy_cleanup(easy);
		return false;
	}

	// A 404 or any other HTTP error still leaves stillRunning == 0, so the WebSocket upgrade has
	// to be verified explicitly through the transfer result.
	int infoRunning = 0;
	CURLMsg* infoMsg = curl_multi_info_read(multi, &infoRunning);
	if (!infoMsg || infoMsg->data.result != CURLE_OK)
	{
		int result = infoMsg ? (int)infoMsg->data.result : -1;
		liveStreamLog("LiveStreamer::connectToRelay: handshake failed (result=%d)\n", result);
		curl_multi_remove_handle(multi, easy);
		curl_multi_cleanup(multi);
		curl_easy_cleanup(easy);
		return false;
	}

	m_curlEasy = easy;
	m_curlMulti = multi;
	m_connected.store(true);
	liveStreamLog("LiveStreamer::connectToRelay connected\n");
	return true;
}

// ============================================================================
// Background network thread
// ============================================================================

void LiveStreamer::networkThreadFunc()
{
	liveStreamLog("LiveStreamer::networkThreadFunc started\n");

	if (!connectToRelay())
	{
		liveStreamLog("LiveStreamer::networkThreadFunc connectToRelay failed\n");
		m_shouldRun.store(false);
		return;
	}

	m_connected.store(true);

	// m_connected is the wind-down signal: a send failure, a relay ERROR frame or the liveness
	// watchdog all clear it, which exits this loop and - because the final drain is gated on it
	// too - stops any further upload.
	while (m_shouldRun.load() && m_connected.load())
	{
		// Pop under the lock, send outside it. curl_ws_send is blocking network I/O and the game
		// thread takes the same mutex every frame in onBodyBytes/onBodyFlush, so holding it
		// across a send stalls the whole game frame.
		std::vector<QueuedFrame> toSend;
		{
			std::lock_guard<std::mutex> lock(m_sendMutex);
			while (!m_outgoingQueue.empty() && m_connected.load())
			{
				toSend.push_back(std::move(m_outgoingQueue.front()));
				m_outgoingQueue.pop_front();
				m_queuedBytes -= toSend.back().data.size();
			}
		}

		for (size_t i = 0; i < toSend.size(); ++i)
		{
			if (!m_connected.load())
				break;
			const QueuedFrame& frame = toSend[i];
			WsSendResult res = sendBinaryFrame(frame);
			if (res == WsSendResult::WouldBlock)
			{
				// Put every unsent frame back at the FRONT of the queue - stream order is data -
				// and retry on the next pass. Backpressure slows the upload, it must not kill it.
				std::lock_guard<std::mutex> lock(m_sendMutex);
				for (size_t j = toSend.size(); j-- > i; )
				{
					m_queuedBytes += toSend[j].data.size();
					m_outgoingQueue.push_front(std::move(toSend[j]));
				}
				break;
			}
			if (res == WsSendResult::Error)
			{
				liveStreamLog("STREAM DEAD: send of type=%d failed (%zu bytes) - connection to relay lost, winding down the session\n",
					(int)frame.type, frame.data.size());
				m_endReason = "send-failed";
				m_connected.store(false);
				break;
			}
			m_sentBytes += frame.data.size();
			m_sentFrames += 1;
			if (frame.type == LIVE_MSG_HEADER)
				liveStreamLog("LiveStreamer: sent HEADER (%zu bytes)\n", frame.data.size());
			else if (frame.type == LIVE_MSG_END)
				liveStreamLog("LiveStreamer: sent END\n");
		}

		if (!m_connected.load())
			break;

		// Receive incoming messages
		std::vector<char> recvBuf;
		while (wsRecv(recvBuf) && m_shouldRun.load() && m_connected.load())
		{
			if (recvBuf.size() < 5)
				continue;

			// char is signed on MSVC, so every length byte must zero-extend through unsigned char.
			unsigned char msgType = (unsigned char)recvBuf[0];
			unsigned int msgLen = (unsigned int)(unsigned char)recvBuf[1]
				| ((unsigned int)(unsigned char)recvBuf[2] << 8)
				| ((unsigned int)(unsigned char)recvBuf[3] << 16)
				| ((unsigned int)(unsigned char)recvBuf[4] << 24);

			if (msgType == LIVE_MSG_ROLE && msgLen > 0 && (uint64_t)recvBuf.size() >= 5ull + msgLen)
			{
				std::string json(recvBuf.data() + 5, msgLen);
				liveStreamLog("LiveStreamer: received role: %s\n", json.c_str());

				// Each key advances by the literal's own strlen, never a hand-counted constant.
				static const char ROLE_KEY[]     = "\"role\":\"";
				static const char ACTION_KEY[]   = "\"action\":\"";
				static const char LOBBY_ID_KEY[] = "\"lobbyid\":\"";
				static const char BODY_OFF_KEY[] = "\"body_offset\":";

				const char* roleStart = strstr(json.c_str(), ROLE_KEY);
				const char* actionStart = strstr(json.c_str(), ACTION_KEY);
				const char* lobbyIdStart = strstr(json.c_str(), LOBBY_ID_KEY);
				const char* bodyOffStart = strstr(json.c_str(), BODY_OFF_KEY);

				AsciiString role("none");
				AsciiString lobbyId;
				uint64_t bodyOffset = 0;

				if (roleStart)
				{
					roleStart += strlen(ROLE_KEY);
					const char* roleEnd = strchr(roleStart, '"');
					if (roleEnd)
						role.set(roleStart, roleEnd - roleStart);
				}
				if (lobbyIdStart)
				{
					lobbyIdStart += strlen(LOBBY_ID_KEY);
					const char* idEnd = strchr(lobbyIdStart, '"');
					if (idEnd)
						lobbyId.set(lobbyIdStart, idEnd - lobbyIdStart);
				}
				if (bodyOffStart)
				{
					bodyOffStart += strlen(BODY_OFF_KEY);
					bodyOffset = (uint64_t)strtoull(bodyOffStart, nullptr, 10);
				}
				// Order matters: onRoleAssigned applies the flags and offset, then onTakeover
				// overrides m_bodySentOffset with the backfill position it establishes.
				onRoleAssigned(role, lobbyId, bodyOffset);

				if (actionStart)
				{
					const char* actPtr = actionStart + strlen(ACTION_KEY);
					if (strncmp(actPtr, "takeover", 8) == 0)
						onTakeover(bodyOffset);
				}
			}
			else if (msgType == LIVE_MSG_ERROR)
			{
				// The relay says our session is gone (reaped/refused) while the socket is still
				// open. Wind down, so the UI stops claiming to stream into nothing.
				std::string errText;
				if (msgLen > 0 && (uint64_t)recvBuf.size() >= 5ull + msgLen)
					errText.assign(recvBuf.data() + 5, msgLen);
				liveStreamLog("STREAM DEAD: relay ERROR frame received (%s) - session is gone, winding down\n",
					errText.empty() ? "no detail" : errText.c_str());
				m_endReason = "relay-error";
				m_connected.store(false);
			}
			else if (msgType == LIVE_MSG_SPECTATOR_CHAT && msgLen >= 8
				&& (uint64_t)recvBuf.size() >= 5ull + msgLen)
			{
				// [nameLen u32 LE][UTF-8 name][textLen u32 LE][UTF-8 text] - live spectator
				// chat for in-game observers (we are a source).
				const unsigned char* p = (const unsigned char*)recvBuf.data() + 5;
				unsigned int nameLen = (unsigned int)p[0]
					| ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
				if ((uint64_t)msgLen >= 8ull + nameLen)
				{
					const unsigned char* t = p + 4 + nameLen;
					unsigned int textLen = (unsigned int)t[0]
						| ((unsigned int)t[1] << 8) | ((unsigned int)t[2] << 16) | ((unsigned int)t[3] << 24);
					if ((uint64_t)msgLen >= 8ull + nameLen + textLen)
					{
						SpectatorChatEntry entry;
						entry.displayName = utf8ToWide(std::string((const char*)(p + 4), nameLen));
						entry.text = utf8ToWide(std::string((const char*)(t + 4), textLen));
						{
							std::lock_guard<std::mutex> lock(m_spectatorChatMutex);
							if (m_spectatorChatQueue.size() < 1000)
								m_spectatorChatQueue.push_back(entry);
						}
					}
				}
			}
		}

		// Relay liveness watchdog, mirroring LiveObserver's: the relay pings this websocket every
		// ~20 s, so a long silence can only mean the relay - or the path to it - is gone. Gated
		// on having seen at least one frame, because the pre-ROLE join may legitimately be quiet.
		if (m_connected.load()
			&& m_lastFrameReceivedMs.load() != 0
			&& (timeGetTime() - m_lastFrameReceivedMs.load()) > (UnsignedInt)LIVE_STREAM_WATCHDOG_MS)
		{
			liveStreamLog("STREAM DEAD: no frame from relay for %ums - relay stopped consuming our stream, winding down the session\n",
				(unsigned)(timeGetTime() - m_lastFrameReceivedMs.load()));
			m_endReason = "relay-silent";
			m_connected.store(false);
			break;
		}

		// Also the loop's sleep. curl_multi_poll's out-param is numfds, not "still running", so
		// curl_multi_perform() must run unconditionally or incoming ROLE/ERROR frames stop
		// being received.
		{
			int numfds = 0;
			curl_multi_poll(m_curlMulti, NULL, 0, 10, &numfds);
			int runningHandles = 0;
			curl_multi_perform((CURLM*)m_curlMulti, &runningHandles);
		}
	}

	// Final drain: frames queued after the last loop iteration (PATCH + END from stopRecording)
	// must still be sent. Same pop-under-lock / send-outside-lock split as the main loop.
	std::vector<QueuedFrame> toSend;
	{
		std::lock_guard<std::mutex> lock(m_sendMutex);
		while (!m_outgoingQueue.empty() && m_connected.load())
		{
			toSend.push_back(std::move(m_outgoingQueue.front()));
			m_outgoingQueue.pop_front();
			m_queuedBytes -= toSend.back().data.size();
		}
	}
	for (size_t i = 0; i < toSend.size(); ++i)
	{
		if (!m_connected.load())
			break;
		const QueuedFrame& frame = toSend[i];
		WsSendResult res = sendBinaryFrame(frame);
		if (res == WsSendResult::WouldBlock)
		{
			// Same as the main loop: keep the unsent frames in order, in case the relay
			// catches up before the process goes away.
			std::lock_guard<std::mutex> lock(m_sendMutex);
			for (size_t j = toSend.size(); j-- > i; )
			{
				m_queuedBytes += toSend[j].data.size();
				m_outgoingQueue.push_front(std::move(toSend[j]));
			}
			break;
		}
		if (res == WsSendResult::Error)
		{
			liveStreamLog("LiveStreamer: final drain send failed, type=%d\n", (int)frame.type);
			break;
		}
		m_sentBytes += frame.data.size();
		m_sentFrames += 1;
	}

	// Cleanup
	if (m_curlMulti)
	{
		if (m_curlEasy)
			curl_multi_remove_handle((CURLM*)m_curlMulti, (CURL*)m_curlEasy);
		curl_multi_cleanup((CURLM*)m_curlMulti);
		m_curlMulti = nullptr;
	}
	if (m_curlEasy)
	{
		curl_easy_cleanup((CURL*)m_curlEasy);
		m_curlEasy = nullptr;
	}
	m_connected.store(false);
	m_isStreaming.store(false);
	liveStreamLog("LiveStreamer::networkThreadFunc ended - lobby=%s reason=%s sentFrames=%zu sentBytes=%zu queuedLeft=%zu\n",
		m_lobbyId.str(), m_endReason.str(), m_sentFrames, m_sentBytes, m_queuedBytes);
}
