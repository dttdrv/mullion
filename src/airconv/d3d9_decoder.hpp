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

#include "d3d9_shader.hpp"
#include <optional>
#include <string>

namespace mullion::d3d9 {

// fills shader.bbs, shader.shader_info and data from SM1-SM3 tokens. Returns an error message on failure.
std::optional<std::string> decode_shader(
    const uint32_t *tokens, size_t count, dxbc::SM50ShaderInternal &shader, ShaderData &data,
    const DecodeOptions &Options = {}
);

} // namespace mullion::d3d9
