#ifndef DUSK_MODS_H
#define DUSK_MODS_H

#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "dusk/mod_sdk.h"

/*
 * Native mod loader (host side).
 *
 * Each mod is a self-describing folder under "mods" (see mod_sdk.h): a mod.json
 * manifest declares its metadata and configurable settings, an optional
 * config.json holds its enabled state and saved setting values, and a per-
 * platform shared library provides its behavior. The loader discovers mods by
 * reading manifests (no code runs until a mod is enabled), then loads/unloads
 * libraries on demand. All persisted state lives in the mod's own config.json.
 *
 * The game side is intentionally generic: it discovers, loads/unloads, and
 * persists mods, but exposes no game features. Mods reach into the game directly
 * (the executable exports its symbols; mods link the game headers and install
 * their own function hooks), so new modding capability never touches the game.
 */
namespace dusk::mods {

enum class SettingType { Bool, Int, Float };

// A single configurable setting plus its current value (drives the UI control).
struct ModSetting {
    std::string key;
    std::string label;
    std::string help;
    SettingType type = SettingType::Bool;
    double defaultValue = 0.0;
    double minValue = 0.0;
    double maxValue = 0.0;
    double step = 0.0;
    double value = 0.0;  // current value
};

// Metadata + runtime state for one discovered mod (drives the Mods UI tab).
struct ModEntry {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string about;
    std::string folder;       // the mod's folder (where config.json lives)
    std::string libraryPath;  // platform shared library (empty if none present)
    bool enabled = false;
    std::vector<ModSetting> settings;
};

// Scan the mods folder(s), read manifests, and (re-)enable mods whose config
// says so. Call once at startup, after the data folder is known.
void initialize();

// Dispose and unload every loaded mod. Call at shutdown.
void shutdown();

// Re-scan the mods folder(s); any newly discovered mod is enabled and loaded.
// Already-known mods are left untouched. Safe to call at runtime.
void refresh();

// Disable+unload then load a mod again (re-reads its library from disk). Lets a
// rebuilt mod be tested without restarting the game. Returns true on success.
bool reload(std::string_view id);

// All discovered mods, in discovery order. Stored in a deque so element
// addresses stay stable as mods are discovered at runtime (loaded mods hold a
// pointer to their entry).
const std::deque<ModEntry>& list();

// Toggle a mod on/off by id: loads+inits or disposes+unloads, then persists to
// the mod's config.json.
bool set_enabled(std::string_view id, bool enabled);
bool is_enabled(std::string_view id);

// Read/update a mod setting value (used by the UI). Persists on change.
double get_setting(std::string_view id, std::string_view key);
void set_setting(std::string_view id, std::string_view key, double value);

}  // namespace dusk::mods

#endif  // DUSK_MODS_H
