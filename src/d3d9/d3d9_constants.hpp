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

#include "d3d9.h"
#include <cstdint>

#ifndef D3DPRESENT_FORCEIMMEDIATE
#define D3DPRESENT_FORCEIMMEDIATE 0x00000100L
#endif

namespace mullion::caps {

constexpr uint32_t MaxStreams = 16;
constexpr uint32_t MaxSimultaneousRenderTargets = 4;
constexpr uint32_t MaxTexturesFF = 8;
constexpr uint32_t MaxEnabledLights = 8;
constexpr uint32_t MaxClipPlanes = 6;
constexpr uint32_t MaxFloatConstantsVS = 256;
constexpr uint32_t MaxFloatConstantsPS = 224;
constexpr uint32_t MaxOtherConstants = 16;
constexpr uint32_t MaxSamplersPS = 16;
constexpr uint32_t MaxSamplersVS = 4;
constexpr uint32_t MaxTransforms = 512;

} // namespace mullion::caps
