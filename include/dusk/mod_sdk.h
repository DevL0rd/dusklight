#ifndef DUSK_MOD_SDK_H
#define DUSK_MOD_SDK_H

/*
 * Dusklight native mod SDK (stable C ABI) -- the *fixed, generic* contract
 * between the game and a mod. It deliberately does NOT expose game features:
 * the executable exports its symbols, so a mod links against the game's own
 * headers and calls/hooks any game function directly. New modding capability
 * therefore never requires changing the game or this header.
 *
 * A mod is a self-describing folder under the "mods" directory:
 *
 *     mods/killable_npcs/
 *         mod.json            metadata (id, name, version, author, about, library)
 *         killable_npcs.so    library for this platform (.so/.dll/.dylib)
 *         settings.json       generated: the mod's settings, declared in code via
 *                             DuskHost::define_settings and written by the host
 *         config.json         generated: enabled state + saved setting values
 *
 * Only mod.json and the library ship with a mod; settings.json and config.json
 * are created at runtime. A mod is enabled by default until config.json says
 * otherwise.
 *
 * Each mod library exports two C-linkage entry points:
 *
 *   int  dusk_mod_init(const DuskHost* host, DuskMod* self);  // 0 == success
 *   void dusk_mod_dispose(void);                              // release all
 *
 * dusk_mod_init/dusk_mod_dispose run when the mod is enabled/disabled at
 * runtime. A mod installs its function hooks (detours) in init and removes them
 * in dispose; the library stays resident while disabled-then-re-enabled is just
 * re-hooking. dispose MUST remove every hook it installed.
 *
 * The host hands the mod only a tiny generic table: logging and access to the
 * mod's own settings (declared in settings.json, shown in the Mods menu, and
 * persisted to config.json). Everything else -- actors, effects, hooking -- the
 * mod does itself against the game headers.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump only if this generic host contract changes incompatibly. */
#define DUSK_MOD_ABI_VERSION 1u

/* Opaque per-mod handle, handed to dusk_mod_init and used for setting access. */
typedef struct DuskMod DuskMod;

typedef enum DuskSettingType {
    DUSK_SETTING_BOOL = 0,
    DUSK_SETTING_INT = 1,
    DUSK_SETTING_FLOAT = 2,
} DuskSettingType;

/* A user-configurable setting a mod declares at init (see define_settings).
 * Numeric fields are doubles; for DUSK_SETTING_BOOL a value is 0 or non-zero. */
typedef struct DuskSetting {
    const char* key;   /* stable id, unique within the mod */
    const char* label; /* display name */
    const char* help;  /* short description (may be NULL) */
    DuskSettingType type;
    double default_value;
    double min_value; /* INT/FLOAT */
    double max_value; /* INT/FLOAT */
    double step;      /* INT/FLOAT UI increment (0 -> sensible default) */
} DuskSetting;

/* The generic capability table. Valid until dusk_mod_dispose() returns. */
typedef struct DuskHost {
    uint32_t abi_version;

    /* Write a line to the Dusklight log (prefixed as a mod message). */
    void (*log)(const char* msg);

    /* Declare this mod's settings (typically once, in dusk_mod_init). The host
     * shows them in the Mods tab, persists their values, and writes the schema
     * to the mod's settings.json. Saved values from config.json are preserved. */
    void (*define_settings)(DuskMod* self, const DuskSetting* settings, uint32_t count);

    /* Read/update one of this mod's settings (keys from define_settings).
     * Values are doubles; a bool setting is 0 or non-zero. set persists. */
    double (*setting_get)(DuskMod* self, const char* key);
    void (*setting_set)(DuskMod* self, const char* key, double value);
} DuskHost;

/* Exported entry-point signatures (for the loader's symbol lookups). */
typedef int (*DuskModInitFn)(const DuskHost* host, DuskMod* self);
typedef void (*DuskModDisposeFn)(void);

#define DUSK_MOD_INIT_SYMBOL "dusk_mod_init"
#define DUSK_MOD_DISPOSE_SYMBOL "dusk_mod_dispose"

#ifdef __cplusplus
}
#endif

#endif /* DUSK_MOD_SDK_H */
