#pragma once

// The countdown as seen by a client that is not running it: arm from the host's start line,
// cancel when the lobby goes un-ready. Engine-free so tools/countdown_mirror_test.sh can compile it.

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
