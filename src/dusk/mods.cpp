#include "dusk/mods.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_loadso.h>

#include "nlohmann/json.hpp"

#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dusk/data.hpp"
#include "dusk/io.hpp"
#include "dusk/logging.h"

namespace dusk::mods {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

// Platform shared-library extension. A mod folder may also ship libraries for
// other platforms; we only load the one matching the host.
#if defined(_WIN32)
constexpr const char* kLibExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kLibExt = ".dylib";
#else
constexpr const char* kLibExt = ".so";
#endif

struct LoadedLib {
    SDL_SharedObject* handle = nullptr;
    DuskModDisposeFn dispose = nullptr;
};

// A deque keeps element addresses stable as mods are discovered (at startup or
// via refresh()), so &ModEntry handed to a loaded mod as DuskMod* stays valid.
std::deque<ModEntry> g_entries;
std::unordered_map<std::string, LoadedLib> g_loaded;  // keyed by mod id

ModEntry* find_entry(std::string_view id) {
    for (ModEntry& e : g_entries) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

ModSetting* find_setting(ModEntry* entry, std::string_view key) {
    if (entry == nullptr) {
        return nullptr;
    }
    for (ModSetting& s : entry->settings) {
        if (s.key == key) {
            return &s;
        }
    }
    return nullptr;
}

// ---- per-mod config.json (enabled state + setting values) -------------------

fs::path config_path(const ModEntry& entry) {
    return fs::path(entry.folder) / "config.json";
}

void read_config(ModEntry& entry) {
    const fs::path path = config_path(entry);
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return;  // keep defaults
    }
    try {
        const auto bytes = io::FileStream::ReadAllBytes(path);
        const json j = json::parse(bytes);
        if (!j.is_object()) {
            return;
        }
        if (auto it = j.find("enabled"); it != j.end() && it->is_boolean()) {
            entry.enabled = it->get<bool>();
        }
        if (auto settings = j.find("settings"); settings != j.end() && settings->is_object()) {
            for (ModSetting& s : entry.settings) {
                if (auto v = settings->find(s.key); v != settings->end() && v->is_number()) {
                    s.value = v->get<double>();
                }
            }
        }
    } catch (const std::exception& e) {
        DuskLog.warn("Mod '{}': failed to read config.json: {}", entry.id, e.what());
    }
}

void write_config(const ModEntry& entry) {
    json settings = json::object();
    for (const ModSetting& s : entry.settings) {
        settings[s.key] = s.value;
    }
    const json j = json{{"enabled", entry.enabled}, {"settings", settings}};
    try {
        io::FileStream::WriteAllText(config_path(entry), j.dump(4));
    } catch (const std::exception& e) {
        DuskLog.warn("Mod '{}': failed to write config.json: {}", entry.id, e.what());
    }
}

void apply_setting(ModEntry* entry, std::string_view key, double value) {
    ModSetting* s = find_setting(entry, key);
    if (s == nullptr || s->value == value) {
        return;
    }
    s->value = value;
    write_config(*entry);
}

// ---- generated settings.json (the schema a mod declares in code) -------------

const char* setting_type_name(SettingType type) {
    switch (type) {
    case SettingType::Bool:
        return "bool";
    case SettingType::Int:
        return "int";
    default:
        return "float";
    }
}

void write_settings_json(const ModEntry& entry) {
    json arr = json::array();
    for (const ModSetting& s : entry.settings) {
        arr.push_back(json{
            {"key", s.key},
            {"label", s.label},
            {"help", s.help},
            {"type", setting_type_name(s.type)},
            {"default", s.defaultValue},
            {"min", s.minValue},
            {"max", s.maxValue},
            {"step", s.step},
        });
    }
    try {
        io::FileStream::WriteAllText(fs::path(entry.folder) / "settings.json", arr.dump(4));
    } catch (const std::exception& e) {
        DuskLog.warn("Mod '{}': failed to write settings.json: {}", entry.id, e.what());
    }
}

// ---- generic host capability table (passed to every mod) --------------------

void host_log(const char* msg) {
    DuskLog.info("[mod] {}", msg ? msg : "");
}

double host_setting_get(DuskMod* self, const char* key) {
    ModSetting* s = find_setting(reinterpret_cast<ModEntry*>(self), key ? key : "");
    return s ? s->value : 0.0;
}

void host_setting_set(DuskMod* self, const char* key, double value) {
    apply_setting(reinterpret_cast<ModEntry*>(self), key ? key : "", value);
}

void host_define_settings(DuskMod* self, const DuskSetting* arr, uint32_t count) {
    ModEntry* entry = reinterpret_cast<ModEntry*>(self);
    if (entry == nullptr) {
        return;
    }
    std::vector<ModSetting> rebuilt;
    rebuilt.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const DuskSetting& src = arr[i];
        if (src.key == nullptr) {
            continue;
        }
        ModSetting s;
        s.key = src.key;
        s.label = src.label ? src.label : src.key;
        s.help = src.help ? src.help : "";
        s.type = static_cast<SettingType>(src.type);
        s.defaultValue = src.default_value;
        s.minValue = src.min_value;
        s.maxValue = src.max_value;
        s.step = src.step;
        s.value = src.default_value;
        rebuilt.push_back(std::move(s));
    }
    entry->settings = std::move(rebuilt);
    read_config(*entry);          // overlay saved values from config.json
    write_settings_json(*entry);  // (re)generate the schema file
}

