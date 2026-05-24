// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <system_error>
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/file_sys/devices/logger.h"
#include "core/file_sys/devices/nop_device.h"
#include "core/file_sys/fs.h"

namespace Core::FileSys {

bool MntPoints::ignore_game_patches = false;

std::string RemoveTrailingSlashes(const std::string& path) {
    // Remove trailing slashes to make comparisons simpler.
    std::string path_sanitized = path;
    while (path_sanitized.ends_with("/")) {
        path_sanitized.pop_back();
    }
    return path_sanitized;
}

static bool IsHostSidecarEntry(const std::filesystem::path& path) {
    return Common::ToLower(path.filename().string()) == ".ds_store";
}

static bool IsSafeGuestRelativePath(std::string_view rel_path) {
    if (rel_path.empty() || rel_path.find('\0') != std::string_view::npos) {
        return false;
    }

    std::filesystem::path parsed{std::string{rel_path}};
    if (parsed.is_absolute() || parsed.has_root_name()) {
        return false;
    }

    for (const auto& component : parsed) {
        if (component.empty() || component == ".." || component.has_root_path()) {
            return false;
        }
#ifdef _WIN32
        const auto component_string = component.string();
        if (component_string.find('\\') != std::string::npos ||
            component_string.find(':') != std::string::npos) {
            return false;
        }
#endif
    }

    return true;
}

static std::filesystem::path CanonicalRoot(const std::filesystem::path& root) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(root, ec);
    return ec ? root.lexically_normal() : canonical;
}

static bool IsPathContainedInRoot(const std::filesystem::path& path,
                                  const std::filesystem::path& root) {
    std::error_code ec;
    const auto normalized_root = CanonicalRoot(root);
    const auto normalized_path = std::filesystem::weakly_canonical(path, ec);
    const auto checked_path = ec ? path.lexically_normal() : normalized_path;
    const auto relative = std::filesystem::relative(checked_path, normalized_root, ec);
    if (ec || relative.empty()) {
        return checked_path == normalized_root;
    }
    return relative.native() != ".." && *relative.begin() != "..";
}

static std::filesystem::path SafeJoinHostPath(const std::filesystem::path& root,
                                              std::string_view rel_path) {
    if (!IsSafeGuestRelativePath(rel_path)) {
        LOG_WARNING(Kernel_Fs, "Rejected unsafe guest relative path '{}'", rel_path);
        return {};
    }

    const auto joined = (root / std::filesystem::path{std::string{rel_path}}).lexically_normal();
    if (!IsPathContainedInRoot(joined, root)) {
        LOG_WARNING(Kernel_Fs, "Rejected guest path escape '{}' under '{}'", joined.string(),
                    root.string());
        return {};
    }

    return joined;
}

void MntPoints::Mount(const std::filesystem::path& host_folder, const std::string& guest_folder,
                      bool read_only) {
    std::scoped_lock lock{m_mutex};
    const auto guest_folder_sanitized = RemoveTrailingSlashes(guest_folder);
    m_mnt_pairs.emplace_back(CanonicalRoot(host_folder), guest_folder_sanitized, read_only);
}

void MntPoints::Unmount(const std::filesystem::path& host_folder, const std::string& guest_folder) {
    std::scoped_lock lock{m_mutex};
    const auto guest_folder_sanitized = RemoveTrailingSlashes(guest_folder);
    auto it = std::remove_if(m_mnt_pairs.begin(), m_mnt_pairs.end(), [&](const MntPair& pair) {
        return pair.mount == guest_folder_sanitized;
    });
    m_mnt_pairs.erase(it, m_mnt_pairs.end());
}

void MntPoints::UnmountAll() {
    std::scoped_lock lock{m_mutex};
    m_mnt_pairs.clear();
}

