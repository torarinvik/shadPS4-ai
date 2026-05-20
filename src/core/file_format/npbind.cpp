// SPDX-FileCopyrightText: Copyright 2025-2026 shadLauncher4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include "npbind.h"

namespace {
constexpr u64 MaxNpBindEntries = 4096;
constexpr u16 NpCommIdType = 0x0010;
constexpr u16 NpCommIdSize = 12;
constexpr u16 TrophyType = 0x0011;
constexpr u16 TrophySize = 12;
constexpr u16 Unknown1Type = 0x0012;
constexpr u16 Unknown1Size = 176;
constexpr u16 Unknown2Type = 0x0013;
constexpr u16 Unknown2Size = 16;

bool IsSafeNpCommId(std::string_view value) {
    if (value.size() != NpCommIdSize || !value.starts_with("NP")) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    });
}
} // namespace

bool NPBindFile::Load(const std::string& path) {
    Clear(); // Clear any existing data

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;

    std::streamsize sz = f.tellg();
    if (sz <= 0)
        return false;

    f.seekg(0, std::ios::beg);
    std::vector<u8> buf(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz))
        return false;

    const u64 size = buf.size();
    if (size < sizeof(NpBindHeader))
        return false;

    // Read header
    memcpy(&m_header, buf.data(), sizeof(NpBindHeader));
    if (m_header.magic != NPBIND_MAGIC)
        return false;
    if (m_header.num_entries > MaxNpBindEntries)
        return false;

    // offset start of bodies
    size_t offset = sizeof(NpBindHeader);

    m_bodies.reserve(static_cast<size_t>(m_header.num_entries));

    // For each body: read 4 TLV entries then skip padding (0x98 = 152 bytes)
    const u64 body_padding = 0x98; // 152

    for (u64 bi = 0; bi < m_header.num_entries; ++bi) {
        // Ensure we have room for 4 entries' headers at least
        if (offset + 4 * 4 > size)
            return false; // 4 entries x (type+size)

        NPBindBody body;

        // helper lambda to read one entry
        auto read_entry = [&](NPBindEntryRaw& e) -> bool {
            if (offset + 4 > size)
                return false;

            memcpy(&e.type, &buf[offset], 2);
            memcpy(&e.size, &buf[offset + 2], 2);
            offset += 4;

            if (offset + e.size > size)
                return false;

            e.data.assign(buf.begin() + offset, buf.begin() + offset + e.size);
            offset += e.size;
            return true;
        };

        // read 4 entries in order
        if (!read_entry(body.npcommid))
            return false;
        if (body.npcommid.type != NpCommIdType || body.npcommid.size != NpCommIdSize)
            return false;
        if (!read_entry(body.trophy))
            return false;
        if (body.trophy.type != TrophyType || body.trophy.size != TrophySize)
            return false;
        if (!read_entry(body.unk1))
            return false;
        if (body.unk1.type != Unknown1Type || body.unk1.size != Unknown1Size)
            return false;
        if (!read_entry(body.unk2))
            return false;
        if (body.unk2.type != Unknown2Type || body.unk2.size != Unknown2Size)
            return false;

        // skip fixed padding after body if present (but don't overrun)
        if (offset + body_padding <= size) {
            offset += body_padding;
        } else {
            // If padding not fully present, allow file to end (some variants may omit)
            offset = size;
        }

        m_bodies.push_back(std::move(body));
    }

    // Read digest if available
    if (size >= 20) {
        // Digest is typically the last 20 bytes, independent of offset
        memcpy(m_digest, &buf[size - 20], 20);
    } else {
        memset(m_digest, 0, 20);
    }

    return true;
}

std::vector<std::string> NPBindFile::GetNpCommIds() const {
    std::vector<std::string> npcommids;
    npcommids.reserve(m_bodies.size());

    for (const auto& body : m_bodies) {
        // Convert binary data to string directly
        if (!body.npcommid.data.empty()) {
            std::string raw_string(reinterpret_cast<const char*>(body.npcommid.data.data()),
                                   body.npcommid.data.size());
            if (IsSafeNpCommId(raw_string)) {
                npcommids.push_back(raw_string);
            }
        }
    }

    return npcommids;
}