const DuskHost g_host = {
    .abi_version = DUSK_MOD_ABI_VERSION,
    .log = host_log,
    .define_settings = host_define_settings,
    .setting_get = host_setting_get,
    .setting_set = host_setting_set,
};

// ---- load / unload ----------------------------------------------------------

template <typename Fn>
Fn load_symbol(SDL_SharedObject* handle, const char* name) {
    return reinterpret_cast<Fn>(SDL_LoadFunction(handle, name));
}

bool load_mod(ModEntry& entry) {
    if (entry.libraryPath.empty()) {
        DuskLog.warn("Mod '{}' has no library for this platform", entry.id);
        return false;
    }

    SDL_SharedObject* handle = SDL_LoadObject(entry.libraryPath.c_str());
    if (handle == nullptr) {
        DuskLog.warn("Mod '{}' failed to load: {}", entry.id, SDL_GetError());
        return false;
    }

    auto init = load_symbol<DuskModInitFn>(handle, DUSK_MOD_INIT_SYMBOL);
    auto dispose = load_symbol<DuskModDisposeFn>(handle, DUSK_MOD_DISPOSE_SYMBOL);
    if (init == nullptr || dispose == nullptr) {
        DuskLog.warn("Mod '{}' is missing required entry points", entry.id);
        SDL_UnloadObject(handle);
        return false;
    }

    if (init(&g_host, reinterpret_cast<DuskMod*>(&entry)) != 0) {
        DuskLog.warn("Mod '{}' init reported failure", entry.id);
        SDL_UnloadObject(handle);
        return false;
    }

    g_loaded[entry.id] = LoadedLib{handle, dispose};
    entry.enabled = true;
    DuskLog.info("Enabled mod '{}' ({})", entry.id, entry.version);
    return true;
}

void unload_mod(ModEntry& entry) {
    auto it = g_loaded.find(entry.id);
    if (it != g_loaded.end()) {
        if (it->second.dispose != nullptr) {
            it->second.dispose();
        }
        if (it->second.handle != nullptr) {
            SDL_UnloadObject(it->second.handle);
        }
        g_loaded.erase(it);
    }
    entry.enabled = false;
    DuskLog.info("Disabled mod '{}'", entry.id);
}

// ---- discovery (mod.json + settings.json) -----------------------------------

SettingType parse_setting_type(const std::string& type) {
    if (type == "bool") {
        return SettingType::Bool;
    }
    if (type == "int") {
        return SettingType::Int;
    }
    return SettingType::Float;
}

// Settings descriptors live in settings.json (a JSON array) next to mod.json.
void parse_settings(const fs::path& folder, ModEntry& entry) {
    const fs::path path = folder / "settings.json";
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return;  // a mod may have no settings
    }
    json arr;
    try {
        arr = json::parse(io::FileStream::ReadAllBytes(path));
    } catch (const std::exception& e) {
        DuskLog.warn("Mod '{}': invalid settings.json: {}", entry.id, e.what());
        return;
    }
    if (!arr.is_array()) {
        return;
    }
    for (const json& s : arr) {
        if (!s.is_object() || !s.contains("key")) {
            continue;
        }
        ModSetting setting;
        setting.key = s.value("key", "");
        if (setting.key.empty()) {
            continue;
        }
        setting.label = s.value("label", setting.key);
        setting.help = s.value("help", "");
        setting.type = parse_setting_type(s.value("type", "float"));
        setting.defaultValue = s.value("default", 0.0);
        setting.minValue = s.value("min", 0.0);
        setting.maxValue = s.value("max", 0.0);
        setting.step = s.value("step", 0.0);
        setting.value = setting.defaultValue;
        entry.settings.push_back(std::move(setting));
    }
}

// Find a platform library in the folder: preferring `<base><ext>` if a base name
// is given, otherwise the first file with the platform extension.
std::string find_platform_lib(const fs::path& folder, const std::string& base) {
    std::error_code ec;
    if (!base.empty()) {
        const fs::path candidate = folder / (base + kLibExt);
        if (fs::exists(candidate, ec)) {
            return candidate.string();
        }
    }
    for (const auto& file : fs::directory_iterator(folder, ec)) {
        if (file.is_regular_file(ec) && file.path().extension() == kLibExt) {
            return file.path().string();
        }
    }
    return {};
}