std::filesystem::path MntPoints::GetHostPath(std::string_view path, bool* is_read_only,
                                             HostPathType path_type) {
    // Evil games like Turok2 pass double slashes e.g /app0//game.kpf
    std::string corrected_path(path);
    size_t pos = corrected_path.find("//");
    while (pos != std::string::npos) {
        corrected_path.replace(pos, 2, "/");
        pos = corrected_path.find("//", pos + 1);
    }

    if (corrected_path.length() > 255)
        return "";

    const std::optional<MntPair> mount = GetMount(corrected_path);
    if (!mount) {
        return "";
    }

    if (is_read_only) {
        *is_read_only = mount->read_only;
    }

    const auto corrected_path_sanitized = RemoveTrailingSlashes(corrected_path);
    std::filesystem::path host_path = mount->host_path;

    std::filesystem::path patch_path = mount->host_path;
    patch_path += "-UPDATE";
    if (!std::filesystem::exists(patch_path)) {
        patch_path = mount->host_path;
        patch_path += "-patch";
    }

    std::filesystem::path mods_path = mount->host_path;
    mods_path += "-mods";

    // If we're just retrieving the mount, return the path for the requested backing layer.
    if (corrected_path_sanitized == mount->mount) {
        if (path_type == HostPathType::Mod) {
            return mods_path;
        } else if (path_type == HostPathType::Patch) {
            return patch_path;
        }
        return host_path;
    }

    // Remove device (e.g /app0) from path to retrieve relative path.
    const auto rel_path = std::string_view{corrected_path}.substr(mount->mount.size() + 1);
    host_path = SafeJoinHostPath(mount->host_path, rel_path);
    if (host_path.empty()) {
        return {};
    }
    patch_path = SafeJoinHostPath(patch_path, rel_path);
    mods_path = SafeJoinHostPath(mods_path, rel_path);

    if (path_type == HostPathType::Mod) {
        return mods_path;
    } else if (path_type == HostPathType::Patch) {
        return patch_path;
    }

    if ((corrected_path.starts_with("/app0") || corrected_path.starts_with("/hostapp")) &&
        path_type != HostPathType::Base && std::filesystem::exists(mods_path)) {
        return mods_path;
    }

    if ((corrected_path.starts_with("/app0") || corrected_path.starts_with("/hostapp")) &&
        path_type != HostPathType::Base && !ignore_game_patches &&
        std::filesystem::exists(patch_path)) {
        return patch_path;
    }

    if (!NeedsCaseInsensitiveSearch) {
        return host_path;
    }

    const auto search = [&](const auto host_path) {
        // If the path does not exist attempt to verify this.
        // Retrieve parent path until we find one that exists.
        std::scoped_lock lk{m_mutex};
        path_parts.clear();
        auto current_path = host_path;
        while (!current_path.empty() && !std::filesystem::exists(current_path)) {
            // We have probably cached this if it's a folder.
            if (auto it = path_cache.find(current_path); it != path_cache.end()) {
                current_path = it->second;
                break;
            }
            path_parts.emplace_back(current_path.filename());
            current_path = current_path.parent_path();
        }
        if (!current_path.empty()) {
            // We have found an anchor. Traverse parts we recoded and see if they
            // exist in filesystem but in different case.
            auto guest_path = current_path;
            while (!path_parts.empty()) {
                const auto part = path_parts.back();
                const auto add_match = [&](const auto& host_part) {
                    current_path /= host_part;
                    guest_path /= part;
                    path_cache[guest_path] = current_path;
                    path_parts.pop_back();
                };
                // Can happen when the mismatch is in upper folder.
                if (std::filesystem::exists(current_path / part)) {
                    add_match(part);
                    continue;
                }
                const auto part_low = Common::ToLower(part.string());
                if (part_low == ".ds_store") {
                    return std::optional<std::filesystem::path>({});
                }
                bool found_match = false;
                for (const auto& path : std::filesystem::directory_iterator(current_path)) {
                    if (IsHostSidecarEntry(path.path())) {
                        continue;
                    }
                    const auto candidate = path.path().filename();
                    const auto filename = Common::ToLower(candidate.string());
                    // Check if a filename matches in case insensitive manner.
                    if (filename != part_low) {
                        continue;
                    }
                    // We found a match, record the actual path in the cache.
                    add_match(candidate);
                    found_match = true;
                    break;
                }
                if (!found_match) {
                    return std::optional<std::filesystem::path>({});
                }
            }
        }
        return std::optional<std::filesystem::path>(current_path);
    };

    if ((corrected_path.starts_with("/app0") || corrected_path.starts_with("/hostapp")) &&
        path_type != HostPathType::Base) {
        if (const auto path = search(mods_path)) {
            return *path;
        }
    }

    if (path_type != HostPathType::Base && !ignore_game_patches) {
        if (const auto path = search(patch_path)) {
            return *path;
        }
    }
    if (const auto path = search(host_path)) {
        return *path;
    }

    // Opening the guest path will surely fail but at least gives
    // a better error message than the empty path.
    return host_path;
}

