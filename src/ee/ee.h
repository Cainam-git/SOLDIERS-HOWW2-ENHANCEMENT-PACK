// ee.h: shared helpers of the Enhanced Edition d3d9.dll proxy (src/ee/).
// Target: Soldiers.exe of the Steam "Enhanced Edition" (sha256 5291a092..., PE timestamp 0x68e61d55).
//
// Module entry points (called once from the proxy's DllMain, PROCESS_ATTACH, before the program's
// own code runs; only memory patches and import hooks there, no LoadLibrary, no threads that wait):
//   void ee_scale3_init();   // src/ee/scale3.cpp
//   void ee_small_init();    // src/ee/small.cpp
// build.sh compiles every src/ee/*.cpp; a missing module gets an empty stand-in in main.cpp.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes n bytes `put` at program address addr, but ONLY if the n bytes there equal `expect`
// and the running program is the known Enhanced Edition build. Logs the outcome either way
// (name = short label for the log). Returns true when the bytes are in place afterwards
// (also when `put` was already there: logged as "already"). Thread-safe.
bool ee_patch(uint32_t addr, const void *expect, const void *put, size_t n, const char *name);

// True when the running program is the known build (checked once: PE timestamp, image size,
// entry point and a few fixed bytes).
bool ee_program_known(void);

// Integer from uiscale.ini beside the program, section [EE]; def when the key is missing.
int ee_ini_int(const char *key, int def);
// String from uiscale.ini [EE] into buf (n chars, always terminated); def when the key is missing. Returns buf.
char *ee_ini_str(const char *key, const char *def, char *buf, size_t n);

// Full path of a file beside the program (buf of MAX_PATH chars); returns buf.
char *ee_beside_program(const char *name, char *buf);

// One line (newline added) to ee.log beside the program, with a time stamp. Only when uiscale.ini
// [EE] Log=1 or the environment has EE_LOG=1; otherwise does nothing. Thread-safe, flushed per line.
void ee_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool ee_log_on(void);

void ee_scale3_init(void);
void ee_small_init(void);
void ee_brand_init(void);    // src/ee/brand.cpp + splash4k.cpp: version line, 4K splash
void ee_menus_init(void);    // src/ee/menus.cpp: main menu without MULTI PLAYER / MODS
void ee_profile_init(void);  // src/ee/profile.cpp: a usable profile at the first start

#ifdef __cplusplus
}
#endif