void read_mod(const fs::path& folder, std::unordered_set<std::string>& seen) {
    const fs::path manifestPath = folder / "mod.json";
    std::error_code ec;
    if (!fs::exists(manifestPath, ec)) {
        return;  // not a mod folder
    }

    json manifest;
    try {
        manifest = json::parse(io::FileStream::ReadAllBytes(manifestPath));
    } catch (const std::exception& e) {
        DuskLog.warn("Skipping '{}': invalid mod.json: {}", folder.string(), e.what());
        return;
    }
    if (!manifest.is_object()) {
        return;
    }

    ModEntry entry;
    entry.folder = folder.string();
    entry.id = manifest.value("id", folder.filename().string());
    entry.name = manifest.value("name", entry.id);
    entry.version = manifest.value("version", "");
    entry.author = manifest.value("author", "");
    entry.about = manifest.value("about", "");
    entry.libraryPath = find_platform_lib(folder, manifest.value("library", std::string{}));

    if (entry.id.empty() || !seen.emplace(entry.id).second) {
        DuskLog.warn("Mod at '{}' has a missing or duplicate id; skipped", folder.string());
        return;
    }

    // Mods are enabled by default; a generated config.json may override this.
    // settings.json (generated by the mod on a prior run) gives the UI its
    // descriptors before the mod is loaded; once loaded, define_settings
    // refreshes them from code.
    entry.enabled = true;
    parse_settings(folder, entry);
    read_config(entry);
    g_entries.push_back(std::move(entry));
}

void scan_root(const fs::path& root, std::unordered_set<std::string>& seen) {
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return;
    }
    for (const auto& dirEntry : fs::directory_iterator(root, ec)) {
        if (dirEntry.is_directory(ec)) {
            read_mod(dirEntry.path(), seen);
        }
    }
}

// Scan every mods root (next to the executable + in the data folder). `seen`
// pre-populated with known ids makes this additive (existing mods are skipped).
void scan_all(std::unordered_set<std::string>& seen) {
    if (const char* base = SDL_GetBasePath()) {
        scan_root(fs::path(base) / "mods", seen);
    }
    scan_root(dusk::data::configured_data_path() / "mods", seen);
}

}  // namespace

void initialize() {
    g_entries.clear();

    std::unordered_set<std::string> seen;
    scan_all(seen);

    DuskLog.info("Discovered {} mod(s)", g_entries.size());

    // Load mods marked enabled in their config (after the full scan so the entry
    // pointers handed to mods stay stable).
    for (ModEntry& entry : g_entries) {
        if (entry.enabled) {
            entry.enabled = false;  // load_mod sets it on success
            load_mod(entry);
        }
    }
}

void shutdown() {
    for (ModEntry& entry : g_entries) {
        if (entry.enabled) {
            unload_mod(entry);
        }
    }
}

void refresh() {
    std::unordered_set<std::string> seen;
    for (const ModEntry& e : g_entries) {
        seen.insert(e.id);
    }

    const std::size_t before = g_entries.size();
    scan_all(seen);  // only ids not already present are appended

    const std::size_t added = g_entries.size() - before;
    DuskLog.info("Refresh: {} new mod(s) discovered", added);

    // Enable + load anything newly discovered (deque keeps prior entries stable).
    for (std::size_t i = before; i < g_entries.size(); ++i) {
        ModEntry& entry = g_entries[i];
        entry.enabled = false;  // load_mod sets it on success
        if (load_mod(entry)) {
            write_config(entry);
        }
    }
}

bool reload(std::string_view id) {
    ModEntry* entry = find_entry(id);
    if (entry == nullptr) {
        return false;
    }
    if (entry->enabled) {
        unload_mod(*entry);  // dispose + SDL_UnloadObject
    }
    const bool ok = load_mod(*entry);  // re-reads the library from disk
    write_config(*entry);
    return ok;
}

const std::deque<ModEntry>& list() {
    return g_entries;
}

bool set_enabled(std::string_view id, bool enabled) {
    ModEntry* entry = find_entry(id);
    if (entry == nullptr) {
        return false;
    }
    if (entry->enabled == enabled) {
        return true;
    }

    bool ok = true;
    if (enabled) {
        ok = load_mod(*entry);
    } else {
        unload_mod(*entry);
    }
    write_config(*entry);
    return ok;
}

bool is_enabled(std::string_view id) {
    const ModEntry* entry = find_entry(id);
    return entry != nullptr && entry->enabled;
}

double get_setting(std::string_view id, std::string_view key) {
    const ModSetting* s = find_setting(find_entry(id), key);
    return s ? s->value : 0.0;
}

void set_setting(std::string_view id, std::string_view key, double value) {
    apply_setting(find_entry(id), key, value);
}

}  // namespace dusk::mods
