// src/ee/profile.cpp: a first start with no profile.
//
// The fault (their program, also without the pack): when the profiles folder holds no profile, the game writes
// lastuser = "player" and stops with "File not found - .../profiles/player/options (eiorepository.cpp, 196)". It
// needs profiles\<name>\options, keys and settings. With those three files it starts normally.
//
// Here, behind uiscale.ini [EE] MakeProfile=1 (default on): before the game reads its profiles, the profile that
// lastuser names (or "player" when there is no usable lastuser) gets whatever of the three files is missing, and
// lastuser is written when it is missing. A complete profile is never touched; a half one gets only its missing
// files. Every file made is logged; one summary line per start ("First-start profile made" / "Profile present").
//
// Timing: Soldiers.exe imports d3d9.dll directly (import table of 5291a092), so this dll's DllMain runs during the
// loader's start-up, before the program's own entry point: no game code has run yet. Only kernel32 file calls
// here (safe in DllMain); the folder comes from the LOCALAPPDATA variable, which is where the program's
// SHGetFolderPathA(CSIDL_LOCAL_APPDATA) points too (small.cpp profiles_dir).
//
// File contents:
//   keys     = the modern WASD layout (src/keys/presets/keys.modern, the bytes NewProfileKeys gives new profiles)
//   options  = the game's default template: the loose resource\interface\profiles\default\options when present,
//              else the template from resource.pak (embedded below, byte for byte). Buffers stays the game's "20"
//              here; small.cpp (SoundVoices) raises it to 64 right after, in the same start, only when its Audio
//              page fix is in place (a 64 without that fix crashes the Options page). Mode Auto "1" = use the
//              desktop resolution; maxFPS "0" = the V-Sync row of our Max FPS list (small.cpp: stored 0 = V-Sync).
//   settings = the loose resource\interface\profiles\default\settings when present, else resource.pak's.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <string>
#include "ee.h"
#include "../keys/modern_keys.inc"