// TODO: Does not handle mount points inside mount points.
void MntPoints::IterateDirectory(std::string_view guest_directory,
                                 const IterateDirectoryCallback& callback) {
    const auto base_path = GetHostPath(guest_directory, nullptr, HostPathType::Base);
    if (base_path.empty()) {
        return;
    }

    // Forces path types so as not to resolve to base path
    const auto patch_path = GetHostPath(guest_directory, nullptr, HostPathType::Patch);
    const auto mod_path = GetHostPath(guest_directory, nullptr, HostPathType::Mod);

    // Prepend entries for . and .., as both are treated as files on PS4.
    callback(base_path / ".", false);
    callback(base_path / "..", false);

    // Pass 1: Any files that existed in the base directory, using mod/patch directory if needed.
    if (std::filesystem::exists(base_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(base_path)) {
            if (IsHostSidecarEntry(entry.path())) {
                continue;
            }
            const auto mod_entry_path = mod_path / entry.path().filename();
            const auto patch_entry_path = patch_path / entry.path().filename();
            if (std::filesystem::exists(mod_entry_path)) {
                callback(mod_entry_path, !std::filesystem::is_directory(mod_entry_path));
                continue;
            } else if (std::filesystem::exists(patch_entry_path)) {
                callback(patch_entry_path, !std::filesystem::is_directory(patch_entry_path));
                continue;
            }
            callback(entry.path(), !entry.is_directory());
        }
    }

    // Pass 2: Any files that exist only in the patch directory.
    if (std::filesystem::exists(patch_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(patch_path)) {
            if (IsHostSidecarEntry(entry.path())) {
                continue;
            }
            const auto base_entry_path = base_path / entry.path().filename();
            if (!std::filesystem::exists(base_entry_path)) {
                const auto mod_entry_path = mod_path / entry.path().filename();
                if (std::filesystem::exists(mod_entry_path)) {
                    callback(mod_entry_path, !std::filesystem::is_directory(mod_entry_path));
                    continue;
                }
                callback(entry.path(), !entry.is_directory());
            }
        }
    }

    // Pass 3: Any files that exist only in the mod directory (confirmed this can be valid)
    if (std::filesystem::exists(mod_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(mod_path)) {
            if (IsHostSidecarEntry(entry.path())) {
                continue;
            }
            const auto base_entry_path = base_path / entry.path().filename();
            const auto patch_entry_path = patch_path / entry.path().filename();
            if (!std::filesystem::exists(base_entry_path) &&
                !std::filesystem::exists(patch_entry_path)) {
                callback(entry.path(), !entry.is_directory());
            }
        }
    }
}

int HandleTable::CreateHandle() {
    std::scoped_lock lock{m_mutex};

    auto* file = new File{};
    file->is_opened = false;

    int existingFilesNum = m_files.size();

    for (int index = 0; index < existingFilesNum; index++) {
        if (m_files.at(index) == nullptr) {
            m_files[index] = file;
            return index;
        }
    }

    m_files.push_back(file);
    return m_files.size() - 1;
}

void HandleTable::DeleteHandle(int d) {
    std::scoped_lock lock{m_mutex};
    delete m_files.at(d);
    m_files[d] = nullptr;
}

File* HandleTable::GetFile(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    return m_files.at(d);
}

File* HandleTable::GetSocket(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    auto file = m_files.at(d);
    if (!file) {
        return nullptr;
    }
    if (file->type != Core::FileSys::FileType::Socket) {
        return nullptr;
    }
    return file;
}

File* HandleTable::GetEpoll(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    auto file = m_files.at(d);
    if (!file) {
        return nullptr;
    }
    if (file->type != Core::FileSys::FileType::Epoll) {
        return nullptr;
    }
    return file;
}

File* HandleTable::GetResolver(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    auto file = m_files.at(d);
    if (!file) {
        return nullptr;
    }
    if (file->type != Core::FileSys::FileType::Resolver) {
        return nullptr;
    }
    return file;
}

File* HandleTable::GetFile(const std::filesystem::path& host_name) {
    std::scoped_lock lock{m_mutex};
    for (auto* file : m_files) {
        if (file != nullptr && file->m_host_name == host_name) {
            return file;
        }
    }
    return nullptr;
}

void HandleTable::CreateStdHandles() {
    auto setup = [this](const char* path, auto* device) {
        int fd = CreateHandle();
        auto* file = GetFile(fd);
        file->is_opened = true;
        file->type = FileType::Device;
        file->m_guest_name = path;
        file->device =
            std::shared_ptr<Devices::BaseDevice>{reinterpret_cast<Devices::BaseDevice*>(device)};
    };
    // order matters
    setup("/dev/stdin", new Devices::Logger("stdin", false));   // stdin
    setup("/dev/stdout", new Devices::Logger("stdout", false)); // stdout
    setup("/dev/stderr", new Devices::Logger("stderr", true));  // stderr
}

int HandleTable::GetFileDescriptor(File* file) {
    std::scoped_lock lock{m_mutex};
    auto it = std::find(m_files.begin(), m_files.end(), file);

    if (it != m_files.end()) {
        return std::distance(m_files.begin(), it);
    }
    return -1;
}

} // namespace Core::FileSys
