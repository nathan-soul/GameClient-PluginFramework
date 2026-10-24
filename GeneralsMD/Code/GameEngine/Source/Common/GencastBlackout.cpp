#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/GencastBlackout.h"
#include "Common/GameAudio.h"

Bool TheGenCastBlackout = FALSE;

void GencastSetBlackout(Bool on)
{
	if (TheGenCastBlackout == on)
		return;

	TheGenCastBlackout = on;

	if (TheAudio == nullptr)
		return;

	if (on)
		TheAudio->muteAudio(AudioManager::MuteAudioReason_GencastBlackout);
	else
		TheAudio->unmuteAudio(AudioManager::MuteAudioReason_GencastBlackout);
}

#endif // GENERALS_ONLINE