namespace {

// resource.pak interface/profiles/default/options (640 bytes) and settings (55 bytes), R1.28.8
const char k_def_options[] =
    "{options\n"
    "\t{Video\n"
    "\t\t{Mode \"1024x768\"\n"
    "\t\t\t{Auto \"1\"}\n"
    "\t\t\t{Depth \"32\"}\n"
    "\t\t\t{refresh \"120\"}\n"
    "\t\t}\n"
    "\t\t{Textures\n"
    "\t\t\t{Auto \"0\"}\n"
    "\t\t\t{Depth \"32\"}\n"
    "\t\t\t{Mipmap \"3\"}\n"
    "\t\t\t{lod \"0\"}\n"
    "\t\t\t{compress \"0\"}\n"
    "\t\t}\n"
    "\t\t{shadows \"3\"}\n"
    "\t\t{gamma \"1\"}\n"
    "\t\t{maxFPS \"0\"}\n"
    "\t}\n"
    "\t{sound \n"
    "\t\t{buffers\t\"20\"\t}\n"
    "\t\t{rate\t\t\"44100\"\t}\n"
    "\t\t{bits\t\t\"16\"\t}\n"
    "\t\t{volume\n"
    "\t\t\t{effects \"100\"}\n"
    "\t\t\t{speak \"100\"}\n"
    "\t\t\t{environment \"100\"}\n"
    "\t\t\t{music \"100\"}\n"
    "\t\t\t{interface \"100\"}\n"
    "\t\t\t{gamemusic \"100\"}\n"
    "\t\t\t{narrator \"100\"}\n"
    "\t\t}\n"
    "\t}\n"
    "\t{game\n"
    "\t\t{backgroundScene \"1\"}\n"
    "\t\t{showIntro \"1\"}\n"
    "\t\t{autoAiming \"1\"}\n"
    "\t\t{invertMouseWheel \"0\"}\n"
    "\t\t{autoLoadLastProfile \"0\"}\n"
    "\t\t{slowDownWhileInventoryExchange \"0\"}\n"
    "\t\t{cameraScrollSpeed \"0.2\"}\n"
    "\t}\n"
    "}\n";
const char k_def_settings[] =
    "{settings\n"
    "\t{multiplayer \n"
    "\t\t{hostName \"soldiers\" }\n"
    "\t}\n"
    "}\n";

bool exists(const std::string &p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// size of a plain file, -1 when there is none (or it is a folder)
long long file_size(const std::string &p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

bool read_small(const std::string &path, std::string &out) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, NULL), got = 0;
    bool ok = size != INVALID_FILE_SIZE && size > 0 && size < (64u << 10);
    if (ok) { out.assign(size, '\0'); ok = ReadFile(f, &out[0], size, &got, NULL) && got == size; }
    CloseHandle(f);
    return ok;
}

// new file only: temp file, then a rename that never replaces anything
bool write_new(const std::string &path, const char *data, size_t n) {
    std::string tmp = path + ".ee-tmp";
    HANDLE o = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (o == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(o, data, (DWORD)n, &w, NULL) && w == n;
    CloseHandle(o);
    if (ok && MoveFileExA(tmp.c_str(), path.c_str(), 0)) return true;
    DeleteFileA(tmp.c_str());   // our own temp file only
    return false;
}

bool make_dir(const std::string &p) { return CreateDirectoryA(p.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS; }

// lastuser holds "name" (with the quotes, no line end, as the game writes it). Returns "" when unusable.
std::string parse_lastuser(const std::string &t) {
    size_t a = t.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b;
    if (t[a] == '"') { a++; b = t.find('"', a); if (b == std::string::npos) return std::string(); }
    else b = t.find_last_not_of(" \t\r\n") + 1;
    std::string n = t.substr(a, b - a);
    if (n.empty() || n == "." || n == ".." || n.size() > 64 || n.find_first_of("\\/:*?\"<>|\r\n\t") != std::string::npos)
        return std::string();
    return n;
}


// The HD textures as loose files (the Enhancement Pack's way since 1.2): a profile that still switches on the
// HD Project MOD (an earlier install put the mod into the AppData mods folder and ticked it) would load the same
// textures twice. With HideMods=1 (no MODS page to change it back) and the loose files present, the profile's
// line for that mod is taken out of its {mods} block before the game reads it; an empty {mods} block goes too.
// The old file is kept once as options.before_ee_hd; the mod folder itself is left alone (an unticked mod is not
// loaded). One log line per profile changed.
const char k_hd_mod_line[] = "{\"0xcb6014da|0x110000101c4b3f8\"}";
const char k_hd_loose_marker[] = "resource\\fx\\_bmp\\explosion\\woronka8.ebm";   // a file only the HD Project has

bool only_blank(const std::string &t, size_t a, size_t b, const char *blank) {
    for (size_t i = a; i < b; i++) if (!strchr(blank, t[i])) return false;
    return true;
}

// removes the whole line holding the mod's entry; false when there is none (or it is not on a line of its own)
bool remove_hd_tick(std::string &t) {
    size_t p = t.find(k_hd_mod_line);
    if (p == std::string::npos) return false;
    size_t a = t.rfind('\n', p);
    a = a == std::string::npos ? 0 : a + 1;
    size_t e = p + sizeof k_hd_mod_line - 1;
    size_t b = t.find('\n', e);
    b = b == std::string::npos ? t.size() : b + 1;
    if (!only_blank(t, a, p, " \t") || !only_blank(t, e, b, " \t\r\n")) return false;
    t.erase(a, b - a);
    size_t m = t.find("{mods");
    if (m != std::string::npos && m + 5 < t.size() && strchr(" \t\r\n", t[m + 5])) {
        size_t ma = t.rfind('\n', m);
        ma = ma == std::string::npos ? 0 : ma + 1;
        size_t q = t.find_first_not_of(" \t\r\n", m + 5);
        if (q != std::string::npos && t[q] == '}' && only_blank(t, ma, m, " \t")) {
            size_t qb = t.find('\n', q);
            qb = qb == std::string::npos ? t.size() : qb + 1;
            t.erase(ma, qb - ma);
        }
    }
    return true;
}

void drop_hd_mod_ticks(const std::string &base) {
    if (!ee_ini_int("HideMods", 1)) return;
    char pb[MAX_PATH];
    if (!exists(ee_beside_program(k_hd_loose_marker, pb))) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((base + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        std::string o = base + fd.cFileName + "\\options", t;
        if (!read_small(o, t) || !remove_hd_tick(t)) continue;
        std::string aside = o + ".before_ee_hd";
        if (!exists(aside) && !CopyFileA(o.c_str(), aside.c_str(), TRUE)) {
            ee_log("profile: '%s' switches the HD Project mod on, but its options COULD NOT be copied aside: left as it is",
                   fd.cFileName);
            continue;
        }
        std::string tmp = o + ".ee-tmp";
        HANDLE f = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD w = 0;
        bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, t.data(), (DWORD)t.size(), &w, NULL) && w == t.size();
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (ok && MoveFileExA(tmp.c_str(), o.c_str(), MOVEFILE_REPLACE_EXISTING))
            ee_log("profile: '%s': the HD Project mod is no longer switched on (its textures are loose files now, they "
                   "would load twice); old options kept as options.before_ee_hd", fd.cFileName);
        else {
            DeleteFileA(tmp.c_str());   // our own temp file only
            ee_log("profile: '%s': COULD NOT take the HD Project mod line out of options (error %lu)", fd.cFileName,
                   GetLastError());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

}   // namespace

extern "C" void ee_profile_init(void) {
    {
        char la0[MAX_PATH] = "";
        DWORD l0 = GetEnvironmentVariableA("LOCALAPPDATA", la0, sizeof la0);
        if (l0 && l0 < sizeof la0) drop_hd_mod_ticks(std::string(la0) + "\\Soldiers Heroes of World War 2\\profiles\\");
    }
    if (!ee_ini_int("MakeProfile", 1)) {
        ee_log("profile: off (MakeProfile=0): the game's own start (a first start with no profile stops with \"File not found\")");
        return;
    }
    char la[MAX_PATH] = "";
    DWORD ln = GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la);
    if (!ln || ln >= sizeof la) {
        ee_log("profile: COULD NOT check the profile: no LOCALAPPDATA in the environment");
        return;
    }
    std::string game = std::string(la) + "\\Soldiers Heroes of World War 2";
    std::string base = game + "\\profiles\\";

    // which profile the game will open: the one lastuser names, else "player" (what the game itself picks)
    std::string lu_path = base + "lastuser", lu_text, name;
    bool lu_there = file_size(lu_path) >= 0;
    if (lu_there && read_small(lu_path, lu_text)) name = parse_lastuser(lu_text);
    bool write_lu = name.empty();
    if (write_lu) name = "player";
    std::string dir = base + name + "\\";

    // template files beside the program (the game's own loose default profile, if the folder has one)
    char pb[MAX_PATH];
    std::string loose = ee_beside_program("resource\\interface\\profiles\\default\\", pb);

    struct F { const char *file; const char *data; size_t n; const char *src; };
    std::string lo, ls;
    F files[3] = {
        {"options", k_def_options, sizeof k_def_options - 1, "the game's default template (resource.pak)"},
        {"keys", k_modern_keys, sizeof k_modern_keys - 1, "the modern WASD layout"},
        {"settings", k_def_settings, sizeof k_def_settings - 1, "the game's default (resource.pak)"},
    };
    if (read_small(loose + "options", lo)) { files[0].data = lo.data(); files[0].n = lo.size(); files[0].src = "the game's loose default template"; }
    if (read_small(loose + "settings", ls)) { files[2].data = ls.data(); files[2].n = ls.size(); files[2].src = "the game's loose default"; }

    bool missing[3];
    int nmiss = 0;
    for (int i = 0; i < 3; i++) {
        missing[i] = file_size(dir + files[i].file) <= 0;   // none, a folder, or empty
        if (missing[i]) nmiss++;
    }
    if (!nmiss && !write_lu) {
        ee_log("profile: Profile present: '%s' (options, keys, settings found; nothing written)", name.c_str());
        return;
    }

    if (!make_dir(game) || !make_dir(game + "\\profiles") || !make_dir(base + name)) {
        ee_log("profile: COULD NOT make the folder %s (error %lu)", dir.c_str(), GetLastError());
        return;
    }
    int made = 0, failed = 0;
    for (int i = 0; i < 3; i++) {
        if (!missing[i]) continue;
        std::string p = dir + files[i].file;
        if (file_size(p) == 0) {   // an empty file: kept aside (never deleted), then made
            std::string aside = p + ".empty_before_ee";
            if (!MoveFileExA(p.c_str(), aside.c_str(), 0)) {
                ee_log("profile: COULD NOT move the empty %s aside: left as it is", p.c_str());
                failed++;
                continue;
            }
            ee_log("profile: empty %s kept as %s", p.c_str(), aside.c_str());
        } else if (exists(p)) {    // a folder of that name: not ours to move
            ee_log("profile: COULD NOT make %s: something else (a folder?) has that name", p.c_str());
            failed++;
            continue;
        }
        if (write_new(p, files[i].data, files[i].n)) {
            ee_log("profile: made %s (%u bytes, %s)", p.c_str(), (unsigned)files[i].n, files[i].src);
            made++;
        } else {
            ee_log("profile: COULD NOT write %s (error %lu)", p.c_str(), GetLastError());
            failed++;
        }
    }
    bool lu_made = false;
    if (write_lu) {
        if (lu_there) {   // there but unusable: kept aside
            std::string aside = lu_path + ".unusable_before_ee";
            if (MoveFileExA(lu_path.c_str(), aside.c_str(), 0)) {
                ee_log("profile: lastuser named no usable profile: kept as %s", aside.c_str());
            } else {
                ee_log("profile: COULD NOT move the unusable lastuser aside (error %lu): left as it is", GetLastError());
                write_lu = false;
                failed++;
            }
        }
    }
    if (write_lu) {
        const char lu[] = "\"player\"";
        lu_made = write_new(lu_path, lu, sizeof lu - 1);
        if (lu_made) ee_log("profile: made %s (\"player\")", lu_path.c_str());
        else { ee_log("profile: COULD NOT write %s (error %lu)", lu_path.c_str(), GetLastError()); failed++; }
    }
    ee_log("profile: First-start profile made: '%s' (%d of 3 files made%s%s)", name.c_str(), made,
           lu_made ? ", lastuser written" : "", failed ? "; SOME COULD NOT be written, see above" : "");
}
