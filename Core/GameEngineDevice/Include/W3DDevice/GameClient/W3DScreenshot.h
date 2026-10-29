/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

#pragma once

#include "GameClient/Display.h"

void W3D_TakeCompressedScreenshot(ScreenshotFormat format, Int jpegQuality);

// Capture-feature variant: writes one PNG to exactly `pathname`, synchronously, so the caller can
// write a sidecar beside a file that already exists. outWidth/outHeight receive the back buffer's
// size. Returns false when the back buffer format is unsupported or the encode failed.
bool W3D_TakeCompressedScreenshotTo(const char* pathname, Int* outWidth, Int* outHeight);

// Called once per frame on the main thread to show messages for screenshots
// that the screenshot thread has finished writing.
void W3D_UpdateScreenshotMessages();

