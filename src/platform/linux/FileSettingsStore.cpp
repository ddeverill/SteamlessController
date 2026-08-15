#include "FileSettingsStore.h"
#include <toml.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

struct FileSettingsStore::Impl {
    toml::table root;
};

namespace {

std::vector<std::string> SplitPath(std::string_view section) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= section.size()) {
        const size_t slash = section.find('/', start);
        const std::string_view part = section.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!part.empty()) parts.emplace_back(part);
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return parts;
}

// Walks (and, when `create`, builds) the nested-table path a slash-separated
// section name describes. Returns nullptr on a missing path when create is
// false, or if an existing non-table value occupies where a table is needed
// (a corrupt/hand-edited config — treated as "nothing here" rather than
// crashing).
toml::table* Navigate(toml::table& root, std::string_view section, bool create) {
    toml::table* cur = &root;
    for (const auto& part : SplitPath(section)) {
        auto* node = cur->get(part);
        if (!node) {
            if (!create) return nullptr;
            cur = cur->insert(part, toml::table{}).first->second.as_table();
            continue;
        }
        auto* tbl = node->as_table();
        if (!tbl) return nullptr;  // occupied by a non-table value
        cur = tbl;
    }
    return cur;
}

}  // namespace

FileSettingsStore::FileSettingsStore(std::string path) : m_path(std::move(path)) {
    m_impl = std::make_unique<Impl>();
    Load();
}

FileSettingsStore::~FileSettingsStore() {
    if (m_dirty) Flush();
}

void FileSettingsStore::Load() {
    std::ifstream f(m_path);
    if (!f) return;  // no config yet — an empty store is a valid, fresh install
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        m_impl->root = toml::parse(ss.str());
    } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "config.toml parse error at %s: %s — starting from an empty config\n",
                     m_path.c_str(), e.what());
        m_impl->root = toml::table{};
    }
}

bool FileSettingsStore::Flush() {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::error_code ec;
    fs::create_directories(fs::path(m_path).parent_path(), ec);

    const std::string tmpPath = m_path + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::trunc);
        if (!out) return false;
        out << m_impl->root;
        if (!out.good()) return false;
    }
    fs::rename(tmpPath, m_path, ec);
    if (ec) return false;
    m_dirty = false;
    return true;
}

std::optional<uint32_t> FileSettingsStore::GetUInt(std::string_view section, std::string_view key) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    toml::table* tbl = Navigate(m_impl->root, section, false);
    if (!tbl) return std::nullopt;
    if (auto v = tbl->get_as<int64_t>(key)) return static_cast<uint32_t>(**v);
    return std::nullopt;
}

std::optional<std::string> FileSettingsStore::GetString(std::string_view section, std::string_view key) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    toml::table* tbl = Navigate(m_impl->root, section, false);
    if (!tbl) return std::nullopt;
    if (auto v = tbl->get_as<std::string>(key)) return **v;
    return std::nullopt;
}

void FileSettingsStore::SetUInt(std::string_view section, std::string_view key, uint32_t value) {
    std::lock_guard<std::mutex> lk(m_mutex);
    toml::table* tbl = Navigate(m_impl->root, section, true);
    tbl->insert_or_assign(std::string(key), static_cast<int64_t>(value));
    m_dirty = true;
}

void FileSettingsStore::SetString(std::string_view section, std::string_view key, const std::string& value) {
    std::lock_guard<std::mutex> lk(m_mutex);
    toml::table* tbl = Navigate(m_impl->root, section, true);
    tbl->insert_or_assign(std::string(key), value);
    m_dirty = true;
}

std::vector<std::string> FileSettingsStore::ListSubsections(std::string_view section) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    toml::table* tbl = Navigate(m_impl->root, section, false);
    std::vector<std::string> out;
    if (!tbl) return out;
    for (auto&& [k, v] : *tbl)
        if (v.is_table()) out.emplace_back(k.str());
    return out;
}

void FileSettingsStore::RemoveSection(std::string_view section) {
    std::lock_guard<std::mutex> lk(m_mutex);
    const auto parts = SplitPath(section);
    if (parts.empty()) return;
    toml::table* parent = &m_impl->root;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        auto* node = parent->get(parts[i]);
        if (!node) return;
        auto* tbl = node->as_table();
        if (!tbl) return;
        parent = tbl;
    }
    parent->erase(parts.back());
    m_dirty = true;
}
