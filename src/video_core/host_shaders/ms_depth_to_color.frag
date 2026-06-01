// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

layout (binding = 0, set = 0) uniform texture2DMS in_depth;

layout (location = 0) out float out_color;

void main()
{
    out_color = texelFetch(in_depth, ivec2(gl_FragCoord.xy), gl_SampleID).r;
}
