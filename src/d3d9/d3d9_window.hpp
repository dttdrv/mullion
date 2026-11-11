/*
 * Copyright 2026 David Skoland
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

#include <windows.h>

namespace mullion {

class MTLD3D9Device;

// A fullscreen device subclasses its focus window to see focus changes, as native D3D9 does.
bool D3D9HookFocusWindow(HWND Window, MTLD3D9Device *pDevice);
void D3D9UnhookFocusWindow(HWND Window, MTLD3D9Device *pDevice);

} // namespace mullion
