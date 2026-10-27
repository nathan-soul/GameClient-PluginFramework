#pragma once

/*
**  GencastMirrorCountdown - the countdown as seen by a client that is not running it.
**
**  The countdown lives on exactly one machine: NGMPGame::StartCountdown() is called from the lobby
**  host's StartPressed(), and every other client only learns that the match is about to begin from
**  what the host broadcasts. When that host runs a plain GO client - no GenCast code - nothing about
**  the countdown reaches the relay at all, and a pregame watcher shows nothing while the players
**  count down.
**
**  A member's client does get two useful things:
**
**    1. the host's announcement line just before it starts the countdown
**       (SendAnnouncementMessageToCurrentLobby with LAN:GameStartTimerPlural), and
**    2. the ready flags collapsing back, because every lobby change resets readiness - which is
**       also what stops the host's countdown.
**
**  So: arm on (1), cancel on (2), and the relay receives an absolute end this client computed from
**  its own system_clock. No utc_clock skew to correct, because the number is made here.
**
**  Engine-free on purpose, so tools/countdown_mirror_test.sh compiles this exact header instead of a
**  copy of the rule.
*/

struct GencastMirrorCountdown
{
	static const int kDefaultDurationSeconds = 5;

	bool armed = false;              ///< a mirrored countdown is running
	long long endsAtUnixMs = 0;      ///< when it ends, on this client's wall clock
	unsigned readySignature = 0;     ///< the lobby's readiness when it was armed

	/// Starts a mirrored countdown of durationSeconds from now, remembering the readiness the lobby
	/// had at that moment. A duration the caller could not read falls back to five seconds, which is
	/// what the lobby counts down by default.
	void arm(long long nowUnixMs, int durationSeconds, unsigned readySignatureNow)
	{
		if (durationSeconds <= 0)
		{
			durationSeconds = kDefaultDurationSeconds;
		}

		armed = true;
		endsAtUnixMs = nowUnixMs + (long long)durationSeconds * 1000;
		readySignature = readySignatureNow;
	}

	/// True while the mirrored countdown still has time left. It is not cleared here: a caller that
	/// wants the state gone asks active() first and clear()s, so the expiry is observable.
	bool active(long long nowUnixMs) const
	{
		return armed && nowUnixMs < endsAtUnixMs;
	}

	/// A lobby change resets readiness on every client, and on the host's client the same change
	/// stops its countdown. So a different signature means the countdown was called off before its
	/// end, and the mirror must stop counting instead of announcing an end that will not happen.
	/// Returns true when it cancelled.
	bool cancelIfReadyChanged(unsigned readySignatureNow)
	{
		if (!armed || readySignatureNow == readySignature)
		{
			return false;
		}

		clear();

		return true;
	}

	void clear()
	{
		armed = false;
		endsAtUnixMs = 0;
		readySignature = 0;
	}
};

/// The readiness of a lobby folded into one number: each member contributes its ready bit, weighted
/// by its position, so a single player going un-ready changes the signature. Lives here, next to the
/// state that consumes it, because "did readiness change" is the cancel signal and the two must agree.
inline unsigned GencastReadySignatureStep(unsigned signature, int slotIndex, bool ready)
{
	return signature * 33u + (unsigned)(slotIndex + 1) * 2u + (ready ? 1u : 0u);
}
