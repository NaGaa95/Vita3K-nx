/* Vita3K launcher -- SDL2 cover-art frontend for the Vita3K Switch port.
 * Scans the installed Vita games (ux0/app/<TITLEID>), shows a cover grid, edits
 * Vita3K config, and chainloads the emulator (Vita3K.nro) with "-r <TITLEID>".
 */
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <curl/curl.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <map>
#include <unordered_map>
#include <deque>
#include <iterator>
#include <array>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <condition_variable>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <unistd.h>

#include "archivescan.h"
#include "compatdb.h"
#include "griddb.h"
#include "tinyxml2.h"
#include "forwarder.h"
#include "firmware.h"
#include "modules.h"
#include "launcher_update.h"
#include "localization.h"
#include "storage.h"
#include "ui_audio.h"

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------
// Switch face buttons vs SDL's Xbox naming: Nintendo A=SDL B, B=SDL A, X=SDL Y.
#define BTN_CONFIRM  SDL_CONTROLLER_BUTTON_B  // Nintendo A
#define BTN_CANCEL   SDL_CONTROLLER_BUTTON_A  // Nintendo B
#define BTN_SETTINGS SDL_CONTROLLER_BUTTON_Y  // Nintendo X

// All under sdmc:/switch/vita3k/ (the emulator hardcodes the same dir). On the
// Switch's case-insensitive FS "vita3k" and "Vita3K" are the same folder.
static const char *DATA_DIR    = "sdmc:/switch/vita3k";
static const char *LAUNCHER_INI= "sdmc:/switch/vita3k/launcher.ini"; // launcher's own prefs (sort, griddb key)
static const char *COVERS_DIR  = "sdmc:/switch/vita3k/covers";
static const char *GAMECFG_DIR = "sdmc:/switch/vita3k/gamecfg";      // launcher per-game overrides
static const char *APP_DIR     = "sdmc:/switch/vita3k/vita/ux0/app"; // installed Vita apps (one folder per TITLEID)
static const char *MODULES_DIR = "sdmc:/switch/vita3k/vita/vs0/sys/external"; // decrypted firmware modules the LLE picker lists
static const char *USER_DIR    = "sdmc:/switch/vita3k/vita/ux0/user";        // one directory per Vita user (savedata + trophies)
static const char *CONFIG_YML  = "sdmc:/switch/vita3k/config.yml";   // emulator config, written by the launcher on launch
static const char *CACHE_DIR   = "sdmc:/switch/vita3k/cache";
static const char *INSTALL_DIR = "sdmc:/switch/vita3k/install";
static const char *LSFG_DIR    = "sdmc:/switch/vita3k/lsfg";
static const char *LSFG_DLL_FILE = "sdmc:/switch/vita3k/lsfg/Lossless.dll";
static const char *EMU_HOST_DIR= "sdmc:/switch/vita3k/.emu";
static const char *EMU_NRO     = "sdmc:/switch/vita3k/.emu/Vita3K.nro"; // extracted emulator core, chainloaded with "-r <TITLEID>"
static const char *EMU_NRO_SRC = "romfs:/emu/Vita3K.nro";           // emulator bundled inside THIS .nro (self-contained build)
static const char *EMU_HASH_SRC= "romfs:/emu/Vita3K.sha256";        // build-time identity of the bundled emulator
static const char *INSTALL_TARGET = "sdmc:/switch/vita3k/install_target.txt"; // SD browser writes the picked .pkg path here; emulator reads it on --install
static const char *INSTALL_ZRIF = "sdmc:/switch/vita3k/install_zrif.txt";     // optional typed license key for that package; the installer consumes it once
static const char *FRONTEND_REQUEST = "sdmc:/switch/vita3k/.frontend_request"; // one-shot request surviving a clean Application restart

// ---------------------------------------------------------------------------
// flat "Section/Key = value" ini store (global launcher.ini + per-game override)
// ---------------------------------------------------------------------------
struct KV { std::string k, v; };
struct Store {
  std::vector<KV> kv;
  mutable std::unordered_map<std::string,size_t> index;
  mutable size_t indexedSize=SIZE_MAX;
};

static Store g_global;   // launcher.ini
static Store g_game;     // gamecfg/<key>.ini (per-game overrides), or empty
static Store g_titles;   // titles.ini: game key -> user-renamed display title
static Store *g_active = &g_global;
static const char *TITLES_INI = "sdmc:/switch/vita3k/titles.ini";

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Consume the request left by Vita3K before appletRestartProgram(). The restart
// intentionally discards the old process/argv, so this is the only state carried
// into the pristine launcher process. Always remove a file we opened: malformed
// or stale requests must never become a permanent startup action.
struct FrontendRequest {
  bool present = false;
};

static FrontendRequest consumeFrontendRequest() {
  FrontendRequest request;
  FILE *f = fopen(FRONTEND_REQUEST, "r");
  if (!f) return request;
  char magic[64] = {0}, action[32] = {0}, title[64] = {0};
  bool ok = fgets(magic,sizeof(magic),f) && fgets(action,sizeof(action),f) && fgets(title,sizeof(title),f);
  fclose(f);
  remove(FRONTEND_REQUEST);
  if (!ok || trim(magic)!="VITA3K_FRONTEND_REQUEST_V1") return request;
  if (trim(action)!="launcher") return request;
  request.present=true;
  return request;
}
static void ensureStoreIndex(const Store &s){
  if(s.indexedSize==s.kv.size())return;
  s.index.clear();s.index.reserve(s.kv.size());
  for(size_t item=0;item<s.kv.size();item++)s.index[s.kv[item].k]=item;
  s.indexedSize=s.kv.size();
}
static void invalidateStoreIndex(Store &s){s.index.clear();s.indexedSize=SIZE_MAX;}
static const char *storeGet(Store &s, const char *key, const char *def) {
  ensureStoreIndex(s);const auto found=s.index.find(key);
  return found==s.index.end()?def:s.kv[found->second].v.c_str();
}
static void storeSet(Store &s, const char *key, const char *val) {
  ensureStoreIndex(s);const auto found=s.index.find(key);
  if(found!=s.index.end()){s.kv[found->second].v=val;return;}
  s.kv.push_back({key,val});s.index[s.kv.back().k]=s.kv.size()-1;s.indexedSize=s.kv.size();
}
static void storeRemove(Store &s, const char *key) {
  ensureStoreIndex(s);const auto found=s.index.find(key);if(found==s.index.end())return;
  s.kv.erase(s.kv.begin()+found->second);invalidateStoreIndex(s);
}
static bool storeHas(const Store &s, const char *key) {
  ensureStoreIndex(s);return s.index.find(key)!=s.index.end();
}
static void storeRemovePrefix(Store &s,const char *prefix){
  const size_t length=strlen(prefix);
  s.kv.erase(std::remove_if(s.kv.begin(),s.kv.end(),[&](const KV &entry){
    return entry.k.compare(0,length,prefix)==0;
  }),s.kv.end());
  invalidateStoreIndex(s);
}
static bool recoverAtomicFile(const std::string &path);
static void storeLoad(Store &s, const char *path) {
  s.kv.clear();
  invalidateStoreIndex(s);
  if (!recoverAtomicFile(path)) return;
  FILE *f = fopen(path, "r");
  if (!f) return;
  // Whole-line reads: the module picker stores a comma-joined list that can run
  // past any fixed buffer, and a split line would corrupt the store.
  std::string line;
  for (;;) {
    const int c = fgetc(f);
    if (c != EOF && c != '\n') {
      if (line.size() < 64u * 1024) line.push_back(static_cast<char>(c));
      continue;
    }
    const std::string t = trim(line);
    line.clear();
    if (!t.empty() && t[0] != '#' && t[0] != ';' && t[0] != '[') {
      const size_t eq = t.find('=');
      if (eq != std::string::npos) {
        std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
        if (!k.empty()) s.kv.push_back({ k, v });
      }
    }
    if (c == EOF) break;
  }
  fclose(f);
}

static bool queryRegularFile(const std::string &path, bool &exists) {
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    exists = true;
    return S_ISREG(st.st_mode);
  }
  exists = false;
  return errno == ENOENT;
}

static bool regularFileExists(const std::string &path) {
  bool exists=false;
  return queryRegularFile(path,exists)&&exists;
}

// How many CPU cores this process may actually use. The NPDM of whatever
// launched us decides it: a shortcut built by the installer grants all four,
// while the album-applet path grants three and leaves core 3 unused.
static int allowedCpuCores() {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
    return 3;
  int cores = 0;
  for (int core = 0; core < 4; core++)
    if (mask & (UINT64_C(1) << core)) cores++;
  return cores ? cores : 3;
}

static bool lsfgDllInstalled() {
  return regularFileExists(LSFG_DLL_FILE);
}

static bool recoverAtomicFile(const std::string &path) {
  const std::string tmp = path + ".tmp";
  const std::string old = path + ".old";
  bool currentExists = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, currentExists) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists)) return false;
  if (!currentExists && oldExists) {
    if (rename(old.c_str(), path.c_str()) != 0) return false;
    fsdevCommitDevice("sdmc");
    currentExists = true;
    oldExists = false;
  }
  if (tmpExists && remove(tmp.c_str()) != 0) return false;
  if (currentExists && oldExists && remove(old.c_str()) != 0) return false;
  if (tmpExists || oldExists) fsdevCommitDevice("sdmc");
  return true;
}

static bool replaceAtomic(const std::string &path, const std::string &tmp) {
  const std::string old = path + ".old";
  bool hadCurrent = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, hadCurrent) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists) || !tmpExists) return false;
  if (oldExists && remove(old.c_str()) != 0) return false;
  if (hadCurrent && rename(path.c_str(), old.c_str()) != 0) return false;
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    if (hadCurrent) {
      rename(old.c_str(), path.c_str());
      fsdevCommitDevice("sdmc");
    }
    return false;
  }
  fsdevCommitDevice("sdmc");
  if (hadCurrent && remove(old.c_str()) == 0) fsdevCommitDevice("sdmc");
  return true;
}

static bool writeAtomicText(const std::string &path, const std::string &text) {
  const std::string tmp = path + ".tmp";
  if (!recoverAtomicFile(path)) return false;
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  if (fflush(f) != 0 || fsync(fileno(f)) != 0) ok = false;
  if (fclose(f) != 0) ok = false;
  if (!ok) { remove(tmp.c_str()); return false; }
  if (!replaceAtomic(path, tmp)) { remove(tmp.c_str()); return false; }
  return true;
}

static bool storeSave(Store &s, const char *path) {
  mkdir(DATA_DIR, 0777);
  std::string text = "# Vita3K launcher\n";
  for (auto &e : s.kv) text += e.k + " = " + e.v + "\n";
  return writeAtomicText(path, text);
}

// active-store accessors: per-game keys fall back to the global value.
static const char *iniGet(const char *key, const char *def) {
  if (g_active == &g_game) {
    for (auto &e : g_game.kv)   if (e.k == key) return e.v.c_str();
    for (auto &e : g_global.kv) if (e.k == key) return e.v.c_str();
    return def;
  }
  return storeGet(*g_active, key, def);
}
static void iniSet(const char *key, const char *val) { storeSet(*g_active, key, val); }

// ---------------------------------------------------------------------------
// settings model (data-driven; one renderer drives every settings screen)
// ---------------------------------------------------------------------------
enum OType { OT_CHOICE, OT_RANGE, OT_SUBMENU, OT_TEXT, OT_STATUS, OT_MULTI };
struct Choice { const char *label, *val; };
struct Opt {
  const char *label;
  const char *key;
  OType type;
  const Choice *ch; int nch;
  int lo, hi, step;
  const char *def;
  int sub;      // OT_SUBMENU target screen
  const char *gateKey;  // if set, this row is greyed + non-adjustable while
  const char *gateOff;  // iniGet(gateKey) == gateOff (e.g. its parent toggle is off)
};
#define O_CHOICE(l,k,c,d)      { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, nullptr, nullptr }
#define O_RANGE(l,k,lo,hi,s,d) { l, k, OT_RANGE,  nullptr,0, lo,hi,s, d, 0, nullptr, nullptr }
#define O_SUB(l,scr)           { l, nullptr, OT_SUBMENU, nullptr,0, 0,0,0, nullptr, scr, nullptr, nullptr }
#define O_CHOICEG(l,k,c,d,gk,go) { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, gk, go }
#define O_RANGEG(l,k,lo,hi,s,d,gk,go) { l, k, OT_RANGE, nullptr,0, lo,hi,s, d, 0, gk, go }
#define O_TEXT(l,k,d)          { l, k, OT_TEXT, nullptr,0, 0,0,0, d, 0, nullptr, nullptr }
#define O_TEXTG(l,k,d,gk,go)   { l, k, OT_TEXT, nullptr,0, 0,0,0, d, 0, gk, go }
#define O_STATUS(l,k)           { l, k, OT_STATUS, nullptr,0, 0,0,0, nullptr, 0, nullptr, nullptr }
#define O_MULTIG(l,k,d,gk,go)   { l, k, OT_MULTI, nullptr,0, 0,0,0, d, 0, gk, go }

// Vita3K settings. Values are the literal strings written into config.yml
// (see vita3k/config/include/config/config.h). Bools are yaml "true"/"false".
static const Choice C_bool[]     = { {"Off","false"}, {"On","true"} };
static const Choice C_boolint[]  = { {"Off","0"}, {"On","1"} };   // int-typed on/off (e.g. psn-signed-in)
static const Choice C_backend[]  = { {"Vulkan (NVK)","Vulkan"}, {"OpenGL (NVC0)","OpenGL"},
                                     {"Zink (OpenGL on NVK)","Zink"} };
static const Choice C_resmult[]  = { {"0.5x","0.5"}, {"1.0x","1.0"}, {"1.5x","1.5"}, {"2.0x","2.0"}, {"3.0x","3.0"} };
static const Choice C_modules[]  = { {"Automatic","0"}, {"Auto + manual","1"}, {"Manual","2"} }; // matches ModulesMode enum
static const Choice C_filter[]   = { {"Nearest","Nearest"}, {"Bilinear","Bilinear"}, {"Bicubic","Bicubic"}, {"FXAA","FXAA"}, {"FSR","FSR"} }; // Vulkan superset; GL exposes Bilinear/FXAA below
static const Choice C_fsrSharpness[] = {
  {"0.0","0.0"}, {"0.1","0.1"}, {"0.2","0.2"}, {"0.3","0.3"}, {"0.4","0.4"},
  {"0.5","0.5"}, {"0.6","0.6"}, {"0.7","0.7"}, {"0.8","0.8"}, {"0.9","0.9"},
  {"1.0","1.0"}, {"1.1","1.1"}, {"1.2","1.2"}, {"1.3","1.3"}, {"1.4","1.4"},
  {"1.5","1.5"}, {"1.6","1.6"}, {"1.7","1.7"}, {"1.8","1.8"}, {"1.9","1.9"}, {"2.0","2.0"},
};
static const Choice C_aniso[]    = { {"Off","1"}, {"2x","2"}, {"4x","4"}, {"8x","8"}, {"16x","16"} };
// GPU memory mapping (config key values from renderer.cpp); unmappable ranges silently fall back from External host to Double buffer.
static const Choice C_memmap[]   = { {"External host","external-host"}, {"Double buffer","double-buffer"}, {"Disabled","disabled"} };
static const Choice C_analog[]   = { {"0.5x","0.5"}, {"1.0x","1.0"}, {"1.5x","1.5"}, {"2.0x","2.0"} };
static const Choice C_reartouch[]= { {"Off","off"}, {"ZL + touchscreen","zl"}, {"ZR + touchscreen","zr"}, {"Front and rear together","both"} };
static const Choice C_vitaface[]= { {"Cross","cross"}, {"Circle","circle"}, {"Triangle","triangle"}, {"Square","square"} };
static const Choice C_sysbtn[]   = { {"Circle","0"}, {"Cross","1"} };  // SCE_SYSTEM_PARAM_ENTER_BUTTON_*
static const Choice C_datefmt[]  = { {"YYYY/MM/DD","0"}, {"DD/MM/YYYY","1"}, {"MM/DD/YYYY","2"} }; // SCE_SYSTEM_PARAM_DATE_FORMAT_*
static const Choice C_timefmt[]  = { {"12-hour","0"}, {"24-hour","1"} };  // SCE_SYSTEM_PARAM_TIME_FORMAT_*
static const Choice C_launcherTheme[] = { {"XMB (PS3)","xmb"}, {"Glow","animated"},
                                          {"Bubbles","homebrew"}, {"Classic","classic"}, {"OLED black","oled"} };
static const Choice C_launcherRotation[] = { {"0 degrees","0"}, {"90 degrees","1"},
                                             {"180 degrees","2"}, {"270 degrees","3"} };
static const Choice C_launcherLanguage[] = { {"System","system"}, {"English","en"},
  {"Français","fr"}, {"Deutsch","de"}, {"Español","es"}, {"Italiano","it"},
  {"Português","pt"} };
static const Choice C_gridColumns[] = { {"3","3"}, {"4","4"}, {"5","5"}, {"6","6"}, {"7","7"}, {"8","8"} };
static const Choice C_gridRows[] = { {"1","1"}, {"2","2"}, {"3","3"} };
static const Choice C_lsfgFlow[] = { {"Quarter","0.25"}, {"Half","0.5"} };
// SCE_SYSTEM_PARAM_LANG_* (0-indexed, matches vita3k util/system.h).
static const Choice C_syslang[]  = {
  {"Japanese","0"}, {"English (US)","1"}, {"French","2"}, {"Spanish","3"}, {"German","4"},
  {"Italian","5"}, {"Dutch","6"}, {"Portuguese","7"}, {"Russian","8"}, {"Korean","9"},
  {"Chinese (Trad.)","10"}, {"Chinese (Simp.)","11"}, {"Finnish","12"}, {"Swedish","13"},
  {"Danish","14"}, {"Norwegian","15"}, {"Polish","16"}, {"Portuguese (BR)","17"},
  {"English (UK)","18"}, {"Turkish","19"},
};

// PerformanceOverlayDetail / PerformanceOverlayPosition (vita3k config.h enums).
static const Choice C_perfdetail[] = {
  {"Minimum","0"}, {"Low","1"}, {"Medium","2"}, {"Maximum","3"},
};
static const Choice C_perfpos[] = {
  {"Top left","0"}, {"Top center","1"}, {"Top right","2"},
  {"Bottom left","3"}, {"Bottom center","4"}, {"Bottom right","5"},
};

enum { SCR_FRAMEGEN, SCR_EMULATION, SCR_GRAPHICS, SCR_AUDIO, SCR_SYSTEM, SCR_NETWORK, SCR_CONTROLLER, SCR_COUNT };

static const Opt S_framegen[] = {
  O_CHOICE("LSFG 2x (Vulkan only)", "switch-lsfg-enabled", C_bool, "false"),
  O_CHOICEG("Flow resolution", "switch-lsfg-flow-scale", C_lsfgFlow,
            "0.25", "switch-lsfg-enabled", "false"),
  O_CHOICEG("Performance mode", "switch-lsfg-performance", C_bool,
            "true", "switch-lsfg-enabled", "false"),
  O_STATUS("Lossless.dll", "lsfg-dll"),
};

// Emulation: module loading plus CPU execution (-> config.yml). The module list
// is only consulted when the mode is not Automatic, so the picker is gated on it.
static const Opt S_emulation[] = {
  O_CHOICE("Modules mode",      "modules-mode",  C_modules, "0"),
  O_MULTIG("Manual module list", "lle-modules",  "", "modules-mode", "0"),
  O_CHOICE("CPU optimisations", "cpu-opt",       C_bool,    "true"),
  O_STATUS("Allowed CPU cores", "cpu-cores"),
  O_RANGE("File loading delay (ms)", "file-loading-delay", 0, 30, 1, "0"),
};
// GPU / Graphics (-> config.yml). All three renderers live in one unified NRO.
static const Opt S_graphics[] = {
  O_CHOICE("Renderer",                "backend-renderer",            C_backend, "Vulkan"),
  O_CHOICE("Resolution scale",        "resolution-multiplier",      C_resmult, "1.0"),
  O_CHOICE("Memory mapping",          "memory-mapping",             C_memmap,  "external-host"),
  O_CHOICE("High accuracy",           "high-accuracy",              C_bool,    "false"),
  O_CHOICE("Full-precision shaders",  "force-full-precision",       C_bool,    "false"),
  O_CHOICE("Screen filter",           "screen-filter",              C_filter,  "Bilinear"),
  O_CHOICE("FSR sharpness",           "fsr-sharpness",              C_fsrSharpness, "0.2"),
  O_CHOICE("Stretch to screen",       "stretch_the_display_area",   C_bool,    "false"),
  O_CHOICE("VSync",                   "v-sync",                     C_bool,    "true"),
  O_CHOICE("Anisotropic filtering",   "anisotropic-filtering",      C_aniso,   "1"),
  O_CHOICE("Disable surface sync",    "disable-surface-sync",       C_bool,    "true"),
  O_CHOICE("Texture cache",           "texture-cache",              C_bool,    "true"),
  O_CHOICE("Async pipeline compile",  "async-pipeline-compilation", C_bool,    "true"),
  O_CHOICE("Show compiling shaders",  "show-compile-shaders",       C_bool,    "true"),
  O_CHOICE("Shader cache",            "shader-cache",               C_bool,    "true"),
  O_CHOICE("Direct SPIR-V",           "spirv-shader",               C_bool,    "false"),
  O_CHOICE("Import textures",         "import-textures",            C_bool,    "false"),
  O_CHOICE("Export textures",         "export-textures",            C_bool,    "false"),
  O_CHOICE("Export as PNG",           "export-as-png",              C_bool,    "true"),
  O_CHOICE("FPS hack",                "fps-hack",                   C_bool,    "false"),
  O_CHOICE("Performance overlay",     "performance-overlay",          C_bool,       "false"),
  O_CHOICE("Overlay detail",          "performance-overlay-detail",   C_perfdetail, "0"),
  O_CHOICE("Overlay position",        "performance-overlay-position", C_perfpos,    "0"),
};
// Audio (-> config.yml).
static const Opt S_audio[] = {
  O_RANGE ("Audio volume",      "audio-volume",  0, 100, 5, "100"),
  O_CHOICE("NGS audio support", "ngs-enable",    C_bool,    "true"),
};
// System (-> config.yml).
static const Opt S_system[] = {
  O_CHOICE("System language",   "sys-lang",        C_syslang, "1"),
  O_CHOICE("Enter button",      "sys-button",      C_sysbtn,  "1"),
  O_CHOICE("Date format",       "sys-date-format", C_datefmt, "2"),
  O_CHOICE("Time format",       "sys-time-format", C_timefmt, "0"),
  O_CHOICE("PS TV mode",        "pstv-mode",       C_bool,    "false"),
};
// Network (-> config.yml).
static const Opt S_network[] = {
  O_CHOICE("HTTP enable",              "http-enable",            C_bool,      "true"),
  O_RANGE ("HTTP timeout attempts",    "http-timeout-attempts",  0, 200,  5,  "50"),
  O_RANGE ("HTTP timeout sleep (ms)",  "http-timeout-sleep-ms",  0, 1000, 50, "100"),
  O_RANGE ("HTTP read-end attempts",   "http-read-end-attempts", 0, 100,  5,  "10"),
  O_RANGE ("HTTP read-end sleep (ms)", "http-read-end-sleep-ms", 0, 2000, 50, "250"),
  O_CHOICE("PSN signed in",            "psn-signed-in",          C_boolint,   "0"),
  O_RANGE ("Ad-hoc address index",     "adhoc-addr",             0, 16,   1,  "0"),
};
// Controller (-> config.yml). The direct libnx input path consumes these Switch
// settings; SDL controller bind arrays remain unused on Horizon.
static const Opt S_controller[] = {
  O_CHOICE("Disable motion",      "disable-motion",               C_bool,      "false"),
  O_CHOICE("Stick sensitivity",   "controller-analog-multiplier", C_analog,    "1.0"),
  O_RANGE ("Stick deadzone (%)",  "switch-stick-deadzone",        0, 40, 2,    "15"),
  O_CHOICE("Rear touch modifier", "switch-rear-touch",            C_reartouch, "zl"),
  O_CHOICE("Rear touch buttons",  "switch-rear-touch-triggers",   C_bool,      "true"),
  O_CHOICE("Switch A maps to",    "switch-button-a",              C_vitaface,  "circle"),
  O_CHOICE("Switch B maps to",    "switch-button-b",              C_vitaface,  "cross"),
  O_CHOICE("Switch X maps to",    "switch-button-x",              C_vitaface,  "triangle"),
  O_CHOICE("Switch Y maps to",    "switch-button-y",              C_vitaface,  "square"),
};
// Launcher-only preferences. These keys never enter Vita3K's config.yml.
static const Opt S_launcher[] = {
  O_CHOICE("Language",          "Wrapper/Language",       C_launcherLanguage, "system"),
  O_CHOICE("Theme",             "Wrapper/Theme",          C_launcherTheme, "xmb"),
  O_CHOICE("Launcher rotation", "Wrapper/LauncherRotation", C_launcherRotation, "0"),
  O_CHOICE("Games per row",     "Wrapper/GridColumns",    C_gridColumns,   "5"),
  O_CHOICE("Rows per page",     "Wrapper/GridRows",       C_gridRows,      "2"),
  O_CHOICE("Show game titles",  "Wrapper/ShowGameTitles", C_bool,          "true"),
  O_CHOICE("Show region flags", "Wrapper/ShowRegionFlags", C_bool,         "true"),
  O_CHOICE("Show custom settings badges", "Wrapper/ShowCustomSettingsBadges", C_bool, "true"),
  O_CHOICE("Show compatibility badges", "Wrapper/ShowCompatBadges", C_bool, "true"),
  O_CHOICE("UI animations",     "Wrapper/UiAnimations",   C_bool,          "true"),
  O_CHOICE("Sound effects",     "Wrapper/UiSounds",       C_bool,          "true"),
  O_CHOICE("Check updates at boot", "Wrapper/CheckUpdatesOnStartup", C_bool, "true"),
  O_TEXT("SteamGridDB API key", "Wrapper/SteamGridDBKey", ""),
};
struct Screen { const char *title; const Opt *opts; int n; bool binds; };
static const Screen g_screens[SCR_COUNT] = {
  { "Frame Generation",  S_framegen,   (int)(sizeof(S_framegen)/sizeof(Opt)),   false },
  { "Emulation",         S_emulation,  (int)(sizeof(S_emulation)/sizeof(Opt)),  false },
  { "GPU / Graphics",    S_graphics,   (int)(sizeof(S_graphics)/sizeof(Opt)),   false },
  { "Audio",             S_audio,      (int)(sizeof(S_audio)/sizeof(Opt)),      false },
  { "System",            S_system,     (int)(sizeof(S_system)/sizeof(Opt)),     false },
  { "Network",           S_network,    (int)(sizeof(S_network)/sizeof(Opt)),    false },
  { "Controller",        S_controller, (int)(sizeof(S_controller)/sizeof(Opt)), false },
};

// commit every managed option of the active store (so what was shown persists)
static void commitAll() {
  for (int s = 0; s < SCR_COUNT; s++)
    for (int i = 0; i < g_screens[s].n; i++) {
      const Opt &o = g_screens[s].opts[i];
      if (o.key && (o.type == OT_CHOICE || o.type == OT_RANGE || o.type == OT_TEXT || o.type == OT_MULTI)) {
        std::string v = iniGet(o.key, o.def);
        iniSet(o.key, v.c_str());
      }
    }
}

// Effective launch settings: per-game override > global > default.
static std::vector<KV> buildEffectiveSettings(const std::string &gameKey) {
  Store perGame;
  if (!gameKey.empty())
    storeLoad(perGame, (std::string(GAMECFG_DIR) + "/" + gameKey + ".ini").c_str());
  std::vector<KV> out;
  for (int s = 0; s < SCR_COUNT; s++)
    for (int i = 0; i < g_screens[s].n; i++) {
      const Opt &o = g_screens[s].opts[i];
      if (!o.key) continue;
      const char *v = nullptr;
      for (auto &e : perGame.kv)  if (e.k == o.key) { v = e.v.c_str(); break; }
      if (!v) for (auto &e : g_global.kv) if (e.k == o.key) { v = e.v.c_str(); break; }
      out.push_back({ o.key, v ? v : (o.def ? o.def : "") });
    }
  const auto backend = std::find_if(out.begin(), out.end(), [](const KV &e){ return e.k == "backend-renderer"; });
  if (backend != out.end() && backend->v != "Vulkan") {
    const auto lsfg = std::find_if(out.begin(), out.end(), [](const KV &e){ return e.k == "switch-lsfg-enabled"; });
    if (lsfg != out.end()) lsfg->v = "false";
    const auto filter = std::find_if(out.begin(), out.end(), [](const KV &e){ return e.k == "screen-filter"; });
    if (filter != out.end() && filter->v != "Bilinear" && filter->v != "FXAA")
      filter->v = "Bilinear";
  }
  if (backend == out.end() || backend->v != "Zink") {
    const auto spirv = std::find_if(out.begin(), out.end(), [](const KV &e){ return e.k == "spirv-shader"; });
    if (spirv != out.end()) spirv->v = "false";
  }
  return out;
}

static std::vector<std::string> splitCsv(const std::string &value);

// Write sdmc:/switch/vita3k/config.yml (flat "key: value" YAML) from the effective
// settings. Vita3K fills any unspecified option from its own defaults on load.
static bool writeConfigYml(const std::vector<KV> &eff) {
  std::string text = "# Written by the Vita3K launcher\n";
  std::string modules;
  for (auto &e : eff) {
    if (e.k == "lle-modules") { modules = e.v; continue; }
    text += e.k + ": " + e.v + "\n";
  }
  // lle-modules has to be a real YAML sequence: yaml-cpp throws on a scalar and
  // Vita3K then discards the entire config file rather than just this key.
  text += "lle-modules: [";
  bool firstModule = true;
  for (const std::string &name : splitCsv(modules)) {
    if (!firstModule) text += ", ";
    text += "\"" + name + "\"";
    firstModule = false;
  }
  text += "]\n";
  // Vector-typed keys the launcher doesn't edit -> empty YAML lists, so the
  // emulator keeps its default controller mapping.
  text += "controller-binds: []\ncontroller-axis-binds: []\n";
  // The emulator picks a user of its own when none is selected, but the launcher
  // rewrites config.yml on every launch, so an explicit choice has to be restated.
  // Emitted only when set: a bare "user-id:" parses as YAML null, and as<std::string>
  // then throws, which makes Vita3K discard the whole config file rather than one key.
  const std::string selectedUser = storeGet(g_global, "Wrapper/UserId", "");
  if (!selectedUser.empty())
    text += "user-id: " + selectedUser + "\n";
  // Release builds keep the SD card clean: no log file at all.
  text += "log-level: 5\nfile-logging: false\n";
  return writeAtomicText(CONFIG_YML, text);
}

// ---------------------------------------------------------------------------
// SDL / assets
// ---------------------------------------------------------------------------
static SDL_Window   *g_win = nullptr;
static SDL_Renderer *g_ren = nullptr;
static TTF_Font     *g_font = nullptr, *g_font_sm = nullptr, *g_font_big = nullptr;
static SDL_Texture  *g_logo = nullptr;
static int SW = 1280, SH = 720;
static int g_outputW = 1280, g_outputH = 720;
static int g_launcherRotation = 0;
static bool g_launcherPortrait = false;
static SDL_Texture *g_uiTarget = nullptr;
static bool g_romfsReady = false;
static bool g_sdlReady = false;
static bool g_ttfReady = false;
static bool g_imgReady = false;
static bool g_plReady = false;
static bool g_networkReady = false;
static bool g_griddbReady = false;
static std::atomic_bool g_usbReady{false};
static std::string g_usbError;
static std::atomic_bool g_storageWorkerComplete{true};
static std::atomic_bool g_storageWorkerCancel{false};
static std::thread g_storageWorker;
static std::string g_launcherNroPath;
static bool g_directForwarderBoot=false;
static std::string g_updateNoticeTag;
static std::string g_updateNotifiedTag;
static Uint32 g_updateNoticeUntil=0;
static bool g_updateInstallExitRequested=false;
static std::string g_toastMessage;
static Uint32 g_toastUntil=0;
static void drawToastOverlay();

static bool configureLauncherOrientation(int rotation) {
  if(!g_ren || g_outputW<1 || g_outputH<1) return false;
  if(rotation<0||rotation>3) rotation=0;
  if(rotation==0){
    SDL_SetRenderTarget(g_ren,nullptr);
    if(g_uiTarget) SDL_DestroyTexture(g_uiTarget);
    g_uiTarget=nullptr;
    g_launcherRotation=0;
    g_launcherPortrait=false;
    SW=g_outputW;
    SH=g_outputH;
    SDL_RenderSetViewport(g_ren,nullptr);
    SDL_RenderSetScale(g_ren,1.0f,1.0f);
    return true;
  }
  const bool portrait=(rotation&1)!=0;
  const int logicalWidth=portrait?g_outputH:g_outputW;
  const int logicalHeight=portrait?g_outputW:g_outputH;
  if(g_uiTarget && SW==logicalWidth && SH==logicalHeight){
    g_launcherRotation=rotation;
    g_launcherPortrait=portrait;
    SDL_SetRenderTarget(g_ren,g_uiTarget);
    return true;
  }
  SDL_Texture *previous=g_uiTarget;
  SDL_SetRenderTarget(g_ren,nullptr);
  SDL_Texture *target=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET,logicalWidth,logicalHeight);
  if(!target){ if(previous) SDL_SetRenderTarget(g_ren,previous); return false; }
  SDL_SetTextureBlendMode(target,SDL_BLENDMODE_NONE);
  if(SDL_SetRenderTarget(g_ren,target)!=0){
    SDL_DestroyTexture(target);
    if(previous) SDL_SetRenderTarget(g_ren,previous);
    return false;
  }
  g_uiTarget=target;
  g_launcherRotation=rotation;
  g_launcherPortrait=portrait;
  SW=logicalWidth;
  SH=logicalHeight;
  if(previous) SDL_DestroyTexture(previous);
  SDL_RenderSetViewport(g_ren,nullptr);
  SDL_RenderSetScale(g_ren,1.0f,1.0f);
  return true;
}

static void presentUi() {
  drawToastOverlay();
  if(!g_uiTarget){ SDL_RenderPresent(g_ren); return; }
  SDL_RenderSetClipRect(g_ren,nullptr);
  SDL_SetRenderTarget(g_ren,nullptr);
  SDL_SetRenderDrawColor(g_ren,0,0,0,255);
  SDL_RenderClear(g_ren);
  SDL_Rect destination=(g_launcherRotation&1)
      ? SDL_Rect{(g_outputW-g_outputH)/2,(g_outputH-g_outputW)/2,g_outputH,g_outputW}
      : SDL_Rect{0,0,g_outputW,g_outputH};
  SDL_RenderCopyEx(g_ren,g_uiTarget,nullptr,&destination,g_launcherRotation*90.0,nullptr,SDL_FLIP_NONE);
  SDL_RenderPresent(g_ren);
  SDL_SetRenderTarget(g_ren,g_uiTarget);
}

enum class LauncherTheme { Bubbles, Glow, Xmb, Classic, Oled };
static LauncherTheme g_launcherTheme = LauncherTheme::Glow;
static bool g_uiAnimations = true;
static bool g_showGameTitles = true;
static bool g_showRegionFlags = true;
static bool g_showCustomSettingsBadges = true;
static bool g_showCompatBadges = true;
static int g_gridColumns = 5;
static int g_gridRows = 2;
static SDL_Texture *g_glowTexture = nullptr;

static SDL_Color COL_BG    = { 8, 12, 24, 255 };
static SDL_Color COL_TXT   = { 235, 239, 247, 255 };
static SDL_Color COL_DIM   = { 151, 163, 184, 255 };
static SDL_Color COL_HI    = { 100, 211, 255, 255 };
static SDL_Color COL_VAL   = { 255, 215, 120, 255 };
static SDL_Color COL_SEL   = { 116, 200, 255, 255 };
static SDL_Color COL_PANEL = { 16, 23, 39, 184 };
static SDL_Color COL_CARD  = { 22, 30, 49, 214 };
static SDL_Color COL_FOCUS = { 28, 69, 92, 210 };

static void fillRect(int x,int y,int w,int h, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); SDL_Rect r={x,y,w,h}; SDL_RenderFillRect(g_ren,&r); }
static void border(int x,int y,int w,int h,int t, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); for(int i=0;i<t;i++){ SDL_Rect r={x-i,y-i,w+2*i,h+2*i}; SDL_RenderDrawRect(g_ren,&r); } }

struct TextKey {
  TTF_Font *font;
  Uint32 color;
  std::string text;
  bool operator==(const TextKey &other) const {
    return font == other.font && color == other.color && text == other.text;
  }
};

struct TextKeyHash {
  size_t operator()(const TextKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<Uint32>{}(key.color) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct TextEntry {
  SDL_Texture *texture;
  int width;
  int height;
  size_t bytes;
  Uint64 use;
};

struct MetricKey {
  TTF_Font *font;
  std::string text;
  bool operator==(const MetricKey &other) const { return font == other.font && text == other.text; }
};

struct MetricKeyHash {
  size_t operator()(const MetricKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    return hash ^ (std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2));
  }
};

struct MetricEntry { int width; Uint64 use; };

struct EllipsisKey {
  TTF_Font *font;
  int maxWidth;
  std::string text;
  bool operator==(const EllipsisKey &other) const {
    return font == other.font && maxWidth == other.maxWidth && text == other.text;
  }
};

struct EllipsisKeyHash {
  size_t operator()(const EllipsisKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<int>{}(key.maxWidth) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct EllipsisEntry { std::string text; Uint64 use; };

static std::unordered_map<TextKey, TextEntry, TextKeyHash> g_textCache;
static std::unordered_map<MetricKey, MetricEntry, MetricKeyHash> g_metricCache;
static std::unordered_map<EllipsisKey, EllipsisEntry, EllipsisKeyHash> g_ellipsisCache;
static size_t g_textCacheBytes = 0;
static Uint64 g_textUseSerial = 0;
static constexpr size_t TEXT_CACHE_LIMIT = 512;
static constexpr size_t TEXT_CACHE_BYTES = 12 * 1024 * 1024;
static constexpr size_t METRIC_CACHE_LIMIT = 2048;
static constexpr size_t ELLIPSIS_CACHE_LIMIT = 512;

static Uint32 packColor(SDL_Color color) {
  return (Uint32)color.r | ((Uint32)color.g << 8) | ((Uint32)color.b << 16) | ((Uint32)color.a << 24);
}

static void rememberTextMetric(TTF_Font *font, const std::string &text, int width) {
  MetricKey key{font, text};
  auto found = g_metricCache.find(key);
  if (found != g_metricCache.end()) {
    found->second.width = width;
    found->second.use = ++g_textUseSerial;
    return;
  }
  if (g_metricCache.size() >= METRIC_CACHE_LIMIT) {
    auto victim = g_metricCache.begin();
    for (auto it = std::next(g_metricCache.begin()); it != g_metricCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    g_metricCache.erase(victim);
  }
  g_metricCache.emplace(std::move(key), MetricEntry{width, ++g_textUseSerial});
}

static void evictTextEntries(size_t incomingBytes) {
  while (!g_textCache.empty() &&
         (g_textCache.size() >= TEXT_CACHE_LIMIT || g_textCacheBytes > TEXT_CACHE_BYTES - incomingBytes)) {
    auto victim = g_textCache.begin();
    for (auto it = std::next(g_textCache.begin()); it != g_textCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    SDL_DestroyTexture(victim->second.texture);
    g_textCacheBytes -= victim->second.bytes;
    g_textCache.erase(victim);
  }
}

static void clearTextCaches() {
  for (auto &entry : g_textCache) SDL_DestroyTexture(entry.second.texture);
  g_textCache.clear();
  g_metricCache.clear();
  g_ellipsisCache.clear();
  g_textCacheBytes = 0;
  g_textUseSerial = 0;
}

static void applyLauncherAppearance() {
  LauncherTheme previous = g_launcherTheme;
  const char *theme = storeGet(g_global, "Wrapper/Theme", "xmb");
  g_launcherTheme = !strcmp(theme, "classic") ? LauncherTheme::Classic :
                    !strcmp(theme, "oled") ? LauncherTheme::Oled :
                    !strcmp(theme, "homebrew") ? LauncherTheme::Bubbles :
                    !strcmp(theme, "xmb") ? LauncherTheme::Xmb : LauncherTheme::Glow;
  g_uiAnimations = strcmp(storeGet(g_global, "Wrapper/UiAnimations", "true"), "false") != 0;
  g_showGameTitles = strcmp(storeGet(g_global, "Wrapper/ShowGameTitles", "true"), "false") != 0;
  g_showRegionFlags = strcmp(storeGet(g_global, "Wrapper/ShowRegionFlags", "true"), "false") != 0;
  g_showCustomSettingsBadges =
      strcmp(storeGet(g_global, "Wrapper/ShowCustomSettingsBadges", "true"), "false") != 0;
  g_showCompatBadges = strcmp(storeGet(g_global, "Wrapper/ShowCompatBadges", "true"), "false") != 0;
  g_gridColumns = std::max(3, std::min(8, atoi(storeGet(g_global, "Wrapper/GridColumns", "5"))));
  g_gridRows = std::max(1, std::min(3, atoi(storeGet(g_global, "Wrapper/GridRows", "2"))));
  int requestedRotation=atoi(storeGet(g_global,"Wrapper/LauncherRotation","0"));
  if(requestedRotation<0||requestedRotation>3) requestedRotation=0;
  if(g_ren && requestedRotation!=g_launcherRotation && !configureLauncherOrientation(requestedRotation)){
    storeSet(g_global,"Wrapper/LauncherRotation","0");
    configureLauncherOrientation(0);
  }

  if (g_launcherTheme == LauncherTheme::Classic) {
    COL_BG={22,24,30,255}; COL_TXT={228,230,235,255}; COL_DIM={150,155,165,255};
    COL_HI={96,200,255,255}; COL_VAL={255,210,100,255}; COL_SEL={255,170,0,255};
    COL_PANEL={28,31,40,255}; COL_CARD={24,26,34,255}; COL_FOCUS={66,56,30,235};
  } else if (g_launcherTheme == LauncherTheme::Oled) {
    COL_BG={0,0,0,255}; COL_TXT={245,247,249,255}; COL_DIM={145,151,158,255};
    COL_HI={105,220,255,255}; COL_VAL={255,255,255,255}; COL_SEL={0,210,190,255};
    COL_PANEL={4,4,5,248}; COL_CARD={8,8,10,250}; COL_FOCUS={0,58,53,245};
  } else if (g_launcherTheme == LauncherTheme::Bubbles) {
    COL_BG={0,8,16,255}; COL_TXT={235,248,255,255}; COL_DIM={143,192,216,255};
    COL_HI={118,222,255,255}; COL_VAL={194,239,255,255}; COL_SEL={61,183,235,255};
    COL_PANEL={4,31,50,190}; COL_CARD={5,35,56,218}; COL_FOCUS={12,76,108,220};
  } else if (g_launcherTheme == LauncherTheme::Glow) {
    COL_BG={8,12,24,255}; COL_TXT={235,239,247,255}; COL_DIM={151,163,184,255};
    COL_HI={100,211,255,255}; COL_VAL={255,215,120,255}; COL_SEL={116,200,255,255};
    COL_PANEL={16,23,39,184}; COL_CARD={22,30,49,214}; COL_FOCUS={28,69,92,208};
  } else {
    COL_BG={3,37,102,255}; COL_TXT={239,248,255,255}; COL_DIM={164,205,234,255};
    COL_HI={137,225,255,255}; COL_VAL={245,252,255,255}; COL_SEL={89,194,247,255};
    COL_PANEL={2,32,86,184}; COL_CARD={4,44,102,218}; COL_FOCUS={15,91,151,220};
  }
  if (previous != g_launcherTheme && g_ren)
    clearTextCaches();
}

static void ensureGlowTexture() {
  if (g_glowTexture || !g_ren) return;
  constexpr int size=256;
  SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,size,size,32,SDL_PIXELFORMAT_RGBA32);
  if(!surface) return;
  if(SDL_LockSurface(surface)==0){
    for(int y=0;y<size;y++){
      auto *row=(Uint32*)((Uint8*)surface->pixels+y*surface->pitch);
      for(int x=0;x<size;x++){
        float dx=(x-(size-1)*0.5f)/(size*0.5f),dy=(y-(size-1)*0.5f)/(size*0.5f);
        float distance=sqrtf(dx*dx+dy*dy);
        float strength=distance>=1.f?0.f:1.f-distance;
        Uint8 alpha=(Uint8)(255.f*strength*strength);
        row[x]=SDL_MapRGBA(surface->format,255,255,255,alpha);
      }
    }
    SDL_UnlockSurface(surface);
    g_glowTexture=SDL_CreateTextureFromSurface(g_ren,surface);
    if(g_glowTexture) SDL_SetTextureBlendMode(g_glowTexture,SDL_BLENDMODE_BLEND);
  }
  SDL_FreeSurface(surface);
}

static bool hasAnimatedBackground() {
  return g_launcherTheme==LauncherTheme::Bubbles||g_launcherTheme==LauncherTheme::Glow||
         g_launcherTheme==LauncherTheme::Xmb;
}

static void drawGlow(float x,float y,float radius,Uint8 red,Uint8 green,Uint8 blue,Uint8 alpha) {
  int diameter=(int)(SH*radius);
  SDL_Rect destination={(int)(SW*x)-diameter/2,(int)(SH*y)-diameter/2,diameter,diameter};
  SDL_SetTextureColorMod(g_glowTexture,red,green,blue);
  SDL_SetTextureAlphaMod(g_glowTexture,alpha);
  SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&destination);
}

static void drawBackgroundParticles(float time,SDL_Color color,int count,float speed) {
  for(int i=0;i<count;i++){
    float travel=fmodf(i*0.371f+time*speed*(0.65f+(i%5)*0.11f),1.12f)-0.06f;
    float y=fmodf(i*0.217f+0.11f*sinf(time*0.29f+i*1.73f),1.f);
    float pulse=0.45f+0.55f*sinf(time*(0.9f+(i%4)*0.17f)+i);
    Uint8 alpha=(Uint8)(color.a*(0.55f+0.45f*pulse));
    int size=(i%9==0)?3:2;
    fillRect((int)(travel*SW),(int)(y*SH),size,size,(SDL_Color){color.r,color.g,color.b,alpha});
  }
}

static Uint8 blendChannel(Uint8 first,Uint8 second,float amount) {
  return (Uint8)(first+(second-first)*std::clamp(amount,0.f,1.f));
}

static float xmbWaveY(float x,float time,float center,float amplitude,float frequency,float slope,float phase) {
  const float primary=sinf(x*6.2831853f*frequency+phase+time*0.115f);
  const float detail=sinf(x*6.2831853f*(frequency*2.07f)+phase*0.61f-time*0.072f);
  return center+slope*(x-0.5f)+amplitude*(primary+detail*0.24f);
}

static void drawXmbRibbon(float time,float center,float amplitude,float frequency,float slope,float phase,
                          int halfWidth,SDL_Color color) {
  constexpr int pointCount=121;
  std::array<SDL_Point,pointCount> points{};
  for(int offset=-halfWidth;offset<=halfWidth;offset++){
    float distance=halfWidth?fabsf((float)offset/halfWidth):0.f;
    Uint8 alpha=(Uint8)(color.a*powf(std::max(0.f,1.f-distance),1.45f));
    if(alpha<2) continue;
    for(int point=0;point<pointCount;point++){
      float x=(float)point/(pointCount-1);
      points[point]={(int)(x*SW),(int)(xmbWaveY(x,time,center,amplitude,frequency,slope,phase)*SH)+offset};
    }
    SDL_SetRenderDrawColor(g_ren,color.r,color.g,color.b,alpha);
    SDL_RenderDrawLines(g_ren,points.data(),pointCount);
  }
}

static void drawXmbFilament(float time,float center,float amplitude,float frequency,float slope,float phase,
                            SDL_Color color) {
  constexpr int pointCount=161;
  std::array<SDL_Point,pointCount> points{};
  for(int point=0;point<pointCount;point++){
    float x=(float)point/(pointCount-1);
    points[point]={(int)(x*SW),(int)(xmbWaveY(x,time,center,amplitude,frequency,slope,phase)*SH)};
  }
  SDL_SetRenderDrawColor(g_ren,color.r,color.g,color.b,color.a);
  SDL_RenderDrawLines(g_ren,points.data(),pointCount);
}

static void drawXmbSparkles(float time) {
  for(int index=0;index<42;index++){
    float x=fmodf(index*0.618034f+time*(0.0022f+(index%5)*0.00045f),1.08f)-0.04f;
    float y=xmbWaveY(x,time,0.585f,0.095f,0.91f,0.075f,0.4f)+
            (fmodf(index*0.413f,1.f)-0.5f)*0.31f;
    float pulse=0.5f+0.5f*sinf(time*(0.55f+(index%7)*0.08f)+index*1.731f);
    Uint8 alpha=(Uint8)(28.f+pulse*(index%9==0?142.f:82.f));
    int px=(int)(x*SW),py=(int)(y*SH);
    fillRect(px,py,index%9==0?3:2,index%9==0?3:2,(SDL_Color){220,246,255,alpha});
    if(index%9==0&&pulse>0.55f){
      SDL_SetRenderDrawColor(g_ren,235,251,255,(Uint8)(alpha*0.62f));
      SDL_RenderDrawLine(g_ren,px-5,py+1,px+7,py+1);
      SDL_RenderDrawLine(g_ren,px+1,py-5,px+1,py+7);
    }
  }
}

static void drawXmbBackground(float time) {
  const SDL_Color top={3,37,102,255},middle={8,93,184,255},bottom={0,20,68,255};
  constexpr int bands=72;
  for(int band=0;band<bands;band++){
    float y=(band+0.5f)/bands;
    SDL_Color color{};
    if(y<0.52f){
      float amount=y/0.52f;
      color={blendChannel(top.r,middle.r,amount),blendChannel(top.g,middle.g,amount),blendChannel(top.b,middle.b,amount),255};
    } else {
      float amount=(y-0.52f)/0.48f;
      color={blendChannel(middle.r,bottom.r,amount),blendChannel(middle.g,bottom.g,amount),blendChannel(middle.b,bottom.b,amount),255};
    }
    int y0=band*SH/bands,y1=(band+1)*SH/bands;
    fillRect(0,y0,SW,y1-y0,color);
  }
  if(g_glowTexture){
    drawGlow(0.10f,0.43f,1.18f,55,157,255,54);
    drawGlow(0.84f,0.38f,0.92f,41,112,228,42);
  }
  drawXmbRibbon(time,0.655f,0.082f,0.78f,-0.105f,2.15f,std::max(12,SH/18),(SDL_Color){63,166,255,31});
  drawXmbRibbon(time,0.575f,0.074f,0.96f,0.080f,0.35f,std::max(10,SH/25),(SDL_Color){189,235,255,48});
  drawXmbRibbon(time,0.605f,0.049f,1.28f,-0.025f,3.82f,std::max(5,SH/54),(SDL_Color){230,250,255,72});
  for(int trace=0;trace<9;trace++){
    float offset=(trace-4)*0.009f;
    drawXmbFilament(time,0.588f+offset,0.083f+trace*0.0017f,0.91f,0.052f,
                    0.62f+trace*0.19f,(SDL_Color){202,241,255,(Uint8)(18+trace%3*8)});
  }
  drawXmbFilament(time,0.578f,0.073f,0.96f,0.080f,0.35f,(SDL_Color){243,253,255,136});
  drawXmbSparkles(time);
}

static void drawBubble(int centerX,int centerY,int radius,Uint8 alpha) {
  if(radius<3||alpha==0) return;
  if(g_glowTexture){
    SDL_SetTextureColorMod(g_glowTexture,90,205,255);
    SDL_SetTextureAlphaMod(g_glowTexture,(Uint8)(alpha/5));
    SDL_Rect glow={centerX-radius*2,centerY-radius*2,radius*4,radius*4};
    SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&glow);
  }
  const int segments=24;
  SDL_SetRenderDrawColor(g_ren,124,220,255,alpha);
  std::array<SDL_Point,segments+1> outer{},inner{};
  for(int segment=0;segment<=segments;segment++){
    float angle=segment*6.2831853f/segments;
    float x=cosf(angle),y=sinf(angle);
    outer[segment]={centerX+(int)(x*radius),centerY+(int)(y*radius)};
    inner[segment]={centerX+(int)(x*(radius-1)),centerY+(int)(y*(radius-1))};
  }
  SDL_RenderDrawLines(g_ren,outer.data(),(int)outer.size());
  SDL_RenderDrawLines(g_ren,inner.data(),(int)inner.size());
  SDL_SetRenderDrawColor(g_ren,235,252,255,(Uint8)std::min(255,(int)alpha+55));
  std::array<SDL_Point,6> highlight{};
  for(int segment=0;segment<(int)highlight.size();segment++){
    float angle=3.55f+segment*0.13f;
    highlight[segment]={centerX+(int)(cosf(angle)*radius),centerY+(int)(sinf(angle)*radius)};
  }
  SDL_RenderDrawLines(g_ren,highlight.data(),(int)highlight.size());
}

static void drawBubblesBackground(float time) {
  const SDL_Color top={20,126,169,255},middle={4,54,82,255},bottom={0,5,11,255};
  constexpr int bands=56;
  for(int band=0;band<bands;band++){
    float y=(band+0.5f)/bands;
    SDL_Color color{};
    if(y<0.58f){
      float amount=y/0.58f;
      color={blendChannel(top.r,middle.r,amount),blendChannel(top.g,middle.g,amount),blendChannel(top.b,middle.b,amount),255};
    } else {
      float amount=(y-0.58f)/0.42f;
      color={blendChannel(middle.r,bottom.r,amount),blendChannel(middle.g,bottom.g,amount),blendChannel(middle.b,bottom.b,amount),255};
    }
    int y0=band*SH/bands,y1=(band+1)*SH/bands;
    fillRect(0,y0,SW,y1-y0,color);
  }

  if(g_glowTexture){
    SDL_SetTextureColorMod(g_glowTexture,118,225,255);
    SDL_SetTextureAlphaMod(g_glowTexture,105);
    SDL_Rect surface={-SW/6,-SH/3,SW*4/3,SH*2/3};
    SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&surface);
    for(int ray=0;ray<7;ray++){
      float sway=sinf(time*(0.10f+ray*0.013f)+ray*1.31f);
      int width=SW*(11+(ray%3)*3)/100;
      int x=SW*(8+ray*14)/100+(int)(sway*SW*0.025f)-width/2;
      SDL_Rect shaft={x,-SH/3,width,SH*4/3};
      SDL_SetTextureAlphaMod(g_glowTexture,(Uint8)(23+(ray%3)*7));
      SDL_RenderCopyEx(g_ren,g_glowTexture,nullptr,&shaft,-9.0+ray*2.7+sway*2.0,nullptr,SDL_FLIP_NONE);
    }
  }

  for(int index=0;index<18;index++){
    float progress=fmodf(index*0.173f+time*(0.038f+(index%5)*0.007f),1.18f);
    float y=1.08f-progress;
    float x=0.05f+fmodf(index*0.283f,0.90f)+0.032f*sinf(time*(0.31f+(index%4)*0.04f)+index);
    float fade=std::min(std::clamp((1.10f-y)*5.f,0.f,1.f),std::clamp((y+0.12f)*6.f,0.f,1.f));
    int radius=(int)(SH*(0.009f+(index%6)*0.0042f));
    if(index%11==0) radius=radius*3/2;
    drawBubble((int)(x*SW),(int)(y*SH),radius,(Uint8)(fade*(85+(index%4)*24)));
  }
  drawBackgroundParticles(time,(SDL_Color){164,228,255,62},24,0.008f);
}

static void clearUiBackground() {
  SDL_RenderSetClipRect(g_ren,nullptr);
  SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255);
  SDL_RenderClear(g_ren);
  if(!hasAnimatedBackground()) return;
  ensureGlowTexture();
  float time=g_uiAnimations?SDL_GetTicks()/1000.f:0.f;
  if(g_launcherTheme==LauncherTheme::Xmb){
    drawXmbBackground(time);
    if(g_glowTexture){ SDL_SetTextureColorMod(g_glowTexture,255,255,255); SDL_SetTextureAlphaMod(g_glowTexture,255); }
    return;
  }
  if(g_launcherTheme==LauncherTheme::Bubbles){
    drawBubblesBackground(time);
    if(g_glowTexture){ SDL_SetTextureColorMod(g_glowTexture,255,255,255); SDL_SetTextureAlphaMod(g_glowTexture,255); }
    return;
  }
  if(!g_glowTexture) return;
  drawGlow(0.10f+0.13f*sinf(time*0.43f),0.20f+0.11f*cosf(time*0.37f),0.90f,45,140,255,128);
  drawGlow(0.84f+0.12f*cosf(time*0.34f),0.34f+0.10f*sinf(time*0.41f),0.78f,154,75,255,112);
  drawGlow(0.54f+0.10f*sinf(time*0.29f),0.91f+0.06f*cosf(time*0.33f),0.94f,0,210,190,94);
  drawGlow(0.42f+0.08f*cosf(time*0.25f),0.48f+0.09f*sinf(time*0.31f),0.58f,64,125,255,67);
  drawBackgroundParticles(time,(SDL_Color){182,224,255,88},28,0.011f);
  SDL_SetTextureColorMod(g_glowTexture,255,255,255);
  SDL_SetTextureAlphaMod(g_glowTexture,255);
}

static void glassPanel(int x,int y,int width,int height) {
  fillRect(x,y,width,height,COL_PANEL);
  border(x,y,width,height,1,(SDL_Color){255,255,255,(Uint8)(hasAnimatedBackground()?28:16)});
}

static void drawText(TTF_Font*f,int x,int y,const char*s,SDL_Color c){
  if(!f||!s||!*s) return;
  TextKey key{f,packColor(c),s};
  auto found=g_textCache.find(key);
  if(found!=g_textCache.end()){
    found->second.use=++g_textUseSerial;
    SDL_Rect d={x,y,found->second.width,found->second.height};
    SDL_RenderCopy(g_ren,found->second.texture,nullptr,&d);
    return;
  }
  SDL_Surface*sf=TTF_RenderUTF8_Blended(f,s,c); if(!sf) return;
  SDL_Texture*t=SDL_CreateTextureFromSurface(g_ren,sf);
  int w=sf->w,h=sf->h; SDL_FreeSurface(sf);
  if(!t) return;
  rememberTextMetric(f,s,w);
  const size_t bytes=(size_t)w*(size_t)h*4;
  if(bytes<=TEXT_CACHE_BYTES){
    evictTextEntries(bytes);
    TextEntry entry{t,w,h,bytes,++g_textUseSerial};
    auto inserted=g_textCache.emplace(std::move(key),entry);
    g_textCacheBytes+=bytes;
    SDL_Rect d={x,y,w,h}; SDL_RenderCopy(g_ren,inserted.first->second.texture,nullptr,&d);
  } else {
    SDL_Rect d={x,y,w,h}; SDL_RenderCopy(g_ren,t,nullptr,&d); SDL_DestroyTexture(t);
  }
}
static int textW(TTF_Font*f,const char*s){
  if(!f||!s||!*s) return 0;
  MetricKey key{f,s}; auto found=g_metricCache.find(key);
  if(found!=g_metricCache.end()){ found->second.use=++g_textUseSerial; return found->second.width; }
  int w=0,h=0; if(TTF_SizeUTF8(f,s,&w,&h)!=0) return 0;
  rememberTextMetric(f,s,w); return w;
}

static const std::string &ellipsizedText(TTF_Font *font, const std::string &text, int maxWidth) {
  EllipsisKey key{font,maxWidth,text};
  auto found=g_ellipsisCache.find(key);
  if(found!=g_ellipsisCache.end()){ found->second.use=++g_textUseSerial; return found->second.text; }

  std::vector<size_t> boundaries{0};
  for(size_t i=0;i<text.size();){
    const unsigned char lead=(unsigned char)text[i];
    size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
    if(i+length>text.size()) length=1;
    for(size_t j=1;j<length;j++) if(((unsigned char)text[i+j]&0xc0)!=0x80){ length=1; break; }
    i+=length; boundaries.push_back(i);
  }
  size_t low=0,high=boundaries.size()-1;
  while(low<high){
    size_t middle=(low+high+1)/2;
    std::string candidate=text.substr(0,boundaries[middle])+"...";
    if(textW(font,candidate.c_str())<=maxWidth) low=middle; else high=middle-1;
  }
  std::string shortened=text.substr(0,boundaries[low])+"...";
  if(g_ellipsisCache.size()>=ELLIPSIS_CACHE_LIMIT){
    auto victim=g_ellipsisCache.begin();
    for(auto it=std::next(g_ellipsisCache.begin());it!=g_ellipsisCache.end();++it)
      if(it->second.use<victim->second.use) victim=it;
    g_ellipsisCache.erase(victim);
  }
  auto inserted=g_ellipsisCache.emplace(std::move(key),EllipsisEntry{std::move(shortened),++g_textUseSerial});
  return inserted.first->second.text;
}
static std::string fittedText(TTF_Font *font,const std::string &text,int maxWidth){
  if(maxWidth<=0) return {};
  if(textW(font,text.c_str())<=maxWidth) return text;
  return ellipsizedText(font,text,maxWidth);
}
static void drawTextR(TTF_Font*f,int xr,int y,const char*s,SDL_Color c){ drawText(f,xr-textW(f,s),y,s,c); }
static void drawTextC(TTF_Font*f,int cx,int y,const char*s,SDL_Color c){ drawText(f,cx-textW(f,s)/2,y,s,c); }

static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col);
static void downloadAllCovers();
static void downloadCompatibilityDatabase();
static void rmrf(const std::string &path);
static bool ensureDirectory(const char *path);
static void toast(const char *msg);
static void toastStatic(const char *msg);
static void modalMessage(const char *title, const std::vector<std::string> &lines);
static void modalMessageStatic(const char *title,std::initializer_list<const char*> lines);
static bool confirmBox(const char *title, const std::vector<std::string> &lines);
static bool confirmBoxStatic(const char *title,std::initializer_list<const char*> lines);
static std::string uiText(const char *text);
static int dropdown(const char *title, const char *const *labels, int n, int cur,
                    bool localizeTitle=true,bool localizeChoices=true);
static bool promptText(const char *header, const char *initial, char *out, size_t outSize);
static bool promptTextAdvanced(const char *header,const char *initial,char *out,size_t outSize,
                               bool password,bool allowEmpty);
static void beginScreenFx();
static void drawFadeIn();
static int topBarH();
static void drawHeader(const char *title,const char *ctx);
static void drawLocalizedHeader(const char *title,const char *ctx);
static void drawScrollTextR(TTF_Font *font,int xRight,int y,int maxWidth,const char *text,SDL_Color color);
static void drawScrollTextL(TTF_Font *font,int x,int y,int maxWidth,const char *text,SDL_Color color);
static void listCol(int *colX,int *colW,int *labelX,int *valX);
static void firmwareSetupFlow();
static void drawSetupProgress(int pct,const char *msg,const char *detail=nullptr,bool cancellable=false);
static void runUpdateScreen();
static std::string installedReleaseTag();
static std::string launcherUpdateStatusText();
static void pollUpdateNotification();
static void drawUpdateNotification();
static SDL_Texture *g_flag[4] = { nullptr, nullptr, nullptr, nullptr }; // [1]=US [2]=EU [3]=JP
static void fillCircle(int cx,int cy,int r,SDL_Color c){
  SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a);
  for(int dy=-r;dy<=r;dy++){ int dx=(int)(sqrt((double)(r*r-dy*dy))+0.5); SDL_RenderDrawLine(g_ren,cx-dx,cy+dy,cx+dx,cy+dy); }
}
static SDL_Texture *makeFlagTex(int region,int W,int H){
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_SetRenderTarget(g_ren,t);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  if(region==3){                                   // Japan: white field + red disc
    fillRect(0,0,W,H,(SDL_Color){245,245,245,255});
    fillCircle(W/2,H/2,H*30/100,(SDL_Color){188,0,45,255});
  } else if(region==1){                            // USA: stripes + blue canton
    for(int i=0;i<7;i++) fillRect(0,i*H/7,W,H/7+1,(i%2)?(SDL_Color){235,235,235,255}:(SDL_Color){178,34,52,255});
    fillRect(0,0,W*2/5,(H*4)/7,(SDL_Color){45,50,110,255});
    for(int ry=0;ry<2;ry++)for(int cc=0;cc<3;cc++) fillRect(5+cc*(W*2/5-8)/3,4+ry*8,2,2,(SDL_Color){255,255,255,255});
  } else if(region==2){                            // Europe: blue + ring of stars
    fillRect(0,0,W,H,(SDL_Color){0,51,153,255});
    for(int i=0;i<12;i++){ double a=i*6.28318/12.0; int sx=W/2+(int)(cos(a)*W*0.30), sy=H/2+(int)(sin(a)*H*0.32);
      fillRect(sx-1,sy-1,2,2,(SDL_Color){255,204,0,255}); }
  }
  SDL_SetRenderTarget(g_ren,nullptr);
  return t;
}
static void makeFlags(){ g_flag[1]=makeFlagTex(1,36,24); g_flag[2]=makeFlagTex(2,36,24); g_flag[3]=makeFlagTex(3,36,24); }

// --- Nintendo-style button glyphs (drawn once): face buttons are circles, shoulders pills.
static SDL_Texture *g_gA=nullptr,*g_gB=nullptr,*g_gX=nullptr,*g_gY=nullptr,
                   *g_gPlus=nullptr,*g_gMinus=nullptr,*g_gLeft=nullptr,*g_gRight=nullptr,
                   *g_gL=nullptr,*g_gR=nullptr;
// Supersample: render at GLYPH_SSx the display size and downscale for smooth edges.
static const int GLYPH_SS = 3;
static SDL_Texture *makeGlyph(const char *label, bool pill){
  if(!g_font_sm || !g_font_big) return nullptr;             // no shared font -> no glyphs
  const int S=GLYPH_SS, base=TTF_FontHeight(g_font_sm)+6;
  int H=base*S, W=(pill? base*8/5 : base)*S;
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_SetRenderTarget(g_ren,t);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  SDL_Color edge={14,16,22,255}, hi={92,99,114,255}, face={52,57,68,255}, ink={246,248,252,255};
  if(pill){                                     // concentric: dark edge -> light rim -> face
    int r=H/2;
    fillCircle(r,r,r,edge);     fillCircle(W-r,r,r,edge);     fillRect(r,0,W-2*r,H,edge);
    fillCircle(r,r,r-S,hi);     fillCircle(W-r,r,r-S,hi);     fillRect(r,S,W-2*r,H-2*S,hi);
    fillCircle(r,r,r-S*2,face); fillCircle(W-r,r,r-S*2,face); fillRect(r,S*2,W-2*r,H-S*4,face);
  } else {
    int R=H/2;
    fillCircle(W/2,H/2,R,edge);        // dark outline
    fillCircle(W/2,H/2,R-S,hi);        // light rim
    fillCircle(W/2,H/2,R-S*2,face);    // face
  }
  SDL_Surface *sf=TTF_RenderUTF8_Blended(g_font_big,label,ink);   // big source -> crisp when downscaled
  if(sf){ SDL_Texture *lt=SDL_CreateTextureFromSurface(g_ren,sf);
    if(lt) SDL_SetTextureBlendMode(lt,SDL_BLENDMODE_BLEND);
    int inner=H*56/100, lw=sf->w, lh=sf->h;
    if(lh>0){ lw=lw*inner/lh; lh=inner; }
    SDL_Rect d={(W-lw)/2,(H-lh)/2,lw,lh}; SDL_FreeSurface(sf);
    if(lt){ SDL_RenderCopy(g_ren,lt,nullptr,&d); SDL_DestroyTexture(lt); } }
  SDL_SetRenderTarget(g_ren,nullptr);
  return t;
}
static void makeGlyphs(){
  g_gA=makeGlyph("A",false); g_gB=makeGlyph("B",false);
  g_gX=makeGlyph("X",false); g_gY=makeGlyph("Y",false);
  g_gPlus=makeGlyph("+",false); g_gMinus=makeGlyph("-",false);
  g_gLeft=makeGlyph("<",false); g_gRight=makeGlyph(">",false);
  g_gL=makeGlyph("L",true); g_gR=makeGlyph("R",true);
}

// --- control hints: centred row of {glyph,label}; each item's tap rect is recorded for touch.
enum FootAct { FA_NONE, FA_LAUNCH, FA_SORT, FA_OPTIONS, FA_SETTINGS, FA_FILTER, FA_PAGEL, FA_PAGER, FA_QUIT };
struct FootItem { SDL_Texture *glyph; const char *label; int act; bool localize=true; };
static SDL_Rect g_footHit[10]; static int g_footAct[10]; static int g_footN=0;
static void drawFooterHints(const FootItem *it,int n,int cy){
  const int gap=8,pairGap=g_launcherPortrait?12:26,glyphGap=g_launcherPortrait?8:16;
  const int fh=TTF_FontHeight(g_font_sm),maxWidth=std::max(80,SW-24);
  int itemWidth[10]={},gapAfter[10]={};
  for(int i=0;i<n&&i<10;i++){
    int gw=0;if(it[i].glyph) SDL_QueryTexture(it[i].glyph,nullptr,nullptr,&gw,nullptr);gw/=GLYPH_SS;
    bool label=it[i].label&&it[i].label[0];
    const std::string_view shown=label?(it[i].localize?LauncherLocalization::Translate(it[i].label):std::string_view(it[i].label)):std::string_view{};
    itemWidth[i]=gw+(label?gap+textW(g_font_sm,shown.data()):0);
    gapAfter[i]=label?pairGap:glyphGap;
  }
  int rowStart[10]={},rowEnd[10]={},rowWidth[10]={},rowCount=0;
  for(int first=0;first<n&&first<10;){
    int last=first,width=0;
    while(last<n&&last<10){
      int added=itemWidth[last]+(last>first?gapAfter[last-1]:0);
      if(last>first&&width+added>maxWidth) break;
      width+=added;last++;
    }
    rowStart[rowCount]=first;rowEnd[rowCount]=last;rowWidth[rowCount]=width;rowCount++;first=last;
  }
  const int rowSpacing=fh+14;
  g_footN=0;
  for(int row=0;row<rowCount;row++){
    int rowY=cy-(rowCount-1-row)*rowSpacing;
    int x=(SW-rowWidth[row])/2;
    for(int i=rowStart[row];i<rowEnd[row];i++){
      int gw=0,gh=0;if(it[i].glyph) SDL_QueryTexture(it[i].glyph,nullptr,nullptr,&gw,&gh);gw/=GLYPH_SS;gh/=GLYPH_SS;
      int x0=x;
      if(it[i].glyph){SDL_Rect d={x,rowY-gh/2,gw,gh};SDL_RenderCopy(g_ren,it[i].glyph,nullptr,&d);}
      x+=gw;bool label=it[i].label&&it[i].label[0];
      if(label){const std::string_view shown=it[i].localize?LauncherLocalization::Translate(it[i].label):std::string_view(it[i].label);
        x+=gap;drawText(g_font_sm,x,rowY-fh/2,shown.data(),COL_DIM);x+=textW(g_font_sm,shown.data());}
      if(g_footN<10){g_footHit[g_footN]={x0-6,rowY-std::max(gh,fh)/2-8,(x-x0)+12,std::max(gh,fh)+16};g_footAct[g_footN]=it[i].act;g_footN++;}
      if(i+1<rowEnd[row])x+=gapAfter[i];
    }
  }
}
static int footTapAct(int px,int py){
  for(int i=0;i<g_footN;i++){ SDL_Rect &r=g_footHit[i];
    if(px>=r.x && px<r.x+r.w && py>=r.y && py<r.y+r.h) return g_footAct[i]; }
  return FA_NONE;
}

// --- touchscreen gestures (handheld). devkitPro SDL2 emits SDL_FINGER* as 0..1 coords; xSW/SH = pixels.
enum TouchKind { TOUCH_NONE, TOUCH_TAP, TOUCH_SWIPE_L, TOUCH_SWIPE_R, TOUCH_SCROLL_UP, TOUCH_SCROLL_DOWN };
struct TouchG {
  bool active=false, vertical=false;
  SDL_FingerID fid=0;
  float x0=0,y0=0,lastY=0;
  Uint32 t0=0;
};
static TouchG g_touch;
static int g_touchScrollSteps=1;
static void touchUiPoint(float normalizedX,float normalizedY,float &x,float &y){
  switch(g_launcherRotation){
    case 1: x=normalizedY*SW; y=(1.0f-normalizedX)*SH; break;
    case 2: x=(1.0f-normalizedX)*SW; y=(1.0f-normalizedY)*SH; break;
    case 3: x=(1.0f-normalizedY)*SW; y=normalizedX*SH; break;
    default: x=normalizedX*SW; y=normalizedY*SH; break;
  }
}
static TouchKind touchFeed(const SDL_Event &e,int *ox,int *oy){
  const int TAP_MOVE=26, SWIPE_DX=90, SCROLL_STEP=30; const Uint32 TAP_MS=400;
  if(e.type==SDL_FINGERDOWN){
    if(g_touch.active && SDL_GetTicks()-g_touch.t0 < 2000) return TOUCH_NONE;
    g_touch.active=true; g_touch.vertical=false; g_touch.fid=e.tfinger.fingerId;
    touchUiPoint(e.tfinger.x,e.tfinger.y,g_touch.x0,g_touch.y0);
    g_touch.lastY=g_touch.y0; g_touch.t0=SDL_GetTicks();
  } else if(e.type==SDL_FINGERMOTION && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    float ux=0,uy=0; touchUiPoint(e.tfinger.x,e.tfinger.y,ux,uy);
    float dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    if(!g_touch.vertical && fabsf(dy)>TAP_MOVE && fabsf(dy)>fabsf(dx)*1.15f) g_touch.vertical=true;
    if(g_touch.vertical){
      float step=uy-g_touch.lastY;
      if(fabsf(step)>=SCROLL_STEP){
        g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(step)/SCROLL_STEP)));
        g_touch.lastY=uy;
        if(ox) *ox=(int)ux;
        if(oy) *oy=(int)uy;
        return step<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
      }
    }
  } else if(e.type==SDL_FINGERUP && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    g_touch.active=false;
    float ux=0,uy=0; touchUiPoint(e.tfinger.x,e.tfinger.y,ux,uy);
    float dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    Uint32 dt=SDL_GetTicks()-g_touch.t0;
    if(ox) *ox=(int)ux;
    if(oy) *oy=(int)uy;
    if(g_touch.vertical || (fabsf(dy)>=55 && fabsf(dy)>fabsf(dx)*1.15f)){
      float remaining=uy-g_touch.lastY;
      if(fabsf(remaining)<18 && g_touch.vertical) return TOUCH_NONE;
      g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(g_touch.vertical?remaining:dy)/SCROLL_STEP)));
      return (g_touch.vertical?remaining:dy)<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
    }
    if(fabsf(dx)>=SWIPE_DX && fabsf(dx)>fabsf(dy)*1.5f) return dx<0?TOUCH_SWIPE_L:TOUCH_SWIPE_R;
    if(fabsf(dx)<=TAP_MOVE && fabsf(dy)<=TAP_MOVE && dt<=TAP_MS) return TOUCH_TAP;
  }
  return TOUCH_NONE;
}

static bool touchScrollList(TouchKind kind,int &sel,int &top,int count,int visible){
  if((kind!=TOUCH_SCROLL_UP && kind!=TOUCH_SCROLL_DOWN) || count<=0) return false;
  const int previous=sel;
  int delta=(kind==TOUCH_SCROLL_UP?1:-1)*g_touchScrollSteps;
  sel=std::max(0,std::min(count-1,sel+delta));
  if(sel<top) top=sel;
  if(sel>=top+visible) top=sel-visible+1;
  if(top<0) top=0;
  if(sel!=previous) uiAudioPlay(UiSound::Navigate);
  return true;
}

static bool g_stickXLatched=false, g_stickYLatched=false;
static char stickNav(const SDL_Event &e){
  const int TH=18000, DZ=8000;
  if(e.type!=SDL_CONTROLLERAXISMOTION) return 0;
  if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTX){
    if(!g_stickXLatched && e.caxis.value<-TH){ g_stickXLatched=true; return 'L'; }
    if(!g_stickXLatched && e.caxis.value> TH){ g_stickXLatched=true; return 'R'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickXLatched=false;
  } else if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTY){
    if(!g_stickYLatched && e.caxis.value<-TH){ g_stickYLatched=true; return 'U'; }
    if(!g_stickYLatched && e.caxis.value> TH){ g_stickYLatched=true; return 'D'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickYLatched=false;
  }
  return 0;
}
static void pumpStick(const SDL_Event &e){
  char n=stickNav(e); if(!n) return;
  SDL_Event s; memset(&s,0,sizeof(s));
  s.type=SDL_CONTROLLERBUTTONDOWN;
  s.cbutton.button = n=='U'?SDL_CONTROLLER_BUTTON_DPAD_UP : n=='D'?SDL_CONTROLLER_BUTTON_DPAD_DOWN
                   : n=='L'?SDL_CONTROLLER_BUTTON_DPAD_LEFT : SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  SDL_PushEvent(&s);
}

static SDL_GameController *g_pad=nullptr;
static bool g_exitRequested=false;
static int g_navHeld=0;
static Uint32 g_navSince=0,g_navLast=0;
static Uint32 g_lastUiActivity=0;
static std::deque<SDL_Event> g_pendingEvents;
static constexpr Uint32 USB_STATUS_EVENT=SDL_USEREVENT+2;
static void pumpCoverDecodeResults();
static void cancelQueuedCoverDecodes();
static void stopCoverDecodeWorker();

static void storageStatusWake(void*){
  if(!g_sdlReady)return;
  SDL_Event event{};event.type=USB_STATUS_EVENT;SDL_PushEvent(&event);
}

static void openController(int index) {
  if (!g_pad && index >= 0 && SDL_IsGameController(index))
    g_pad = SDL_GameControllerOpen(index);
}

static void closeController() {
  if (!g_pad) return;
  SDL_GameControllerClose(g_pad);
  g_pad = nullptr;
  g_stickXLatched = g_stickYLatched = false;
  g_navHeld = 0;
  g_navSince = g_navLast = 0;
}

static bool beginUiFrame() {
  if (g_exitRequested) return false;
  if (!appletMainLoop()) {
    g_exitRequested = true;
    return false;
  }
  if(g_pad&&!SDL_GameControllerGetAttached(g_pad)) closeController();
  pumpCoverDecodeResults();
  return true;
}

static int keyboardNavigationButton(SDL_Keycode key) {
  switch(key){
    case SDLK_RETURN: case SDLK_KP_ENTER: return BTN_CONFIRM;
    case SDLK_ESCAPE: return BTN_CANCEL;
    case SDLK_UP: return SDL_CONTROLLER_BUTTON_DPAD_UP;
    case SDLK_DOWN: return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    case SDLK_LEFT: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    case SDLK_RIGHT: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    case SDLK_PAGEUP: return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    case SDLK_PAGEDOWN: return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    case SDLK_s: return SDL_CONTROLLER_BUTTON_X;
    case SDLK_F1: return BTN_SETTINGS;
    case SDLK_SPACE: return SDL_CONTROLLER_BUTTON_START;
    default: return -1;
  }
}

static bool pollUiEvent(SDL_Event &event) {
  auto nextEvent=[&](){
    if(!g_pendingEvents.empty()){
      event=g_pendingEvents.front();
      g_pendingEvents.pop_front();
      return true;
    }
    return SDL_PollEvent(&event)==1;
  };
  while (nextEvent()) {
    g_lastUiActivity=SDL_GetTicks();
    if (event.type == SDL_QUIT) {
      g_exitRequested = true;
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
      openController(event.cdevice.which);
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
      if (g_pad) {
        SDL_Joystick *joystick = SDL_GameControllerGetJoystick(g_pad);
        if (joystick && SDL_JoystickInstanceID(joystick) == event.cdevice.which)
          closeController();
      }
      continue;
    }
    if(event.type==SDL_KEYDOWN){
      int button=keyboardNavigationButton(event.key.keysym.sym);
      if(button>=0){
        SDL_Event press{};
        press.type=SDL_CONTROLLERBUTTONDOWN;
        press.cbutton.button=(Uint8)button;
        SDL_PushEvent(&press);
      }
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
      switch (event.cbutton.button) {
        case BTN_CONFIRM: uiAudioPlay(UiSound::Confirm); break;
        case BTN_CANCEL: uiAudioPlay(UiSound::Back); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
          uiAudioPlay(UiSound::Navigate); break;
        default: break;
      }
    }
    return true;
  }
  return false;
}

// Animated themes are paced to the display while static screens sleep until an
// SDL input/worker/hotplug event arrives.  The short lifecycle timeout is only
// used to notice HOME/app shutdown requests; it is not a redraw timer.
static void waitForNextFrame(bool forceAnimation=false) {
  if(g_exitRequested || SDL_HasEvents(SDL_FIRSTEVENT,SDL_LASTEVENT)) return;
  for(;;){
    const Uint32 now=SDL_GetTicks();
    const bool recentInput=now-g_lastUiActivity<240;
    const bool animate=forceAnimation || (g_uiAnimations&&hasAnimatedBackground()) ||
                       recentInput || g_navHeld!=0 || g_touch.active;
    const bool toastVisible=!g_toastMessage.empty()&&!SDL_TICKS_PASSED(now,g_toastUntil);
    int waitMilliseconds=animate?16:250;
    if(toastVisible)waitMilliseconds=std::min(waitMilliseconds,
      std::max(1,(int)(g_toastUntil-now)));
    SDL_Event event{};
    if(SDL_WaitEventTimeout(&event,waitMilliseconds)==1){g_pendingEvents.push_back(event);return;}
    if(animate)return;
    if(toastVisible&&SDL_TICKS_PASSED(SDL_GetTicks(),g_toastUntil))return;
    // The lifecycle poll does not return to the renderer, so a static screen
    // consumes no frames until an actual SDL/worker/hotplug event arrives.
    if(!appletMainLoop()){g_exitRequested=true;return;}
  }
}

static void navRepeat(){
  if(!g_pad) return;
  const int TH=18000;
  int dir=0;
  if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_UP)   || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_UP;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_DOWN;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_LEFT;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  Uint32 now=SDL_GetTicks();
  if(dir!=g_navHeld){ g_navHeld=dir; g_navSince=now; g_navLast=now; return; }
  if(!dir) return;
  const Uint32 DELAY=360, RATE=85;
  if(now-g_navSince<DELAY || now-g_navLast<RATE) return;
  g_navLast=now;
  SDL_Event s; memset(&s,0,sizeof(s)); s.type=SDL_CONTROLLERBUTTONDOWN; s.cbutton.button=(Uint8)dir;
  SDL_PushEvent(&s);
}

// ---------------------------------------------------------------------------
// game scanning
// ---------------------------------------------------------------------------
struct Game {
  std::string path;      // full sdmc path of the app dir (ux0/app/<TITLEID>)
  std::string file;      // == title_id (kept for symmetry with the shared UI helpers)
  std::string title;     // display title from param.sfo / SteamGridDB search term
  std::string key;       // == title_id (cover + gamecfg key)
  std::string title_id;  // Vita title id, the app folder name (e.g. "PCSB00180")
  std::string iconPath;  // sce_sys/icon0.png (cover fallback)
  SDL_Texture *cover = nullptr;
  Uint32 coverAt = 0;   // when the texture was decoded -> brief fade-in on the grid
  Uint64 coverUse = 0;  // LRU stamp; only a bounded working set stays decoded
  Uint64 coverRequest = 0;
  bool coverQueued = false;
  bool triedCover = false;
  bool hasCfg = false;  // a gamecfg/<key>.ini exists (shows a marker)
  int region = 0;       // 0 unknown, 1 US, 2 EU, 3 JP (detected once at scan)
  long long added = 0;  // file mtime -> "recently added" sort
  long long played = 0; // last-played sequence -> "recently played" sort
  long long metadataSize = 0;
  long long metadataModified = 0;
  std::string metadataTitle;
  bool metadataChanged = false;
};
static std::vector<Game> g_games;
static std::vector<Game*> g_visibleGames;
static std::unordered_set<std::string> g_favoriteGames;
struct Collection { std::string name; std::unordered_set<std::string> members; };
static std::vector<Collection> g_collections;
static std::string g_searchQuery;
static int g_activeLibraryView=0; // 0 all, 1 favorites, 2+ collection (session-only)
static Uint64 g_coverUseSerial = 0;
static constexpr size_t COVER_CACHE_LIMIT = 64;

// listing order (persisted as Wrapper/SortMode); Y on the grid cycles it.
enum { SORT_ALPHA, SORT_RECENT, SORT_ADDED, SORT_COUNT };
static const char *SORT_NAME[SORT_COUNT] = { "A-Z", "Recently played", "Recently added" };
static int g_sort = SORT_ALPHA;
static Store g_recent;   // recent.ini: game key -> last-played sequence number
static const char *RECENT_INI = "sdmc:/switch/vita3k/recent.ini";

static std::string lowerLibraryText(std::string value){
  std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return (char)std::tolower(c);});
  return value;
}

static void rebuildVisibleGames(){
  g_visibleGames.clear();
  const std::string query=lowerLibraryText(g_searchQuery);
  for(Game &game:g_games){
    bool allowed=g_activeLibraryView==0 ||
      (g_activeLibraryView==1&&g_favoriteGames.find(game.key)!=g_favoriteGames.end());
    if(g_activeLibraryView>=2){const size_t index=(size_t)(g_activeLibraryView-2);
      allowed=index<g_collections.size()&&
        g_collections[index].members.find(game.key)!=g_collections[index].members.end();}
    if(allowed&&!query.empty())allowed=lowerLibraryText(game.title).find(query)!=std::string::npos||
      lowerLibraryText(game.title_id).find(query)!=std::string::npos;
    if(allowed)g_visibleGames.push_back(&game);
  }
}

static Game *visibleGame(int index){
  return index>=0&&index<(int)g_visibleGames.size()?g_visibleGames[index]:nullptr;
}

static int visibleIndexForKey(const std::string &key){
  for(size_t i=0;i<g_visibleGames.size();i++)if(g_visibleGames[i]->key==key)return (int)i;
  return 0;
}

static void loadLibraryOrganization(){
  g_favoriteGames.clear();g_collections.clear();g_searchQuery.clear();g_activeLibraryView=0;
  int favorites=atoi(storeGet(g_global,"Library/FavoriteCount","0"));
  for(int i=0;i<favorites;i++){std::string key="Library/Favorite"+std::to_string(i);
    const char *value=storeGet(g_global,key.c_str(),"");if(*value)g_favoriteGames.insert(value);}
  int collections=std::max(0,atoi(storeGet(g_global,"Library/CollectionCount","0")));
  for(int i=0;i<collections;i++){Collection collection;std::string prefix="Library/Collection"+std::to_string(i);
    collection.name=storeGet(g_global,(prefix+"Name").c_str(),"");
    int members=std::max(0,atoi(storeGet(g_global,(prefix+"MemberCount").c_str(),"0")));
    for(int j=0;j<members;j++){const char *key=storeGet(g_global,(prefix+"Member"+std::to_string(j)).c_str(),"");if(*key)collection.members.insert(key);}
    if(!collection.name.empty())g_collections.push_back(std::move(collection));}
}

static void saveLibraryOrganization(){
  storeRemovePrefix(g_global,"Library/Favorite");storeRemovePrefix(g_global,"Library/Collection");
  std::vector<std::string> favorites(g_favoriteGames.begin(),g_favoriteGames.end());std::sort(favorites.begin(),favorites.end());
  storeSet(g_global,"Library/FavoriteCount",std::to_string(favorites.size()).c_str());
  for(size_t i=0;i<favorites.size();i++)storeSet(g_global,("Library/Favorite"+std::to_string(i)).c_str(),favorites[i].c_str());
  storeSet(g_global,"Library/CollectionCount",std::to_string(g_collections.size()).c_str());
  for(size_t i=0;i<g_collections.size();i++){std::string prefix="Library/Collection"+std::to_string(i);
    storeSet(g_global,(prefix+"Name").c_str(),g_collections[i].name.c_str());
    std::vector<std::string> members(g_collections[i].members.begin(),g_collections[i].members.end());std::sort(members.begin(),members.end());
    storeSet(g_global,(prefix+"MemberCount").c_str(),std::to_string(members.size()).c_str());
    for(size_t j=0;j<members.size();j++)storeSet(g_global,(prefix+"Member"+std::to_string(j)).c_str(),members[j].c_str());}
  storeSave(g_global,LAUNCHER_INI);
}

static int settingsFooterReserve();

static void applySort() {
  auto cmpTitle = [](const Game &a, const Game &b){ return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; };
  std::sort(g_games.begin(), g_games.end(), [&](const Game &a, const Game &b){
    if (g_sort == SORT_RECENT && a.played != b.played) return a.played > b.played;
    if (g_sort == SORT_ADDED  && a.added  != b.added)  return a.added  > b.added;
    return cmpTitle(a, b);   // tiebreak + default
  });
  rebuildVisibleGames();
}
// stamp a game as just-played (monotonic sequence; no wall clock on Switch).
static void recordPlayed(const std::string &key){
  long long seq = atoll(storeGet(g_global,"Wrapper/PlaySeq","0")) + 1;
  char b[24]; snprintf(b,sizeof(b),"%lld",seq);
  storeSet(g_global,"Wrapper/PlaySeq",b); storeSave(g_global,LAUNCHER_INI);
  storeSet(g_recent,key.c_str(),b);        storeSave(g_recent,RECENT_INI);
}

static void manageCollections(){
  int selection=0,top=0;beginScreenFx();
  for(;;){
    if(!beginUiFrame())return;
    const int count=1+(int)g_collections.size();
    const int rowH=56,start=std::max(topBarH()+34,110);
    const int visible=std::max(1,std::min(8,(SH-start-settingsFooterReserve())/rowH));
    SDL_Event event;navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);if(event.type!=SDL_CONTROLLERBUTTONDOWN)continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)selection=(selection+count-1)%count;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)selection=(selection+1)%count;
      else if(event.cbutton.button==BTN_CONFIRM){
        if(selection==0){char name[96];if(promptText("Create collection","",name,sizeof(name))){g_collections.push_back({name,{}});selection=(int)g_collections.size();saveLibraryOrganization();}}
        else {g_activeLibraryView=selection+1;g_searchQuery.clear();rebuildVisibleGames();return;}
      } else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&selection>0){char name[96];Collection &collection=g_collections[selection-1];
        if(promptText("Rename collection",collection.name.c_str(),name,sizeof(name))){collection.name=name;saveLibraryOrganization();}}
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_Y&&selection>0){Collection &collection=g_collections[selection-1];
        if(confirmBox("Delete collection?",{collection.name,uiText("Games and files are not deleted.")})){g_collections.erase(g_collections.begin()+selection-1);selection=std::min(selection,(int)g_collections.size());saveLibraryOrganization();}}
      else if(event.cbutton.button==BTN_CANCEL)return;
      if(selection<top)top=selection;
      if(selection>=top+visible)top=selection-visible+1;
      top=std::max(0,std::min(top,std::max(0,count-visible)));
    }
    clearUiBackground();drawLocalizedHeader("Manage collections",nullptr);
    int colX,colW,labelX,valX;listCol(&colX,&colW,&labelX,&valX);
    glassPanel(colX-12,start-10,colW+24,std::min(count,visible)*rowH+18);
    for(int row=0;row<visible&&top+row<count;row++){const int index=top+row,y=start+row*rowH;const bool current=index==selection;
      if(current){fillRect(colX,y+2,colW,rowH-4,COL_FOCUS);fillRect(colX,y+2,5,rowH-4,COL_SEL);}
      const std::string label=index==0?std::string(LauncherLocalization::Translate("Create collection")):g_collections[index-1].name;
      drawText(g_font,labelX,y+(rowH-TTF_FontHeight(g_font))/2,label.c_str(),current?COL_VAL:COL_TXT);}
    if(count>visible){const int trackX=colX+colW+16,trackH=visible*rowH;
      fillRect(trackX,start,4,trackH,(SDL_Color){40,44,54,255});
      const int thumbH=std::max(12,trackH*visible/count),denominator=std::max(1,count-visible);
      fillRect(trackX,start+(trackH-thumbH)*top/denominator,4,thumbH,COL_SEL);}
    FootItem footer[]={{g_gA,"Open",FA_NONE},{g_gX,"Rename",FA_NONE},{g_gY,"Delete",FA_NONE},{g_gB,"Back",FA_NONE}};
    drawFooterHints(footer,selection>0?4:2,SH-26);drawFadeIn();presentUi();waitForNextFrame();
  }
}

static void libraryFilterMenu(){
  std::vector<std::string> labels={"All games","Search games","Favorites","Manage collections"};
  for(const Collection &collection:g_collections)labels.push_back(collection.name);
  std::vector<const char*> choices;for(const std::string &label:labels)choices.push_back(label.c_str());
  const int selected=dropdown("Filter",choices.data(),(int)choices.size(),0,true,false);
  if(selected<0)return;
  if(selected==0){g_activeLibraryView=0;g_searchQuery.clear();}
  else if(selected==1){char query[128];if(promptTextAdvanced("Search games",g_searchQuery.c_str(),query,sizeof(query),false,true)){g_searchQuery=query;g_activeLibraryView=0;}}
  else if(selected==2){g_activeLibraryView=1;g_searchQuery.clear();}
  else if(selected==3){manageCollections();}
  else {g_activeLibraryView=selected-2;g_searchQuery.clear();}
  rebuildVisibleGames();
}

static void editGameOrganization(Game &game){
  std::vector<std::string> labels;
  labels.push_back(g_favoriteGames.find(game.key)!=g_favoriteGames.end()?"Remove from favorites":"Add to favorites");
  for(const Collection &collection:g_collections)
    labels.push_back((collection.members.find(game.key)!=collection.members.end()?"[x] ":"[ ] ")+collection.name);
  labels.push_back("Manage collections");
  std::vector<const char*> choices;for(const std::string &label:labels)choices.push_back(label.c_str());
  const int selected=dropdown("Collections",choices.data(),(int)choices.size(),0,true,false);
  if(selected<0)return;
  if(selected==0){if(!g_favoriteGames.erase(game.key))g_favoriteGames.insert(game.key);}
  else if(selected==(int)labels.size()-1){manageCollections();}
  else {auto &members=g_collections[selected-1].members;if(!members.erase(game.key))members.insert(game.key);}
  saveLibraryOrganization();rebuildVisibleGames();
}

// param.sfo: extract a string value (TITLE / STITLE) via the binary SFO layout.
static std::string sfoString(const std::string &path, const char *want) {
  if(!want||!*want)return {};
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return {};
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 20 || sz > (1 << 20)) { fclose(f); return {}; }
  std::vector<uint8_t> d(sz);
  if (fread(d.data(), 1, sz, f) != (size_t)sz) { fclose(f); return {}; }
  fclose(f);

  auto rd32 = [&](size_t o) -> uint32_t {
    return static_cast<uint32_t>(d[o]) |
      (static_cast<uint32_t>(d[o + 1]) << 8) |
      (static_cast<uint32_t>(d[o + 2]) << 16) |
      (static_cast<uint32_t>(d[o + 3]) << 24);
  };
  if (rd32(0) != 0x46535000) return {}; // "\0PSF"
  const uint32_t key_tbl = rd32(0x08);
  const uint32_t data_tbl = rd32(0x0C);
  const uint32_t count = rd32(0x10);
  const size_t fileSize=d.size();
  if(key_tbl>fileSize||data_tbl>fileSize||key_tbl>data_tbl)return {};
  const size_t maximumEntries=(fileSize-0x14)/16;
  if(count>maximumEntries)return {};
  std::string result;
  for (uint32_t i = 0; i < count; i++) {
    const size_t ent=0x14+static_cast<size_t>(i)*16;
    const uint16_t key_off = d[ent] | (d[ent + 1] << 8);
    const uint32_t data_len = rd32(ent + 4);
    const uint32_t data_off = rd32(ent + 12);
    const size_t keyStart=static_cast<size_t>(key_tbl)+key_off;
    if(keyStart<key_tbl||keyStart>=fileSize)continue;
    const void *keyEnd=std::memchr(d.data()+keyStart,'\0',fileSize-keyStart);
    if(!keyEnd)continue;
    const size_t keyLength=static_cast<const uint8_t*>(keyEnd)-(d.data()+keyStart);
    const size_t wantedLength=std::strlen(want);
    if(keyLength==wantedLength&&std::memcmp(d.data()+keyStart,want,wantedLength)==0){
      const size_t start=static_cast<size_t>(data_tbl)+data_off;
      if(start<data_tbl||start>fileSize||data_len>fileSize-start)continue;
      const char *value=reinterpret_cast<const char *>(d.data()+start);
      result.assign(value,strnlen(value,data_len));
      break;
    }
  }
  return result;
}
// Scanning never touches SDL objects or the mutable stores off the main thread.
// Results are published progressively and wake the event-driven UI.
static constexpr Uint32 LIBRARY_READY_EVENT = SDL_USEREVENT + 1;
struct LibraryScanState {
  std::atomic<bool> cancel{false};
  std::atomic<bool> complete{true};
  std::mutex mutex;
  std::deque<Game> ready;
  std::thread worker;
  bool cacheDirty=false;
  bool firstBatch=true;
  size_t unsortedPublished=0;
};
static LibraryScanState g_libraryScan;

static std::string metadataCacheKey(const std::string &tid,const char *field){
  return "LibraryCache/"+tid+"/"+field;
}

static void stopGameScan(){
  g_libraryScan.cancel.store(true,std::memory_order_release);
  if(g_libraryScan.worker.joinable())g_libraryScan.worker.join();
  std::lock_guard<std::mutex> lock(g_libraryScan.mutex);
  g_libraryScan.ready.clear();g_libraryScan.complete.store(true,std::memory_order_release);
}

static void startGameScan(){
  stopGameScan();
  cancelQueuedCoverDecodes();
  for(Game &game:g_games)if(game.cover)SDL_DestroyTexture(game.cover);
  g_games.clear();g_visibleGames.clear();g_coverUseSerial=0;
  g_libraryScan.cancel.store(false,std::memory_order_release);
  g_libraryScan.complete.store(false,std::memory_order_release);
  g_libraryScan.cacheDirty=false;g_libraryScan.firstBatch=true;g_libraryScan.unsortedPublished=0;
  Store globalSnapshot=g_global,titlesSnapshot=g_titles,recentSnapshot=g_recent;
  g_libraryScan.worker=std::thread([globalSnapshot,titlesSnapshot,recentSnapshot]() mutable {
    DIR *directory=opendir(APP_DIR);
    if(directory){
      struct dirent *entry=nullptr;
      while(!g_libraryScan.cancel.load(std::memory_order_acquire)&&(entry=readdir(directory))){
        if(entry->d_name[0]=='.')continue;
        Game game;game.title_id=entry->d_name;game.key=game.title_id;game.file=game.title_id;
        game.path=std::string(APP_DIR)+"/"+game.title_id;
        const std::string sfo=game.path+"/sce_sys/param.sfo";
        struct stat sfoStat{};if(stat(sfo.c_str(),&sfoStat)!=0||!S_ISREG(sfoStat.st_mode))continue;
        game.iconPath=game.path+"/sce_sys/icon0.png";
        game.metadataSize=(long long)sfoStat.st_size;game.metadataModified=(long long)sfoStat.st_mtime;
        const std::string sizeKey=metadataCacheKey(game.title_id,"Size");
        const std::string timeKey=metadataCacheKey(game.title_id,"Modified");
        const std::string titleKey=metadataCacheKey(game.title_id,"Title");
        const bool cacheValid=atoll(storeGet(globalSnapshot,sizeKey.c_str(),"-1"))==game.metadataSize&&
          atoll(storeGet(globalSnapshot,timeKey.c_str(),"-1"))==game.metadataModified;
        if(cacheValid)game.metadataTitle=storeGet(globalSnapshot,titleKey.c_str(),"");
        if(game.metadataTitle.empty()){
          game.metadataTitle=sfoString(sfo,"TITLE");
          if(game.metadataTitle.empty())game.metadataTitle=sfoString(sfo,"STITLE");
          game.metadataChanged=true;
        }
        game.title=game.metadataTitle.empty()?game.title_id:game.metadataTitle;
        const char *custom=storeGet(titlesSnapshot,game.key.c_str(),"");
        if(*custom)game.title=custom;
        std::replace(game.title.begin(),game.title.end(),'\n',' ');
        game.played=atoll(storeGet(recentSnapshot,game.key.c_str(),"0"));
        struct stat pathStat{};if(stat(game.path.c_str(),&pathStat)==0)game.added=(long long)pathStat.st_mtime;
        game.hasCfg=stat((std::string(GAMECFG_DIR)+"/"+game.key+".ini").c_str(),&pathStat)==0;
        {
          std::lock_guard<std::mutex> lock(g_libraryScan.mutex);
          g_libraryScan.ready.push_back(std::move(game));
        }
        SDL_Event wake{};wake.type=LIBRARY_READY_EVENT;SDL_PushEvent(&wake);
      }
      closedir(directory);
    }
    g_libraryScan.complete.store(true,std::memory_order_release);
    SDL_Event wake{};wake.type=LIBRARY_READY_EVENT;SDL_PushEvent(&wake);
  });
}

static bool pumpGameScan(){
  std::deque<Game> batch;
  const size_t limit=2;
  {
    std::lock_guard<std::mutex> lock(g_libraryScan.mutex);
    while(!g_libraryScan.ready.empty()&&batch.size()<limit){batch.push_back(std::move(g_libraryScan.ready.front()));g_libraryScan.ready.pop_front();}
  }
  if(!batch.empty()){
    const size_t published=batch.size();
    g_libraryScan.firstBatch=false;
    while(!batch.empty()){
      Game game=std::move(batch.front());batch.pop_front();
      if(game.metadataChanged){
        const std::string sizeKey=metadataCacheKey(game.title_id,"Size"),timeKey=metadataCacheKey(game.title_id,"Modified"),titleKey=metadataCacheKey(game.title_id,"Title");
        storeSet(g_global,sizeKey.c_str(),std::to_string(game.metadataSize).c_str());
        storeSet(g_global,timeKey.c_str(),std::to_string(game.metadataModified).c_str());
        storeSet(g_global,titleKey.c_str(),game.metadataTitle.c_str());g_libraryScan.cacheDirty=true;
      }
      g_games.push_back(std::move(game));
    }
    g_libraryScan.unsortedPublished+=published;
    if(g_games.size()<=24||g_libraryScan.unsortedPublished>=16){
      applySort();g_libraryScan.unsortedPublished=0;
    }else rebuildVisibleGames();
  }
  bool empty=false;{std::lock_guard<std::mutex> lock(g_libraryScan.mutex);empty=g_libraryScan.ready.empty();}
  if(g_libraryScan.complete.load(std::memory_order_acquire)&&empty){
    if(g_libraryScan.worker.joinable())g_libraryScan.worker.join();
    if(g_libraryScan.cacheDirty){storeSave(g_global,LAUNCHER_INI);g_libraryScan.cacheDirty=false;}
    applySort();
    return true;
  }
  return false;
}

static std::string coverPath(const Game &g) { return std::string(COVERS_DIR) + "/" + g.key + ".png"; }

// Explicit artwork previews still use this renderer-side helper. Grid covers
// are decoded by the worker below and uploaded with a strict per-frame budget.
static int g_cover_budget = 1 << 30;
static constexpr int COVER_REQUEST_BUDGET = 48;
static constexpr int COVER_UPLOAD_BUDGET = 2;
static constexpr size_t COVER_JOB_LIMIT = 96;
static constexpr size_t COVER_READY_LIMIT = 4;

// Load a cover, downscaled (cap long edge at 360px) to keep the whole library resident cheaply.
static SDL_Texture *loadCoverTexture(const std::string &path) {
  SDL_Surface *s = IMG_Load(path.c_str());
  if (!s) return nullptr;
  const int MAXW = 360, MAXH = 540;
  int width=s->w,height=s->h;
  if(width>MAXW){ height=(int)((long long)height*MAXW/width); width=MAXW; }
  if(height>MAXH){ width=(int)((long long)width*MAXH/height); height=MAXH; }
  if(width<1) width=1;
  if(height<1) height=1;
  if (width != s->w || height != s->h) {
    SDL_Surface *d = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!d) { SDL_FreeSurface(s); return nullptr; }
    SDL_BlendMode blend=SDL_BLENDMODE_NONE;
    SDL_GetSurfaceBlendMode(s,&blend);
    SDL_SetSurfaceBlendMode(s,SDL_BLENDMODE_NONE);
    const bool scaled=SDL_BlitScaled(s,nullptr,d,nullptr)==0;
    SDL_SetSurfaceBlendMode(s,blend);
    SDL_FreeSurface(s);
    if(!scaled){ SDL_FreeSurface(d); return nullptr; }
    s=d;
  }
  SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, s);
  SDL_FreeSurface(s);
  if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  return t;
}
static Game *findGameByKey(const std::string &key){
  const auto found=std::find_if(g_games.begin(),g_games.end(),[&](const Game &game){return game.key==key;});
  return found==g_games.end()?nullptr:&*found;
}

struct CoverDecodeJob {
  std::string key;
  std::vector<std::string> paths;
  Uint64 request=0;
  Uint64 epoch=0;
};
struct CoverDecodeResult {
  std::string key;
  Uint64 request=0;
  Uint64 epoch=0;
  int width=0,height=0;
  std::vector<Uint8> pixels;
};
static std::mutex g_coverDecodeMutex;
static std::condition_variable g_coverDecodeCondition;
static std::deque<CoverDecodeJob> g_coverDecodeJobs;
static std::deque<CoverDecodeResult> g_coverDecodeReady;
static std::thread g_coverDecodeWorker;
static bool g_coverDecodeStarted=false,g_coverDecodeStop=false;
static Uint64 g_coverDecodeEpoch=1,g_coverRequestSerial=0;

static std::vector<std::string> coverCandidatePaths(const Game &game){
  std::vector<std::string> paths{coverPath(game)};
  if(!game.iconPath.empty()&&game.iconPath!=paths.front())paths.emplace_back(game.iconPath);
  return paths;
}

static CoverDecodeResult decodeCover(const CoverDecodeJob &job){
  CoverDecodeResult result;result.key=job.key;result.request=job.request;result.epoch=job.epoch;
  SDL_Surface *source=nullptr;
  for(const std::string &path:job.paths){source=IMG_Load(path.c_str());if(source)break;}
  if(!source||source->w<1||source->h<1||source->w>8192||source->h>8192||
     (Uint64)source->w*(Uint64)source->h>16ull*1024*1024){
    if(source)SDL_FreeSurface(source);
    return result;
  }
  constexpr int maxWidth=360,maxHeight=540;
  int width=source->w,height=source->h;
  if(width>maxWidth){height=(int)((long long)height*maxWidth/width);width=maxWidth;}
  if(height>maxHeight){width=(int)((long long)width*maxHeight/height);height=maxHeight;}
  width=std::max(1,width);height=std::max(1,height);
  SDL_Surface *rgba=SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_RGBA32);
  if(!rgba){SDL_FreeSurface(source);return result;}
  SDL_BlendMode blend=SDL_BLENDMODE_NONE;SDL_GetSurfaceBlendMode(source,&blend);
  SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);
  const bool converted=SDL_BlitScaled(source,nullptr,rgba,nullptr)==0;
  SDL_SetSurfaceBlendMode(source,blend);SDL_FreeSurface(source);
  if(!converted){SDL_FreeSurface(rgba);return result;}
  const bool mustLock=SDL_MUSTLOCK(rgba);
  if(mustLock&&SDL_LockSurface(rgba)!=0){SDL_FreeSurface(rgba);return result;}
  result.pixels.resize((size_t)width*(size_t)height*4);
  for(int row=0;row<height;row++)memcpy(
      result.pixels.data()+(size_t)row*(size_t)width*4,
      (const Uint8*)rgba->pixels+(size_t)row*(size_t)rgba->pitch,(size_t)width*4);
  if(mustLock)SDL_UnlockSurface(rgba);
  SDL_FreeSurface(rgba);result.width=width;result.height=height;
  return result;
}

static void coverDecodeThread(){
  for(;;){
    CoverDecodeJob job;
    {
      std::unique_lock<std::mutex> lock(g_coverDecodeMutex);
      g_coverDecodeCondition.wait(lock,[]{return g_coverDecodeStop||
          (!g_coverDecodeJobs.empty()&&g_coverDecodeReady.size()<COVER_READY_LIMIT);});
      if(g_coverDecodeStop)return;
      job=std::move(g_coverDecodeJobs.front());g_coverDecodeJobs.pop_front();
    }
    CoverDecodeResult result=decodeCover(job);bool publish=false;
    {
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      if(!g_coverDecodeStop&&job.epoch==g_coverDecodeEpoch){
        g_coverDecodeReady.emplace_back(std::move(result));publish=true;
      }
    }
    if(publish)storageStatusWake(nullptr);
  }
}

static void startCoverDecodeWorker(){
  std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
  if(g_coverDecodeStarted)return;
  g_coverDecodeStop=false;g_coverDecodeStarted=true;
  g_coverDecodeWorker=std::thread(coverDecodeThread);
}

static void stopCoverDecodeWorker(){
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
    if(!g_coverDecodeStarted)return;
    g_coverDecodeStop=true;g_coverDecodeJobs.clear();g_coverDecodeReady.clear();
  }
  g_coverDecodeCondition.notify_all();
  if(g_coverDecodeWorker.joinable())g_coverDecodeWorker.join();
  std::lock_guard<std::mutex> lock(g_coverDecodeMutex);g_coverDecodeStarted=false;
}

static void cancelQueuedCoverDecodes(){
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
    ++g_coverDecodeEpoch;g_coverDecodeJobs.clear();g_coverDecodeReady.clear();
  }
  for(Game &game:g_games){game.coverQueued=false;game.coverRequest=0;}
  g_coverDecodeCondition.notify_all();
}

static void queueCoverDecode(Game &game,bool priority){
  if(game.cover||game.triedCover)return;
  if(game.coverQueued){
    if(priority){
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      const auto found=std::find_if(g_coverDecodeJobs.begin(),g_coverDecodeJobs.end(),
          [&](const CoverDecodeJob &job){return job.request==game.coverRequest;});
      if(found!=g_coverDecodeJobs.end()&&found!=g_coverDecodeJobs.begin()){
        CoverDecodeJob job=std::move(*found);g_coverDecodeJobs.erase(found);
        g_coverDecodeJobs.emplace_front(std::move(job));g_coverDecodeCondition.notify_one();
      }
    }
    return;
  }
  if(g_cover_budget<=0)return;
  --g_cover_budget;
  CoverDecodeJob job;job.key=game.key;job.paths=coverCandidatePaths(game);
  job.request=++g_coverRequestSerial;game.coverRequest=job.request;game.coverQueued=true;
  CoverDecodeJob dropped;bool didDrop=false;
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);job.epoch=g_coverDecodeEpoch;
    if(g_coverDecodeJobs.size()>=COVER_JOB_LIMIT){
      dropped=std::move(g_coverDecodeJobs.back());g_coverDecodeJobs.pop_back();didDrop=true;
    }
    if(priority)g_coverDecodeJobs.emplace_front(std::move(job));
    else g_coverDecodeJobs.emplace_back(std::move(job));
  }
  if(didDrop)if(Game *old=findGameByKey(dropped.key))if(old->coverRequest==dropped.request){
    old->coverQueued=false;old->coverRequest=0;
  }
  g_coverDecodeCondition.notify_one();
}
// lazy-load a cover texture (once, within the frame's budget); keep null if none
static void touchCover(Game &g) {
  if (g.cover) g.coverUse = ++g_coverUseSerial;
}

static void evictLeastRecentlyUsedCover() {
  Game *victim = nullptr;
  for (auto &candidate : g_games)
    if (candidate.cover && (!victim || candidate.coverUse < victim->coverUse)) victim = &candidate;
  if (!victim) return;
  SDL_DestroyTexture(victim->cover);
  victim->cover = nullptr;
  victim->coverUse = 0;
  victim->triedCover = false;
}

static void installCover(Game &g, SDL_Texture *cover) {
  if (!cover) return;
  size_t resident = 0;
  for (const auto &candidate : g_games) if (candidate.cover) resident++;
  if (resident >= COVER_CACHE_LIMIT) evictLeastRecentlyUsedCover();
  g.cover = cover;
  g.coverAt = SDL_GetTicks();
  touchCover(g);
}

static SDL_Texture *uploadCoverTexture(const CoverDecodeResult &result){
  if(result.width<1||result.height<1||result.pixels.empty()||!g_ren)return nullptr;
  SDL_Texture *texture=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_STATIC,result.width,result.height);
  if(texture&&SDL_UpdateTexture(texture,nullptr,result.pixels.data(),result.width*4)!=0){
    SDL_DestroyTexture(texture);texture=nullptr;
  }
  if(!texture){
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<Uint8*>(result.pixels.data()),result.width,result.height,
        32,result.width*4,SDL_PIXELFORMAT_RGBA32);
    if(surface){texture=SDL_CreateTextureFromSurface(g_ren,surface);SDL_FreeSurface(surface);}
  }
  if(texture)SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
  return texture;
}

static void pumpCoverDecodeResults(){
  int uploads=0,processed=0;
  while(processed<12){
    CoverDecodeResult result;
    {
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      if(g_coverDecodeReady.empty())break;
      if(!g_coverDecodeReady.front().pixels.empty()&&uploads>=COVER_UPLOAD_BUDGET)break;
      result=std::move(g_coverDecodeReady.front());g_coverDecodeReady.pop_front();
    }
    g_coverDecodeCondition.notify_one();++processed;
    Game *game=findGameByKey(result.key);
    if(!game||game->coverRequest!=result.request)continue;
    game->coverQueued=false;game->triedCover=true;
    if(!result.pixels.empty()){
      SDL_Texture *texture=uploadCoverTexture(result);++uploads;
      if(texture)installCover(*game,texture);
    }
  }
}

static void ensureCover(Game &g,bool priority=false) {
  if (g.cover) { touchCover(g); return; }
  queueCoverDecode(g,priority);
}
static void reloadCover(Game &g) {
  if (g.cover) { SDL_DestroyTexture(g.cover); g.cover = nullptr; }
  g.coverUse=0;g.triedCover=false;g.coverQueued=false;g.coverRequest=0;
  g_cover_budget=std::max(g_cover_budget,1);queueCoverDecode(g,true);
}

// Switch software keyboard (libnx swkbd, an overlay applet). true + fills out on OK.
static bool promptTextAdvanced(const char *header, const char *initial, char *out,
                               size_t outSize, bool password, bool allowEmpty) {
  SwkbdConfig kbd;
  out[0] = 0;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
  if(password) swkbdConfigMakePresetPassword(&kbd);
  else swkbdConfigMakePresetDefault(&kbd);
  if (header) swkbdConfigSetHeaderText(&kbd, header);
  if (initial && *initial) swkbdConfigSetInitialText(&kbd, initial);
  swkbdConfigSetStringLenMax(&kbd, (u32)(outSize - 1));
  Result rc = swkbdShow(&kbd, out, outSize);
  swkbdClose(&kbd);
  return R_SUCCEEDED(rc) && (allowEmpty || out[0]);
}
static bool promptText(const char *header, const char *initial, char *out, size_t outSize) {
  return promptTextAdvanced(header,initial,out,outSize,false,false);
}


// ---------------------------------------------------------------------------
// generic settings screen (list). Returns when the user backs out.
// binds==true -> A on a bind row triggers press-to-bind capture.
// ---------------------------------------------------------------------------
static int choiceIdx(const Opt &o) {
  const char *cur = iniGet(o.key, o.def);
  for (int i=0;i<o.nch;i++) if (!strcmp(o.ch[i].val, cur)) return i;
  return -1;
}
static bool isVulkanOnlyOption(const Opt &o) {
  if (!o.key) return false;
  return !strcmp(o.key,"memory-mapping") || !strcmp(o.key,"high-accuracy") ||
         !strcmp(o.key,"force-full-precision") ||
         !strcmp(o.key,"async-pipeline-compilation") || !strcmp(o.key,"fsr-sharpness") ||
         !strncmp(o.key,"switch-lsfg-",12);
}
// a gated option is inactive (greyed, non-adjustable) while its parent is off
static bool optEnabled(const Opt &o) {
  if(o.key && !strcmp(o.key,"fsr-sharpness") &&
     strcmp(iniGet("screen-filter","Bilinear"),"FSR")) return false;
  if(o.key && !strcmp(o.key,"spirv-shader") &&
     strcmp(iniGet("backend-renderer","Vulkan"),"Zink")) return false;
  if(o.type!=OT_STATUS && isVulkanOnlyOption(o) &&
     strcmp(iniGet("backend-renderer","Vulkan"),"Vulkan")) return false;
  if(o.type!=OT_STATUS && o.key && !strncmp(o.key,"switch-lsfg-",12) &&
     !lsfgDllInstalled()) return false;
  return !o.gateKey || strcmp(iniGet(o.gateKey, ""), o.gateOff) != 0;
}
static void optValue(const Opt &o, char *out, int n) {
  out[0]=0;
  if(o.key && !strcmp(o.key,"spirv-shader") &&
     strcmp(iniGet("backend-renderer","Vulkan"),"Zink")) {
    snprintf(out,n,"%s",LauncherLocalization::Translate("Zink only").data()); return;
  }
  if(isVulkanOnlyOption(o) &&
     strcmp(iniGet("backend-renderer","Vulkan"),"Vulkan")) {
    snprintf(out,n,"%s",LauncherLocalization::Translate("Vulkan only").data()); return;
  }
  if(o.key && !strcmp(o.key,"screen-filter") &&
     strcmp(iniGet("backend-renderer","Vulkan"),"Vulkan")) {
    const char *filter=iniGet(o.key,o.def);
    if(strcmp(filter,"FXAA")) filter="Bilinear";
    snprintf(out,n,"%s",LauncherLocalization::Translate(filter).data()); return;
  }
  if (o.type==OT_CHOICE){ int i=choiceIdx(o); const char *raw=i>=0?o.ch[i].label:iniGet(o.key,o.def);
    const std::string_view shown=i>=0?LauncherLocalization::Translate(raw):std::string_view(raw);
    snprintf(out,n,"%s",shown.data()); }
  else if (o.type==OT_RANGE){ snprintf(out,n,"%s", iniGet(o.key,o.def)); }
  else if (o.type==OT_TEXT){
    const char *v=iniGet(o.key,o.def);
    if(!strcmp(o.key,"Wrapper/SteamGridDBKey"))
      snprintf(out,n,"%s",LauncherLocalization::Translate(v&&*v?"Configured":"Not configured").data());
    else snprintf(out,n,"%s", (v&&*v)?v:"(none)");
  }
  else if (o.type==OT_MULTI){
    const int count=(int)splitCsv(iniGet(o.key,o.def?o.def:"")).size();
    if(count==0) snprintf(out,n,"%s",LauncherLocalization::Translate("None").data());
    else snprintf(out,n,"%d",count);
  }
  else if (o.type==OT_SUBMENU) snprintf(out,n,">");
  else if (o.type==OT_STATUS) {
    if(o.key && !strcmp(o.key,"cpu-cores")) snprintf(out,n,"%d / 4",allowedCpuCores());
    else {const std::string_view shown=LauncherLocalization::Translate(lsfgDllInstalled()?"Installed":"Missing");snprintf(out,n,"%s",shown.data());}
  }
}

// A status row reports a measured fact rather than a stored value, so it paints
// its own verdict: green when the console is set up as intended, red when it is
// not. Returns false for rows that follow the normal selection colours.
static bool optValueVerdictColor(const Opt &o, SDL_Color *out) {
  if(o.type!=OT_STATUS || !o.key || strcmp(o.key,"cpu-cores")) return false;
  *out = allowedCpuCores()>=4 ? (SDL_Color){120,215,130,255} : (SDL_Color){235,125,125,255};
  return true;
}
static void optAdjust(const Opt &o, int dir) {
  if (!optEnabled(o)) return;
  if(o.type==OT_CHOICE && o.key && !strcmp(o.key,"screen-filter") &&
     strcmp(iniGet("backend-renderer","Vulkan"),"Vulkan")) {
    const bool fxaa=!strcmp(iniGet(o.key,o.def),"FXAA");
    iniSet(o.key,fxaa?"Bilinear":"FXAA"); return;
  }
  if (o.type==OT_CHOICE){ int i=choiceIdx(o); if(i<0)i=0; i=(i+dir+o.nch)%o.nch; iniSet(o.key,o.ch[i].val); }
  else if (o.type==OT_RANGE){ int v=atoi(iniGet(o.key,o.def))+dir*o.step; if(v<o.lo)v=o.lo; if(v>o.hi)v=o.hi; char b[24]; snprintf(b,sizeof(b),"%d",v); iniSet(o.key,b); }
}

static bool resetOption(const Opt &option) {
  if(!option.key || !option.def ||
     (option.type!=OT_CHOICE&&option.type!=OT_RANGE&&option.type!=OT_TEXT&&option.type!=OT_MULTI)) return false;
  if(g_active==&g_game) storeRemove(g_game,option.key);
  else storeSet(*g_active,option.key,option.def);
  return true;
}

struct SettingHelpEntry { const char *key; const char *kind; const char *text; };
static const SettingHelpEntry SETTING_HELP[] = {
  {"modules-mode","Compatibility","Controls whether Vita system modules are loaded automatically or from a manual list. Automatic is recommended for most games."},
  {"lle-modules","Compatibility","Chooses which decrypted firmware modules are loaded natively instead of being emulated. It needs Modules mode set to something other than Automatic, and installed firmware. Picking nothing in Manual mode loads no modules at all, which is usually worse than Automatic."},
  {"cpu-opt","Performance","Enables Vita3K CPU optimisations. Disable only when diagnosing a title-specific CPU emulation problem."},
  {"cpu-cores","Performance","How many of the console's four CPU cores this session may use. Four is correct: the emulator keeps vblank, audio and rendering on the fourth core so all three others stay free for the emulated Vita. Three means the launcher was started without a shortcut, and that work has to share the game's cores. Create a shortcut from the installer to get the fourth."},
  {"file-loading-delay","Compatibility","Adds a delay to file reads for games that depend on storage timing. 0 ms disables it."},
  {"switch-lsfg-enabled","Frame generation","Prepares Vulkan LSFG 2x support for this game. Open the in-game quick menu with L + R + Plus to turn generated frames on or off. It does not increase emulation speed."},
  {"switch-lsfg-flow-scale","Frame generation quality","Sets the optical-flow resolution. Quarter is recommended on Switch; Half can improve motion detail but costs more GPU time and memory."},
  {"switch-lsfg-performance","Frame generation performance","Uses LSFG's lighter performance-oriented path. Disable it only when image quality matters more than GPU headroom."},
  {"backend-renderer","Display backend","Chooses the Switch renderer. Vulkan (NVK) is recommended and supports LSFG. OpenGL uses native NVC0, while Zink runs OpenGL on NVK as an additional compatibility path."},
  {"resolution-multiplier","Graphics","Scales the Vita render resolution. Higher values sharpen the image but increase GPU and memory cost."},
  {"memory-mapping","Compatibility","Selects how Vulkan sees Vita GPU memory. External host maps the Vita's own memory straight to the GPU (fastest, default). Double buffer keeps a checked copy instead. Disabled turns mapping off entirely, which some games need."},
  {"high-accuracy","Compatibility","Uses more accurate GPU behavior for games that render incorrectly, at a possible performance cost."},
  {"force-full-precision","Compatibility","Uses full precision for Vulkan shader inputs and outputs. It may fix rendering errors at a performance cost."},
  {"screen-filter","Graphics","Chooses the final image scaling filter. Nearest is sharp, Bilinear is inexpensive, and advanced filters cost more GPU time."},
  {"fsr-sharpness","Graphics","Adjusts FSR sharpening: 0.0 is strongest, 2.0 is softer. The default is 0.2. Requires Vulkan and the FSR screen filter."},
  {"stretch_the_display_area","Graphics","Fills the whole screen instead of preserving the Vita 16:9.4 aspect. The image is distorted, and touch coordinates follow the stretched area."},
  {"v-sync","Graphics","Synchronizes presentation to the display refresh to avoid visible tearing. On drivers that expose only FIFO presentation this remains enabled by the driver."},
  {"anisotropic-filtering","Graphics","Improves texture clarity at oblique angles. Higher levels use additional GPU bandwidth."},
  {"disable-surface-sync","Performance","Skips expensive surface synchronization. The Switch default favors performance, but a game with missing or stale graphics may need it enabled."},
  {"texture-cache","Performance","Caches decoded and uploaded textures. Disabling is intended only for graphics troubleshooting."},
  {"async-pipeline-compilation","Performance","Compiles Vulkan pipelines asynchronously to reduce stalls. Newly encountered effects can appear briefly after compilation."},
  {"show-compile-shaders","Interface","Shows Vita3K's shader compilation indicator while new graphics pipelines are prepared."},
  {"shader-cache","Performance","Reuses compiled shaders between sessions to reduce later stutter. Clearing a broken cache is available from the game menu."},
  {"spirv-shader","Graphics","Uses SPIR-V directly on Zink, with GLSL fallback if unsupported. Native NVC0 disables this extension pending testing."},
  {"import-textures","Modding","Loads replacement textures from Vita3K's texture import directory."},
  {"export-textures","Modding","Dumps textures used by the game for replacement or inspection. This increases SD-card I/O."},
  {"export-as-png","Modding","Writes exported textures as PNG rather than their raw format. PNG is convenient but slower to encode."},
  {"fps-hack","Compatibility","Enables Vita3K's experimental frame-rate hack. It can alter game speed or timing in unsupported titles."},
  {"performance-overlay","Diagnostics","Shows the emulator performance overlay while a game is running."},
  {"performance-overlay-detail","Diagnostics","Controls how much timing and performance information the overlay displays."},
  {"performance-overlay-position","Interface","Places the performance overlay in a screen corner or along the top or bottom edge."},
  {"audio-volume","Audio","Sets Vita3K's output volume before it reaches the Switch system volume."},
  {"ngs-enable","Compatibility","Enables emulation of the Vita NGS audio engine used by many games."},
  {"sys-lang","Vita system","Sets the language reported to Vita software. Games that support it may choose matching text and audio."},
  {"sys-button","Vita system","Chooses whether Cross or Circle is reported as the Vita system confirmation button."},
  {"sys-date-format","Vita system","Sets the date format exposed through Vita system parameters."},
  {"sys-time-format","Vita system","Sets the 12-hour or 24-hour clock format exposed to games."},
  {"pstv-mode","Compatibility","Reports a PlayStation TV environment to software. Some games change controls or block unsupported modes."},
  {"http-enable","Network","Allows Vita software to use Vita3K's HTTP networking implementation."},
  {"http-timeout-attempts","Network","Sets how many polling attempts an HTTP operation receives before timing out."},
  {"http-timeout-sleep-ms","Network","Sets the delay between HTTP timeout polling attempts."},
  {"http-read-end-attempts","Network","Sets how many times Vita3K checks for the end of an HTTP response."},
  {"http-read-end-sleep-ms","Network","Sets the delay between HTTP response-end checks."},
  {"psn-signed-in","Network","Reports a signed-in PSN state to games. It does not sign the console into PlayStation Network."},
  {"adhoc-addr","Network","Selects the local address index used by Vita ad-hoc networking."},
  {"disable-motion","Controls","Disables Vita motion-sensor input derived from the active Switch controller."},
  {"controller-analog-multiplier","Controls","Scales analog stick movement before it is sent to the emulated Vita."},
  {"switch-stick-deadzone","Controls","Ignores small stick movements to reduce drift. Too high a value reduces fine control."},
  {"switch-rear-touch","Controls","Chooses the shoulder-button modifier used with the touchscreen to emulate the Vita rear touch panel. \"Front and rear together\" uses no modifier and reports every touch on both panels at once, for games that ask for the two to be pressed together. Like the other choices here it needs \"Rear touch buttons\" below turned off."},
  {"switch-rear-touch-triggers","Controls","Puts L2, R2, L3 and R3 on the quadrants of the Vita rear touch panel, where most games expect them: ZL and ZR press the top two, the stick clicks the bottom two. Takes over those four buttons, so the rear touch modifier above is ignored."},
  {"switch-button-a","Controls","Chooses which Vita face button is produced by Nintendo Switch A."},
  {"switch-button-b","Controls","Chooses which Vita face button is produced by Nintendo Switch B."},
  {"switch-button-x","Controls","Chooses which Vita face button is produced by Nintendo Switch X."},
  {"switch-button-y","Controls","Chooses which Vita face button is produced by Nintendo Switch Y."},
  {"Wrapper/Theme","Launcher appearance","Selects the launcher background and color treatment. XMB, Bubbles, and Glow include optional animation."},
  {"Wrapper/LauncherRotation","Launcher appearance","Rotates the complete launcher interface and touch coordinates in 90-degree steps."},
  {"Wrapper/GridColumns","Library layout","Sets the number of game covers shown across each library page."},
  {"Wrapper/GridRows","Library layout","Sets the number of cover rows shown on each library page."},
  {"Wrapper/ShowGameTitles","Library layout","Shows or hides game names below cover artwork."},
  {"Wrapper/ShowRegionFlags","Library layout","Shows or hides the region flag in the top-left corner of each game cover."},
  {"Wrapper/ShowCustomSettingsBadges","Library layout","Shows or hides the square badge on games that have per-game settings. The settings themselves are not changed."},
  {"Wrapper/ShowCompatBadges","Library layout","Shows or hides the coloured compatibility dot in the bottom-left corner of each cover. Download the database from Library & storage first."},
  {"Wrapper/UiAnimations","Launcher appearance","Enables animated backgrounds, fades, highlight easing, and cover transitions."},
  {"Wrapper/UiSounds","Launcher audio","Enables navigation, confirmation, and back sound effects in the launcher."},
  {"Wrapper/CheckUpdatesOnStartup","Launcher updates","Checks the official Vita3K-nx GitHub releases in the background when the library opens. Direct HOME shortcut launches skip the check."},
  {"Wrapper/Language","Launcher language","Changes only the SDL launcher's language. Vita system language is configured separately under System."},
  {"Wrapper/SteamGridDBKey","Artwork service","Sets the SteamGridDB API key used for cover and HOME shortcut artwork searches. Leave it empty to remove the key."},
};

static const SettingHelpEntry *settingHelpFor(const Opt &option){
  if(option.key) for(const auto &entry:SETTING_HELP) if(!strcmp(entry.key,option.key)) return &entry;
  return nullptr;
}

static void drawWrapped(TTF_Font *font,int x,int y,int maxWidth,int lineHeight,
                        int maxLines,const char *text,SDL_Color color){
  if(!font||!text||maxWidth<=0||maxLines<=0)return;
  std::string source=text,line;size_t cursor=0;int row=0;
  while(cursor<source.size()&&row<maxLines){
    while(cursor<source.size()&&source[cursor]==' ')cursor++;
    size_t end=source.find(' ',cursor);
    std::string word=source.substr(cursor,end==std::string::npos?source.size()-cursor:end-cursor);
    std::string candidate=line.empty()?word:line+" "+word;
    if(!line.empty()&&textW(font,candidate.c_str())>maxWidth){
      drawText(font,x,y+row*lineHeight,line.c_str(),color);row++;line=word;
    }else line=std::move(candidate);
    if(end==std::string::npos){cursor=source.size();break;}
    cursor=end+1;
  }
  if(row<maxLines&&!line.empty())
    drawText(font,x,y+row*lineHeight,fittedText(font,line,maxWidth).c_str(),color);
}

static void showHelpCard(const char *section,const char *title,const char *kind,
                         const std::string &description,const char *current,
                         const char *scope){
  for(;;){
    if(!beginUiFrame())return;
    SDL_Event event;
    while(pollUiEvent(event)){
      pumpStick(event);int tx=0,ty=0;
      if(touchFeed(event,&tx,&ty)==TOUCH_TAP)return;
      if(event.type==SDL_CONTROLLERBUTTONDOWN&&
         (event.cbutton.button==BTN_CONFIRM||event.cbutton.button==BTN_CANCEL||
          event.cbutton.button==BTN_SETTINGS))return;
    }
    clearUiBackground();
    const int panelWidth=std::min(SW-(g_launcherPortrait?64:120),1000);
    const int panelHeight=std::min(SH-96,500);
    const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
    glassPanel(panelX,panelY,panelWidth,panelHeight);
    border(panelX,panelY,panelWidth,panelHeight,3,COL_SEL);
    const std::string_view shownSection=LauncherLocalization::Translate(section&&*section?section:"Settings");
    const std::string_view shownTitle=LauncherLocalization::Translate(title&&*title?title:"Setting help");
    drawText(g_font_sm,panelX+40,panelY+24,shownSection.data(),COL_DIM);
    drawText(g_font_big,panelX+40,panelY+58,
             fittedText(g_font_big,std::string(shownTitle),panelWidth-80).c_str(),COL_VAL);
    std::string metadata(LauncherLocalization::Translate(kind&&*kind?kind:"Setting"));
    if(scope&&*scope){metadata+="  |  ";metadata+=LauncherLocalization::Translate(scope);}
    drawText(g_font_sm,panelX+40,panelY+114,
             fittedText(g_font_sm,metadata,panelWidth-80).c_str(),COL_SEL);
    int bodyY=panelY+164;
    if(current&&*current){
      const std::string value=std::string(LauncherLocalization::Translate("Current:"))+" "+current;
      drawText(g_font_sm,panelX+40,panelY+146,
               fittedText(g_font_sm,value,panelWidth-80).c_str(),COL_TXT);
      bodyY=panelY+198;
    }
    fillRect(panelX+40,bodyY-18,panelWidth-80,2,(SDL_Color){70,78,92,210});
    // Help prose is exact Vita3K documentation. If a locale has no reviewed
    // translation for this exact sentence, retain the useful English text.
    const std::string_view shownDescription=LauncherLocalization::Translate(description);
    drawWrapped(g_font,panelX+40,bodyY,panelWidth-80,32,7,shownDescription.data(),COL_TXT);
    FootItem closeHints[]={{g_gA,"Close",FA_NONE},{g_gB,"",FA_NONE},{g_gX,"",FA_NONE}};
    drawFooterHints(closeHints,3,panelY+panelHeight-50);
    drawTextC(g_font_sm,SW/2,panelY+panelHeight-24,LauncherLocalization::Translate("Touch anywhere to close").data(),COL_DIM);
    presentUi();waitForNextFrame();
  }
}

static void showOptionHelp(const char *section,const Opt &option,const char *scope){
  char value[128];optValue(option,value,sizeof(value));
  const SettingHelpEntry *help=settingHelpFor(option);
  const char *kind=help?help->kind:(option.type==OT_STATUS?"Required component":"Settings category");
  const std::string description=help?help->text:
    (option.type==OT_STATUS
      ?"Place Lossless.dll in sdmc:/switch/vita3k/lsfg/. The proprietary DLL is never bundled with Vita3K."
      :"Opens this Vita3K settings category.");
  showHelpCard(section,option.label,kind,description,
               option.type==OT_SUBMENU?nullptr:value,scope);
}


// --- shared UI polish: eased selection highlight + a brief screen fade-in -------
static float g_hy = -1;        // animated highlight top (settings list); <0 = snap
static Uint32 g_fxT = 0;       // screen-entry timestamp for the fade-in
static void beginScreenFx(){ g_fxT = SDL_GetTicks(); g_hy = -1; }
static void drawFadeIn(){
  if(!g_uiAnimations) return;
  const int D = 160; int el = (int)(SDL_GetTicks() - g_fxT);
  if (el < D) fillRect(0,0,SW,SH,(SDL_Color){0,0,0,(Uint8)(200*(D-el)/D)});
}
static bool highResolutionUi(){ return g_outputW>=1600; }
// top-bar height, shared by the grid header (gridLayout y0) and the settings header
static int topBarH(){
  if(g_launcherPortrait) return highResolutionUi()?132:104;
  return highResolutionUi()?112:80;
}
// header band: same height + logo as the grid's top bar, with a centred title
static void drawScrollTextR(TTF_Font*f,int xRight,int y,int maxW,const char*s,SDL_Color c); // defined below
static void drawScrollTextL(TTF_Font*f,int x,int y,int maxW,const char*s,SDL_Color c);      // defined below
static void drawHeader(const char *title, const char *ctx){
  int bandH = topBarH() - 4;                        // identical to renderGrid's band
  fillRect(0,0,SW,bandH,COL_PANEL);
  if(!hasAnimatedBackground()) fillRect(0,bandH,SW,2,COL_SEL);
  if(g_launcherPortrait){
    const int logoH=std::min(bandH-18,highResolutionUi()?62:48);
    if(g_logo){SDL_Rect ld={18,(bandH-logoH)/2,logoH,logoH};SDL_RenderCopy(g_ren,g_logo,nullptr,&ld);}
    int titleY=ctx&&*ctx?(highResolutionUi()?18:12):(bandH-TTF_FontHeight(g_font_big))/2;
    const std::string_view rawTitle=title?std::string_view(title):std::string_view{};
    std::string shown=fittedText(g_font_big,std::string(rawTitle),std::max(80,SW-2*(logoH+34)));
    drawTextC(g_font_big,SW/2,titleY,shown.c_str(),COL_VAL);
    if(ctx&&*ctx){
      std::string context=fittedText(g_font_sm,ctx,SW-52);
      drawTextC(g_font_sm,SW/2,bandH-TTF_FontHeight(g_font_sm)-12,context.c_str(),COL_DIM);
    }
    return;
  }
  int lh = bandH - 12;
  if(g_logo){ SDL_Rect ld={26,(bandH-lh)/2,lh,lh}; SDL_RenderCopy(g_ren,g_logo,nullptr,&ld); }
  const std::string_view rawTitle=title?std::string_view(title):std::string_view{};
  std::string shown=fittedText(g_font_big,std::string(rawTitle),SW/2);
  drawTextC(g_font_big,SW/2,(bandH-TTF_FontHeight(g_font_big))/2,shown.c_str(),COL_VAL);
  if (ctx&&*ctx){
    // Bound context text to the space right of the title; ping-pong scroll if still too long.
    int titleRight = SW/2 + textW(g_font_big,rawTitle.data())/2;
    int maxW = (SW-28) - titleRight - 30;
    if(maxW > 40) drawScrollTextR(g_font_sm,SW-28,(bandH-TTF_FontHeight(g_font_sm))/2,maxW,ctx,COL_VAL);
  }
}
static void drawLocalizedHeader(const char *title,const char *ctx){
  const std::string_view shown=LauncherLocalization::Translate(title?title:"");
  drawHeader(shown.data(),ctx);
}
// the centred settings column geometry (one source of truth for render + scroll)
static int settingsRowH(){ return g_launcherPortrait?(highResolutionUi()?78:64):46; }
static int settingsListY(){ return g_launcherPortrait?topBarH()+(highResolutionUi()?38:28):118; }
static int settingsFooterReserve(){ return g_launcherPortrait?(highResolutionUi()?104:88):72; }
static int portraitRowInset(){ return 1; }
#define ROW_H (settingsRowH())
#define LIST_Y0 (settingsListY())
static void listCol(int *colX,int *colW,int *labelX,int *valX){
  int margin=g_launcherPortrait?32:90;
  int w=SW-margin*2;if(w>980)w=980;if(w<160)w=std::max(80,SW-24);
  *colW=w;*colX=(SW-w)/2;*labelX=*colX+(g_launcherPortrait?24:40);*valX=*colX+w-(g_launcherPortrait?24:40);
}
static int listVis(){ int v=(SH-LIST_Y0-settingsFooterReserve())/ROW_H; return v<1?1:v; }

static bool settingsRowNeedsStackedText(const char *label,const char *value,
                                        int labelX,int valX){
  if(!g_launcherPortrait)return false;
  const int gap=highResolutionUi()?32:24;
  return textW(g_font,label)+gap+textW(g_font,value)>valX-labelX;
}

static void drawSettingsRowText(const char *label,const char *value,
                                int slotY,int colW,int labelX,int valX,
                                bool current,SDL_Color labelColor,
                                SDL_Color valueColor,bool scrollValue=false,
                                int rowHeight=0){
  const int actualRowHeight=rowHeight>0?rowHeight:settingsRowH();
  if(settingsRowNeedsStackedText(label,value,labelX,valX)){
    const int maxWidth=std::max(40,valX-labelX);
    const int labelHeight=TTF_FontHeight(g_font);
    const int valueHeight=TTF_FontHeight(g_font_sm);
    const int gap=highResolutionUi()?5:3;
    const int blockHeight=labelHeight+gap+valueHeight;
    const int labelY=slotY+(actualRowHeight-blockHeight)/2;
    if(current)drawScrollTextL(g_font,labelX,labelY,maxWidth,label,labelColor);
    else drawText(g_font,labelX,labelY,fittedText(g_font,label,maxWidth).c_str(),labelColor);
    const int valueY=labelY+labelHeight+gap;
    drawScrollTextR(g_font_sm,valX,valueY,maxWidth,value,valueColor);
    return;
  }
  const int y=slotY+(actualRowHeight-TTF_FontHeight(g_font))/2;
  drawText(g_font,labelX,y,fittedText(g_font,label,std::max(80,colW/2)).c_str(),labelColor);
  if(scrollValue)drawScrollTextR(g_font,valX,y,colW/2-40,value,valueColor);
  else drawTextR(g_font,valX,y,fittedText(g_font,value,std::max(80,colW/2-32)).c_str(),valueColor);
}

static void renderSettings(int scr,int sel,int top,const char *ctx){
  clearUiBackground();
  const Screen &S=g_screens[scr];
  drawLocalizedHeader(S.title, ctx);
  int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
  int vis=listVis();
  glassPanel(colX-12,LIST_Y0-10,colW+24,vis*ROW_H+18);
  // eased highlight bar with a left accent, tracking the selected row.
  const int rowInset=g_launcherPortrait?portraitRowInset():1;
  float ty = (float)(LIST_Y0 + (sel-top)*ROW_H + rowInset);
  g_hy = (!g_uiAnimations||g_hy<0) ? ty : g_hy + (ty-g_hy)*0.30f;
  fillRect(colX,(int)g_hy,colW,ROW_H-rowInset*2,COL_FOCUS);
  fillRect(colX,(int)g_hy,5,ROW_H-rowInset*2,COL_SEL);
  for(int r=0;r<vis && top+r<S.n;r++){
    int i=top+r,slotY=LIST_Y0+r*ROW_H; bool cur=(i==sel); bool en=optEnabled(S.opts[i]);
    SDL_Color lc = !en?(SDL_Color){92,98,110,255}:(cur?COL_VAL:COL_TXT);
    SDL_Color vc = !en?(SDL_Color){92,98,110,255}:(cur?COL_VAL:COL_DIM);
    SDL_Color verdict; if(optValueVerdictColor(S.opts[i],&verdict)) vc=verdict;
    char v[96]; optValue(S.opts[i],v,sizeof(v));
    const std::string_view label=LauncherLocalization::Translate(S.opts[i].label);
    drawSettingsRowText(label.data(),v,slotY,colW,labelX,valX,cur,lc,vc);
  }
  if(S.n>vis){                                  // slim scrollbar at the column's right
    int trH=vis*ROW_H, trX=colX+colW+16, trY=LIST_Y0-2;
    fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
    int thH=trH*vis/S.n, denom=(S.n-vis>0?S.n-vis:1);
    fillRect(trX,trY+(trH-thH)*top/denom,4,thH,COL_SEL);
  }
  FootItem helpFooter[]={{g_gX,"Help",FA_NONE},{g_gY,"Reset",FA_NONE},{g_gB,"Back",FA_NONE}};
  drawFooterHints(helpFooter,3,SH-26);
  drawFadeIn();
  presentUi();
}

// A modal dropdown: pick one label from labels[0..n). Returns the chosen index, or `cur` on cancel.
static int dropdown(const char *title, const char *const *labels, int n, int cur,
                    bool localizeTitle,bool localizeChoices) {
  int sel = (cur < 0 || cur >= n) ? 0 : cur, top = 0;
  const int rowH = 52;
  int vis = (SH - 200) / rowH; if (vis < 1) vis = 1; if (vis > n) vis = n;
  beginScreenFx();
  for (;;) {
    if (!beginUiFrame()) return cur;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(touchScrollList(tk,sel,top,n,vis)) continue;
        if(tk==TOUCH_TAP){ int pw=SW>760?760:SW-160,px=(SW-pw)/2,ly=(SH-(90+vis*rowH))/2+70;
          for(int r=0;r<vis&&top+r<n;r++){ int y=ly+r*rowH; if(ty>=y&&ty<y+rowH&&tx>=px&&tx<px+pw){ return top+r; } }
        } }
      if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n;   break;
        case BTN_CONFIRM: return sel;
        case BTN_CANCEL:  return cur;
      }
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
      if(top<0) top=0;
    }
    clearUiBackground();
    int pw = SW>760?760:SW-160, ph = 90 + vis*rowH, px=(SW-pw)/2, py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    const std::string_view shownTitle=localizeTitle?LauncherLocalization::Translate(title):std::string_view(title);
    drawTextC(g_font_big, SW/2, py+18, shownTitle.data(), COL_VAL);
    int ly = py+70;
    for(int r=0;r<vis && top+r<n;r++){
      int i=top+r, y=ly+r*rowH; bool curr=(i==sel);
      if(curr){ fillRect(px+8,y,pw-16,rowH-4,COL_FOCUS); fillRect(px+8,y,5,rowH-4,COL_SEL); }
      const std::string_view shown=localizeChoices?LauncherLocalization::Translate(labels[i]):std::string_view(labels[i]);
      drawText(g_font, px+34, y+(rowH-TTF_FontHeight(g_font))/2, shown.data(), curr?COL_VAL:COL_TXT);
    }
    if(n>vis){ int trH=vis*rowH,trX=px+pw-12,trY=ly; fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
      int thH=trH*vis/n,dn=(n-vis>0?n-vis:1); fillRect(trX,trY+(trH-thH)*top/dn,4,thH,COL_SEL); }
    drawFadeIn();
    presentUi();
    waitForNextFrame();
  }
}
// ---------------------------------------------------------------------------
// LLE module picker
// ---------------------------------------------------------------------------

static std::vector<std::string> splitCsv(const std::string &value){
  std::vector<std::string> out; std::string current;
  for(char c:value){
    if(c==','){ if(!current.empty()) out.push_back(current); current.clear(); }
    else current.push_back(c);
  }
  if(!current.empty()) out.push_back(current);
  return out;
}

static std::string joinCsv(const std::vector<std::string> &values){
  std::string out;
  for(const std::string &value:values){ if(!out.empty()) out+=','; out+=value; }
  return out;
}

// Decrypted firmware modules the emulator can load. Vita3K matches the stem of
// vs0:sys/external/<name>.suprx case-sensitively, so the names are reported
// exactly as they sit on disk and the .suprx test is lower-case only, mirroring
// config::get_modules_list.
static std::vector<std::string> installedModules(){
  std::vector<std::string> names;
  DIR *dir=opendir(MODULES_DIR);
  if(!dir) return names;
  while(dirent *entry=readdir(dir)){
    const std::string name=entry->d_name;
    if(name.size()<=6||name.compare(name.size()-6,6,".suprx")!=0) continue;
    const std::string stem=name.substr(0,name.size()-6);
    // A comma or a quote would corrupt the stored list or the emitted YAML.
    if(stem.empty()||stem.find_first_of(",\"\r\n")!=std::string::npos) continue;
    names.push_back(stem);
  }
  closedir(dir);
  std::sort(names.begin(),names.end());
  return names;
}

// Multi-select over those modules. A toggles, Y clears, B applies and closes.
static void moduleListPicker(const Opt &option){
  const std::vector<std::string> modules=installedModules();
  if(modules.empty()){
    modalMessageStatic("Manual module list",
      {"No decrypted modules were found.",
       "Install Vita firmware from Library & storage first."});
    return;
  }

  std::vector<std::string> chosen=splitCsv(iniGet(option.key,option.def?option.def:""));
  std::vector<char> enabled(modules.size(),0);
  for(size_t i=0;i<modules.size();i++)
    enabled[i]=std::find(chosen.begin(),chosen.end(),modules[i])!=chosen.end()?1:0;
  // Entries whose .suprx is gone would otherwise be dropped silently on save.
  // Names saved earlier whose .suprx is no longer installed. They are preserved
  // across edits so removing a module temporarily does not silently drop it.
  std::vector<std::string> missing;
  for(const std::string &name:chosen)
    if(std::find(modules.begin(),modules.end(),name)==modules.end()) missing.push_back(name);

  const int count=(int)modules.size();
  int sel=0,top=0;
  const int rowH=52;
  int vis=(SH-200)/rowH; if(vis<1)vis=1; if(vis>count)vis=count;

  const auto apply=[&]{
    std::vector<std::string> out=missing;
    for(size_t i=0;i<modules.size();i++) if(enabled[i]) out.push_back(modules[i]);
    iniSet(option.key,joinCsv(out).c_str());
  };

  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      { int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,count,vis)) continue;
        if(touch==TOUCH_TAP){
          const int pw=SW>760?760:SW-160,px=(SW-pw)/2,ly=(SH-(90+vis*rowH))/2+70;
          for(int r=0;r<vis&&top+r<count;r++){
            const int y=ly+r*rowH;
            if(ty>=y&&ty<y+rowH&&tx>=px&&tx<px+pw){ sel=top+r; enabled[sel]=!enabled[sel]; break; }
          }
          continue;
        } }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(event.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   sel=(sel+count-1)%count; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%count;       break;
        case BTN_CONFIRM: enabled[sel]=!enabled[sel]; break;
        case SDL_CONTROLLER_BUTTON_X:   // Nintendo Y, as the footer shows
          std::fill(enabled.begin(),enabled.end(),0);
          missing.clear();              // otherwise "clear all" cannot drop stale names
          break;
        case BTN_CANCEL: apply(); return;
      }
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
      if(top<0) top=0;
    }

    clearUiBackground();
    const int pw=SW>760?760:SW-160,ph=90+vis*rowH,px=(SW-pw)/2,py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    int active=0; for(char c:enabled) active+=c?1:0;
    char header[96];
    snprintf(header,sizeof(header),"%s  (%d)",
      LauncherLocalization::Translate("Manual module list").data(),active+(int)missing.size());
    drawTextC(g_font_big,SW/2,py+18,header,COL_VAL);
    const int ly=py+70;
    for(int r=0;r<vis&&top+r<count;r++){
      const int i=top+r,y=ly+r*rowH; const bool cur=(i==sel);
      if(cur){ fillRect(px+8,y,pw-16,rowH-4,COL_FOCUS); fillRect(px+8,y,5,rowH-4,COL_SEL); }
      const int box=20,bx=px+30,by=y+(rowH-4-box)/2;
      border(bx,by,box,box,2,cur?COL_SEL:COL_DIM);
      if(enabled[i]) fillRect(bx+5,by+5,box-10,box-10,COL_VAL);
      drawText(g_font,px+70,y+(rowH-TTF_FontHeight(g_font))/2,modules[i].c_str(),cur?COL_VAL:COL_TXT);
    }
    if(count>vis){
      const int trH=vis*rowH,trX=px+pw-12,trY=ly;
      fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
      const int thH=trH*vis/count,dn=(count-vis>0?count-vis:1);
      fillRect(trX,trY+(trH-thH)*top/dn,4,thH,COL_SEL);
    }
    FootItem footer[]={{g_gA,"Toggle",FA_NONE},{g_gY,"Clear all",FA_NONE},{g_gB,"Done",FA_NONE}};
    drawFooterHints(footer,3,SH-26);
    drawFadeIn(); presentUi(); waitForNextFrame();
  }
}

// Open the dropdown for an OT_CHOICE option and store the chosen value.
static void optChoosePopup(const Opt &o) {
  if(o.type!=OT_CHOICE || o.nch<=0) return;
  const char* labels[32]; int n = o.nch>32?32:o.nch;
  for(int i=0;i<n;i++) labels[i]=o.ch[i].label;
  int idx = dropdown(o.label, labels, n, choiceIdx(o));
  if(idx>=0 && idx<o.nch) iniSet(o.key, o.ch[idx].val);
}

static int s_setSel[SCR_COUNT]={0}, s_setTop[SCR_COUNT]={0};   // per-screen position, remembered across re-entry
static void runSettings(int scr, SDL_GameController *pad, const char *ctx) {
  const Screen &S=g_screens[scr];
  int sel=s_setSel[scr], top=s_setTop[scr];
  if(sel<0||sel>=S.n) sel=0;
  if(top<0||top>=S.n) top=0;
  while(sel<S.n-1 && !optEnabled(S.opts[sel])) sel++;   // start on the first enabled row
  auto nav=[&](int dir){ for(int k=0;k<S.n;k++){ sel=(sel+dir+S.n)%S.n; if(optEnabled(S.opts[sel])) break; } };
  beginScreenFx();
  for(;;){
    if (!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);              // touchscreen
        int visible=listVis();
        if(touchScrollList(tk,sel,top,S.n,visible)){ s_setSel[scr]=sel; s_setTop[scr]=top; continue; }
        if(tk==TOUCH_SWIPE_L){ optAdjust(S.opts[sel],-1); continue; }
        if(tk==TOUCH_SWIPE_R){ optAdjust(S.opts[sel],+1); continue; }
        if(tk==TOUCH_TAP){
          if(ty<topBarH() || ty>=SH-40){ return; }                     // tap the title bar or bottom = back
          int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX); int vis=listVis();
          for(int r=0;r<vis && top+r<S.n;r++){ int y=LIST_Y0+r*ROW_H;
            if(ty>=y && ty<y+ROW_H){ int ni=top+r; if(optEnabled(S.opts[ni])){ sel=ni;
              if(tx>=colX+colW/2){ SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); } }
              break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   nav(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav(+1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  optAdjust(S.opts[sel],-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: optAdjust(S.opts[sel], 1); break;
        case BTN_CONFIRM: {
          const Opt &o=S.opts[sel];
          if(o.type==OT_SUBMENU){ runSettings(o.sub,pad,ctx); beginScreenFx(); }
          else if(o.type==OT_TEXT){                            // A: edit via on-screen keyboard
            if(optEnabled(o)){
              char buf[128];
              if(promptText(o.label, iniGet(o.key,o.def), buf, sizeof(buf))) iniSet(o.key,buf);
            }
            beginScreenFx();
          }
          else if(o.type==OT_MULTI){ if(optEnabled(o)) moduleListPicker(o); beginScreenFx(); }
          else if(o.type==OT_CHOICE && o.nch>2 && optEnabled(o)){ optChoosePopup(o); beginScreenFx(); } // dropdown for >2 choices
          else optAdjust(o,1);                                 // toggle for 2-choice / range
          break;
        }
        case BTN_SETTINGS:
          showOptionHelp(S.title,S.opts[sel],ctx&&*ctx?"Per-game override":"Global Vita3K setting");
          beginScreenFx();
          break;
        case SDL_CONTROLLER_BUTTON_X:
          if(resetOption(S.opts[sel])) toastStatic(
            g_active==&g_game?"Setting reset to global":"Setting reset to default");
          break;
        case BTN_CANCEL: return;
      }
      int vis=listVis(); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1; if(top<0)top=0;
      s_setSel[scr]=sel; s_setTop[scr]=top;   // remember position for next entry
    }
    renderSettings(scr,sel,top,ctx);
    waitForNextFrame();
  }
}
// The settings root separates launcher/library tools from Vita3K emulator
// settings. Content selected in the full file manager is staged here before
// Vita3K.nro --install is chainloaded.
static bool g_rescanAfterSettings = false;    // reserved: main() re-scans on return
static bool g_pendingInstall = false;         // set by the firmware/install flows → main() chainloads Vita3K --install

// ---------------------------------------------------------------------------
// Vita install staging shared by the full file manager's contextual PKG,
// license, and firmware actions. Remote files are copied into install/ before
// the emulator starts, so no devoptab or network mount survives the chainload.
// ---------------------------------------------------------------------------
struct BrowseEntry { std::string name; bool isDir; bool isUp; long long size; };
enum class ImportKind { PackageLicense, FirmwarePup };
enum class ImportFileType { Package, License, Firmware, Archive };
struct ImportSelection { std::string path,name,directory; long long size=0; ImportFileType type=ImportFileType::Package; };
struct ImportStageFile { std::string source,name; long long size=0; ImportFileType type=ImportFileType::Package; };

static std::string lowerAscii(std::string text){
  for(char &character:text) character=(char)tolower((unsigned char)character);
  return text;
}
static std::string fileExtensionLower(const std::string &name){
  const size_t dot=name.find_last_of('.');
  return dot==std::string::npos?std::string():lowerAscii(name.substr(dot));
}
static bool importFileType(const std::string &name,ImportKind kind,ImportFileType *type){
  const std::string lower=lowerAscii(name),extension=fileExtensionLower(lower);
  if(kind==ImportKind::FirmwarePup){
    if(extension!=".pup") return false;
    if(type)*type=ImportFileType::Firmware;
    return true;
  }
  if(extension==".pkg"){
    if(type)*type=ImportFileType::Package;
    return true;
  }
  if(lower=="work.bin"||extension==".rif"||extension==".bin"){
    if(type)*type=ImportFileType::License;
    return true;
  }
  // Homebrew and repacks: install_archive handles .vpk/.zip and dispatches .vci.
  if(extension==".vpk"||extension==".zip"||extension==".vci"){
    if(type)*type=ImportFileType::Archive;
    return true;
  }
  return false;
}
static bool safeImportName(const std::string &name){
  if(name.empty()||name=="."||name==".."||name.size()>NAME_MAX||
     name.find('/')!=std::string::npos||name.find('\\')!=std::string::npos) return false;
  for(unsigned char character:name) if(character<0x20||character==0x7f) return false;
  return true;
}

// join a directory and a child name into an absolute sdmc path (root already ends in '/').
static std::string browseChild(const std::string &dir, const std::string &name){
  if(!dir.empty() && dir.back()=='/') return dir + name;
  return dir + "/" + name;
}
static std::string normalizedBrowseRoot(std::string root){
  while(root.size()>1&&root.back()=='/') root.pop_back();
  return root;
}
static bool browseIsRoot(const std::string &dir,const std::string &root){
  return normalizedBrowseRoot(dir)==normalizedBrowseRoot(root);
}
// One level up, constrained to the selected SD/USB/SMB browse root.
static std::string browseParent(const std::string &dir,const std::string &root){
  std::string d = dir;
  while(d.size()>1 && d.back()=='/') d.pop_back();
  const std::string boundary=normalizedBrowseRoot(root);
  if(d==boundary||d.size()<=boundary.size()) return root;
  size_t slash = d.find_last_of('/');
  if(slash==std::string::npos) return root;
  std::string p = d.substr(0, slash);
  if(p.size()<boundary.size()||p.compare(0,boundary.size(),boundary)!=0) return root;
  return p.empty()?root:p;
}
static std::string browseFmtSize(long long bytes){
  char b[32]; double mb=(double)bytes/(1024.0*1024.0);
  if(mb>=1024.0) snprintf(b,sizeof(b),"%.2f GB", mb/1024.0);
  else           snprintf(b,sizeof(b),"%.0f MB", mb);
  return b;
}
// List supported files only, with folders first and a root-constrained parent row.
static bool browseList(const std::string &dir,const std::string &root,ImportKind kind,
                       std::vector<BrowseEntry> &out,std::string *error=nullptr){
  out.clear();
  if(error)error->clear();
  if(!browseIsRoot(dir,root)) out.push_back({ "..", true, true, 0 });
  std::vector<BrowseEntry> dirs, files;
  DIR *directory=opendir(dir.c_str());
  if(!directory){if(error)*error="Could not open this storage folder.";return false;}
  struct dirent *entry;
  while((entry=readdir(directory))){
    if(entry->d_name[0]=='.') continue;
    std::string name=entry->d_name;
    if(!safeImportName(name))continue;
    struct stat fileStat{};
    if(stat(browseChild(dir,name).c_str(),&fileStat)!=0)continue;
    if(S_ISDIR(fileStat.st_mode))dirs.push_back({name,true,false,0});
    else if(S_ISREG(fileStat.st_mode)&&importFileType(name,kind,nullptr))
      files.push_back({name,false,false,(long long)fileStat.st_size});
  }
  closedir(directory);
  auto byName=[](const BrowseEntry&a,const BrowseEntry&b){ return strcasecmp(a.name.c_str(),b.name.c_str())<0; };
  std::sort(dirs.begin(),dirs.end(),byName);
  std::sort(files.begin(),files.end(),byName);
  for(auto &x:dirs)  out.push_back(x);
  for(auto &x:files) out.push_back(x);
  return true;
}

static std::string g_smbMountSignature;
static bool runCancellableStorageTask(const char *title,const std::string &detail,
    const std::function<bool(const std::atomic_bool&,std::string&)> &task,std::string &error){
  std::atomic_bool cancel{false},complete{false};bool ok=false;std::string workerError;
  std::thread worker([&]{ok=task(cancel,workerError);complete.store(true,std::memory_order_release);
    SDL_Event wake{};wake.type=USB_STATUS_EVENT;SDL_PushEvent(&wake);});
  beginScreenFx();
  while(!complete.load(std::memory_order_acquire)){
    if(!beginUiFrame()){cancel.store(true,std::memory_order_release);break;}
    SDL_Event event;while(pollUiEvent(event)){pumpStick(event);int x=0,y=0;const TouchKind touch=touchFeed(event,&x,&y);
      if((event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL)||
         (touch==TOUCH_TAP&&y>=SH-80))cancel.store(true,std::memory_order_release);}
    clearUiBackground();drawLocalizedHeader(title,nullptr);
    drawTextC(g_font,SW/2,SH/2-18,fittedText(g_font,detail,SW-140).c_str(),COL_TXT);
    if(cancel.load(std::memory_order_acquire))drawTextC(g_font_sm,SW/2,SH/2+30,"Cancelling...",COL_DIM);
    else {FootItem footer[]={{g_gB,"Cancel",FA_NONE}};drawFooterHints(footer,1,SH-26);}
    drawFadeIn();presentUi();waitForNextFrame();
  }
  cancel.store(true,std::memory_order_release);if(worker.joinable())worker.join();
  error=std::move(workerError);return ok;
}

static bool mountSmbWithUi(const Vita3KLauncher::Storage::SmbShare &share,
                           bool reconnect,std::string &error){
  const std::string detail=(reconnect?"Reconnecting to ":"Connecting to ")+
    (share.name.empty()?share.server:share.name);
  return runCancellableStorageTask(reconnect?"Reconnect SMB share":"Connect SMB share",detail,
    [&](const std::atomic_bool &cancel,std::string &failure){
      return reconnect?Vita3KLauncher::Storage::ReconnectSmb(share.id,&failure,&cancel):
        Vita3KLauncher::Storage::MountSmb(share,&failure,&cancel);},error);
}

static bool appendConfiguredSmb(std::vector<Vita3KLauncher::Storage::Location> &locations,
                                std::string &error){
  using namespace Vita3KLauncher::Storage;
  error.clear();
  const bool enabled=strcmp(storeGet(g_global,"Wrapper/SmbEnabled","false"),"false")!=0;
  if(!enabled){
    if(IsSmbMounted("main")) UnmountSmb("main",nullptr);
    g_smbMountSignature.clear();
    return false;
  }
  if(!g_networkReady){error="SMB is unavailable because network initialization failed.";return false;}
  SmbShare share;
  share.id="main";
  share.name="SMB";
  share.server=trim(storeGet(g_global,"Wrapper/SmbServer",""));
  share.share=trim(storeGet(g_global,"Wrapper/SmbShare",""));
  share.path=trim(storeGet(g_global,"Wrapper/SmbPath",""));
  share.user=trim(storeGet(g_global,"Wrapper/SmbUser",""));
  share.password=storeGet(g_global,"Wrapper/SmbPassword","");
  share.domain=trim(storeGet(g_global,"Wrapper/SmbDomain",""));
  if(share.server.empty()||share.share.empty()){
    error="Configure both SMB server and share in Launcher settings.";
    return false;
  }
  const std::string signature=share.server+'\n'+share.share+'\n'+share.path+'\n'+share.user+'\n'+share.password+'\n'+share.domain;
  if(signature!=g_smbMountSignature){
    if(!UnmountSmb("main",&error))return false;
    g_smbMountSignature.clear();
  }
  if(!IsSmbMounted("main")){
    if(!mountSmbWithUi(share,false,error))return false;
    g_smbMountSignature=signature;
  }
  std::string path=SmbBrowsePath(share);
  if(path.empty()){error="The configured SMB start folder is invalid.";return false;}
  Vita3KLauncher::Storage::Location location;
  location.path=path;location.label="SMB - "+share.server+"/"+share.share;
  location.id="smb:"+share.id;location.mount_alias=Vita3KLauncher::Storage::SmbRootPath(share.id);
  locations.push_back(std::move(location));
  return true;
}

static std::vector<Vita3KLauncher::Storage::Location> importLocations(std::string &warning){
  std::vector<Vita3KLauncher::Storage::Location> locations=Vita3KLauncher::Storage::ListLocalLocations();
  std::string smbWarning;
  appendConfiguredSmb(locations,smbWarning);
  warning.clear();
  if(!g_usbReady.load(std::memory_order_acquire)){
    if(!g_storageWorkerComplete.load(std::memory_order_acquire))warning="USB storage is still being initialized";
    else if(!g_usbError.empty())warning=g_usbError;
  }
  if(!smbWarning.empty()){
    if(!warning.empty())warning+="  ";
    warning+=smbWarning;
  }
  return locations;
}

static bool chooseImportLocation(Vita3KLauncher::Storage::Location &selected){
  using Vita3KLauncher::Storage::Location;
  std::string warning;
  std::vector<Location> locations=importLocations(warning);
  std::uint64_t generation=Vita3KLauncher::Storage::UsbStatusGeneration();
  int sel=0,top=0;
  beginScreenFx();
  for(;;){
    if(!beginUiFrame())return false;
    const std::uint64_t currentGeneration=Vita3KLauncher::Storage::UsbStatusGeneration();
    if(currentGeneration!=generation){
      std::string keep=locations.empty()?std::string():locations[std::min(sel,(int)locations.size()-1)].path;
      locations=importLocations(warning);generation=currentGeneration;sel=0;
      for(size_t index=0;index<locations.size();++index)if(locations[index].path==keep){sel=(int)index;break;}
      top=0;
    }
    const int rowH=ROW_H,listY=LIST_Y0;
    int visible=std::max(1,(SH-listY-settingsFooterReserve())/rowH);
    if(visible>(int)locations.size())visible=(int)locations.size();
    SDL_Event event;navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);
      if(touchScrollList(touch,sel,top,(int)locations.size(),std::max(1,visible)))continue;
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40)return false;
        for(int row=0;row<visible&&top+row<(int)locations.size();++row)
          if(ty>=listY+row*rowH&&ty<listY+(row+1)*rowH){sel=top+row;selected=locations[sel];return true;}
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN)continue;
      const int count=(int)locations.size();
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP&&count)sel=(sel+count-1)%count;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN&&count)sel=(sel+1)%count;
      else if(event.cbutton.button==BTN_CONFIRM&&count){selected=locations[sel];return true;}
      else if(event.cbutton.button==BTN_CANCEL)return false;
      if(sel<top) top=sel;
      if(sel>=top+std::max(1,visible)) top=sel-std::max(1,visible)+1;
    }
    clearUiBackground();drawLocalizedHeader("Choose import storage",nullptr);
    int colX,colW,labelX,valX;listCol(&colX,&colW,&labelX,&valX);
    if(visible>0)glassPanel(colX-12,listY-10,colW+24,visible*rowH+18);
    for(int row=0;row<visible&&top+row<(int)locations.size();++row){
      int index=top+row,y=listY+row*rowH,current=index==sel;
      if(current){fillRect(colX,y,colW,rowH-2,COL_FOCUS);fillRect(colX,y,5,rowH-2,COL_SEL);}
      std::string label=fittedText(g_font,locations[index].label,colW-64);
      drawText(g_font,labelX,y+(rowH-TTF_FontHeight(g_font))/2,label.c_str(),current?COL_VAL:COL_TXT);
    }
    if(locations.empty())drawTextC(g_font,SW/2,SH/2,"No storage locations are available",COL_DIM);
    if(!warning.empty()){
      std::string shown=fittedText(g_font_sm,warning,SW-80);
      drawTextC(g_font_sm,SW/2,SH-76,shown.c_str(),(SDL_Color){240,160,95,255});
    }
    FootItem footer[]={{g_gA,"Open",FA_NONE},{g_gB,"Back",FA_NONE}};
    drawFooterHints(footer,2,SH-26);drawFadeIn();presentUi();waitForNextFrame();
  }
}

static bool validateImportFile(const std::string &path,ImportFileType type,long long expectedSize,
                               std::string &error){
  struct stat fileStat{};
  if(stat(path.c_str(),&fileStat)!=0||!S_ISREG(fileStat.st_mode)||fileStat.st_size<=0||
     (expectedSize>0&&fileStat.st_size!=expectedSize)){
    error="The selected source file is missing or changed while importing.";
    return false;
  }
  FILE *file=fopen(path.c_str(),"rb");
  if(!file){error="Could not open the selected source file.";return false;}
  unsigned char header[8]{};
  const size_t read=fread(header,1,sizeof(header),file);
  const bool readError=ferror(file)!=0;
  fclose(file);
  if(readError){error="Could not read the selected source file.";return false;}
  if(type==ImportFileType::Package){
    if(read<4||header[0]!=0x7f||header[1]!='P'||header[2]!='K'||header[3]!='G'){
      error="The selected file does not contain a valid Vita PKG header.";return false;
    }
  } else if(type==ImportFileType::License){
    if(fileStat.st_size!=512){error="A Vita work.bin/.rif license must be exactly 512 bytes.";return false;}
  } else if(type==ImportFileType::Archive){
    // .vpk and .zip are ZIP containers; a .vci carries its own header, which the
    // emulator validates, so only reject something that is clearly neither.
    const bool zip=read>=4&&header[0]=='P'&&header[1]=='K'&&
                   (header[2]==0x03||header[2]==0x05||header[2]==0x07);
    if(!zip&&fileExtensionLower(lowerAscii(path))!=".vci"){
      error="The selected file is not a readable .vpk/.zip archive.";return false;
    }
  } else if(read<5||memcmp(header,"SCEUF",5)!=0||fileStat.st_size<(1<<20)||
            fileStat.st_size>(512LL<<20)){
    error="The selected file does not contain a valid Vita firmware PUP header.";return false;
  }
  return true;
}

static bool recoverImportDestination(const std::string &destination,std::string &error){
  const std::string temporary=destination+".import.tmp";
  const std::string backup=destination+".import.old";
  bool currentExists=false,temporaryExists=false,backupExists=false;
  if(!queryRegularFile(destination,currentExists)||!queryRegularFile(temporary,temporaryExists)||
     !queryRegularFile(backup,backupExists)){
    error="An import destination is not a regular file.";return false;
  }
  bool changed=false;
  if(temporaryExists){if(remove(temporary.c_str())!=0){error="Could not remove an incomplete import.";return false;}changed=true;}
  if(!currentExists&&backupExists){
    if(rename(backup.c_str(),destination.c_str())!=0){error="Could not restore an interrupted import.";return false;}
    currentExists=true;backupExists=false;changed=true;
  }
  if(currentExists&&backupExists){
    if(remove(backup.c_str())!=0){error="Could not remove an old import backup.";return false;}
    changed=true;
  }
  if(changed&&R_FAILED(fsdevCommitDevice("sdmc"))){error="Could not commit import recovery to the SD card.";return false;}
  return true;
}

static bool recoverInterruptedImports(std::string &error){
  error.clear();
  DIR *directory=opendir(INSTALL_DIR);
  if(!directory){
    if(errno==ENOENT)return true;
    error="Could not scan the Vita3K install folder for interrupted imports.";
    return false;
  }
  std::vector<std::string> temporary,backups;
  struct dirent *entry;
  while((entry=readdir(directory))){
    const std::string name=entry->d_name;
    if(!safeImportName(name))continue;
    if(name.size()>11&&name.compare(name.size()-11,11,".import.tmp")==0)temporary.push_back(browseChild(INSTALL_DIR,name));
    else if(name.size()>11&&name.compare(name.size()-11,11,".import.old")==0)backups.push_back(browseChild(INSTALL_DIR,name));
  }
  closedir(directory);
  bool changed=false;
  for(const std::string &path:temporary){
    if(remove(path.c_str())!=0&&errno!=ENOENT){error="Could not remove an incomplete staged import.";return false;}
    changed=true;
  }
  for(const std::string &backup:backups){
    const std::string destination=backup.substr(0,backup.size()-11);
    bool currentExists=false;
    if(!queryRegularFile(destination,currentExists)){error="An interrupted import destination is invalid.";return false;}
    if(currentExists){
      if(remove(backup.c_str())!=0){error="Could not clean up an old import backup.";return false;}
    } else if(rename(backup.c_str(),destination.c_str())!=0){
      error="Could not restore a file from an interrupted import.";return false;
    }
    changed=true;
  }
  if(changed&&R_FAILED(fsdevCommitDevice("sdmc"))){error="Could not commit import recovery to the SD card.";return false;}
  return true;
}

struct ImportWorkerProgress {
  std::atomic_bool cancel{false};
  std::atomic<std::uint64_t> total{0},done{0};
  std::mutex mutex;
  std::string current;
};

static void setImportWorkerCurrent(ImportWorkerProgress &progress,const std::string &value){
  std::lock_guard<std::mutex> lock(progress.mutex);progress.current=value;
}

struct PreparedImport {
  ImportStageFile file;
  std::string destination,temporary,backup;
  bool skip=false,hadCurrent=false,activated=false;
};

static bool rollbackPreparedImports(std::vector<PreparedImport> &prepared,std::string &error){
  bool ok=true;
  for(auto iterator=prepared.rbegin();iterator!=prepared.rend();++iterator){
    PreparedImport &item=*iterator;
    bool destinationRemoved=true;
    if(item.activated&&remove(item.destination.c_str())!=0&&errno!=ENOENT){
      destinationRemoved=false;ok=false;
    }
    if(item.hadCurrent){
      if(destinationRemoved&&rename(item.backup.c_str(),item.destination.c_str())==0)
        item.hadCurrent=false;
      else
        ok=false;
    }
    if(remove(item.temporary.c_str())!=0&&errno!=ENOENT)ok=false;
  }
  if(R_FAILED(fsdevCommitDevice("sdmc")))ok=false;
  if(!ok){
    if(!error.empty())error+=' ';
    error+="Rollback was incomplete; preserved .import.old files will be recovered at the next launcher start.";
  }
  return ok;
}

static bool stageImportFilesWorker(const std::vector<ImportStageFile> &files,
                                   std::vector<std::string> &destinations,std::string &error,
                                   ImportWorkerProgress &progress){
  destinations.clear();error.clear();
  if(files.empty()){error="No files were selected for import.";return false;}
  if(mkdir(INSTALL_DIR,0777)!=0&&errno!=EEXIST){error="Could not create the Vita3K install folder.";return false;}
  std::vector<PreparedImport> prepared;
  prepared.reserve(files.size());
  std::uint64_t bytesToCopy=0;
  for(const ImportStageFile &file:files){
    if(!safeImportName(file.name)||file.size<=0){error="An imported filename or size is invalid.";return false;}
    const std::string lowerName=lowerAscii(file.name);
    for(const PreparedImport &existing:prepared)
      if(lowerAscii(existing.file.name)==lowerName){error="Two selected files use the same install filename.";return false;}
    if(!validateImportFile(file.source,file.type,file.size,error))return false;
    PreparedImport item;
    item.file=file;item.destination=browseChild(INSTALL_DIR,file.name);
    item.temporary=item.destination+".import.tmp";item.backup=item.destination+".import.old";
    if(!recoverImportDestination(item.destination,error))return false;
    item.skip=strcasecmp(item.file.source.c_str(),item.destination.c_str())==0;
    if(!item.skip)bytesToCopy+=(std::uint64_t)item.file.size;
    prepared.emplace_back(std::move(item));
  }
  struct statvfs space{};
  if(bytesToCopy&&statvfs(INSTALL_DIR,&space)==0){
    const unsigned __int128 available=(unsigned __int128)space.f_bavail*space.f_frsize;
    if(available<(unsigned __int128)bytesToCopy+8*1024*1024){error="There is not enough free SD-card space for this import.";return false;}
  }
  progress.total.store(bytesToCopy,std::memory_order_release);

  static std::array<unsigned char,256*1024> buffer{};
  std::uint64_t copiedTotal=0;
  for(PreparedImport &item:prepared){
    if(progress.cancel.load(std::memory_order_acquire)){
      error="Import cancelled.";rollbackPreparedImports(prepared,error);return false;
    }
    setImportWorkerCurrent(progress,item.file.name);
    if(item.skip){destinations.push_back(item.destination);continue;}
    remove(item.temporary.c_str());
    FILE *source=fopen(item.file.source.c_str(),"rb");
    FILE *destination=source?fopen(item.temporary.c_str(),"wb"):nullptr;
    struct stat openedSource{};
    if(!source||!destination||fstat(fileno(source),&openedSource)!=0||
       !S_ISREG(openedSource.st_mode)||openedSource.st_size!=item.file.size){
      if(source) fclose(source);
      if(destination) fclose(destination);
      remove(item.temporary.c_str());error="Could not open an import source or SD staging file.";
      rollbackPreparedImports(prepared,error);return false;
    }
    std::uint64_t copiedFile=0;
    bool ok=true,cancelled=false;
    while(copiedFile<(std::uint64_t)item.file.size){
      if(progress.cancel.load(std::memory_order_acquire)){cancelled=true;ok=false;break;}
      const size_t wanted=(size_t)std::min<std::uint64_t>(buffer.size(),(std::uint64_t)item.file.size-copiedFile);
      const size_t count=fread(buffer.data(),1,wanted,source);
      if(count==0||fwrite(buffer.data(),1,count,destination)!=count){ok=false;break;}
      copiedFile+=count;copiedTotal+=count;
      progress.done.store(copiedTotal,std::memory_order_release);
    }
    unsigned char extra=0;
    if(ok&&copiedFile==(std::uint64_t)item.file.size&&fread(&extra,1,1,source)!=0){
      ok=false;error="The selected source file changed while it was being imported.";
    }
    struct stat finishedSource{};
    if(ferror(source)||fstat(fileno(source),&finishedSource)!=0||
       !S_ISREG(finishedSource.st_mode)||finishedSource.st_size!=item.file.size){
      ok=false;
      if(error.empty())error="The selected source file changed while it was being imported.";
    }
    if(fclose(source)!=0)ok=false;
    if(fflush(destination)!=0||fsync(fileno(destination))!=0)ok=false;
    if(fclose(destination)!=0)ok=false;
    if(!ok||copiedFile!=(std::uint64_t)item.file.size||
       !validateImportFile(item.temporary,item.file.type,item.file.size,error)){
      remove(item.temporary.c_str());
      if(cancelled)error="Import cancelled.";else if(error.empty())error="The imported file could not be copied completely.";
      rollbackPreparedImports(prepared,error);return false;
    }
    destinations.push_back(item.destination);
  }

  for(PreparedImport &item:prepared){
    if(item.skip)continue;
    bool exists=false;
    if(!queryRegularFile(item.destination,exists)){
      error="An import destination changed unexpectedly.";
      rollbackPreparedImports(prepared,error);return false;
    }
    if(exists&&rename(item.destination.c_str(),item.backup.c_str())!=0){
      error="Could not preserve an existing staged file.";
      rollbackPreparedImports(prepared,error);return false;
    }
    item.hadCurrent=exists;
    if(rename(item.temporary.c_str(),item.destination.c_str())!=0){
      error="Could not activate an imported file.";
      rollbackPreparedImports(prepared,error);return false;
    }
    item.activated=true;
  }
  if(R_FAILED(fsdevCommitDevice("sdmc"))){
    error="Could not commit imported files to the SD card.";
    rollbackPreparedImports(prepared,error);return false;
  }
  for(PreparedImport &item:prepared){
    if(!item.skip&&!validateImportFile(item.destination,item.file.type,item.file.size,error)){
      rollbackPreparedImports(prepared,error);return false;
    }
  }
  bool cleanupOk=true;
  for(PreparedImport &item:prepared)if(item.hadCurrent&&remove(item.backup.c_str())!=0)cleanupOk=false;
  if(R_FAILED(fsdevCommitDevice("sdmc")))cleanupOk=false;
  if(!cleanupOk){error="Files were imported, but an old staging backup could not be removed safely.";return false;}
  return true;
}

static bool stageImportFiles(const std::vector<ImportStageFile> &files,
                             std::vector<std::string> &destinations,std::string &error){
  ImportWorkerProgress progress;std::atomic_bool complete{false};bool ok=false;
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  std::thread worker([&]{ok=stageImportFilesWorker(files,destinations,error,progress);
    complete.store(true,std::memory_order_release);SDL_Event wake{};wake.type=USB_STATUS_EVENT;SDL_PushEvent(&wake);});
  while(!complete.load(std::memory_order_acquire)){
    if(!beginUiFrame()){progress.cancel.store(true,std::memory_order_release);break;}
    SDL_Event event;while(pollUiEvent(event)){pumpStick(event);
      if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL)
        progress.cancel.store(true,std::memory_order_release);}
    const std::uint64_t total=progress.total.load(std::memory_order_acquire),done=progress.done.load(std::memory_order_acquire);
    std::string current;{std::lock_guard<std::mutex> lock(progress.mutex);current=progress.current;}
    const int percent=total?(int)((unsigned __int128)std::min(done,total)*100/total):0;
    const bool cancelling=progress.cancel.load(std::memory_order_acquire);
    const std::string status=cancelling?std::string(LauncherLocalization::Translate("Cancelling import..."))
      :std::string(LauncherLocalization::Translate(current.empty()?"Preparing import...":"Importing"));
    drawSetupProgress(percent,status.c_str(),cancelling?nullptr:current.c_str(),!cancelling);
    waitForNextFrame();
  }
  if(worker.joinable())
    worker.join();
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  return ok;
}
// left-anchored name bounded to maxW: fits -> draw; overflow -> ellipsis.
static void drawEllipsisL(TTF_Font*f,int x,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0||!s||!*s) return;
  if(textW(f,s)<=maxW){ drawText(f,x,y,s,c); return; }
  const std::string original=s;
  const std::string &shortened=ellipsizedText(f,original,maxW);
  drawText(f,x,y,shortened.c_str(),c);
}

static bool pathWithinBrowseRoot(const std::string &path,const std::string &root){
  const std::string normalizedPath=normalizedBrowseRoot(path),normalizedRoot=normalizedBrowseRoot(root);
  return normalizedPath==normalizedRoot||
    (normalizedPath.size()>normalizedRoot.size()&&normalizedPath.compare(0,normalizedRoot.size(),normalizedRoot)==0&&
     normalizedPath[normalizedRoot.size()]=='/');
}

static bool browseStorageFile(ImportKind kind,ImportSelection &selection){
  Vita3KLauncher::Storage::Location location;
  if(!chooseImportLocation(location))return false;
  const std::string root=location.path;
  std::string dir=storeGet(g_global,"Wrapper/LastImportDir",root.c_str());
  if(!pathWithinBrowseRoot(dir,root))dir=root;
  {DIR *directory=opendir(dir.c_str());if(directory)closedir(directory);else dir=root;}
  std::vector<BrowseEntry> entries;
  std::string listError;
  if(!browseList(dir,root,kind,entries,&listError)){
    modalMessage("Storage unavailable",{listError,location.label});return false;
  }
  int sel=0, top=0;

  // layout (fixed for the session; mirrors the settings list column + row height)
  const int bandH  = topBarH()-4;
  const int pathY  = bandH + 10;
  const int listY0 = pathY + TTF_FontHeight(g_font_sm) + 18;
  const int rowH   = ROW_H;
  const int hintY  = SH - 40;
  int vis = (hintY - 14 - listY0)/rowH; if(vis<1) vis=1;
  int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
  const int fh = TTF_FontHeight(g_font), fhs = TTF_FontHeight(g_font_sm);

  auto enterDir=[&](const std::string &next)->bool{
    std::vector<BrowseEntry> updated;std::string failure;
    if(!pathWithinBrowseRoot(next,root)||!browseList(next,root,kind,updated,&failure)){
      modalMessage("Could not open folder",{failure.empty()?uiText("The selected folder is outside this storage root."):failure});
      beginScreenFx();return false;
    }
    dir=next;entries.swap(updated);sel=0;top=0;g_hy=-1;return true;
  };
  std::uint64_t usbGeneration=Vita3KLauncher::Storage::UsbStatusGeneration();
  beginScreenFx();
  for(;;){
    if (!beginUiFrame()) return false;
    const std::uint64_t currentGeneration=Vita3KLauncher::Storage::UsbStatusGeneration();
    if(currentGeneration!=usbGeneration){
      usbGeneration=currentGeneration;
      if(root.rfind("ums",0)==0){
        bool found=false;
        for(const auto &usb:Vita3KLauncher::Storage::ListUsbLocations())if(normalizedBrowseRoot(usb.path)==normalizedBrowseRoot(root)){found=true;break;}
        if(!found){modalMessageStatic("USB storage removed",{"The selected USB device was disconnected.","No partial import file was kept."});return false;}
      }
    }
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);              // touchscreen
        if(touchScrollList(tk,sel,top,(int)entries.size(),vis)) continue;
        if(tk==TOUCH_TAP){
          if(ty<bandH){ return false; }
          if(ty>=SH-40){ if(browseIsRoot(dir,root)) return false; enterDir(browseParent(dir,root)); continue; }
          for(int r=0;r<vis && top+r<(int)entries.size();r++){ int y=listY0+r*rowH;
            if(ty>=y && ty<y+rowH){ sel=top+r;
              SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      int n=(int)entries.size();
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   if(n) sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: if(n) sel=(sel+1)%n;   break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  sel-=vis; if(sel<0) sel=0; break;      // page jump
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: sel+=vis; if(sel>=n) sel=n?n-1:0; break;
        case BTN_CONFIRM: {
          if(!n) break;
          BrowseEntry &en = entries[sel];
          if(en.isUp)        enterDir(browseParent(dir,root));
          else if(en.isDir)  enterDir(browseChild(dir,en.name));
          else {
            ImportFileType type;
            if(!importFileType(en.name,kind,&type))break;
            selection={browseChild(dir,en.name),en.name,dir,en.size,type};
            storeSet(g_global,"Wrapper/LastImportDir",dir.c_str());
            storeSave(g_global,LAUNCHER_INI);
            return true;
          }
          break;
        }
        case BTN_CANCEL:
          if(browseIsRoot(dir,root)) return false;
          enterDir(browseParent(dir,root));
          break;
      }
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
      if(top<0) top=0;
    }

    // render ------------------------------------------------------------------
    clearUiBackground();
    drawLocalizedHeader(kind==ImportKind::FirmwarePup?"Import Vita firmware PUP":"Import Vita package / license", location.label.c_str());
    int n=(int)entries.size();
    drawScrollTextL(g_font, labelX, pathY, valX-labelX, dir.c_str(), COL_HI);   // current path
    glassPanel(colX-12,listY0-10,colW+24,vis*rowH+18);
    if(n){                                        // eased highlight bar + left accent (matches settings)
      float ty=(float)(listY0+(sel-top)*rowH+1);
      g_hy=(!g_uiAnimations||g_hy<0)?ty:g_hy+(ty-g_hy)*0.30f;
      fillRect(colX,(int)g_hy,colW,rowH-2,COL_FOCUS);
      fillRect(colX,(int)g_hy,5,rowH-2,COL_SEL);
    }
    for(int r=0;r<vis && top+r<n;r++){
      int i=top+r, y=listY0+r*rowH+(rowH-fh)/2, ys=listY0+r*rowH+(rowH-fhs)/2; bool cur=(i==sel);
      BrowseEntry &en=entries[i];
      SDL_Color nc = cur?COL_VAL:(en.isDir?COL_HI:COL_TXT);            // folders drawn in the accent colour
      if(en.isUp){ drawText(g_font,labelX,y,".. (up one level)",nc); continue; }
      if(en.isDir){
        std::string nm = en.name + "/";
        if(cur) drawScrollTextL(g_font,labelX,y,valX-labelX-96,nm.c_str(),nc);
        else    drawEllipsisL  (g_font,labelX,y,valX-labelX-96,nm.c_str(),nc);
        drawTextR(g_font_sm,valX,ys,"folder",cur?COL_VAL:COL_DIM);
      } else {
        if(cur) drawScrollTextL(g_font,labelX,y,valX-labelX-150,en.name.c_str(),nc);
        else    drawEllipsisL  (g_font,labelX,y,valX-labelX-150,en.name.c_str(),nc);
        std::string sz=browseFmtSize(en.size);
        drawTextR(g_font_sm,valX,ys,sz.c_str(),cur?COL_VAL:COL_DIM);
      }
    }
    if(n==0) drawTextC(g_font,SW/2,listY0+rowH,
      kind==ImportKind::FirmwarePup?"No folders or .pup files here":"No Vita .pkg, .vpk, .zip, .vci or license files here",COL_DIM);
    if(n>vis){                                    // slim scrollbar (matches settings)
      int trH=vis*rowH, trX=colX+colW+16, trY=listY0-2;
      fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
      int thH=trH*vis/n, denom=(n-vis>0?n-vis:1);
      fillRect(trX,trY+(trH-thH)*top/denom,4,thH,COL_SEL);
    }
    drawTextC(g_font_sm, SW/2, hintY, "A: Open / Select      B: Up / Back", COL_DIM);
    drawFadeIn();
    presentUi();
    waitForNextFrame();
  }
}

static bool findCompanionLicense(const ImportSelection &package,ImportStageFile &license){
  DIR *directory=opendir(package.directory.c_str());
  if(!directory)return false;
  std::vector<ImportStageFile> candidates;
  const std::string packageStem=lowerAscii(package.name.substr(0,package.name.size()-fileExtensionLower(package.name).size()));
  struct dirent *entry;
  while((entry=readdir(directory))){
    const std::string name=entry->d_name;
    ImportFileType type;
    if(!safeImportName(name)||!importFileType(name,ImportKind::PackageLicense,&type)||type!=ImportFileType::License)continue;
    const std::string path=browseChild(package.directory,name);
    struct stat fileStat{};
    if(stat(path.c_str(),&fileStat)!=0||!S_ISREG(fileStat.st_mode)||fileStat.st_size!=512)continue;
    candidates.push_back({path,name,(long long)fileStat.st_size,type});
  }
  closedir(directory);
  if(candidates.empty())return false;
  auto priority=[&](const ImportStageFile &candidate){
    const std::string lower=lowerAscii(candidate.name);
    if(lower=="work.bin")return 0;
    const std::string extension=fileExtensionLower(lower);
    const std::string stem=lower.substr(0,lower.size()-extension.size());
    return stem==packageStem?1:2;
  };
  std::stable_sort(candidates.begin(),candidates.end(),[&](const ImportStageFile &left,const ImportStageFile &right){return priority(left)<priority(right);});
  if(priority(candidates.front())==2&&candidates.size()!=1)return false;
  license=candidates.front();
  return true;
}

// Stage a Vita PKG or license selected by either the legacy picker or the full
// file manager, then hand off to the emulator installer. A package automatically
// offers the best adjacent 512-byte work.bin/.rif companion in the same folder.
// A retail PKG needs a license. When no work.bin sits next to it, offer to type
// the zRIF key instead; install_pkg turns that into the license itself. Returns
// false only when the user backs out of installing altogether.
static bool offerZrifForPackage(bool &staged){
  staged=false;
  static const char *choices[]={"Enter the zRIF license key","Install without a license","Cancel"};
  const int choice=dropdown("No license found for this package",choices,3,-1);
  if(choice<0||choice==2)return false;
  if(choice==1)return true;
  std::vector<char> typed(2048);
  if(!promptTextAdvanced("zRIF license key","",typed.data(),typed.size(),false,false))return false;
  const std::string key=trim(typed.data());
  if(key.empty()){
    modalMessageStatic("No key entered",{"Nothing was entered, so the package was not staged."});
    return false;
  }
  if(!writeAtomicText(INSTALL_ZRIF,key+"\n")){
    modalMessageStatic("Could not stage the key",{"The zRIF could not be written to the SD card."});
    return false;
  }
  staged=true;
  return true;
}

// A package or archive already on the SD card is read where it lies, instead of
// being copied into install/ only to be extracted a moment later - that copy is
// the slow first progress bar, and for a multi-gigabyte title it is most of the
// wait. The installer looks for a package's license in the target's own folder,
// so a companion work.bin is still found. USB and SMB sources must still be
// staged: those devices are not mounted in the emulator process.
static bool stagePackageOrLicense(const ImportSelection &selected){
  const bool installInPlace=(selected.type==ImportFileType::Archive
                             ||selected.type==ImportFileType::Package)
    && selected.path.rfind("sdmc:/",0)==0;
  std::vector<ImportStageFile> files={{selected.path,selected.name,selected.size,selected.type}};
  bool zrifStaged=false;
  if(selected.type==ImportFileType::Package){
    ImportStageFile companion;
    const bool hasCompanion=findCompanionLicense(selected,companion);
    if(hasCompanion&&!installInPlace){
      if(confirmBox("Companion license found",{companion.name,"",uiText("Import the companion license with the selected package?")}))files.push_back(companion);
    } else if(!hasCompanion&&!offerZrifForPackage(zrifStaged))return false;
  } else if(selected.type==ImportFileType::Archive){
    // A game archive from a download site is usually a .pkg in a zip, which needs
    // a license just like a bare .pkg. Reading the zip's index is quick even for a
    // multi-gigabyte file, so ask now rather than after a long unpack.
    const ArchiveContents contents=archive_inspect(selected.path);
    if(contents.readable&&contents.package&&!contents.license&&!contents.app_content){
      ImportStageFile companion;
      if(!findCompanionLicense(selected,companion)&&!offerZrifForPackage(zrifStaged))return false;
    }
  }
  // A key left over from an earlier attempt must never be applied to this one.
  if(!zrifStaged&&remove(INSTALL_ZRIF)!=0&&errno!=ENOENT){
    modalMessageStatic("Import failed",{"A stale zRIF request could not be removed."});return false;
  }
  std::vector<std::string> destinations;std::string error;
  if(installInPlace){
    if(!validateImportFile(selected.path,selected.type,selected.size,error)){
      modalMessage("Import failed",{error});beginScreenFx();return false;
    }
  } else if(!stageImportFiles(files,destinations,error)){
    modalMessage(error=="Import cancelled."?"Import cancelled":"Import failed",{error});beginScreenFx();return false;
  }
  if(selected.type==ImportFileType::Package||selected.type==ImportFileType::Archive){
    const std::string packagePath=installInPlace?selected.path:browseChild(INSTALL_DIR,selected.name);
    if(!writeAtomicText(INSTALL_TARGET,packagePath+"\n")){
      modalMessageStatic("Import staged",{"The files were copied safely, but the one-shot installer request could not be written.",
        "They remain in sdmc:/switch/vita3k/install/ for a later retry."});return false;
    }
  } else {
    if(remove(INSTALL_TARGET)!=0&&errno!=ENOENT){
      modalMessageStatic("Import staged",{"The license was copied safely, but a stale installer request could not be removed.",
        "Restart the launcher before installing."});return false;
    }
    (void)fsdevCommitDevice("sdmc");
  }
  toastStatic(installInPlace?"Starting Vita3K installer"
    :selected.type==ImportFileType::Package?"Package staged - starting Vita3K installer"
    :selected.type==ImportFileType::Archive?"Archive staged - starting Vita3K installer"
    :"License staged - starting Vita3K installer");
  g_pendingInstall=true;return true;
}

static bool stageFirmwarePup(const ImportSelection &selected){
  static const char *roles[]={"Pre-install firmware","System firmware","System data / font"};
  const int role=dropdown("Choose PUP role",roles,3,-1);
  if(role<0||role>=3)return false;
  std::vector<ImportStageFile> files={{selected.path,FIRMWARE_PUP_NAMES[role],selected.size,ImportFileType::Firmware}};
  std::vector<std::string> destinations;std::string error;
  if(!stageImportFiles(files,destinations,error)){
    modalMessage(error=="Import cancelled."?"Import cancelled":"Firmware import failed",{error});beginScreenFx();return false;
  }
  if(remove(INSTALL_TARGET)!=0&&errno!=ENOENT){
    modalMessageStatic("Firmware staged",{"The PUP was copied safely, but a stale installer request could not be removed."});return false;
  }
  (void)fsdevCommitDevice("sdmc");
  std::vector<std::string> missing;
  if(firmware_local_files_present(&missing)){
    if(confirmBoxStatic("Firmware files ready",{"All three Vita firmware PUPs are staged.","","Start the Vita3K installer now?"})){
      g_pendingInstall=true;return true;
    }
  } else {
    std::vector<std::string> lines={"Imported as "+std::string(FIRMWARE_PUP_NAMES[role])+".","","Still needed:"};
    for(const std::string &name:missing)lines.push_back("   "+name);
    modalMessage("Firmware PUP imported",lines);
  }
  beginScreenFx();return false;
}

// Legacy wrappers retained for the graphical firmware setup flow. Normal Vita
// content installation is exposed from the full file manager below.
[[maybe_unused]] static bool browseInstallPkg(){
  ImportSelection selected;
  return browseStorageFile(ImportKind::PackageLicense,selected)&&stagePackageOrLicense(selected);
}

[[maybe_unused]] static bool importFirmwarePup(){
  ImportSelection selected;
  return browseStorageFile(ImportKind::FirmwarePup,selected)&&stageFirmwarePup(selected);
}

// ---------------------------------------------------------------------------
// Full storage browser / file manager. Vita package, license, and firmware
// installation is exposed as a contextual file action instead of a separate
// filtered import browser.
// ---------------------------------------------------------------------------
static std::string joinPath(const std::string &base,const std::string &name){
  std::string result=base;
  if(!result.empty()&&result.back()=='/')result.pop_back();
  return result+"/"+name;
}

static std::string foldedKey(std::string value){
  std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return (char)tolower(c);});
  return value;
}

static std::string normalizeLocationPath(const std::string &input){
  std::string path=trim(input);if(path.empty())return {};
  std::string output;output.reserve(path.size()+1);bool slash=false;
  for(char c:path){
    if(c=='\\')c='/';
    if(c=='/'){if(slash)continue;slash=true;}else slash=false;
    output+=c;
  }
  size_t colon=output.find(':');
  if(colon!=std::string::npos&&colon+1==output.size())output+='/';
  const size_t minimum=colon==std::string::npos?1:colon+2;
  while(output.size()>minimum&&output.back()=='/')output.pop_back();
  return output;
}

static std::string pathIdentity(const std::string &path){return foldedKey(normalizeLocationPath(path));}

static std::vector<std::string> loadFavoriteFolders(){
  std::vector<std::string> paths;std::unordered_set<std::string> seen;
  const int count=std::max(0,std::min(24,atoi(storeGet(g_global,"Browser/FavoriteCount","0"))));
  for(int index=0;index<count;index++){
    const std::string key="Browser/Favorite"+std::to_string(index);
    std::string path=normalizeLocationPath(storeGet(g_global,key.c_str(),""));
    if(!path.empty()&&seen.insert(pathIdentity(path)).second)paths.push_back(std::move(path));
  }
  return paths;
}

static void saveFavoriteFolders(const std::vector<std::string> &input){
  std::vector<std::string> paths;std::unordered_set<std::string> seen;
  for(const std::string &entry:input){
    std::string path=normalizeLocationPath(entry);
    if(!path.empty()&&seen.insert(pathIdentity(path)).second&&paths.size()<24)paths.push_back(std::move(path));
  }
  storeRemovePrefix(g_global,"Browser/Favorite");
  storeSet(g_global,"Browser/FavoriteCount",std::to_string(paths.size()).c_str());
  for(size_t index=0;index<paths.size();index++){
    const std::string key="Browser/Favorite"+std::to_string(index);
    storeSet(g_global,key.c_str(),paths[index].c_str());
  }
  storeSave(g_global,LAUNCHER_INI);
}

static std::vector<Vita3KLauncher::Storage::SmbShare> loadSmbSharesFromStore(){
  using Vita3KLauncher::Storage::SmbShare;
  std::vector<SmbShare> shares;std::unordered_set<std::string> ids;
  const int count=std::max(0,std::min(8,atoi(storeGet(g_global,"Storage/SmbCount","0"))));
  for(int index=0;index<count;index++){
    const std::string prefix="Storage/Smb"+std::to_string(index);
    SmbShare share;
    share.id=storeGet(g_global,(prefix+"Id").c_str(),"");
    share.name=storeGet(g_global,(prefix+"Name").c_str(),"");
    share.server=storeGet(g_global,(prefix+"Server").c_str(),"");
    share.share=storeGet(g_global,(prefix+"Share").c_str(),"");
    share.path=storeGet(g_global,(prefix+"Path").c_str(),"");
    share.user=storeGet(g_global,(prefix+"User").c_str(),"");
    share.password=storeGet(g_global,(prefix+"Password").c_str(),"");
    share.domain=storeGet(g_global,(prefix+"Domain").c_str(),"");
    const char *automatic=storeGet(g_global,(prefix+"AutoMount").c_str(),"true");
    share.auto_mount=!strcmp(automatic,"true")||!strcmp(automatic,"1");
    if(!Vita3KLauncher::Storage::SmbRootPath(share.id).empty()&&!share.server.empty()&&
       !share.share.empty()&&ids.insert(share.id).second)shares.push_back(std::move(share));
  }
  return shares;
}

static void stopStorageWorker(){
  g_storageWorkerCancel.store(true,std::memory_order_release);
  if(g_storageWorker.joinable())g_storageWorker.join();
  g_storageWorkerComplete.store(true,std::memory_order_release);
}

static void startStorageWorker(){
  stopStorageWorker();
  g_storageWorkerCancel.store(false,std::memory_order_release);
  g_storageWorkerComplete.store(false,std::memory_order_release);
  const auto shares=loadSmbSharesFromStore();
  const bool networkReady=g_networkReady;
  g_storageWorker=std::thread([shares,networkReady]{
    std::string usbError;
    const bool usbReady=Vita3KLauncher::Storage::InitializeUsb(&usbError);
    g_usbError=std::move(usbError);
    g_usbReady.store(usbReady,std::memory_order_release);
    if(networkReady)for(const auto &share:shares){
      if(g_storageWorkerCancel.load(std::memory_order_acquire))break;
      if(!share.auto_mount)continue;
      std::string error;
      Vita3KLauncher::Storage::MountSmb(share,&error,&g_storageWorkerCancel);
    }
    g_storageWorkerComplete.store(true,std::memory_order_release);
    SDL_Event event{};event.type=USB_STATUS_EVENT;SDL_PushEvent(&event);
  });
}

static void saveSmbShares(const std::vector<Vita3KLauncher::Storage::SmbShare> &shares){
  storeRemovePrefix(g_global,"Storage/Smb");
  storeSet(g_global,"Storage/SmbCount",std::to_string(shares.size()).c_str());
  for(size_t index=0;index<shares.size();index++){
    const auto &share=shares[index];const std::string prefix="Storage/Smb"+std::to_string(index);
    storeSet(g_global,(prefix+"Id").c_str(),share.id.c_str());
    storeSet(g_global,(prefix+"Name").c_str(),share.name.c_str());
    storeSet(g_global,(prefix+"Server").c_str(),share.server.c_str());
    storeSet(g_global,(prefix+"Share").c_str(),share.share.c_str());
    storeSet(g_global,(prefix+"Path").c_str(),share.path.c_str());
    storeSet(g_global,(prefix+"User").c_str(),share.user.c_str());
    storeSet(g_global,(prefix+"Password").c_str(),share.password.c_str());
    storeSet(g_global,(prefix+"Domain").c_str(),share.domain.c_str());
    storeSet(g_global,(prefix+"AutoMount").c_str(),share.auto_mount?"true":"false");
  }
  storeSave(g_global,LAUNCHER_INI);
}

struct FileClipboard{std::string path;bool move=false;};
static FileClipboard g_fileClipboard;

static bool filesystemRoot(const std::string &path){
  const std::string normalized=normalizeLocationPath(path);const size_t colon=normalized.find(':');
  if(colon==std::string::npos)return normalized=="/";
  for(size_t index=colon+1;index<normalized.size();index++)if(normalized[index]!='/')return false;
  return true;
}
static std::string parentFolder(const std::string &path){
  const std::string normalized=normalizeLocationPath(path);if(filesystemRoot(normalized))return {};
  const size_t slash=normalized.find_last_of('/');if(slash==std::string::npos)return {};
  const size_t colon=normalized.find(':');
  if(colon!=std::string::npos&&slash<=colon+1)return normalized.substr(0,colon+2);
  return normalized.substr(0,slash);
}
static std::string fileNameOf(const std::string &path){
  const std::string normalized=normalizeLocationPath(path);const size_t slash=normalized.find_last_of('/');
  return slash==std::string::npos?normalized:normalized.substr(slash+1);
}
static std::string deviceOf(const std::string &path){
  const size_t colon=path.find(':');return foldedKey(colon==std::string::npos?std::string{}:path.substr(0,colon));
}
static bool pathAtOrBelow(const std::string &path,const std::string &root){
  const std::string candidate=pathIdentity(path),base=pathIdentity(root);
  if(base.empty()||candidate.size()<base.size()||candidate.compare(0,base.size(),base)!=0)return false;
  return candidate.size()==base.size()||base.back()=='/'||candidate[base.size()]=='/';
}

static bool safeVitaTitleId(const std::string &value){
  return !value.empty()&&value.size()<=32&&
    std::all_of(value.begin(),value.end(),[](unsigned char c){
      return std::isalnum(c)||c=='_'||c=='-';
    });
}

static std::string forwarderTitleId(const std::string &argument){
  std::string candidate=normalizeLocationPath(argument);
  std::string titleId;
  if(safeVitaTitleId(candidate)) titleId=candidate;
  else if(pathAtOrBelow(candidate,APP_DIR)){
    const std::string root=normalizeLocationPath(APP_DIR);
    std::string relative=candidate.substr(std::min(candidate.size(),root.size()));
    while(!relative.empty()&&relative.front()=='/') relative.erase(relative.begin());
    const size_t slash=relative.find('/');
    titleId=relative.substr(0,slash);
  }
  if(!safeVitaTitleId(titleId)) return {};
  const std::string sfo=std::string(APP_DIR)+"/"+titleId+"/sce_sys/param.sfo";
  struct stat info{};
  return stat(sfo.c_str(),&info)==0&&S_ISREG(info.st_mode)?titleId:std::string{};
}
static bool validEntryName(const std::string &name){
  if(name.empty()||name=="."||name==".."||name.size()>255)return false;
  for(unsigned char c:name)if(c<' '||c=='/'||c=='\\'||c==':')return false;
  return true;
}

static void replaceFavoritePathPrefix(const std::string &oldPath,const std::string &newPath){
  const std::string normalizedOld=normalizeLocationPath(oldPath),normalizedNew=normalizeLocationPath(newPath);
  const std::string oldIdentity=pathIdentity(normalizedOld);auto favorites=loadFavoriteFolders();
  for(std::string &path:favorites){
    const std::string normalizedPath=normalizeLocationPath(path),identity=pathIdentity(normalizedPath);
    if(identity==oldIdentity)path=normalizedNew;
    else if(identity.size()>oldIdentity.size()&&identity.compare(0,oldIdentity.size(),oldIdentity)==0&&identity[oldIdentity.size()]=='/')
      path=normalizeLocationPath(normalizedNew+normalizedPath.substr(normalizedOld.size()));
  }
  saveFavoriteFolders(favorites);
  if(!g_fileClipboard.path.empty()&&pathAtOrBelow(g_fileClipboard.path,normalizedOld)){
    const std::string clipboard=normalizeLocationPath(g_fileClipboard.path);
    g_fileClipboard.path=normalizeLocationPath(normalizedNew+clipboard.substr(normalizedOld.size()));
  }
}

static void removeFavoritePathsBelow(const std::string &root){
  auto favorites=loadFavoriteFolders();
  favorites.erase(std::remove_if(favorites.begin(),favorites.end(),[&](const std::string &path){return pathAtOrBelow(path,root);}),favorites.end());
  saveFavoriteFolders(favorites);
  if(!g_fileClipboard.path.empty()&&pathAtOrBelow(g_fileClipboard.path,root))g_fileClipboard={};
}

static bool removeTreeInternal(const std::string &path){
  if(filesystemRoot(path))return false;
  struct stat info{};
  if(lstat(path.c_str(),&info)!=0)return errno==ENOENT;
  if(S_ISREG(info.st_mode)||S_ISLNK(info.st_mode))return remove(path.c_str())==0;
  if(!S_ISDIR(info.st_mode))return false;
  DIR *directory=opendir(path.c_str());if(!directory)return false;
  bool ok=true;struct dirent *entry;
  while(ok&&(entry=readdir(directory))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
    ok=removeTreeInternal(joinPath(path,entry->d_name));
  }
  if(closedir(directory)!=0)ok=false;
  return ok&&rmdir(path.c_str())==0;
}

struct TransferState{
  std::atomic<std::uint64_t> total{0},done{0};
  std::string current,error;
  std::vector<unsigned char> buffer=std::vector<unsigned char>(1<<18);
  std::mutex details;
  std::atomic<bool> cancelled{false};
};

static void setTransferDetail(TransferState &state,const std::string &current,const std::string &error={}){
  std::lock_guard<std::mutex> lock(state.details);
  if(!current.empty())state.current=current;
  if(!error.empty())state.error=error;
}

static std::string transferError(TransferState &state){
  std::lock_guard<std::mutex> lock(state.details);return state.error;
}

static bool transferFrame(TransferState &state){
  if(!beginUiFrame()){state.cancelled.store(true);return false;}
  SDL_Event event;
  while(pollUiEvent(event)){
    pumpStick(event);int tx=0,ty=0;
    if(touchFeed(event,&tx,&ty)==TOUCH_TAP&&ty>=SH-80)state.cancelled.store(true);
    if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL)state.cancelled.store(true);
  }
  std::string current;{std::lock_guard<std::mutex> lock(state.details);current=state.current;}
  clearUiBackground();drawLocalizedHeader("File transfer",nullptr);
  drawTextC(g_font_sm,SW/2,settingsListY()+28,
            ellipsizedText(g_font_sm,current,SW-160).c_str(),COL_DIM);
  const int width=SW*2/3,x=(SW-width)/2,y=SH/2-24,height=42;
  border(x,y,width,height,2,COL_SEL);
  const std::uint64_t done=state.done.load(),total=state.total.load();
  const std::uint64_t progress=total?std::min(done,total):0;
  fillRect(x+3,y+3,total?(int)((width-6)*progress/total):0,height-6,COL_HI);
  char status[96];
  snprintf(status,sizeof(status),"%d%%  -  %.1f / %.1f MiB",
           total?(int)(progress*100/total):0,done/1048576.0,total/1048576.0);
  drawTextC(g_font,SW/2,y+66,status,COL_TXT);
  if(state.cancelled.load())drawTextC(g_font_sm,SW/2,SH-48,"Cancelling...",COL_VAL);
  else {FootItem footer[]={{g_gB,"Cancel",FA_NONE}};drawFooterHints(footer,1,SH-26);}
  presentUi();return !state.cancelled.load();
}

static bool measureTree(const std::string &path,TransferState &state){
  if(state.cancelled.load())return false;
  struct stat info{};
  if(lstat(path.c_str(),&info)!=0){setTransferDetail(state,{},"Source is no longer available");return false;}
  if(S_ISREG(info.st_mode)){state.total.fetch_add((std::uint64_t)info.st_size);return true;}
  if(!S_ISDIR(info.st_mode)){setTransferDetail(state,{},"Unsupported file type");return false;}
  DIR *directory=opendir(path.c_str());
  if(!directory){setTransferDetail(state,{},"Could not open a source folder");return false;}
  bool ok=true;struct dirent *entry;
  while(ok&&!state.cancelled.load()&&(entry=readdir(directory))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
    ok=measureTree(joinPath(path,entry->d_name),state);
  }
  if(closedir(directory)!=0)ok=false;
  return ok;
}

static bool copyFileAtomic(const std::string &source,const std::string &destination,TransferState &state){
  setTransferDetail(state,fileNameOf(source));
  const std::string partial=destination+".nx-part",backup=destination+".nx-old";
  remove(partial.c_str());
  FILE *input=fopen(source.c_str(),"rb");
  if(!input){setTransferDetail(state,{},"Could not open the source file");return false;}
  FILE *output=fopen(partial.c_str(),"wb");
  if(!output){fclose(input);setTransferDetail(state,{},"Could not create the destination file");return false;}
  bool ok=true;
  while(ok&&!state.cancelled.load()){
    const size_t count=fread(state.buffer.data(),1,state.buffer.size(),input);
    if(count){
      if(fwrite(state.buffer.data(),1,count,output)!=count){setTransferDetail(state,{},"Write failed; check free space and permissions");ok=false;break;}
      state.done.fetch_add(count);
    }
    if(count<state.buffer.size()){if(ferror(input)){setTransferDetail(state,{},"Read failed");ok=false;}break;}
  }
  if(state.cancelled.load())ok=false;
  if(ok&&fflush(output)!=0){setTransferDetail(state,{},"Could not flush the destination file");ok=false;}
  if(ok&&fsync(fileno(output))!=0){setTransferDetail(state,{},"Could not commit the destination file");ok=false;}
  if(fclose(input)!=0&&ok){setTransferDetail(state,{},"Could not close the source file");ok=false;}
  if(fclose(output)!=0&&ok){setTransferDetail(state,{},"Could not close the destination file");ok=false;}
  if(!ok){remove(partial.c_str());return false;}
  struct stat destinationInfo{};const bool existed=lstat(destination.c_str(),&destinationInfo)==0;
  if(existed){
    struct stat oldInfo{};
    if(lstat(backup.c_str(),&oldInfo)==0||rename(destination.c_str(),backup.c_str())!=0){
      setTransferDetail(state,{},"Could not preserve the existing destination");remove(partial.c_str());return false;
    }
  }
  if(rename(partial.c_str(),destination.c_str())!=0){
    if(existed)rename(backup.c_str(),destination.c_str());
    setTransferDetail(state,{},"Could not finalize the copied file");remove(partial.c_str());return false;
  }
  if(existed)remove(backup.c_str());
  return true;
}

static bool copyTree(const std::string &source,const std::string &destination,TransferState &state){
  struct stat info{};
  if(lstat(source.c_str(),&info)!=0){setTransferDetail(state,{},"Source is no longer available");return false;}
  if(S_ISREG(info.st_mode))return copyFileAtomic(source,destination,state);
  if(!S_ISDIR(info.st_mode)){setTransferDetail(state,{},"Unsupported file type");return false;}
  if(mkdir(destination.c_str(),0777)!=0){setTransferDetail(state,{},"Could not create a destination folder");return false;}
  DIR *directory=opendir(source.c_str());
  if(!directory){setTransferDetail(state,{},"Could not open a source folder");return false;}
  bool ok=true;struct dirent *entry;
  while(ok&&!state.cancelled.load()&&(entry=readdir(directory))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
    ok=copyTree(joinPath(source,entry->d_name),joinPath(destination,entry->d_name),state);
  }
  if(closedir(directory)!=0&&ok){setTransferDetail(state,{},"Could not close a source folder");ok=false;}
  return ok&&!state.cancelled.load();
}

static bool enoughFreeSpace(const std::string &folder,std::uint64_t bytes){
  struct statvfs info{};
  if(statvfs(folder.c_str(),&info)!=0||!info.f_frsize)return true;
  return bytes<=static_cast<std::uint64_t>(info.f_bavail)*info.f_frsize;
}

static bool executePaste(const std::string &folder){
  if(g_fileClipboard.path.empty())return false;
  struct stat sourceInfo{};
  if(lstat(g_fileClipboard.path.c_str(),&sourceInfo)!=0){
    modalMessageStatic("Paste failed",{"The copied item is no longer available."});g_fileClipboard={};return false;
  }
  const std::string destination=joinPath(folder,fileNameOf(g_fileClipboard.path));
  if(pathIdentity(destination)==pathIdentity(g_fileClipboard.path)||
     (S_ISDIR(sourceInfo.st_mode)&&pathAtOrBelow(destination,g_fileClipboard.path))){
    modalMessageStatic("Paste failed",{"The destination cannot be inside the source."});return false;
  }
  struct stat destinationInfo{};const bool exists=lstat(destination.c_str(),&destinationInfo)==0;
  if(exists&&S_ISDIR(sourceInfo.st_mode)){
    modalMessage("Folder already exists",{uiText("Choose another destination or rename the folder first."),destination});return false;
  }
  if(exists&&!S_ISREG(destinationInfo.st_mode)){
    modalMessageStatic("Paste failed",{"The destination is not a regular file."});return false;
  }
  if(exists&&!confirmBox("Replace existing file?",{fileNameOf(destination),"",uiText("The existing file will be replaced.")}))return false;

  const bool sameDevice=deviceOf(g_fileClipboard.path)==deviceOf(destination);
  if(g_fileClipboard.move&&sameDevice){
    const std::string backup=destination+".nx-old";bool preserved=false;
    if(exists){
      struct stat backupInfo{};
      if(lstat(backup.c_str(),&backupInfo)==0||rename(destination.c_str(),backup.c_str())!=0){
        modalMessageStatic("Move failed",{"Could not preserve the existing destination."});return false;
      }
      preserved=true;
    }
    if(rename(g_fileClipboard.path.c_str(),destination.c_str())==0){
      if(preserved)remove(backup.c_str());
      replaceFavoritePathPrefix(g_fileClipboard.path,destination);g_fileClipboard={};
      toastStatic("Move complete");return true;
    }
    if(preserved)rename(backup.c_str(),destination.c_str());
  }

  TransferState state;setTransferDetail(state,"Preparing transfer...");
  bool ok=false;std::atomic<bool> complete{false};
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  std::thread worker([&](){
    ok=measureTree(g_fileClipboard.path,state);
    if(ok&&!state.cancelled.load()&&!enoughFreeSpace(folder,state.total.load())){
      setTransferDetail(state,{},"The destination does not have enough available space");ok=false;
    }
    if(ok&&!state.cancelled.load())ok=copyTree(g_fileClipboard.path,destination,state);
    complete.store(true,std::memory_order_release);
  });
  while(!complete.load(std::memory_order_acquire)){transferFrame(state);waitForNextFrame();}
  worker.join();appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  if(!ok&&S_ISDIR(sourceInfo.st_mode))removeTreeInternal(destination);
  if(ok&&g_fileClipboard.move){
    if(removeTreeInternal(g_fileClipboard.path))replaceFavoritePathPrefix(g_fileClipboard.path,destination);
    else {modalMessageStatic("Move incomplete",{"The copy completed, but the original could not be removed completely.","Review both locations before trying again."});ok=false;}
  }
  if(ok){if(g_fileClipboard.move)g_fileClipboard={};toastStatic("Transfer complete");}
  else if(state.cancelled.load()){toastStatic("Transfer cancelled");}
  else {const std::string error=transferError(state);modalMessage("Transfer failed",{error.empty()?uiText("The file transfer could not be completed."):error});}
  return ok;
}

static std::string nextSmbId(){
  std::unordered_set<std::string> used;
  for(const auto &share:loadSmbSharesFromStore())used.insert(foldedKey(share.id));
  for(int index=1;index<=99;index++){
    const std::string id="share"+std::to_string(index);
    if(!used.count(id))return id;
  }
  return {};
}

static bool validSmbSubpath(const std::string &path){
  if(path.find(':')!=std::string::npos||path.find('\\')!=std::string::npos)return false;
  size_t start=0;
  while(start<=path.size()){
    const size_t slash=path.find('/',start);
    const std::string part=path.substr(start,slash==std::string::npos?std::string::npos:slash-start);
    if(part=="."||part=="..")return false;
    if(slash==std::string::npos)break;
    start=slash+1;
  }
  return true;
}

static bool editSmbShare(Vita3KLauncher::Storage::SmbShare &share,bool creating){
  using Vita3KLauncher::Storage::SmbShare;
  SmbShare edited=share;
  if(creating){edited.id=nextSmbId();edited.auto_mount=true;}
  constexpr int saveRow=8,rowCount=9;
  int sel=0;
  auto clean=[&](){
    edited.name=trim(edited.name);edited.server=trim(edited.server);
    edited.share=trim(edited.share);edited.path=trim(edited.path);
    edited.user=trim(edited.user);edited.domain=trim(edited.domain);
    if(edited.server.rfind("smb://",0)==0)edited.server.erase(0,6);
    while(!edited.server.empty()&&edited.server.back()=='/')edited.server.pop_back();
    std::replace(edited.share.begin(),edited.share.end(),'\\','/');
    std::replace(edited.path.begin(),edited.path.end(),'\\','/');
    while(!edited.share.empty()&&edited.share.front()=='/')edited.share.erase(edited.share.begin());
    while(!edited.share.empty()&&edited.share.back()=='/')edited.share.pop_back();
    while(!edited.path.empty()&&edited.path.front()=='/')edited.path.erase(edited.path.begin());
    while(!edited.path.empty()&&edited.path.back()=='/')edited.path.pop_back();
  };
  auto validate=[&](){
    clean();
    if(edited.id.empty()){modalMessageStatic("SMB limit reached",{"No free SMB device identifier is available."});return false;}
    if(edited.name.empty()){modalMessageStatic("Display name required",{"Enter a name used to identify this share in Vita3K."});return false;}
    if(edited.server.empty()||edited.server.find('/')!=std::string::npos||edited.server.find('\\')!=std::string::npos){
      modalMessageStatic("Invalid SMB server",{"Enter only a host name or IP address.","Example: 192.168.1.20"});return false;
    }
    if(edited.share.empty()||edited.share.find('/')!=std::string::npos||!validSmbSubpath(edited.share)||!validSmbSubpath(edited.path)){
      modalMessageStatic("Invalid shared folder",{"Enter the Windows share name separately from its optional start folder.","The path cannot contain . or .. components."});return false;
    }
    return true;
  };
  auto editText=[&](int field){
    char buffer[512];const char *title="SMB value";const std::string *value=nullptr;bool password=false,allowEmpty=false;
    switch(field){
      case 0:title="Display name";value=&edited.name;break;
      case 1:title="Server or IP address";value=&edited.server;break;
      case 2:title="Shared folder";value=&edited.share;break;
      case 3:title="Optional start folder";value=&edited.path;allowEmpty=true;break;
      case 4:title="Username (optional)";value=&edited.user;allowEmpty=true;break;
      case 5:title="Password (optional)";value=&edited.password;password=true;allowEmpty=true;break;
      case 6:title="Domain (optional)";value=&edited.domain;allowEmpty=true;break;
      default:return;
    }
    if(promptTextAdvanced(title,value->c_str(),buffer,sizeof(buffer),password,allowEmpty)){
      switch(field){
        case 0:edited.name=buffer;break;case 1:edited.server=buffer;break;
        case 2:edited.share=buffer;break;case 3:edited.path=buffer;break;
        case 4:edited.user=buffer;break;case 5:edited.password=buffer;break;
        case 6:edited.domain=buffer;break;
      }
    }
  };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame())return false;
    SDL_Event event;navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);
      const int rowHeight=settingsRowH(),start=settingsListY();
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40)return false;
        for(int row=0;row<rowCount;row++)if(ty>=start+row*rowHeight&&ty<start+(row+1)*rowHeight){
          sel=row;SDL_Event press{};press.type=SDL_CONTROLLERBUTTONDOWN;press.cbutton.button=BTN_CONFIRM;SDL_PushEvent(&press);break;
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN)continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)sel=(sel+rowCount-1)%rowCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)sel=(sel+1)%rowCount;
      else if(event.cbutton.button==BTN_CANCEL)return false;
      else if(event.cbutton.button==BTN_CONFIRM){
        if(sel<7)editText(sel);
        else if(sel==7)edited.auto_mount=!edited.auto_mount;
        else if(validate()){share=std::move(edited);return true;}
        beginScreenFx();
      } else if(event.cbutton.button==BTN_SETTINGS){
        static const char *help[rowCount]={
          "Friendly name shown by Vita3K.","Computer name or IP address; do not include smb://.",
          "The exported Windows or Samba share name.","Optional folder opened inside the share.",
          "Leave empty for guest access.","Stored locally in launcher.ini; leave empty for guest access.",
          "Optional Windows domain or workgroup.","Connect this share when the launcher starts.",
          "Validate and save this network share."};
        showHelpCard("SMB network share",sel==saveRow?"Save share":"Connection setting","SMB configuration",help[sel],nullptr,"Library & storage");
        beginScreenFx();
      }
    }

    clean();clearUiBackground();drawLocalizedHeader(creating?"Add SMB share":"Edit SMB share",nullptr);
    int columnX,columnWidth,labelX,valueX;listCol(&columnX,&columnWidth,&labelX,&valueX);
    const int rowHeight=settingsRowH(),start=settingsListY();
    glassPanel(columnX-12,start-10,columnWidth+24,rowCount*rowHeight+18);
    const int inset=g_launcherPortrait?portraitRowInset():2;
    fillRect(columnX,start+sel*rowHeight+inset,columnWidth,rowHeight-inset*2,COL_FOCUS);
    fillRect(columnX,start+sel*rowHeight+inset,5,rowHeight-inset*2,COL_SEL);
    const char *labels[rowCount]={"Display name","Server","Shared folder","Start folder","Username","Password","Domain","Connect at startup","Save share"};
    std::string masked=edited.password.empty()?"Not set":std::string(std::min<size_t>(edited.password.size(),12),'*');
    const char *values[rowCount]={edited.name.c_str(),edited.server.c_str(),edited.share.c_str(),edited.path.empty()?"Root":edited.path.c_str(),
      edited.user.empty()?"Guest":edited.user.c_str(),masked.c_str(),edited.domain.empty()?"Default":edited.domain.c_str(),edited.auto_mount?"On":"Off",">"};
    for(int row=0;row<rowCount;row++)drawSettingsRowText(labels[row],values[row],start+row*rowHeight,columnWidth,labelX,valueX,row==sel,
      row==sel?COL_VAL:COL_TXT,row==sel?COL_VAL:COL_DIM,false,rowHeight);
    FootItem footer[]={{g_gA,"Edit",FA_NONE},{g_gX,"Help",FA_NONE},{g_gB,"Back",FA_NONE}};
    drawFooterHints(footer,3,SH-26);drawFadeIn();presentUi();waitForNextFrame();
  }
}

static void networkSharesScreen(){
  using namespace Vita3KLauncher::Storage;
  int sel=0,top=0;
  for(;;){
    auto shares=loadSmbSharesFromStore();const int count=1+(int)shares.size();
    const int listY=settingsListY(),rowHeight=g_launcherPortrait?settingsRowH():60;
    const int visible=std::max(1,(SH-listY-settingsFooterReserve())/rowHeight);
    sel=std::max(0,std::min(sel,count-1));if(sel<top)top=sel;if(sel>=top+visible)top=sel-visible+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame())return;
      SDL_Event event;navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event);int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,count,visible))continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-40)return;
          for(int row=0;row<visible&&top+row<count;row++)if(ty>=listY+row*rowHeight&&ty<listY+(row+1)*rowHeight){
            sel=top+row;SDL_Event press{};press.type=SDL_CONTROLLERBUTTONDOWN;press.cbutton.button=BTN_CONFIRM;SDL_PushEvent(&press);break;
          }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN)continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)sel=(sel+count-1)%count;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)sel=(sel+1)%count;
        else if(event.cbutton.button==BTN_CANCEL)return;
        else if(event.cbutton.button==BTN_CONFIRM){
          if(sel==0){
            if(shares.size()>=8){toastStatic("Maximum of 8 SMB shares");continue;}
            SmbShare share;
            if(editSmbShare(share,true)){
              shares.push_back(share);saveSmbShares(shares);
              if(g_networkReady){std::string error;if(!mountSmbWithUi(share,false,error))modalMessage("SMB connection failed",{error});}
              else modalMessageStatic("Network unavailable",{"The share was saved, but network services could not be initialized."});
              sel=(int)shares.size();rebuild=true;
            }
          } else {
            SmbShare &share=shares[sel-1];const bool registered=IsSmbMounted(share.id);
            const SmbConnectionState state=GetSmbConnectionState(share.id);
            const bool mounted=state==SmbConnectionState::Connected;
            const char *actions[]={mounted?"Disconnect":(registered?"Reconnect":"Connect"),"Edit","Toggle connect at startup","Remove"};
            const int action=dropdown(share.name.c_str(),actions,4,-1,false,true);
            if(action==0){
              std::string error;
              if(mounted){if(!UnmountSmb(share.id,&error))modalMessage("SMB disconnect failed",{error});}
              else if(registered){if(!mountSmbWithUi(share,true,error))modalMessage("SMB reconnect failed",{error});}
              else if(!g_networkReady)modalMessageStatic("Network unavailable",{"Network services could not be initialized."});
              else if(!mountSmbWithUi(share,false,error))modalMessage("SMB connection failed",{error});
              rebuild=true;
            } else if(action==1){
              SmbShare edited=share;
              if(editSmbShare(edited,false)){
                std::string error;
                if(registered&&!UnmountSmb(share.id,&error)){modalMessage("SMB disconnect failed",{error});continue;}
                share=std::move(edited);saveSmbShares(shares);
                if((registered||share.auto_mount)&&g_networkReady&&!mountSmbWithUi(share,false,error))modalMessage("SMB connection failed",{error});
                rebuild=true;
              }
            } else if(action==2){share.auto_mount=!share.auto_mount;saveSmbShares(shares);rebuild=true;}
            else if(action==3&&confirmBox("Remove SMB share?",{share.name,"",uiText("No files on the server will be deleted.")})){
              std::string error;
              if(registered&&!UnmountSmb(share.id,&error)){modalMessage("SMB disconnect failed",{error});continue;}
              const std::string root=SmbRootPath(share.id);shares.erase(shares.begin()+sel-1);saveSmbShares(shares);
              removeFavoritePathsBelow(root);sel=std::max(0,sel-1);rebuild=true;
            }
          }
        }
      }
      if(rebuild)break;
      clearUiBackground();
      const std::string summary=std::to_string(shares.size())+(shares.size()==1?" saved share":" saved shares");
      drawLocalizedHeader("SMB network shares",summary.c_str());
      for(int row=0;row<visible&&top+row<count;row++){
        const int index=top+row,slot=listY+row*rowHeight;const bool current=index==sel;
        if(current){fillRect(54,slot+2,SW-108,rowHeight-4,COL_FOCUS);fillRect(54,slot+2,5,rowHeight-4,COL_SEL);}
        if(index==0)drawText(g_font,80,slot+(rowHeight-TTF_FontHeight(g_font))/2,"[ Add SMB share ]",current?COL_VAL:COL_HI);
        else {
          const auto &share=shares[index-1];const SmbConnectionState state=GetSmbConnectionState(share.id);
          const bool mounted=state==SmbConnectionState::Connected;
          const std::string address="smb://"+share.server+"/"+share.share+(share.path.empty()?std::string{}:"/"+share.path);
          drawText(g_font,80,slot+4,fittedText(g_font,share.name,SW/2).c_str(),current?COL_VAL:COL_TXT);
          const char *status=mounted?"Connected":state==SmbConnectionState::Connecting?"Connecting":
            state==SmbConnectionState::Reconnecting?"Reconnecting":state==SmbConnectionState::Failed?"Connection failed":
            (share.auto_mount?"Disconnected - auto":"Disconnected");
          drawTextR(g_font_sm,SW-80,slot+8,status,mounted?(SDL_Color){120,220,120,255}:COL_DIM);
          drawText(g_font_sm,80,slot+34,fittedText(g_font_sm,address,SW-160).c_str(),COL_DIM);
        }
      }
      FootItem footer[]={{g_gA,"Select",FA_NONE},{g_gB,"Back",FA_NONE}};
      drawFooterHints(footer,2,SH-26);drawFadeIn();presentUi();waitForNextFrame();
    }
  }
}

static bool ensurePathMounted(const std::string &path){
  for(const auto &share:loadSmbSharesFromStore()){
    const std::string root=Vita3KLauncher::Storage::SmbRootPath(share.id);
    if(pathAtOrBelow(path,root)){
      if(Vita3KLauncher::Storage::IsSmbMounted(share.id))return true;
      if(!g_networkReady){modalMessageStatic("Network unavailable",{"Network services could not be initialized."});return false;}
      std::string error;
      if(mountSmbWithUi(share,false,error))return true;
      modalMessage("SMB connection failed",{share.name,error});return false;
    }
  }
  return true;
}

static bool isUsbStoragePath(const std::string &path){
  const size_t colon=path.find(':');
  if(colon<4||tolower((unsigned char)path[0])!='u'||tolower((unsigned char)path[1])!='m'||tolower((unsigned char)path[2])!='s')return false;
  for(size_t index=3;index<colon;index++)if(!isdigit((unsigned char)path[index]))return false;
  return true;
}

enum class BrowserItemKind{Up,Paste,Favorite,Directory,File,Location,Smb,ManageSmb};
struct BrowserItem{
  std::string label,path;
  BrowserItemKind kind=BrowserItemKind::File;
  bool directory=false;
  long long size=0;
  std::string storageId;
  BrowserItem()=default;
  BrowserItem(std::string itemLabel,std::string itemPath,BrowserItemKind itemKind,
              bool isDirectory,long long itemSize,std::string id={})
    :label(std::move(itemLabel)),path(std::move(itemPath)),kind(itemKind),
     directory(isDirectory),size(itemSize),storageId(std::move(id)){}
};

static std::vector<BrowserItem> browserItems(const std::string &current,bool &opened,bool imageOnly=false){
  std::vector<BrowserItem> items;opened=true;
  if(current.empty()){
    items.push_back({"SD card","sdmc:/",BrowserItemKind::Location,true,0,{}});
    for(const auto &usb:Vita3KLauncher::Storage::ListUsbLocations())
      items.push_back({usb.label,usb.path,BrowserItemKind::Location,true,0,usb.id});
    for(const auto &share:loadSmbSharesFromStore()){
      const bool mounted=Vita3KLauncher::Storage::IsSmbMounted(share.id);
      const std::string label="SMB - "+(share.name.empty()?share.share:share.name)+(mounted?"":" (disconnected)");
      items.push_back({label,Vita3KLauncher::Storage::SmbBrowsePath(share),BrowserItemKind::Smb,true,0});
    }
    for(const auto &favorite:loadFavoriteFolders())
      items.push_back({"Pinned - "+favorite,favorite,BrowserItemKind::Location,true,0});
    items.push_back({"Manage SMB shares","",BrowserItemKind::ManageSmb,true,0});
    return items;
  }
  if(!imageOnly&&!g_fileClipboard.path.empty())items.push_back({std::string("[ Paste ")+(g_fileClipboard.move?"moved":"copied")+" item here ]",current,BrowserItemKind::Paste,true,0});
  if(!imageOnly){
    auto favorites=loadFavoriteFolders();
    const bool pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){return pathIdentity(path)==pathIdentity(current);});
    items.push_back({pinned?"[ Unpin this folder ]":"[ Pin this folder ]",current,BrowserItemKind::Favorite,true,0});
  }
  items.push_back({"[ .. locations / parent ]",parentFolder(current),BrowserItemKind::Up,true,0});
  DIR *directory=opendir(current.c_str());
  if(!directory){opened=false;return items;}
  std::vector<BrowserItem> entries;struct dirent *entry;
  while((entry=readdir(directory))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
    const std::string path=joinPath(current,entry->d_name);struct stat info{};
    bool isDirectory=entry->d_type==DT_DIR;
    if(entry->d_type==DT_UNKNOWN||entry->d_type==DT_LNK){if(stat(path.c_str(),&info)!=0)continue;isDirectory=S_ISDIR(info.st_mode);}
    else if(!isDirectory&&stat(path.c_str(),&info)!=0)continue;
    if(imageOnly&&!isDirectory){
      const size_t dot=path.find_last_of('.');std::string extension=dot==std::string::npos?std::string{}:path.substr(dot);
      std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char value){return (char)std::tolower(value);});
      if(extension!=".png"&&extension!=".jpg"&&extension!=".jpeg"&&extension!=".webp"&&extension!=".bmp")continue;
    }
    entries.push_back({std::string(entry->d_name)+(isDirectory?"/":""),path,
      isDirectory?BrowserItemKind::Directory:BrowserItemKind::File,isDirectory,isDirectory?0:(long long)info.st_size});
  }
  closedir(directory);
  std::sort(entries.begin(),entries.end(),[](const BrowserItem &left,const BrowserItem &right){
    if(left.directory!=right.directory)return left.directory>right.directory;
    return strcasecmp(left.label.c_str(),right.label.c_str())<0;
  });
  items.insert(items.end(),std::make_move_iterator(entries.begin()),std::make_move_iterator(entries.end()));
  return items;
}

static bool toggleFavorite(const std::string &path){
  auto favorites=loadFavoriteFolders();const std::string identity=pathIdentity(path);
  auto found=std::find_if(favorites.begin(),favorites.end(),[&](const std::string &entry){return pathIdentity(entry)==identity;});
  const bool pin=found==favorites.end();
  if(pin){
    if(favorites.size()>=24){toastStatic("Maximum of 24 pinned folders");return false;}
    favorites.push_back(normalizeLocationPath(path));
  } else favorites.erase(found);
  saveFavoriteFolders(favorites);toastStatic(pin?"Folder pinned":"Folder unpinned");return true;
}

static bool installBrowserFile(const BrowserItem &item){
  if(item.kind!=BrowserItemKind::File)return false;
  struct stat info{};
  if(stat(item.path.c_str(),&info)!=0||!S_ISREG(info.st_mode)||info.st_size<=0){
    modalMessageStatic("File unavailable",{"The selected file is no longer available."});return false;
  }
  ImportFileType type;
  if(importFileType(item.label,ImportKind::PackageLicense,&type)){
    return stagePackageOrLicense({item.path,fileNameOf(item.path),parentFolder(item.path),(long long)info.st_size,type});
  }
  if(importFileType(item.label,ImportKind::FirmwarePup,&type)){
    return stageFirmwarePup({item.path,fileNameOf(item.path),parentFolder(item.path),(long long)info.st_size,type});
  }
  return false;
}

static bool browserActions(const BrowserItem &item){
  if(item.kind!=BrowserItemKind::Directory&&item.kind!=BrowserItemKind::File)return false;
  enum Action{Install,Copy,Move,Rename,Pin};
  std::vector<Action> actions;std::vector<std::string> labels;
  ImportFileType importType;bool installable=false;
  if(item.kind==BrowserItemKind::File){
    if(importFileType(item.label,ImportKind::PackageLicense,&importType)){
      installable=true;actions.push_back(Install);
      labels.push_back(importType==ImportFileType::Package?"Install Vita package"
        :importType==ImportFileType::Archive?"Install Vita app (VPK)"
        :"Install Vita license");
    } else if(importFileType(item.label,ImportKind::FirmwarePup,&importType)){
      installable=true;actions.push_back(Install);labels.push_back("Use as Vita firmware");
    }
  }
  actions.push_back(Copy);labels.push_back("Copy");
  actions.push_back(Move);labels.push_back("Move");
  actions.push_back(Rename);labels.push_back("Rename");
  if(item.directory){
    auto favorites=loadFavoriteFolders();
    const bool pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){return pathIdentity(path)==pathIdentity(item.path);});
    actions.push_back(Pin);labels.push_back(pinned?"Unpin folder":"Pin folder");
  }
  std::vector<const char*> choices;for(const auto &label:labels)choices.push_back(label.c_str());
  const int selected=dropdown("File options",choices.data(),(int)choices.size(),-1);
  if(selected<0||selected>=(int)actions.size())return false;
  switch(actions[selected]){
    case Install:return installable&&installBrowserFile(item);
    case Copy:g_fileClipboard={item.path,false};toastStatic("Copied to clipboard");return false;
    case Move:g_fileClipboard={item.path,true};toastStatic("Move queued");return false;
    case Rename:{
      char name[256];const std::string oldName=fileNameOf(item.path);
      if(!promptText("Rename",oldName.c_str(),name,sizeof(name)))return false;
      const std::string newName=trim(name);
      if(!validEntryName(newName)){modalMessageStatic("Invalid name",{"Names cannot contain /, \\, :, or control characters."});return false;}
      const std::string destination=joinPath(parentFolder(item.path),newName);struct stat destinationInfo{};
      if(lstat(destination.c_str(),&destinationInfo)==0){modalMessageStatic("Rename failed",{"An item with that name already exists."});return false;}
      if(rename(item.path.c_str(),destination.c_str())!=0){modalMessage("Rename failed",{strerror(errno)});return false;}
      replaceFavoritePathPrefix(item.path,destination);toastStatic("Renamed");return true;
    }
    case Pin:return toggleFavorite(item.path);
  }
  return false;
}

static bool runFileBrowser(std::string *selectedImage=nullptr,const std::string &start={}){
  const bool imageMode=selectedImage!=nullptr;
  std::string current=normalizeLocationPath(start);if(!current.empty()&&!ensurePathMounted(current))current.clear();int sel=0,top=0;
  std::uint64_t usbGeneration=Vita3KLauncher::Storage::UsbStatusGeneration();
  for(;;){
    bool opened=false;auto items=browserItems(current,opened,imageMode);
    if(!opened){modalMessage("Folder unavailable",{current,"",uiText("The device may be disconnected.")});current.clear();sel=top=0;continue;}
    const int rowHeight=g_launcherPortrait?settingsRowH():48,listY=settingsListY();
    const int count=(int)items.size(),visible=std::max(1,(SH-listY-settingsFooterReserve())/rowHeight);
    if(!count){current.clear();continue;}
    sel=std::max(0,std::min(sel,count-1));if(sel<top)top=sel;if(sel>=top+visible)top=sel-visible+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame())return false;
      const std::uint64_t generation=Vita3KLauncher::Storage::UsbStatusGeneration();
      if(generation!=usbGeneration){
        usbGeneration=generation;
        if(!current.empty()&&isUsbStoragePath(current)){
          DIR *test=opendir(current.c_str());
          if(test)closedir(test);else {modalMessageStatic("USB storage removed",{"The current USB device was disconnected."});current.clear();sel=top=0;rebuild=true;continue;}
        } else if(current.empty()){rebuild=true;continue;}
      }
      SDL_Event event;navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event);int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,count,visible))continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-40)return g_pendingInstall;
          for(int row=0;row<visible&&top+row<count;row++)if(ty>=listY+row*rowHeight&&ty<listY+(row+1)*rowHeight){
            sel=top+row;SDL_Event press{};press.type=SDL_CONTROLLERBUTTONDOWN;press.cbutton.button=BTN_CONFIRM;SDL_PushEvent(&press);break;
          }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN)continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)sel=(sel+count-1)%count;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)sel=(sel+1)%count;
        else if(event.cbutton.button==BTN_CANCEL){if(current.empty())return g_pendingInstall;current=parentFolder(current);sel=top=0;rebuild=true;}
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_START){
          const BrowserItem &selected=items[sel];
          if(current.empty()&&!selected.storageId.empty()){
            if(confirmBox("Safely eject USB?",{selected.label,uiText("Close files using this drive before ejecting.")})){
              std::string error;
              if(Vita3KLauncher::Storage::SafelyEjectUsb(selected.storageId,&error)){
                toastStatic("USB drive can now be removed");rebuild=true;
              } else modalMessage("USB eject failed",{error});
            }
            beginScreenFx();
          }
          if(g_pendingInstall)return true;
        }
        else if(event.cbutton.button==BTN_SETTINGS&&!imageMode){
          const BrowserItem &selected=items[sel];
          if(browserActions(selected))rebuild=true;
          if(g_pendingInstall)return true;
        }
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&!imageMode&&!current.empty()&&!g_fileClipboard.path.empty()){executePaste(current);rebuild=true;}
        else if(event.cbutton.button==BTN_CONFIRM){
          const BrowserItem item=items[sel];
          if(item.kind==BrowserItemKind::Paste){executePaste(current);rebuild=true;}
          else if(item.kind==BrowserItemKind::Favorite){toggleFavorite(current);rebuild=true;}
          else if(item.kind==BrowserItemKind::Up){current=item.path;sel=top=0;rebuild=true;}
          else if(item.kind==BrowserItemKind::ManageSmb){networkSharesScreen();sel=top=0;rebuild=true;}
          else if(item.kind==BrowserItemKind::Directory){current=item.path;sel=top=0;rebuild=true;}
          else if(item.kind==BrowserItemKind::Location||item.kind==BrowserItemKind::Smb){
            if(ensurePathMounted(item.path)){
              DIR *test=opendir(item.path.c_str());
              if(test){closedir(test);current=item.path;sel=top=0;rebuild=true;}
              else modalMessage("Location unavailable",{item.path});
            }
          } else if(item.kind==BrowserItemKind::File){
            if(imageMode){*selectedImage=item.path;return false;}
            if(browserActions(item))rebuild=true;
            if(g_pendingInstall)return true;
          }
        }
        if(sel<top)top=sel;
        if(sel>=top+visible)top=sel-visible+1;
      }
      if(rebuild)break;
      clearUiBackground();drawLocalizedHeader(imageMode?"Select local cover":"File manager",current.empty()?"Locations":current.c_str());
      int columnX,columnWidth,labelX,valueX;listCol(&columnX,&columnWidth,&labelX,&valueX);
      glassPanel(columnX-12,listY-10,columnWidth+24,std::min(count,visible)*rowHeight+18);
      for(int row=0;row<visible&&top+row<count;row++){
        const int index=top+row,slot=listY+row*rowHeight;const bool selected=index==sel;const BrowserItem &item=items[index];
        if(selected){fillRect(columnX,slot+2,columnWidth,rowHeight-4,COL_FOCUS);fillRect(columnX,slot+2,5,rowHeight-4,COL_SEL);}
        const SDL_Color base=(item.kind==BrowserItemKind::Paste||item.kind==BrowserItemKind::Favorite)?COL_HI:(item.directory?COL_TXT:(SDL_Color){120,220,120,255});
        const int rightSpace=item.kind==BrowserItemKind::File?150:20;
        drawText(g_font,labelX,slot+(rowHeight-TTF_FontHeight(g_font))/2,
          ellipsizedText(g_font,item.label,valueX-labelX-rightSpace).c_str(),selected?COL_VAL:base);
        if(item.kind==BrowserItemKind::File){const std::string size=browseFmtSize(item.size);drawTextR(g_font_sm,valueX,slot+(rowHeight-TTF_FontHeight(g_font_sm))/2,size.c_str(),selected?COL_VAL:COL_DIM);}
      }
      if(imageMode){
        FootItem footer[]={{g_gA,"Open / Select",FA_NONE},{g_gB,"Back",FA_NONE}};
        drawFooterHints(footer,2,SH-26);
      }else if(current.empty()&&!items[sel].storageId.empty()){
        FootItem footer[]={{g_gA,"Open",FA_NONE},{g_gPlus,"Safely eject",FA_NONE},{g_gB,"Back",FA_NONE}};
        drawFooterHints(footer,3,SH-26);
      }else{
        FootItem footer[]={{g_gA,"Open",FA_NONE},{g_gX,"Actions",FA_NONE},{g_gY,"Paste",FA_NONE},{g_gB,"Back",FA_NONE}};
        drawFooterHints(footer,4,SH-26);
      }
      drawFadeIn();presentUi();waitForNextFrame();
    }
  }
}

static bool runFileManager(){return runFileBrowser();}

static std::string browseCoverImage(const std::string &start){
  std::string selected;runFileBrowser(&selected,start);return selected;
}

static void launcherSettingsScreen() {
  static int savedSelection=0;
  const int optionCount=(int)(sizeof(S_launcher)/sizeof(Opt));
  const int listCount=optionCount;
  const int updateRow=listCount,selectionCount=listCount+1;
  int sel=std::max(0,std::min(savedSelection,selectionCount-1)),top=0;
  auto applyChange=[&](){
    LauncherLocalization::SetLanguage(storeGet(g_global,"Wrapper/Language","system"));
    clearTextCaches();
    applyLauncherAppearance();
    const int requested=atoi(storeGet(g_global,"Wrapper/LauncherRotation","0"));
    if(!configureLauncherOrientation(requested)){
      storeSet(g_global,"Wrapper/LauncherRotation",std::to_string(g_launcherRotation).c_str());
      toastStatic("Could not change launcher orientation");
    }else{
      g_touch={};
      beginScreenFx();
    }
    uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  };
  auto finish=[&](){ savedSelection=sel; storeSave(g_global,LAUNCHER_INI); };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ finish(); return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      const int rowH=settingsRowH(),listY=settingsListY();
      const int visible=std::min(std::max(1,(SH-listY-190)/rowH),listCount);
      const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
      const int buttonX=(SW-buttonWidth)/2;
      const int buttonY=std::min(SH-buttonHeight-104,listY+visible*rowH+24);
      if(touchScrollList(touch,sel,top,listCount,visible)) continue;
      if(touch==TOUCH_SWIPE_L&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); continue; }
      if(touch==TOUCH_SWIPE_R&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); continue; }
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ finish(); return; }
        if(tx>=buttonX&&tx<buttonX+buttonWidth&&ty>=buttonY&&ty<buttonY+buttonHeight){
          sel=updateRow;
          SDL_Event press{};press.type=SDL_CONTROLLERBUTTONDOWN;press.cbutton.button=BTN_CONFIRM;SDL_PushEvent(&press);
          continue;
        }
        for(int row=0;row<visible&&top+row<listCount;row++){
          int y=listY+row*rowH;
          if(ty>=y&&ty<y+rowH){
            sel=top+row;
            SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press);
            break;
          }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+selectionCount-1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); }
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); }
      else if(event.cbutton.button==BTN_CONFIRM){
        if(sel==updateRow){ runUpdateScreen(); if(g_updateInstallExitRequested){finish();return;} beginScreenFx(); }
        else {
          const Opt &option=S_launcher[sel];
          if(option.type==OT_TEXT){
            char value[1024];
            if(promptTextAdvanced(option.label,iniGet(option.key,option.def),value,sizeof(value),false,true))
              iniSet(option.key,value);
            beginScreenFx();
          }
          else if(option.type==OT_CHOICE&&option.nch>2){ optChoosePopup(option); beginScreenFx(); }
          else optAdjust(option,1);
          applyChange();
        }
      } else if(event.cbutton.button==BTN_SETTINGS){
        if(sel<optionCount) showOptionHelp("Launcher",S_launcher[sel],"Launcher setting");
        else showHelpCard("Launcher","Check for Updates","Launcher updates",
          "Checks NaGaa95/Vita3K-nx releases, displays their notes, verifies the exact Vita3K.nro asset, and replaces this launcher transactionally.",nullptr,"Launcher action");
        beginScreenFx();
      } else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&sel<optionCount){
        if(resetOption(S_launcher[sel])){applyChange();toastStatic("Setting reset to default");}
      } else if(event.cbutton.button==BTN_CANCEL){ finish(); return; }
      if(sel<listCount){if(sel<top)top=sel;if(sel>=top+visible)top=sel-visible+1;}
    }

    clearUiBackground();
    drawLocalizedHeader("Launcher",nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    const int rowH=settingsRowH(),listY=settingsListY();
    const int visible=std::min(std::max(1,(SH-listY-190)/rowH),listCount);
    glassPanel(colX-12,listY-10,colW+24,visible*rowH+18);
    const int rowInset=g_launcherPortrait?portraitRowInset():1;
    if(sel<listCount){
      float target=(float)(listY+(sel-top)*rowH+rowInset);
      g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
      fillRect(colX,(int)g_hy,colW,rowH-rowInset*2,COL_FOCUS);
      fillRect(colX,(int)g_hy,5,rowH-rowInset*2,COL_SEL);
    }
    for(int row=0;row<visible&&top+row<listCount;row++){
      int index=top+row,slotY=listY+row*rowH;bool current=index==sel;
      char value[96]; optValue(S_launcher[index],value,sizeof(value));
      const std::string_view label=LauncherLocalization::Translate(S_launcher[index].label);
      drawSettingsRowText(label.data(),value,slotY,colW,labelX,valX,current,
        current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM);
    }
    const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
    const int buttonX=(SW-buttonWidth)/2;
    const int buttonY=std::min(SH-buttonHeight-104,listY+visible*rowH+24);
    const bool updateSelected=sel==updateRow;
    fillRect(buttonX,buttonY,buttonWidth,buttonHeight,updateSelected?COL_FOCUS:(SDL_Color){35,40,50,225});
    border(buttonX,buttonY,buttonWidth,buttonHeight,2,updateSelected?COL_SEL:COL_DIM);
    drawTextC(g_font,SW/2,buttonY+(buttonHeight-TTF_FontHeight(g_font))/2,
              "Check for Updates",updateSelected?COL_VAL:COL_TXT);
    const std::string status=launcherUpdateStatusText();
    drawTextC(g_font_sm,SW/2,buttonY+buttonHeight+8,status.c_str(),updateSelected?COL_VAL:COL_DIM);
    FootItem helpFooter[]={{g_gA,"Choose",FA_NONE},{g_gX,"Help",FA_NONE},{g_gY,"Reset",FA_NONE},{g_gB,"Back",FA_NONE}};
    drawFooterHints(helpFooter,4,SH-26);
    drawFadeIn(); presentUi(); waitForNextFrame();
  }
}

// Optional modules fetched from GitHub. Not firmware and not shipped with the
// launcher: a title that wants one simply fails without it, so what each row
// reports is whether the file is on the card, not whether a download ever ran.
static void homebrewModulesScreen(){
  static int savedSelection=0;
  const int rowCount=modules_count();
  const int rowHeight=g_launcherPortrait?settingsRowH():64;
  const int startY=std::max(settingsListY(),topBarH()+36);
  int sel=std::max(0,std::min(savedSelection,rowCount-1));
  modules_refresh();

  auto installRow=[&](){
    const ModuleEntry &entry=modules_entry(sel);
    std::atomic_bool cancel{false};
    drawSetupProgress(0,"Contacting GitHub",entry.repo,false);presentUi();
    std::string error;
    const int result=modules_install(sel,&cancel,error);
    modules_refresh();
    if(result==MODULE_OK)
      toast((std::string(entry.name)+" installed").c_str());
    else if(result==MODULE_UP_TO_DATE)
      toast((std::string(entry.name)+" is already up to date").c_str());
    else
      modalMessage(modules_error_text(result),
        {error.empty()?std::string("The module could not be installed."):error});
    beginScreenFx();
  };

  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){savedSelection=sel;return;}
    SDL_Event event;navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){savedSelection=sel;return;}
        for(int row=0;row<rowCount;row++)if(ty>=startY+row*rowHeight&&ty<startY+(row+1)*rowHeight){sel=row;installRow();break;}
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN)continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)sel=(sel+rowCount-1)%rowCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)sel=(sel+1)%rowCount;
      else if(event.cbutton.button==BTN_CONFIRM)installRow();
      else if(event.cbutton.button==BTN_SETTINGS){
        const ModuleEntry &entry=modules_entry(sel);
        showHelpCard("Homebrew modules",entry.name,"Optional module",entry.description,nullptr,entry.repo);
        beginScreenFx();
      } else if(event.cbutton.button==BTN_CANCEL){savedSelection=sel;return;}
    }

    clearUiBackground();drawLocalizedHeader("Homebrew modules",nullptr);
    int columnX,columnWidth,labelX,valueX;listCol(&columnX,&columnWidth,&labelX,&valueX);
    glassPanel(columnX-12,startY-10,columnWidth+24,rowCount*rowHeight+18);
    const int inset=g_launcherPortrait?portraitRowInset():2;
    float target=(float)(startY+sel*rowHeight+inset);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(columnX,(int)g_hy,columnWidth,rowHeight-inset*2,COL_FOCUS);
    fillRect(columnX,(int)g_hy,5,rowHeight-inset*2,COL_SEL);

    for(int row=0;row<rowCount;row++){
      const ModuleEntry &entry=modules_entry(row);
      const bool installed=modules_installed(row);
      const char *tag=modules_installed_tag(row);
      const std::string value=installed
        ?(tag&&*tag?std::string("Installed ")+tag:std::string("Installed"))
        :std::string("Not installed");
      // Green only ever means the file is on the card, so a failed install can
      // never read as a successful one.
      const SDL_Color installedColor{120,215,130,255};
      const SDL_Color valueColor=installed?installedColor:(row==sel?COL_VAL:COL_DIM);
      drawSettingsRowText(entry.name,value.c_str(),startY+row*rowHeight,columnWidth,labelX,valueX,
        row==sel,row==sel?COL_VAL:COL_TXT,valueColor,false,rowHeight);
    }
    FootItem footer[]={{g_gA,"Install / update",FA_NONE},{g_gX,"Help",FA_NONE},{g_gB,"Back",FA_NONE}};
    drawFooterHints(footer,3,SH-26);drawFadeIn();presentUi();waitForNextFrame();
  }
}

static void libraryStorageScreen(){
  static int savedSelection=0;
  constexpr int rowCount=6;
  const int rowHeight=g_launcherPortrait?settingsRowH():64;
  const int startY=std::max(settingsListY(),topBarH()+36);
  int sel=std::max(0,std::min(savedSelection,rowCount-1));
  auto openRow=[&](){
    if(sel==0)runFileManager();
    else if(sel==1)firmwareSetupFlow();
    else if(sel==2)networkSharesScreen();
    else if(sel==3)downloadAllCovers();
    else if(sel==4)downloadCompatibilityDatabase();
    else homebrewModulesScreen();
    beginScreenFx();
  };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){savedSelection=sel;return;}
    SDL_Event event;navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){savedSelection=sel;return;}
        for(int row=0;row<rowCount;row++)if(ty>=startY+row*rowHeight&&ty<startY+(row+1)*rowHeight){sel=row;openRow();break;}
        if(g_pendingInstall)return;
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN)continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)sel=(sel+rowCount-1)%rowCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)sel=(sel+1)%rowCount;
      else if(event.cbutton.button==BTN_CONFIRM){openRow();if(g_pendingInstall)return;}
      else if(event.cbutton.button==BTN_SETTINGS){
        static const char *titles[rowCount]={"File manager","Firmware setup","SMB network shares","Download covers","Compatibility database","Homebrew modules"};
        static const char *descriptions[rowCount]={
          "Browse every file and folder on SD, USB, and SMB. Copy, move, rename, pin folders, and install Vita PKGs, licenses, or PUP firmware from a selected file's Actions menu.",
          "Download official Vita firmware, inspect staged PUP files, or start the Vita3K firmware installer.",
          "Add, edit, connect, disconnect, and remove reusable SMB network locations for the file manager.",
          "Downloads only missing Vita game covers from SteamGridDB. Existing local artwork is preserved.",
          "Downloads the Vita3K compatibility database, which colours a small dot on each cover with that game's reported playability. The emulator reads the same file.",
          "Installs the optional modules some titles and homebrew ports expect to already be present: the SceShaccCg shader compiler, and the kubridge and FdFix plugins. Each is fetched from its own latest GitHub release."};
        showHelpCard("Library & storage",titles[sel],"Storage category",descriptions[sel],nullptr,"Launcher tools");
        beginScreenFx();
      } else if(event.cbutton.button==BTN_CANCEL){savedSelection=sel;return;}
    }

    clearUiBackground();drawLocalizedHeader("Library & storage",nullptr);
    int columnX,columnWidth,labelX,valueX;listCol(&columnX,&columnWidth,&labelX,&valueX);
    glassPanel(columnX-12,startY-10,columnWidth+24,rowCount*rowHeight+18);
    const int inset=g_launcherPortrait?portraitRowInset():2;
    float target=(float)(startY+sel*rowHeight+inset);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(columnX,(int)g_hy,columnWidth,rowHeight-inset*2,COL_FOCUS);
    fillRect(columnX,(int)g_hy,5,rowHeight-inset*2,COL_SEL);
    const auto shares=loadSmbSharesFromStore();size_t mounted=0;
    for(const auto &share:shares)if(Vita3KLauncher::Storage::IsSmbMounted(share.id))mounted++;
    const std::string smbValue=std::to_string(mounted)+" / "+std::to_string(shares.size())+" connected";
    char compatValue[48];
    if(compatdb_loaded()) snprintf(compatValue,sizeof(compatValue),"%d titles",compatdb_count());
    else snprintf(compatValue,sizeof(compatValue),"%s","Not downloaded");
    modules_refresh();
    int installedModules=0;
    for(int i=0;i<modules_count();i++)if(modules_installed(i))installedModules++;
    const std::string moduleValue=std::to_string(installedModules)+" / "+std::to_string(modules_count())+" installed";
    const char *labels[rowCount]={"File manager","Firmware setup","SMB network shares","Download covers","Compatibility database","Homebrew modules"};
    const char *values[rowCount]={"SD / USB / SMB",firmware_is_installed()?"Installed":"Not installed",smbValue.c_str(),"SteamGridDB",compatValue,moduleValue.c_str()};
    for(int row=0;row<rowCount;row++){const std::string_view label=LauncherLocalization::Translate(labels[row]);
      const bool literalValue=row==2||row==5||(row==4&&compatdb_loaded());
      const std::string_view value=literalValue?std::string_view(values[row]):LauncherLocalization::Translate(values[row]);
      drawSettingsRowText(label.data(),value.data(),startY+row*rowHeight,columnWidth,labelX,valueX,row==sel,
      row==sel?COL_VAL:COL_TXT,row==sel?COL_VAL:COL_DIM,false,rowHeight);
    }
    FootItem footer[]={{g_gA,"Open",FA_NONE},{g_gX,"Help",FA_NONE},{g_gB,"Back",FA_NONE}};
    drawFooterHints(footer,3,SH-26);drawFadeIn();presentUi();waitForNextFrame();
  }
}

static const char *settingsCategoryDescription(int screen){
  switch(screen){
    case SCR_FRAMEGEN:return "Configures LSFG 2x Vulkan frame generation. Lossless.dll must be installed separately in sdmc:/switch/vita3k/lsfg/.";
    case SCR_EMULATION:return "Controls Vita module loading and CPU execution.";
    case SCR_GRAPHICS:return "Controls renderer selection, graphics quality, scaling, caches, and performance diagnostics.";
    case SCR_AUDIO:return "Controls Switch audio output, volume, and Vita NGS support.";
    case SCR_SYSTEM:return "Controls Vita language, formats, PS TV behavior, and logging.";
    case SCR_NETWORK:return "Controls Vita network emulation, PSN state, and ad-hoc configuration.";
    case SCR_CONTROLLER:return "Controls Switch buttons, sticks, motion, and rear-touch emulation.";
    default:return "Vita3K configuration category.";
  }
}

// ---------------------------------------------------------------------------
// Vita users
//
// One directory per user under ux0/user/<ID>, each holding a user.xml the
// emulator parses in app::load_users. A user owns its savedata and its trophy
// tree, so switching users switches saves. The launcher owns the choice and
// emits it as config.yml's user-id, because it rewrites that file on every
// launch and would otherwise clobber whatever the emulator picked.
// ---------------------------------------------------------------------------

struct VitaUser { std::string id, name; };

static std::string userDirectory(const std::string &id){
  return std::string(USER_DIR) + "/" + id;
}

static bool g_userSummaryValid=false;
static std::string g_userSummary;
static void invalidateUserSummary(){ g_userSummaryValid=false; }

static std::vector<VitaUser> loadVitaUsers(){
  std::vector<VitaUser> users;
  DIR *dir=opendir(USER_DIR);
  if(!dir) return users;
  while(dirent *entry=readdir(dir)){
    const std::string name=entry->d_name;
    if(name.empty()||name=="."||name=="..") continue;
    const std::string path=userDirectory(name);
    struct stat info{};
    if(stat(path.c_str(),&info)!=0||!S_ISDIR(info.st_mode)) continue;

    tinyxml2::XMLDocument doc;
    if(doc.LoadFile((path+"/user.xml").c_str())!=tinyxml2::XML_SUCCESS) continue;
    const tinyxml2::XMLElement *root=doc.FirstChildElement("user");
    if(!root) continue;

    VitaUser user;
    // The directory name is the identity, not the id attribute: every path here
    // is built from it, and saveVitaUser keeps the attribute in step.
    user.id=name;
    const char *label=root->Attribute("name");
    user.name=(label&&*label)?label:user.id;
    users.push_back(std::move(user));
  }
  closedir(dir);
  std::sort(users.begin(),users.end(),[](const VitaUser &a,const VitaUser &b){return a.id<b.id;});
  return users;
}

// Matches app::next_free_id: the lowest unused two-digit id.
static std::string nextFreeUserId(const std::vector<VitaUser> &users){
  for(int candidate=0;candidate<100;candidate++){
    char id[4]; snprintf(id,sizeof(id),"%02d",candidate);
    bool taken=false;
    for(const VitaUser &user:users) if(user.id==id){taken=true;break;}
    if(!taken) return id;
  }
  return {};
}

// Rewrites only the name, so a theme or background the emulator stored survives.
static bool saveVitaUser(const std::string &id,const std::string &name){
  const std::string path=userDirectory(id);
  if(!ensureDirectory(USER_DIR)||!ensureDirectory(path.c_str())) return false;
  const std::string file=path+"/user.xml";

  tinyxml2::XMLDocument doc;
  tinyxml2::XMLElement *root=nullptr;
  if(doc.LoadFile(file.c_str())==tinyxml2::XML_SUCCESS) root=doc.FirstChildElement("user");
  if(!root){
    doc.Clear();
    doc.InsertFirstChild(doc.NewDeclaration(R"(xml version="1.0" encoding="utf-8")"));
    root=doc.NewElement("user");
    doc.InsertEndChild(root);
    tinyxml2::XMLElement *theme=doc.NewElement("theme");
    theme->SetAttribute("use-background",true);
    tinyxml2::XMLElement *content=doc.NewElement("content-id");
    content->SetText("default");
    theme->InsertEndChild(content);
    root->InsertEndChild(theme);
    tinyxml2::XMLElement *start=doc.NewElement("start-screen");
    start->SetAttribute("type","default");
    start->InsertEndChild(doc.NewElement("path"));
    root->InsertEndChild(start);
    root->InsertEndChild(doc.NewElement("backgrounds"));
  }
  root->SetAttribute("id",id.c_str());
  root->SetAttribute("name",name.c_str());

  const std::string temporary=file+".tmp";
  if(doc.SaveFile(temporary.c_str())!=tinyxml2::XML_SUCCESS){ remove(temporary.c_str()); return false; }
  remove(file.c_str());
  if(rename(temporary.c_str(),file.c_str())!=0){ remove(temporary.c_str()); return false; }
  fsdevCommitDevice("sdmc");
  invalidateUserSummary();
  return true;
}

static std::string currentUserId(){ return storeGet(g_global,"Wrapper/UserId",""); }

// Recomputed only when the user set changes: runSettingsRoot draws this every
// frame, and loadVitaUsers scans a directory and parses XML off the SD card.
static std::string activeUserSummaryUncached(){
  const std::string active=currentUserId();
  const std::vector<VitaUser> users=loadVitaUsers();
  for(const VitaUser &user:users) if(user.id==active) return user.name;
  if(users.empty()) return std::string(LauncherLocalization::Translate("no users yet"));
  char summary[48];
  snprintf(summary,sizeof(summary),"%d %s",(int)users.size(),
    LauncherLocalization::Translate(users.size()==1?"user":"users").data());
  return summary;
}

static const std::string &activeUserSummary(){
  if(!g_userSummaryValid){ g_userSummary=activeUserSummaryUncached(); g_userSummaryValid=true; }
  return g_userSummary;
}

static void setCurrentUserId(const std::string &id){
  storeSet(g_global,"Wrapper/UserId",id.c_str());
  storeSave(g_global,LAUNCHER_INI);
  invalidateUserSummary();
}

// SceNpManager rejects names longer than this, which breaks trophy-using titles.
static constexpr size_t kUserNameMax=16;

static void usersScreen(){
  std::vector<VitaUser> users=loadVitaUsers();
  std::string active=currentUserId();
  // A user deleted outside the launcher must not stay selected.
  if(!active.empty()&&std::none_of(users.begin(),users.end(),
       [&](const VitaUser &user){return user.id==active;})){
    active.clear();
    setCurrentUserId("");
  }

  int sel=0;
  const int rowHeight=g_launcherPortrait?settingsRowH():64;
  const int startY=std::max(settingsListY(),topBarH()+36);

  const auto rowCount=[&]{return (int)users.size()+1;};   // + "Create a user"
  const auto refresh=[&]{
    users=loadVitaUsers();
    active=currentUserId();
    if(sel>=rowCount()) sel=rowCount()-1;
    if(sel<0) sel=0;
  };

  const auto createUser=[&]{
    char name[kUserNameMax+1];
    if(!promptText("Name for the new user","",name,sizeof(name))) return;
    const std::string id=nextFreeUserId(users);
    if(id.empty()){ toastStatic("No free user slot is available"); return; }
    if(!saveVitaUser(id,name)){ toastStatic("Could not create the user"); return; }
    if(active.empty()) setCurrentUserId(id);
    refresh();
    toastStatic("User created");
  };

  const auto userActions=[&](const VitaUser &user){
    const bool isActive=user.id==active;
    const char *titles[3]={"Use this user","Rename","Delete"};
    const char *kinds[3]={"Active profile","Profile name","Profile and its data"};
    const char *descriptions[3]={
      "Selects this user for the next game launch. Save data and trophies belong to the selected user.",
      "Changes the displayed name. Vita software rejects names longer than 16 characters.",
      "Deletes this user together with all of its save data and trophies. This cannot be undone."};
    int choice=0;
    beginScreenFx();
    for(;;){
      if(!beginUiFrame()) return;
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touch==TOUCH_TAP){
          if(ty<topBarH()||ty>=SH-40) return;
          for(int row=0;row<3;row++)
            if(ty>=startY+row*rowHeight&&ty<startY+(row+1)*rowHeight){
              choice=row;
              SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM;
              SDL_PushEvent(&press); break;
            }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) choice=(choice+2)%3;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) choice=(choice+1)%3;
        else if(event.cbutton.button==BTN_SETTINGS){
          showHelpCard("Users",titles[choice],kinds[choice],descriptions[choice],nullptr,"User action");
          beginScreenFx();
        }
        else if(event.cbutton.button==BTN_CANCEL) return;
        else if(event.cbutton.button==BTN_CONFIRM){
          if(choice==0){
            setCurrentUserId(user.id);
            toastStatic("User selected");
            return;
          }
          if(choice==1){
            char name[kUserNameMax+1];
            if(promptText("New name",user.name.c_str(),name,sizeof(name))){
              if(saveVitaUser(user.id,name)) toastStatic("User renamed");
              else toastStatic("Could not rename the user");
            }
            return;
          }
          if(confirmBox("Delete this user?",
               {user.name+" ("+user.id+")",
                uiText("All of this user's save data and trophies are deleted."),
                uiText("Installed games and their files are not touched.")})){
            rmrf(userDirectory(user.id));
            fsdevCommitDevice("sdmc");
            invalidateUserSummary();
            if(user.id==active) setCurrentUserId("");
            toastStatic("User deleted");
          }
          return;
        }
      }
      clearUiBackground(); drawLocalizedHeader("Users",user.name.c_str());
      int columnX,columnWidth,labelX,valueX; listCol(&columnX,&columnWidth,&labelX,&valueX);
      glassPanel(columnX-12,startY-10,columnWidth+24,3*rowHeight+18);
      const int inset=g_launcherPortrait?portraitRowInset():2;
      const float target=(float)(startY+choice*rowHeight+inset);
      g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
      fillRect(columnX,(int)g_hy,columnWidth,rowHeight-inset*2,COL_FOCUS);
      fillRect(columnX,(int)g_hy,5,rowHeight-inset*2,COL_SEL);
      for(int row=0;row<3;row++){
        const std::string_view label=LauncherLocalization::Translate(titles[row]);
        const char *value=row==0?(isActive?"Selected":"Not selected"):"";
        const std::string_view shown=*value?LauncherLocalization::Translate(value):std::string_view("");
        drawSettingsRowText(label.data(),shown.data(),startY+row*rowHeight,columnWidth,labelX,valueX,
          row==choice,row==choice?COL_VAL:COL_TXT,row==choice?COL_VAL:COL_DIM,false,rowHeight);
      }
      FootItem footer[]={{g_gA,"Select",FA_NONE},{g_gX,"Help",FA_NONE},{g_gB,"Back",FA_NONE}};
      drawFooterHints(footer,3,SH-26); drawFadeIn(); presentUi(); waitForNextFrame();
    }
  };

  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return;
    int count=rowCount();
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40) return;
        for(int row=0;row<count;row++)
          if(ty>=startY+row*rowHeight&&ty<startY+(row+1)*rowHeight){
            sel=row;
            SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM;
            SDL_PushEvent(&press); break;
          }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+count-1)%count;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%count;
      else if(event.cbutton.button==BTN_SETTINGS){
        showHelpCard("Users","Vita users","User profiles",
          "Each user has its own save data and trophies. The selected user is the one every game is launched as. "
          "Vita3K creates a default user the first time a game runs if none exists.",nullptr,"Launcher section");
        beginScreenFx();
      }
      else if(event.cbutton.button==BTN_CANCEL) return;
      else if(event.cbutton.button==BTN_CONFIRM){
        if(sel==(int)users.size()) createUser();
        else userActions(users[(size_t)sel]);
        refresh();
        beginScreenFx();
      }
    }

    // The handlers above can add or delete a user, so re-read the row count
    // before drawing rather than trusting the value sampled at frame start.
    count=rowCount();
    clearUiBackground(); drawLocalizedHeader("Users",nullptr);
    int columnX,columnWidth,labelX,valueX; listCol(&columnX,&columnWidth,&labelX,&valueX);
    glassPanel(columnX-12,startY-10,columnWidth+24,count*rowHeight+18);
    const int inset=g_launcherPortrait?portraitRowInset():2;
    const float target=(float)(startY+sel*rowHeight+inset);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(columnX,(int)g_hy,columnWidth,rowHeight-inset*2,COL_FOCUS);
    fillRect(columnX,(int)g_hy,5,rowHeight-inset*2,COL_SEL);
    for(int row=0;row<count;row++){
      const bool current=row==sel;
      if(row==(int)users.size()){
        const std::string_view label=LauncherLocalization::Translate("Create a user");
        drawSettingsRowText(label.data(),"+",startY+row*rowHeight,columnWidth,labelX,valueX,
          current,current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowHeight);
        continue;
      }
      const VitaUser &user=users[(size_t)row];
      const std::string_view value=user.id==active
        ? LauncherLocalization::Translate("Selected")
        : std::string_view(user.id);
      drawSettingsRowText(user.name.c_str(),value.data(),startY+row*rowHeight,columnWidth,labelX,valueX,
        current,current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowHeight);
    }
    if(users.empty())
      drawTextC(g_font_sm,SW/2,startY+count*rowHeight+26,
        LauncherLocalization::Translate("Vita3K creates a default user on first launch").data(),COL_DIM);
    FootItem footer[]={{g_gA,"Open",FA_NONE},{g_gX,"Help",FA_NONE},{g_gB,"Back",FA_NONE}};
    drawFooterHints(footer,3,SH-26); drawFadeIn(); presentUi(); waitForNextFrame();
  }
}

static void runSettingsRoot(SDL_GameController *pad,const char *ctx){
  static const int emulatorOrder[]={SCR_EMULATION,SCR_GRAPHICS,SCR_AUDIO,SCR_SYSTEM,SCR_NETWORK,SCR_CONTROLLER};
  const bool global=!(ctx&&*ctx);
  constexpr int launcherRows=4;
  const int emulatorCount=(int)(sizeof(emulatorOrder)/sizeof(*emulatorOrder));
  const int count=global?launcherRows+emulatorCount:1+emulatorCount;
  const int rowHeight=settingsRowH(),startY=settingsListY();
  const int sectionGap=global?(g_launcherPortrait?42:34):0;
  int sel=0;
  auto rowY=[&](int index){return startY+index*rowHeight+(global&&index>=launcherRows?sectionGap:0);};
  beginScreenFx();
  for(;;){
    if(!beginUiFrame())return;
    SDL_Event event;navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40)return;
        for(int row=0;row<count;row++)if(ty>=rowY(row)&&ty<rowY(row)+rowHeight){
          sel=row;SDL_Event press{};press.type=SDL_CONTROLLERBUTTONDOWN;press.cbutton.button=BTN_CONFIRM;SDL_PushEvent(&press);break;
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN)continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)sel=(sel+count-1)%count;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)sel=(sel+1)%count;
      else if(event.cbutton.button==BTN_CONFIRM){
        if(global&&sel==0){launcherSettingsScreen();if(g_updateInstallExitRequested)return;}
        else if(global&&sel==1){libraryStorageScreen();if(g_pendingInstall)return;}
        else if(global&&sel==2)usersScreen();
        else if(global&&sel==3)runSettings(SCR_FRAMEGEN,pad,ctx);
        else {
          const int offset=global?sel-launcherRows:sel-1;
          const int screen=!global&&sel==0?SCR_FRAMEGEN:emulatorOrder[offset];
          runSettings(screen,pad,ctx);
        }
        beginScreenFx();
      } else if(event.cbutton.button==BTN_SETTINGS){
        if(global&&sel==0)showHelpCard("Settings","Launcher","Launcher settings","Appearance, rotation, library grid, animations, sounds, artwork downloads, and launcher updates.",nullptr,"Launcher section");
        else if(global&&sel==1)showHelpCard("Settings","Library & storage","Storage tools","Full SD, USB, and SMB file management, Vita content installation, firmware setup, and network-share management.",nullptr,"Launcher section");
        else if(global&&sel==2)showHelpCard("Settings","Users","User profiles","Creates, renames, deletes, and selects Vita users. Each user owns its own save data and trophies.",nullptr,"Launcher section");
        else {
          const int offset=global?sel-launcherRows:sel-1;
          const int screen=(global&&sel==3)||(!global&&sel==0)?SCR_FRAMEGEN:emulatorOrder[offset];
          showHelpCard(global?"Settings":"Per-game settings",g_screens[screen].title,"Vita3K setting category",settingsCategoryDescription(screen),nullptr,global?"Global emulator settings":"Game override");
        }
        beginScreenFx();
      } else if(event.cbutton.button==BTN_CANCEL)return;
    }

    clearUiBackground();drawLocalizedHeader(global?"Settings":"Per-game settings",global?nullptr:ctx);
    int columnX,columnWidth,labelX,valueX;listCol(&columnX,&columnWidth,&labelX,&valueX);
    if(global){
      glassPanel(columnX-12,startY-10,columnWidth+24,launcherRows*rowHeight+18);
      const int emulatorY=rowY(launcherRows);
      glassPanel(columnX-12,emulatorY-10,columnWidth+24,emulatorCount*rowHeight+18);
    }else{
      glassPanel(columnX-12,startY-10,columnWidth+24,count*rowHeight+18);
    }
    const int inset=g_launcherPortrait?portraitRowInset():2;
    float target=(float)(rowY(sel)+inset);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(columnX,(int)g_hy,columnWidth,rowHeight-inset*2,COL_FOCUS);
    fillRect(columnX,(int)g_hy,5,rowHeight-inset*2,COL_SEL);
    for(int index=0;index<count;index++){
      const int slot=rowY(index);const bool current=index==sel;
      if(global&&index==0){
        const char *theme=storeGet(g_global,"Wrapper/Theme","xmb");
        const char *value=!strcmp(theme,"animated")?"Glow":(!strcmp(theme,"xmb")?"XMB (PS3)":(!strcmp(theme,"classic")?"Classic":(!strcmp(theme,"oled")?"OLED black":"Bubbles")));
        const std::string_view label=LauncherLocalization::Translate("Launcher");
        drawSettingsRowText(label.data(),value,slot,columnWidth,labelX,valueX,current,current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowHeight);
      } else if(global&&index==1){
        const std::string_view label=LauncherLocalization::Translate("Library & storage");
        drawSettingsRowText(label.data(),"files / firmware / network",slot,columnWidth,labelX,valueX,current,current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowHeight);
      } else if(global&&index==2){
        const std::string_view label=LauncherLocalization::Translate("Users");
        const std::string &userValue=activeUserSummary();
        drawSettingsRowText(label.data(),userValue.c_str(),slot,columnWidth,labelX,valueX,current,current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowHeight);
      } else {
        const int offset=global?index-launcherRows:index-1;
        const int screen=(global&&index==3)||(!global&&index==0)?SCR_FRAMEGEN:emulatorOrder[offset];
        const char *value=">";
        std::string frameValue;
        if(screen==SCR_FRAMEGEN){frameValue=!lsfgDllInstalled()?"Lossless.dll missing":(!strcmp(iniGet("switch-lsfg-enabled","false"),"true")?"LSFG 2x enabled":"Off");value=frameValue.c_str();}
        const std::string_view label=LauncherLocalization::Translate(g_screens[screen].title);
        drawSettingsRowText(label.data(),value,slot,columnWidth,labelX,valueX,current,current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowHeight);
      }
    }
    FootItem footer[]={{g_gA,"Open",FA_NONE},{g_gX,"Help",FA_NONE},{g_gB,"Back",FA_NONE}};
    drawFooterHints(footer,3,SH-26);drawFadeIn();presentUi();waitForNextFrame();
  }
}
static std::vector<std::string> wrapTextLines(TTF_Font *font,const std::string &text,int maxWidth){
  std::vector<std::string> output;
  if(text.empty()){output.emplace_back();return output;}
  std::string current;
  size_t position=0;
  while(position<text.size()){
    while(position<text.size()&&text[position]==' ')position++;
    size_t end=text.find(' ',position);
    std::string word=text.substr(position,end==std::string::npos?std::string::npos:end-position);
    if(word.empty())break;
    std::string candidate=current.empty()?word:current+" "+word;
    if(!current.empty()&&textW(font,candidate.c_str())>maxWidth){output.push_back(current);current=word;}
    else current=std::move(candidate);
    if(end==std::string::npos)break;
    position=end+1;
  }
  if(!current.empty())output.push_back(current);
  if(output.empty())output.emplace_back();
  for(std::string &line:output)if(textW(font,line.c_str())>maxWidth)line=fittedText(font,line,maxWidth);
  return output;
}

static std::vector<std::string> wrapDialogLines(TTF_Font *font,const std::vector<std::string> &lines,int maxWidth){
  std::vector<std::string> output;
  for(const std::string &line:lines){
    auto wrapped=wrapTextLines(font,line,maxWidth);
    output.insert(output.end(),wrapped.begin(),wrapped.end());
  }
  return output;
}

static void drawToastOverlay(){
  if(g_toastMessage.empty())return;
  if(SDL_TICKS_PASSED(SDL_GetTicks(),g_toastUntil)){g_toastMessage.clear();return;}
  const int pw=std::min(820,SW-32),ph=120,px=(SW-pw)/2,py=(SH-ph)/2;
  glassPanel(px,py,pw,ph);border(px,py,pw,ph,2,COL_HI);
  const std::string shown=fittedText(g_font,g_toastMessage,pw-40);
  drawTextC(g_font,SW/2,py+(ph-TTF_FontHeight(g_font))/2,shown.c_str(),COL_TXT);
}

static void toast(const char *msg) {
  g_toastMessage=msg?msg:"";
  g_toastUntil=SDL_GetTicks()+1800;
  // Present over the current frame immediately; subsequent screen renders keep
  // the overlay visible until its timer expires without blocking input or work.
  presentUi();
}
static void toastStatic(const char *msg){
  const std::string_view shown=LauncherLocalization::Translate(msg?msg:"");
  toast(shown.data());
}

static void modalMessageRaw(const char *title, const std::vector<std::string> &lines) {
  const int pw=std::min(SW-32,g_launcherPortrait?SW-40:SW*3/4);
  TTF_Font *bodyFont=g_font;
  std::vector<std::string> wrapped=wrapDialogLines(bodyFont,lines,pw-56);
  int lineStep=TTF_FontHeight(bodyFont)+12;
  if(150+(int)wrapped.size()*lineStep>SH-32){
    bodyFont=g_font_sm;wrapped=wrapDialogLines(bodyFont,lines,pw-56);lineStep=TTF_FontHeight(bodyFont)+10;
  }
  for (;;) {
    if (!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP) return; }
      if (e.type == SDL_CONTROLLERBUTTONDOWN &&
          (e.cbutton.button == BTN_CONFIRM || e.cbutton.button == BTN_CANCEL)) return;
    }
    clearUiBackground();
    int ph=std::min(SH-32,150+(int)wrapped.size()*lineStep),px=(SW-pw)/2,py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    std::string shownTitle=fittedText(g_font_big,title?title:"",pw-48);
    drawTextC(g_font_big,SW/2,py+24,shownTitle.c_str(),COL_SEL);
    int y=py+88;
    SDL_Rect clip={px+18,y-4,pw-36,std::max(1,py+ph-58-y)};SDL_RenderSetClipRect(g_ren,&clip);
    for(const std::string &line:wrapped){drawTextC(bodyFont,SW/2,y,line.c_str(),COL_TXT);y+=lineStep;}
    SDL_RenderSetClipRect(g_ren,nullptr);
    drawTextC(g_font_sm, SW/2, py+ph-42, LauncherLocalization::Translate("Press A to continue").data(), COL_DIM);
    presentUi(); waitForNextFrame();
  }
}

static void modalMessage(const char *title,const std::vector<std::string> &lines){
  const std::string_view shown=LauncherLocalization::Translate(title?title:"");
  modalMessageRaw(shown.data(),lines);
}

static std::string uiText(const char *text){
  return std::string(LauncherLocalization::Translate(text?text:""));
}

static void modalMessageStatic(const char *title,std::initializer_list<const char*> lines){
  std::vector<std::string> translated;translated.reserve(lines.size());
  for(const char *line:lines)translated.emplace_back(uiText(line));
  modalMessage(title,translated);
}

static bool confirmBoxRaw(const char *title, const std::vector<std::string> &lines) {
  int pw=std::min(SW-32,g_launcherPortrait?SW-40:SW*3/4);
  TTF_Font *bodyFont=g_font;
  std::vector<std::string> wrapped=wrapDialogLines(bodyFont,lines,pw-56);
  int lineStep=TTF_FontHeight(bodyFont)+12;
  if(200+(int)wrapped.size()*lineStep>SH-32){bodyFont=g_font_sm;wrapped=wrapDialogLines(bodyFont,lines,pw-56);lineStep=TTF_FontHeight(bodyFont)+10;}
  int ph=std::min(SH-32,190+(int)wrapped.size()*lineStep),px=(SW-pw)/2,py=(SH-ph)/2;
  int bw=std::min(210,(pw-54)/2),bh=56,bby=py+ph-bh-22,yesx=SW/2-bw-12,nox=SW/2+12;
  for(;;){
    if (!beginUiFrame()) return false;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP && ty>=bby && ty<bby+bh){
          if(tx>=yesx && tx<yesx+bw) return true;
          if(tx>=nox  && tx<nox+bw)  return false;
      } }
      if(e.type==SDL_QUIT) return false;
      if(e.type==SDL_CONTROLLERBUTTONDOWN){
        if(e.cbutton.button==BTN_CONFIRM) return true;
        if(e.cbutton.button==BTN_CANCEL) return false;
      }
    }
    clearUiBackground();
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,(SDL_Color){210,70,70,255});
    std::string shownTitle=fittedText(g_font_big,title?title:"",pw-48);
    drawTextC(g_font_big,SW/2,py+24,shownTitle.c_str(),(SDL_Color){235,120,120,255});
    int y=py+88;SDL_Rect clip={px+18,y-4,pw-36,std::max(1,bby-y-10)};SDL_RenderSetClipRect(g_ren,&clip);
    for(const std::string &line:wrapped){drawTextC(bodyFont,SW/2,y,line.c_str(),COL_TXT);y+=lineStep;}
    SDL_RenderSetClipRect(g_ren,nullptr);
    int fh=TTF_FontHeight(g_font);
    fillRect(yesx,bby,bw,bh,(SDL_Color){150,50,50,255}); border(yesx,bby,bw,bh,2,(SDL_Color){215,95,95,255});
    const std::string yes=std::string(LauncherLocalization::Translate("Yes"))+"  (A)";
    drawTextC(g_font,yesx+bw/2,bby+(bh-fh)/2,yes.c_str(),COL_TXT);
    fillRect(nox,bby,bw,bh,(SDL_Color){48,54,64,255}); border(nox,bby,bw,bh,2,COL_DIM);
    const std::string no=std::string(LauncherLocalization::Translate("No"))+"  (B)";
    drawTextC(g_font,nox+bw/2,bby+(bh-fh)/2,no.c_str(),COL_TXT);
    presentUi(); waitForNextFrame();
  }
}
static bool confirmBox(const char *title,const std::vector<std::string> &lines){
  const std::string_view shown=LauncherLocalization::Translate(title?title:"");
  return confirmBoxRaw(shown.data(),lines);
}

static bool confirmBoxStatic(const char *title,std::initializer_list<const char*> lines){
  std::vector<std::string> translated;translated.reserve(lines.size());
  for(const char *line:lines)translated.emplace_back(uiText(line));
  return confirmBox(title,translated);
}

static std::string installedReleaseTag(){
  const std::string built=LauncherUpdate_BuiltReleaseTag();
  const std::string stored=trim(storeGet(g_global,"Wrapper/InstalledReleaseTag",""));
  if(stored.empty()||LauncherUpdate_IsNewer(built,stored))return built;
  return stored;
}

static std::string launcherUpdateStatusText(){
  LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  switch(snapshot.state){
    case LauncherUpdateState::Checking:return "Checking...";
    case LauncherUpdateState::UpdateAvailable:return "Version "+snapshot.release.tag+" available";
    case LauncherUpdateState::UpToDate:return "Up to date";
    case LauncherUpdateState::Downloading:return "Downloading...";
    case LauncherUpdateState::ReadyToInstall:return "Ready to install";
    case LauncherUpdateState::Installing:return "Installing...";
    case LauncherUpdateState::Installed:return "Installed";
    case LauncherUpdateState::Cancelled:return "Cancelled";
    case LauncherUpdateState::Error:return "Check failed";
    default:return "Installed "+installedReleaseTag();
  }
}

static std::vector<std::string> wrapReleaseNotes(const std::string &notes,int maxWidth){
  std::vector<std::string> output;
  size_t start=0;
  while(start<=notes.size()){
    size_t end=notes.find('\n',start);
    std::string line=trim(notes.substr(start,end==std::string::npos?std::string::npos:end-start));
    if(line.empty())output.emplace_back();
    else {
      auto wrapped=wrapTextLines(g_font_sm,line,maxWidth);
      output.insert(output.end(),wrapped.begin(),wrapped.end());
    }
    if(end==std::string::npos)break;
    start=end+1;
  }
  if(output.empty())output.push_back("No release notes were provided.");
  return output;
}

static void runUpdateScreen(){
  if(!g_networkReady){
    modalMessageStatic("Network unavailable",{
      "The launcher update check needs an initialized network connection.",
      "Reconnect the console and restart the launcher before trying again."});
    return;
  }
  LauncherUpdateSnapshot initial=LauncherUpdate_GetSnapshot();
  if(initial.state==LauncherUpdateState::Idle||initial.state==LauncherUpdateState::UpToDate)
    LauncherUpdate_StartCheck(installedReleaseTag());
  beginScreenFx();
  for(;;){
    if(!beginUiFrame())return;
    SDL_Event event;navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP&&ty>=SH-92){
        SDL_Event press{};press.type=SDL_CONTROLLERBUTTONDOWN;
        press.cbutton.button=tx<SW/2?BTN_CONFIRM:BTN_CANCEL;SDL_PushEvent(&press);continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN)continue;
      LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
      if(event.cbutton.button==BTN_CANCEL){
        if(snapshot.state==LauncherUpdateState::Checking||snapshot.state==LauncherUpdateState::Downloading)
          LauncherUpdate_Cancel();
        return;
      }
      if(event.cbutton.button!=BTN_CONFIRM)continue;
      if(snapshot.state==LauncherUpdateState::UpdateAvailable){
        if(!LauncherUpdate_StartDownload(g_launcherNroPath))
          modalMessageStatic("Update",{"The download could not be started."});
      } else if(snapshot.state==LauncherUpdateState::ReadyToInstall){
        LauncherUpdate_RequestInstallation();
        g_updateInstallExitRequested=true;
        return;
      } else if(snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled||
                snapshot.state==LauncherUpdateState::UpToDate||snapshot.state==LauncherUpdateState::Idle){
        LauncherUpdate_StartCheck(installedReleaseTag());
      }
      beginScreenFx();
    }

    LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
    clearUiBackground();drawLocalizedHeader("Vita3K-nx Update",nullptr);
    int panelX=g_launcherPortrait?24:std::max(48,SW/10),panelW=SW-panelX*2;
    int panelY=topBarH()+(g_launcherPortrait?24:32),panelH=SH-panelY-(g_launcherPortrait?116:86);
    glassPanel(panelX,panelY,panelW,panelH);
    std::string status,detail;
    switch(snapshot.state){
      case LauncherUpdateState::Idle:status="Ready to check for updates";break;
      case LauncherUpdateState::Checking:status="Checking GitHub releases...";break;
      case LauncherUpdateState::UpdateAvailable:
        status="Version "+snapshot.release.tag+" is available";
        detail="Installed: "+installedReleaseTag()+"    Asset: "+snapshot.release.assetName;break;
      case LauncherUpdateState::UpToDate:
        status="You are up to date";detail="Installed: "+installedReleaseTag();break;
      case LauncherUpdateState::Downloading:{
        status="Downloading "+snapshot.release.assetName;
        char progress[128];
        if(snapshot.total)snprintf(progress,sizeof(progress),"%.1f / %.1f MiB    %llu%%",
          snapshot.downloaded/1048576.0,snapshot.total/1048576.0,
          (unsigned long long)std::min<std::uint64_t>(100,snapshot.downloaded*100/snapshot.total));
        else snprintf(progress,sizeof(progress),"%.1f MiB",snapshot.downloaded/1048576.0);
        detail=progress;break;
      }
      case LauncherUpdateState::ReadyToInstall:status="Download verified";detail="Press A to install the launcher update and exit.";break;
      case LauncherUpdateState::Installing:status="Installing update...";break;
      case LauncherUpdateState::Installed:status="Update installed";break;
      case LauncherUpdateState::Cancelled:status="Update cancelled";detail="Press A to check again.";break;
      case LauncherUpdateState::Error:status="Update failed";detail=snapshot.error;break;
    }
    std::string shownStatus=fittedText(g_font_big,status,panelW-64);
    drawTextC(g_font_big,SW/2,panelY+28,shownStatus.c_str(),COL_VAL);
    if(!detail.empty()){
      auto details=wrapTextLines(g_font_sm,detail,panelW-64);int y=panelY+92;
      for(const std::string &line:details){drawTextC(g_font_sm,SW/2,y,line.c_str(),snapshot.state==LauncherUpdateState::Error?(SDL_Color){245,130,120,255}:COL_DIM);y+=TTF_FontHeight(g_font_sm)+7;}
    }
    if(snapshot.state==LauncherUpdateState::Downloading&&snapshot.total){
      int barX=panelX+32,barY=panelY+150,barW=panelW-64;
      fillRect(barX,barY,barW,14,(SDL_Color){35,44,62,255});
      fillRect(barX,barY,(int)(barW*std::min<std::uint64_t>(snapshot.downloaded,snapshot.total)/snapshot.total),14,COL_SEL);
    }
    if(!snapshot.release.notes.empty()){
      int notesY=panelY+(snapshot.state==LauncherUpdateState::Downloading?190:150);
      drawText(g_font,panelX+32,notesY,"Release notes",COL_HI);
      notesY+=TTF_FontHeight(g_font)+12;
      auto notes=wrapReleaseNotes(snapshot.release.notes,panelW-64);
      SDL_Rect clip={panelX+28,notesY-4,panelW-56,std::max(1,panelY+panelH-notesY-18)};
      SDL_RenderSetClipRect(g_ren,&clip);
      int maxLines=std::max(1,clip.h/(TTF_FontHeight(g_font_sm)+7));
      for(int i=0;i<(int)notes.size()&&i<maxLines;i++){
        drawText(g_font_sm,panelX+32,notesY+i*(TTF_FontHeight(g_font_sm)+7),notes[i].c_str(),COL_TXT);
      }
      SDL_RenderSetClipRect(g_ren,nullptr);
    }
    if(snapshot.state==LauncherUpdateState::UpdateAvailable){FootItem foot[]={{g_gA,"Download",FA_NONE},{g_gB,"Back",FA_NONE}};drawFooterHints(foot,2,SH-26);}
    else if(snapshot.state==LauncherUpdateState::ReadyToInstall){FootItem foot[]={{g_gA,"Install & Exit",FA_NONE},{g_gB,"Back",FA_NONE}};drawFooterHints(foot,2,SH-26);}
    else if(snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled||snapshot.state==LauncherUpdateState::UpToDate){FootItem foot[]={{g_gA,"Check Again",FA_NONE},{g_gB,"Back",FA_NONE}};drawFooterHints(foot,2,SH-26);}
    else {FootItem foot[]={{g_gB,(snapshot.state==LauncherUpdateState::Checking||snapshot.state==LauncherUpdateState::Downloading)?"Cancel":"Back",FA_NONE}};drawFooterHints(foot,1,SH-26);}
    drawFadeIn();presentUi();waitForNextFrame();
  }
}

static void pollUpdateNotification(){
  LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  if(snapshot.state==LauncherUpdateState::UpdateAvailable&&!snapshot.release.tag.empty()&&snapshot.release.tag!=g_updateNotifiedTag){
    g_updateNoticeTag=snapshot.release.tag;g_updateNotifiedTag=snapshot.release.tag;
    g_updateNoticeUntil=SDL_GetTicks()+9000;
  }
}

static void drawUpdateNotification(){
  if(g_updateNoticeTag.empty()||SDL_TICKS_PASSED(SDL_GetTicks(),g_updateNoticeUntil))return;
  int width=std::min(SW-32,g_launcherPortrait?SW-40:620),height=g_launcherPortrait?108:82;
  int x=(SW-width)/2,y=topBarH()+16;
  fillRect(x,y,width,height,COL_CARD);border(x,y,width,height,2,COL_SEL);
  std::string title=fittedText(g_font,"Vita3K-nx "+g_updateNoticeTag+" is available",width-36);
  drawTextC(g_font,SW/2,y+14,title.c_str(),COL_VAL);
  drawTextC(g_font_sm,SW/2,y+height-TTF_FontHeight(g_font_sm)-13,"Settings > Launcher > Check for Updates",COL_DIM);
}

static const char *gridDbErrorText(int result) {
  if(result==GRIDDB_NO_KEY) return "The SteamGridDB API key was rejected.";
  if(result==GRIDDB_NO_NET) return "Could not connect to SteamGridDB.";
  if(result==GRIDDB_NOT_FOUND) return "No matching artwork was found.";
  return "SteamGridDB returned an unexpected error.";
}

static bool runCancellableNetworkTask(const char *title,const std::string &detail,
                                      const std::function<void(const std::atomic_bool&)> &task){
  std::atomic_bool cancel{false},complete{false};
  std::thread worker([&]{task(cancel);complete.store(true,std::memory_order_release);
    SDL_Event wake{};wake.type=USB_STATUS_EVENT;SDL_PushEvent(&wake);});
  beginScreenFx();
  while(!complete.load(std::memory_order_acquire)){
    if(!beginUiFrame()){cancel.store(true,std::memory_order_release);break;}
    SDL_Event event;while(pollUiEvent(event)){pumpStick(event);int x=0,y=0;const TouchKind touch=touchFeed(event,&x,&y);
      if((event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL)||
         (touch==TOUCH_TAP&&y>=SH-80))cancel.store(true,std::memory_order_release);}
    clearUiBackground();drawLocalizedHeader(title,nullptr);
    const auto lines=wrapTextLines(g_font,detail,SW-160);int y=SH/2-(int)lines.size()*24;
    for(const std::string &line:lines){drawTextC(g_font,SW/2,y,line.c_str(),COL_TXT);y+=TTF_FontHeight(g_font)+8;}
    FootItem footer[]={{g_gB,"Cancel",FA_NONE}};drawFooterHints(footer,1,SH-26);
    drawFadeIn();presentUi();waitForNextFrame();
  }
  if(worker.joinable())worker.join();
  return !cancel.load(std::memory_order_acquire);
}

static int chooseCoverArtwork(const std::vector<GridDbArtwork> &artworks,const char *gameName) {
  if(artworks.empty()) return -1;
  const int rowHeight=52;
  const int listX=g_launcherPortrait?48:56;
  const int listWidth=g_launcherPortrait?SW-96:SW/2-78;
  const int previewX=g_launcherPortrait?48:SW/2+28;
  const int previewAreaWidth=g_launcherPortrait?SW-96:SW-previewX-56;
  const int previewHeight=g_launcherPortrait?
      std::min(highResolutionUi()?720:510,(previewAreaWidth*3)/2):
      std::min(SH-210,highResolutionUi()?720:510);
  const int previewWidth=previewHeight*2/3;
  const int previewY=g_launcherPortrait?topBarH()+20:116;
  const int startY=g_launcherPortrait?previewY+previewHeight+30:116;
  const int visible=std::max(1,(SH-startY-settingsFooterReserve())/rowHeight);
  const std::string temporary=std::string(COVERS_DIR)+"/.sgdb-preview.img";
  int sel=0,top=0,loaded=-1;
  SDL_Texture *preview=nullptr;
  bool previewFailed=false;

  auto releasePreview=[&](){
    if(preview) SDL_DestroyTexture(preview);
    preview=nullptr;
    remove(temporary.c_str());
  };
  auto loadPreview=[&](int index){
    releasePreview();
    loaded=index; previewFailed=false;
    const std::string &url=artworks[index].thumbnailUrl.empty()?artworks[index].url:artworks[index].thumbnailUrl;
    int result=GRIDDB_ERROR;
    runCancellableNetworkTask("Choose cover artwork","Loading preview for "+std::string(gameName),
      [&](const std::atomic_bool &cancel){result=griddb_download_image(url,temporary,&cancel);});
    if(result==GRIDDB_OK)
      preview=loadCoverTexture(temporary);
    previewFailed=preview==nullptr;
    remove(temporary.c_str());
    beginScreenFx();
  };

  mkdir(COVERS_DIR,0777);
  loadPreview(0);
  for(;;){
    if(!beginUiFrame()){ releasePreview(); return -1; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      int touchSelection=sel;
      if(touchScrollList(touch,sel,top,(int)artworks.size(),visible)){
        if(sel!=touchSelection) loadPreview(sel);
        continue;
      }
      if(touch==TOUCH_TAP){
        if(ty>=SH-48){ releasePreview(); return -1; }
        if(tx>=listX&&tx<listX+listWidth){
          for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
            int y=startY+row*rowHeight;
            if(ty>=y&&ty<y+rowHeight){ sel=top+row; if(loaded!=sel) loadPreview(sel); break; }
          }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      int previous=sel;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+(int)artworks.size()-1)%(int)artworks.size();
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%(int)artworks.size();
      else if(event.cbutton.button==BTN_CONFIRM){ releasePreview(); return sel; }
      else if(event.cbutton.button==BTN_CANCEL){ releasePreview(); return -1; }
      if(sel<top) top=sel;
      if(sel>=top+visible) top=sel-visible+1;
      if(sel!=previous) loadPreview(sel);
    }

    clearUiBackground();
    drawLocalizedHeader("Choose cover artwork",gameName);
    glassPanel(listX-10,startY-10,listWidth+20,std::min(visible,(int)artworks.size())*rowHeight+18);
    for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
      int index=top+row,y=startY+row*rowHeight,currentY=y+(rowHeight-TTF_FontHeight(g_font))/2;
      bool current=index==sel;
      if(current){ fillRect(listX,y,listWidth,rowHeight-3,COL_FOCUS); fillRect(listX,y,5,rowHeight-3,COL_SEL); }
      std::string label="Artwork "+std::to_string(index+1);
      drawText(g_font,listX+26,currentY,label.c_str(),current?COL_VAL:COL_TXT);
      if(artworks[index].width>0&&artworks[index].height>0){
        std::string dimensions=std::to_string(artworks[index].width)+"x"+std::to_string(artworks[index].height);
        drawTextR(g_font_sm,listX+listWidth-20,currentY+(TTF_FontHeight(g_font)-TTF_FontHeight(g_font_sm))/2,dimensions.c_str(),current?COL_VAL:COL_DIM);
      }
    }
    int imageX=previewX+(previewAreaWidth-previewWidth)/2,imageY=previewY;
    fillRect(imageX,imageY,previewWidth,previewHeight,COL_CARD);
    if(loaded==sel&&preview){ SDL_Rect destination={imageX,imageY,previewWidth,previewHeight}; SDL_RenderCopy(g_ren,preview,nullptr,&destination); }
    else if(loaded==sel&&previewFailed) drawTextC(g_font_sm,imageX+previewWidth/2,imageY+previewHeight/2,"Preview unavailable",COL_DIM);
    border(imageX,imageY,previewWidth,previewHeight,2,loaded==sel?COL_SEL:COL_DIM);
    FootItem footer[]={{g_gA,"Use artwork",FA_NONE},{g_gB,"Back",FA_NONE}};
    drawFooterHints(footer,2,SH-26);
    drawFadeIn(); presentUi(); waitForNextFrame();
  }
}

static void downloadCover(Game &g) {
  std::string key = storeGet(g_global, "Wrapper/SteamGridDBKey", "");
  if (key.empty()) {
    char buf[128];
    if (promptText("Enter your free SteamGridDB API key", "", buf, sizeof(buf))) {
      key = buf;
      storeSet(g_global, "Wrapper/SteamGridDBKey", buf);
      storeSave(g_global, LAUNCHER_INI);
    } else { toastStatic("A SteamGridDB API key is required"); return; }
  }
  mkdir(COVERS_DIR, 0777);
  std::string query=g.title;
  GridDbGameResult selectedGame;
  for(;;){
    std::vector<GridDbGameResult> matches;
    int searchResult=GRIDDB_ERROR;
    if(!runCancellableNetworkTask("SteamGridDB","Searching for "+query,
      [&](const std::atomic_bool &cancel){searchResult=griddb_search_games(key,query,matches,&cancel);}))return;
    if(searchResult!=GRIDDB_OK&&searchResult!=GRIDDB_NOT_FOUND){
      modalMessage("Cover search failed",{gridDbErrorText(searchResult)});
      return;
    }
    std::vector<std::string> labels={"Custom search..."};
    for(const auto &match:matches) labels.push_back(match.name);
    std::vector<const char*> names;
    names.reserve(labels.size());
    for(const auto &label:labels) names.push_back(label.c_str());
    int gameIndex=dropdown("Choose matching title",names.data(),(int)names.size(),-1,true,false);
    if(gameIndex<0) return;
    if(gameIndex==0){
      char custom[256];
      if(!promptText("Custom SteamGridDB search",query.c_str(),custom,sizeof(custom))) continue;
      std::string nextQuery=trim(custom);
      if(!nextQuery.empty()) query=std::move(nextQuery);
      continue;
    }
    selectedGame=matches[gameIndex-1];
    break;
  }

  std::vector<GridDbArtwork> artworks;
  int result=GRIDDB_ERROR;
  if(!runCancellableNetworkTask("SteamGridDB","Loading artwork for "+selectedGame.name,
    [&](const std::atomic_bool &cancel){result=griddb_fetch_artworks(key,selectedGame.id,artworks,&cancel);}))return;
  if(result!=GRIDDB_OK){ modalMessage("Artwork search failed",{gridDbErrorText(result)}); return; }
  int artworkIndex=chooseCoverArtwork(artworks,selectedGame.name.c_str());
  if(artworkIndex<0) return;

  if(!runCancellableNetworkTask("SteamGridDB","Downloading selected cover",
    [&](const std::atomic_bool &cancel){result=griddb_download_image(artworks[artworkIndex].url,coverPath(g),&cancel);}))return;
  if(result==GRIDDB_OK){ reloadCover(g); toastStatic("Cover downloaded"); }
  else toastStatic("Cover download failed");
}

static void importCoverFromFile(Game &g){
  const std::string selected=browseCoverImage(parentFolder(g.path));if(selected.empty())return;
  mkdir(COVERS_DIR,0777);const std::string destination=coverPath(g),temporary=destination+".tmp";
  bool imported=false;std::string reason,detail;
  if(!runCancellableNetworkTask("Importing local cover",fileNameOf(selected),[&](const std::atomic_bool &cancel){
    const auto fail=[&](const char *message,const char *technical=nullptr){reason=message;if(technical)detail=technical;remove(temporary.c_str());};
    struct stat info{};if(cancel.load())return;
    if(stat(selected.c_str(),&info)!=0||!S_ISREG(info.st_mode)){fail("The selected cover file is unavailable.",strerror(errno));return;}
    if(info.st_size<1||(uint64_t)info.st_size>32ull*1024*1024){fail("The selected cover file is too large.");return;}
    if(!recoverAtomicFile(destination)){fail("Vita3K could not prepare the cover file safely.",strerror(errno));return;}
    using Surface=std::unique_ptr<SDL_Surface,decltype(&SDL_FreeSurface)>;
    Surface source{IMG_Load(selected.c_str()),SDL_FreeSurface};if(!source){fail("The selected file is not a supported image.",IMG_GetError());return;}
    if(source->w<=0||source->h<=0||source->w>8192||source->h>8192||(uint64_t)source->w*(uint64_t)source->h>16ull*1024*1024){fail("The selected image dimensions are too large.");return;}
    if(cancel.load())return;
    Surface converted{SDL_ConvertSurfaceFormat(source.get(),SDL_PIXELFORMAT_RGBA32,0),SDL_FreeSurface};source.reset();
    if(!converted||IMG_SavePNG(converted.get(),temporary.c_str())!=0){fail("Vita3K could not convert the selected image to PNG.",IMG_GetError());return;}
    converted.reset();if(cancel.load()){remove(temporary.c_str());return;}
    Surface verify{IMG_Load(temporary.c_str()),SDL_FreeSurface};if(!verify||verify->w<=0||verify->h<=0){fail("Vita3K could not verify the converted cover.",IMG_GetError());return;}verify.reset();
    FILE *saved=fopen(temporary.c_str(),"rb+");if(!saved){fail("Vita3K could not save the converted cover.",strerror(errno));return;}
    const bool synced=fsync(fileno(saved))==0,closed=fclose(saved)==0;if(!synced||!closed){fail("Vita3K could not save the converted cover.",strerror(errno));return;}
    if(cancel.load()){remove(temporary.c_str());return;}if(!replaceAtomic(destination,temporary)){fail("Vita3K could not replace the current cover safely.",strerror(errno));return;}imported=true;
  }))return;
  if(imported){reloadCover(g);toastStatic("Cover imported");return;}
  std::vector<std::string> lines{std::string(LauncherLocalization::Translate(reason.empty()?"The selected cover could not be imported safely.":reason))};if(!detail.empty())lines.push_back(detail);
  modalMessage(LauncherLocalization::Translate("Cover import failed").data(),lines);
}

static void coverSettings(Game &g){
  int selection=0;const bool portrait=g_launcherPortrait;const int margin=portrait?36:70,gap=portrait?24:30;
  const int cardsTop=topBarH()+40,cardsBottom=SH-settingsFooterReserve();
  SDL_Rect cards[2];
  if(portrait){const int height=(cardsBottom-cardsTop-gap)/2;cards[0]={margin,cardsTop,SW-margin*2,height};cards[1]={margin,cardsTop+height+gap,SW-margin*2,height};}
  else{const int width=(SW-margin*2-gap)/2;cards[0]={margin,cardsTop,width,cardsBottom-cardsTop};cards[1]={margin+width+gap,cardsTop,width,cardsBottom-cardsTop};}
  const char *titles[2]={"Download from SteamGridDB","Import cover from file"};const char *kinds[2]={"Online artwork","Local image"};
  const char *descriptions[2]={"Search SteamGridDB and replace this game's custom cover with selected online artwork.","Choose a PNG, JPEG, WebP or BMP image from SD, USB or SMB storage. It is validated and saved safely as PNG."};
  const auto inside=[](const SDL_Rect&r,int x,int y){return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;};
  const auto removeCustom=[&]{const std::string path=coverPath(g);if(!regularFileExists(path)||!confirmBox("Remove custom cover?",{uiText("The downloaded or imported cover will be deleted."),uiText("The launcher will use the game's embedded artwork when available.")}))return;
    if(remove(path.c_str())!=0&&errno!=ENOENT)modalMessage(LauncherLocalization::Translate("Cover removal failed").data(),{strerror(errno)});else{fsdevCommitDevice("sdmc");reloadCover(g);toastStatic("Custom cover removed");}};
  beginScreenFx();for(;;){
    if(!beginUiFrame())return;
    const bool hasCustom=regularFileExists(coverPath(g));SDL_Event event;navRepeat();
    while(pollUiEvent(event)){pumpStick(event);int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);bool choose=false;
      if(touch==TOUCH_TAP){if(inside(cards[0],tx,ty)){selection=0;choose=true;}else if(inside(cards[1],tx,ty)){selection=1;choose=true;}else if(ty>=SH-40)return;}
      if(event.type==SDL_CONTROLLERBUTTONDOWN){if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)selection=0;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)selection=1;
        else if(event.cbutton.button==BTN_CONFIRM)choose=true;else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&hasCustom){removeCustom();beginScreenFx();}else if(event.cbutton.button==BTN_CANCEL)return;}
      if(choose){if(selection==0)downloadCover(g);else importCoverFromFile(g);beginScreenFx();}}
    clearUiBackground();drawLocalizedHeader("Cover settings",g.title.c_str());
    for(int index=0;index<2;index++){const SDL_Rect&card=cards[index];const bool current=index==selection;fillRect(card.x+5,card.y+7,card.w,card.h,(SDL_Color){0,0,0,62});fillRect(card.x,card.y,card.w,card.h,current?COL_FOCUS:COL_CARD);border(card.x,card.y,card.w,card.h,current?4:2,current?COL_SEL:COL_DIM);if(current)fillRect(card.x,card.y,8,card.h,COL_SEL);
      const std::string title(LauncherLocalization::Translate(titles[index]));
      drawTextC(g_font_big,card.x+card.w/2,card.y+34,fittedText(g_font_big,title,card.w-60).c_str(),current?COL_VAL:COL_TXT);
      drawTextC(g_font,card.x+card.w/2,card.y+(portrait?92:126),LauncherLocalization::Translate(kinds[index]).data(),current?COL_HI:COL_DIM);
      drawWrapped(g_font_sm,card.x+38,card.y+(portrait?148:194),card.w-76,TTF_FontHeight(g_font_sm)+7,portrait?4:5,LauncherLocalization::Translate(descriptions[index]).data(),current?COL_TXT:COL_DIM);}
    if(hasCustom){FootItem footer[]={{g_gA,"Choose",FA_NONE},{g_gY,"Remove custom cover",FA_NONE},{g_gB,"Back",FA_NONE}};drawFooterHints(footer,3,SH-26);}else{FootItem footer[]={{g_gA,"Choose",FA_NONE},{g_gB,"Back",FA_NONE}};drawFooterHints(footer,2,SH-26);}
    drawFadeIn();presentUi();waitForNextFrame();}
}
// Fetches the Vita3K compatibility database into CACHE_DIR. The emulator reads
// the very same file at boot (switch_bootstrap.cpp calls compat::load_from_disk
// on its cache path), so one download serves both processes.
static void downloadCompatibilityDatabase(){
  if(!g_networkReady){toastStatic("No network connection is available");return;}
  int result=COMPATDB_NO_NET;
  if(!runCancellableNetworkTask("Compatibility database",
        "Checking Vita3K for a newer compatibility database",
        [&](const std::atomic_bool &cancel){result=compatdb_update(CACHE_DIR,&cancel);}))
    return;
  switch(result){
    case COMPATDB_OK:{
      char message[128];
      snprintf(message,sizeof(message),"%d titles in the compatibility database",compatdb_count());
      toast(message);
      break;
    }
    case COMPATDB_UP_TO_DATE: toastStatic("The compatibility database is already up to date");break;
    case COMPATDB_CANCELLED:  break;
    case COMPATDB_BAD_DATA:   toastStatic("The downloaded compatibility database was unreadable");break;
    case COMPATDB_WRITE_FAILED: toastStatic("Could not save the compatibility database");break;
    default: toastStatic("Could not reach the compatibility database");break;
  }
}

static void downloadAllCovers() {
  std::string key = storeGet(g_global, "Wrapper/SteamGridDBKey", "");
  if (key.empty()) {
    char buf[128];
    if (promptText("Enter your free SteamGridDB API key", "", buf, sizeof(buf))) {
      key = buf; storeSet(g_global, "Wrapper/SteamGridDBKey", buf); storeSave(g_global, LAUNCHER_INI);
    } else { toastStatic("A SteamGridDB API key is required"); return; }
  }
  mkdir(COVERS_DIR, 0777);
  struct CoverTask { std::string key,title,path; };
  std::vector<CoverTask> tasks;
  for(const Game &game:g_games){struct stat info{};const std::string path=coverPath(game);
    if(stat(path.c_str(),&info)!=0)tasks.push_back({game.key,game.title,path});}
  if(tasks.empty()){toastStatic("All covers already downloaded");return;}

  std::atomic_bool cancel{false},complete{false};
  std::atomic_int current{0},done{0},downloaded{0},failed{0};
  std::vector<std::string> successful;
  std::thread worker([&,tasks,key]{
    for(size_t index=0;index<tasks.size()&&!cancel.load(std::memory_order_acquire);index++){
      current.store((int)index,std::memory_order_release);
      const int result=griddb_fetch_cover(key,tasks[index].title,tasks[index].path,&cancel);
      if(cancel.load(std::memory_order_acquire))break;
      if(result==GRIDDB_OK){downloaded.fetch_add(1);successful.push_back(tasks[index].key);}
      else failed.fetch_add(1);
      done.fetch_add(1,std::memory_order_release);
      SDL_Event wake{};wake.type=USB_STATUS_EVENT;SDL_PushEvent(&wake);
    }
    complete.store(true,std::memory_order_release);
    SDL_Event wake{};wake.type=USB_STATUS_EVENT;SDL_PushEvent(&wake);
  });

  beginScreenFx();
  while(!complete.load(std::memory_order_acquire)){
    if(!beginUiFrame()){cancel.store(true,std::memory_order_release);break;}
    SDL_Event event;while(pollUiEvent(event)){
      pumpStick(event);int x=0,y=0;const TouchKind touch=touchFeed(event,&x,&y);
      if((event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL)||
         (touch==TOUCH_TAP&&y>=SH-90))cancel.store(true,std::memory_order_release);
    }
    const int completed=done.load(std::memory_order_acquire);
    const int index=std::min(current.load(std::memory_order_acquire),(int)tasks.size()-1);
    clearUiBackground();drawLocalizedHeader("Download covers",nullptr);
    drawTextC(g_font,SW/2,SH/2-96,("Downloading  "+std::to_string(std::min(completed+1,(int)tasks.size()))+" / "+std::to_string(tasks.size())).c_str(),COL_VAL);
    drawTitleCell(SW/2,SW-260,SH/2-44,tasks[index].title,true,COL_TXT);
    const int bw=SW-360,bx=180,by=SH/2+16,bh=26;
    fillRect(bx,by,bw,bh,(SDL_Color){40,44,54,255});border(bx,by,bw,bh,2,COL_DIM);
    fillRect(bx,by,(int)(bw*(long long)completed/tasks.size()),bh,COL_SEL);
    char status[80];snprintf(status,sizeof(status),"%d downloaded    %d failed",
      downloaded.load(),failed.load());drawTextC(g_font_sm,SW/2,by+46,status,COL_DIM);
    FootItem footer[]={{g_gB,"Cancel",FA_NONE}};drawFooterHints(footer,1,SH-26);
    drawFadeIn();presentUi();waitForNextFrame();
  }
  cancel.store(true,std::memory_order_release);
  if(worker.joinable())worker.join();
  // Invalidate only newly downloaded covers. Decoding remains lazy and within
  // the normal per-frame texture budget.
  for(const std::string &gameKey:successful)for(Game &game:g_games)if(game.key==gameKey){
    if(game.cover)SDL_DestroyTexture(game.cover);
    game.cover=nullptr;game.coverUse=0;game.triedCover=false;break;}
  char message[96];snprintf(message,sizeof(message),"Covers: %d downloaded, %d failed%s",
    downloaded.load(),failed.load(),cancel.load()&&done.load()<(int)tasks.size()?" (cancelled)":"");
  toast(message);
}


// Pick an image for the forwarder icon: the game's Vita icon, its cover, or SteamGridDB icons.
static bool pickIcon(Game &g, char *outPath, size_t outSize) {
  std::string base = std::string(DATA_DIR) + "/forwarders", tmp = base + "/iconpick";
  mkdir(base.c_str(),0777); mkdir(tmp.c_str(),0777);
  if(DIR*d=opendir(tmp.c_str())){ struct dirent*e; while((e=readdir(d))) if(e->d_name[0]!='.') remove((tmp+"/"+std::string(e->d_name)).c_str()); closedir(d); }
  std::vector<std::string> paths; struct stat st;
  if(!g.iconPath.empty() && stat(g.iconPath.c_str(),&st)==0) paths.push_back(g.iconPath);
  { std::string cp=coverPath(g); if(stat(cp.c_str(),&st)==0) paths.push_back(cp); }
  std::string key = storeGet(g_global,"Wrapper/SteamGridDBKey","");
  if(!key.empty()){
    int nf=0;
    runCancellableNetworkTask("Choose an icon","Fetching icons for "+g.title,
      [&](const std::atomic_bool &cancel){nf=griddb_fetch_icons(key,g.title,tmp,14,&cancel);});
    for(int i=0;i<nf;i++){ char p[300]; snprintf(p,sizeof(p),"%s/gicon_%d.png",tmp.c_str(),i); paths.push_back(p); }
  }
  if(paths.empty()){ toastStatic("No icon found - add a SteamGridDB key or download a cover first"); return false; }
  int n=(int)paths.size(); std::vector<SDL_Texture*> tex(n,nullptr);
  for(int i=0;i<n;i++) tex[i]=IMG_LoadTexture(g_ren,paths[i].c_str());
  // Adaptive grid: up to 5 columns, cell sized so every row fits on screen.
  int cols=n<5?n:5; if(cols<1)cols=1;
  int rows=(n+cols-1)/cols, gap=18, top=150, bot=40;
  int cw=(SW-80-(cols-1)*gap)/cols, ch=(SH-top-bot-(rows-1)*gap)/rows;
  int cell=cw<ch?cw:ch; if(cell>200)cell=200; if(cell<90)cell=90;
  int x0=(SW-(cols*cell+(cols-1)*gap))/2, y0=top;
  int sel=0, chosen=-1; bool done=false; beginScreenFx();
  while(!done){
    if (!beginUiFrame()) return false;
    SDL_Event e; navRepeat();
    while(pollUiEvent(e)){ pumpStick(e);
      int tx=0,ty=0;TouchKind touch=touchFeed(e,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty>=SH-48){done=true;continue;}
        for(int i=0;i<n;i++){
          int row=i/cols,column=i%cols,x=x0+column*(cell+gap),y=y0+row*(cell+gap);
          if(tx>=x&&tx<x+cell&&ty>=y&&ty<y+cell){sel=i;chosen=i;done=true;break;}
        }
        continue;
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: sel=(sel+1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel+cols)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel-cols+n)%n; break;
        case BTN_CONFIRM: chosen=sel; done=true; break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    clearUiBackground();
    drawLocalizedHeader("Choose an icon", g.title.c_str());
    for(int i=0;i<n;i++){ int r=i/cols,c=i%cols, x=x0+c*(cell+gap), y=y0+r*(cell+gap);
      if(i==sel) fillRect(x-6,y-6,cell+12,cell+12,COL_SEL);
      fillRect(x,y,cell,cell,COL_CARD);
      if(tex[i]){ SDL_Rect d{x,y,cell,cell}; SDL_RenderCopy(g_ren,tex[i],nullptr,&d); }
      else drawTextC(g_font_sm,x+cell/2,y+cell/2,"?",COL_DIM);
    }
    FootItem footer[]={{g_gA,"Use icon",FA_NONE},{g_gB,"Back",FA_NONE}};
    drawFooterHints(footer,2,SH-26);
    drawFadeIn(); presentUi(); waitForNextFrame();
  }
  for(auto t:tex) if(t) SDL_DestroyTexture(t);
  if(chosen>=0 && chosen<n){ snprintf(outPath,outSize,"%s",paths[chosen].c_str()); return true; }
  return false;
}

// Create + auto-install a HOME shortcut. Single window: icon on the left (A opens the picker),
// name on the right (auto-filled, A edits), and a Create button. Author and
// version are preserved from the launcher's bundled NACP.
static void forwarderWizard(Game &g) {
  char name[256]; snprintf(name,sizeof(name),"%s",g.title.c_str());
  char icon[300]={0};
  // Same candidates and the same rule the library grid uses: take the first
  // image that actually decodes, not merely the first that exists. A cover that
  // is present but unreadable would otherwise reach the icon converter.
  for(const std::string &candidate:coverCandidatePaths(g)){
    if(SDL_Surface *probe=IMG_Load(candidate.c_str())){
      SDL_FreeSurface(probe);
      snprintf(icon,sizeof(icon),"%s",candidate.c_str());
      break;
    }
  }
  SDL_Texture *iconTex = icon[0] ? IMG_LoadTexture(g_ren, icon) : nullptr;

  const int isz=g_launcherPortrait?std::min(280,SW-160):280;
  const int ix=g_launcherPortrait?(SW-isz)/2:110;
  const int iy=g_launcherPortrait?topBarH()+30:176;
  const int rx=g_launcherPortrait?56:ix+isz+70;
  const int rw=g_launcherPortrait?SW-112:SW-rx-90;
  const int nameY=g_launcherPortrait?iy+isz+62:196;
  const int createY=g_launcherPortrait?nameY+116:330;
  const int fieldH=64,createH=58;
  int sel=0; bool done=false; beginScreenFx();              // 0 icon, 1 name, 2 create

  auto edit=[&](const char *hdr, char *buf, size_t sz){ char b[256]; if(promptText(hdr, buf, b, sizeof(b)) && b[0]) snprintf(buf,sz,"%.*s",(int)sz-1,b); };
  auto build=[&](){
    if(!icon[0]){ toastStatic("Pick an icon first"); return; }
    clearUiBackground();
    drawLocalizedHeader("Creating HOME shortcut", g.title.c_str());
    drawTextC(g_font, SW/2, SH/2, "Building + installing forwarder...", COL_TXT);
    presentUi();
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    char err[256]={0}; bool ok=forwarder_create(g.key,name,icon,err,sizeof(err));
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if(ok){ toastStatic("HOME shortcut installed"); done=true; }
    else modalMessage("Shortcut failed", { err[0]?err:uiText("Unknown error"), "", uiText("A HOME forwarder needs sigpatches on your CFW.") });
    beginScreenFx();
  };
  auto activate=[&](){
    if(sel==0){ char p[300]; if(pickIcon(g,p,sizeof(p))){ snprintf(icon,sizeof(icon),"%s",p); if(iconTex)SDL_DestroyTexture(iconTex); iconTex=IMG_LoadTexture(g_ren,icon); } beginScreenFx(); }
    else if(sel==1) edit("Shortcut name", name, sizeof(name));
    else build();
  };

  while(!done){
    if (!beginUiFrame()) return;
    SDL_Event e; navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_TAP){
          if(tx>=ix&&tx<ix+isz&&ty>=iy&&ty<iy+isz){ sel=0; activate(); }
          else if(ty>=nameY-6&&ty<nameY+fieldH){ sel=1; activate(); }
          else if(ty>=createY-6&&ty<createY+createH){ sel=2; activate(); }
          else if(ty>=SH-40) done=true;
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=0; break;                 // icon is on the left
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if(sel==0) sel=1; break;      // back to the fields
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel==0)?2:(sel==1?2:1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel==0)?1:(sel==2?1:2); break;
        case BTN_CONFIRM: activate(); break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    clearUiBackground();
    drawLocalizedHeader("Create HOME shortcut", g.title.c_str());
    // left: icon preview
    if(sel==0) fillRect(ix-6,iy-6,isz+12,isz+12,COL_SEL);
    fillRect(ix,iy,isz,isz,COL_CARD);
    if(iconTex){ SDL_Rect d{ix,iy,isz,isz}; SDL_RenderCopy(g_ren,iconTex,nullptr,&d); }
    else drawTextC(g_font_sm,ix+isz/2,iy+isz/2,"(no icon)",COL_DIM);
    drawTextC(g_font_sm, ix+isz/2, iy+isz+20, "Icon", sel==0?COL_VAL:COL_DIM);
    // right: name field
    auto field=[&](int idx,int y,const char*label,const char*val){ bool cur=sel==idx;
      if(cur){ fillRect(rx-10,y-6,rw+20,fieldH,COL_FOCUS); fillRect(rx-10,y-6,5,fieldH,COL_SEL); }
      drawText(g_font_sm, rx, y, label, cur?COL_VAL:COL_DIM);
      drawScrollTextL(g_font, rx, y+26, rw-8, val, cur?COL_VAL:COL_TXT); };
    field(1,nameY,"Name",name);
    { bool cur=sel==2;
      fillRect(rx-10,createY-6,rw+20,createH, cur?(SDL_Color){44,86,44,240}:(SDL_Color){30,46,32,200});
      if(cur) fillRect(rx-10,createY-6,5,createH,COL_SEL);
      drawTextC(g_font, rx+rw/2, createY+12, "Create shortcut", cur?COL_VAL:(SDL_Color){150,225,150,255}); }
    drawFadeIn(); presentUi(); waitForNextFrame();
  }
  if(iconTex) SDL_DestroyTexture(iconTex);
}

// Recursively delete a directory tree (a Vita app is a folder under ux0/app).
static void rmrf(const std::string &path) {
  if (DIR *d = opendir(path.c_str())) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      std::string c = path + "/" + e->d_name;
      struct stat st;
      if (stat(c.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) rmrf(c);
      else remove(c.c_str());
    }
    closedir(d);
  }
  rmdir(path.c_str());
}

static int perGameMenu(Game &g, SDL_GameController *pad) {
  const char *items[] = { "Launch", "Per-game settings", "Rename game", "Cover settings", "Favorites & collections", "Create HOME shortcut", "Clear cache", "Clear per-game settings", "Delete game (remove from SD)" };
  int n=9, sel=0;
  // load this game's override store
  std::string gp = std::string(GAMECFG_DIR) + "/" + g.key + ".ini";
  storeLoad(g_game, gp.c_str());
  const int coverWidth=g_launcherPortrait?(highResolutionUi()?300:240):300;
  const int coverHeight=coverWidth*3/2;
  const int coverX=g_launcherPortrait?(SW-coverWidth)/2:90;
  const int coverY=g_launcherPortrait?topBarH()+30:(SH-coverHeight)/2;
  const int menuX=g_launcherPortrait?56:coverX+coverWidth+64;
  const int menuWidth=g_launcherPortrait?SW-112:SW-menuX-70;
  const int menuRowH=g_launcherPortrait?(highResolutionUi()?72:62):56;
  const int menuStartY=g_launcherPortrait?coverY+coverHeight+40:210;
  beginScreenFx();
  for(;;){
    if (!beginUiFrame()) return 0;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);              // touchscreen
        if(tk==TOUCH_TAP){
          if(ty>=SH-40){ return 0; }
          for(int i=0;i<n;i++){ const int rowTop=menuStartY+i*menuRowH;
            if(ty>=rowTop && ty<rowTop+menuRowH){ sel=i;
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n; break;
        case BTN_CANCEL: return 0;
        case BTN_CONFIRM:
          if(sel==0) return 1;
          else if(sel==1){
            g_active=&g_game;
            runSettingsRoot(pad,g.title.c_str());   // edits only write changed keys into g_game
            g_active=&g_global;
            mkdir(GAMECFG_DIR,0777);
            storeSave(g_game, gp.c_str());
            g.hasCfg = !g_game.kv.empty();
            beginScreenFx();
          }
          else if(sel==2){                          // rename (software keyboard)
            char buf[128];
            if(promptText("Rename game", g.title.c_str(), buf, sizeof(buf))){
              g.title = buf;
              storeSet(g_titles, g.key.c_str(), buf);
              storeSave(g_titles, TITLES_INI);
            }
          }
          else if(sel==3){ coverSettings(g); beginScreenFx(); }
          else if(sel==4){ editGameOrganization(g); beginScreenFx(); }
          else if(sel==5){ forwarderWizard(g); beginScreenFx(); }
          else if(sel==6){
            if(confirmBox("Clear cache?", { g.title, "", uiText("Deletes compiled shaders and shader logs."), uiText("Save data and game files are not changed.") })){
              rmrf(std::string(CACHE_DIR)+"/shaders/"+g.title_id);
              rmrf(std::string(CACHE_DIR)+"/shaderlog/"+g.title_id); // compatibility with older layouts
              rmrf(std::string(DATA_DIR)+"/shaderlog/"+g.title_id);
              toastStatic("Cache cleared"); beginScreenFx();
            }
          }
          else if(sel==7){ g_game.kv.clear(); remove(gp.c_str()); g.hasCfg=false; toastStatic("Per-game settings cleared"); beginScreenFx(); }
          else if(sel==8){                          // delete the app folder from the SD entirely
            if(confirmBox("Delete game?", { g.title, "", uiText("This permanently deletes the game from the SD card."),
                                            uiText("This cannot be undone.") })){
              rmrf(g.path);                         // ux0/app/<TITLEID> tree
              remove(coverPath(g).c_str());         // its cached cover
              remove(gp.c_str());                   // its per-game settings
              storeRemove(g_titles, g.key.c_str()); storeSave(g_titles, TITLES_INI);  // rename entry
              storeRemove(g_recent, g.key.c_str()); storeSave(g_recent, RECENT_INI);  // play history
              toastStatic("Game deleted");
              return 2;                              // tell the caller to re-scan
            }
          }
          break;
      }
    }
    // render: cover on left, menu on right
    clearUiBackground();
    if(g_launcherPortrait) drawLocalizedHeader("Game menu",g.title.c_str());
    g_cover_budget = 1;         // single cover here -- allow it to load
    ensureCover(g);
    int cw=coverWidth,chh=coverHeight,cx=coverX,cy=coverY;
    fillRect(cx+5,cy+7,cw,chh,(SDL_Color){0,0,0,60}); fillRect(cx+2,cy+3,cw,chh,(SDL_Color){0,0,0,75});  // cover shadow
    if(g.cover){ SDL_SetTextureAlphaMod(g.cover,255); SDL_SetTextureColorMod(g.cover,255,255,255);  // clear any grid dim/fade
      SDL_Rect d={cx,cy,cw,chh}; SDL_RenderCopy(g_ren,g.cover,nullptr,&d); border(cx,cy,cw,chh,2,COL_DIM); }
    else { fillRect(cx,cy,cw,chh,(SDL_Color){40,44,54,255}); border(cx,cy,cw,chh,2,COL_DIM); drawTextC(g_font,cx+cw/2,cy+chh/2,"NO COVER",COL_DIM); }
    if(!g_launcherPortrait)
      drawScrollTextL(g_font_big,cx+cw+70,120,SW-(cx+cw+70)-50,g.title.c_str(),COL_TXT);
    int mx=menuX,mw=menuWidth;
    const int rowInset=g_launcherPortrait?portraitRowInset():2;
    float ty=(float)(menuStartY+sel*menuRowH+rowInset);
    g_hy=(!g_uiAnimations||g_hy<0)?ty:g_hy+(ty-g_hy)*0.30f;
    const int highlightHeight=menuRowH-rowInset*2;
    fillRect(mx,(int)g_hy,mw,highlightHeight,COL_FOCUS);
    fillRect(mx,(int)g_hy,5,highlightHeight,COL_SEL);
    for(int i=0;i<n;i++){const int slot=menuStartY+i*menuRowH,y=slot+(menuRowH-TTF_FontHeight(g_font))/2;const bool cur=i==sel;
      SDL_Color rc = (i==n-1) ? (SDL_Color){228,120,120,255} : COL_TXT;   // delete row = red
      const std::string_view translated=LauncherLocalization::Translate(items[i]);
      std::string shown=fittedText(g_font,translated.data(),mw-52);
      drawText(g_font,mx+30,y,shown.c_str(),cur?COL_VAL:rc);
    }
    drawFadeIn();
    presentUi();
    waitForNextFrame();
  }
}


// cover grid (main screen). Returns the chosen game index, or -1 to quit.
// ---------------------------------------------------------------------------
// Adaptive grid: fixed 2 rows; cover size from height, column count from width.
struct GLay { int cols, rows, cw, chh, gapx, gapy, x0, y0, titleH; };
static GLay gridLayout(){
  GLay g;
  bool big=highResolutionUi();
  g.gapx=big?24:18;g.gapy=big?18:14;
  if(g_launcherPortrait){g.gapx=big?20:14;g.gapy=big?20:16;}
  g.titleH=g_showGameTitles?(big?30:24):0;
  int topBar=topBarH(),footer=g_launcherPortrait?(big?124:96):(big?54:38);
  g.rows = g_gridRows;
  g.cols = g_gridColumns;
  if(g_launcherPortrait){
    const int capacity=g_gridColumns*g_gridRows;
    long long bestArea=-1;int bestColumns=1,bestRows=capacity;
    const int margin=big?60:32,caption=g.titleH?g.titleH+8:0;
    const int availableHeight=SH-topBar-footer;
    for(int columns=1;columns<=capacity;columns++){
      if(capacity%columns)continue;
      const int rows=capacity/columns;
      const int width=(SW-2*margin-(columns-1)*g.gapx)/columns;
      const int height=(availableHeight-(rows-1)*g.gapy-rows*caption)/rows;
      if(width<48||height<72)continue;
      const int coverHeight=std::min(height,width*3/2),coverWidth=coverHeight*2/3;
      const long long area=(long long)coverWidth*coverHeight;
      if(area>bestArea){bestArea=area;bestColumns=columns;bestRows=rows;}
    }
    g.cols=bestColumns;g.rows=bestRows;
  }
  int availH = SH - topBar - footer;
  int caption=g.titleH?g.titleH+8:0;
  int maxCoverH=(availH-(g.rows-1)*g.gapy-g.rows*caption)/g.rows;
  if(maxCoverH<72) maxCoverH=72;
  int margin = big?60:(g_launcherPortrait?32:40);
  int autoWidth=maxCoverH*2/3;
  int maxCoverW=(SW-2*margin-(g.cols-1)*g.gapx)/g.cols;
  g.cw=std::max(48,std::min(autoWidth,maxCoverW));
  g.chh=std::min(maxCoverH,g.cw*3/2);
  g.cw=g.chh*2/3;
  int gridW = g.cols*g.cw + (g.cols-1)*g.gapx;
  g.x0 = (SW - gridW)/2;
  int gridH=g.rows*(g.chh+caption)+(g.rows-1)*g.gapy;
  g.y0=topBar+std::max(0,(availH-gridH)/2);
  return g;
}
static int gridHitTest(int px,int py,int top){
  GLay L=gridLayout(); int n=(int)g_visibleGames.size();
  int rowStride=L.chh+(L.titleH?L.titleH+8:0)+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c; if(idx>=n) continue;
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    if(px>=x-4 && px<x+L.cw+4 && py>=y-4 && py<y+L.chh+(L.titleH?L.titleH+8:0)) return idx;
  }
  return -1;
}
static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col){
  TTF_Font*f=g_font_sm;
  int tw=textW(f,title.c_str());
  if(tw<=cellW){ drawTextC(f,cx,y,title.c_str(),col); return; }
  int x0=cx-cellW/2;
  if(!sel){
    const std::string &shortened=ellipsizedText(f,title,cellW);
    drawTextC(f,cx,y,shortened.c_str(),col);
    return;
  }
  SDL_Rect clip={x0,y-2,cellW,(f?TTF_FontHeight(f):26)+8};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-cellW;
  float t=(SDL_GetTicks()%5000)/5000.0f;
  float pp = t<0.5f ? t*2.f : (1.f-t)*2.f;
  drawText(f,x0-(int)(pp*span),y,title.c_str(),col);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void drawScrollTextR(TTF_Font*f,int xRight,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawTextR(f,xRight,y,s,c); return; }
  int x0=xRight-maxW;
  SDL_Rect clip={x0,y-2,maxW,(f?TTF_FontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-maxW;
  float t=(SDL_GetTicks()%6000)/6000.0f;
  float pp=t<0.5f? t*2.f : (1.f-t)*2.f;
  drawText(f,x0-(int)(pp*span),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}
static void drawScrollTextL(TTF_Font*f,int x,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawText(f,x,y,s,c); return; }
  SDL_Rect clip={x,y-2,maxW,(f?TTF_FontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-maxW;
  float t=(SDL_GetTicks()%6000)/6000.0f;
  float pp=t<0.5f? t*2.f : (1.f-t)*2.f;
  drawText(f,x-(int)(pp*span),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void renderGrid(int sel,int top,const char*gamedirLabel){
  clearUiBackground();
  g_cover_budget = COVER_REQUEST_BUDGET;
  if(Game *selected=visibleGame(sel)) ensureCover(*selected,true);
  GLay L=gridLayout();
  int n=(int)g_visibleGames.size(), per=L.cols*L.rows;
  int pages=n?(n+per-1)/per:1,pageIndex=n?sel/per:0,page=pageIndex+1;
  int bandH = g_launcherPortrait?topBarH()-4:L.y0-4;
  fillRect(0,0,SW,bandH,COL_PANEL);
  if(!hasAnimatedBackground()) fillRect(0,bandH,SW,2,COL_SEL);
  char pinfo[160]; snprintf(pinfo,sizeof(pinfo),"%d / %d    \xc2\xb7    Page %d / %d    \xc2\xb7    Sort: %s",n?sel+1:0,n,page,pages,SORT_NAME[g_sort]);
  if(g_launcherPortrait){
    int logoH=highResolutionUi()?62:48;
    if(g_logo){SDL_Rect logoRect={18,10,logoH,logoH};SDL_RenderCopy(g_ren,g_logo,nullptr,&logoRect);}
    std::string shownInfo=fittedText(g_font_sm,pinfo,std::max(80,SW-2*(logoH+34)));
    drawTextC(g_font_sm,SW/2,highResolutionUi()?22:15,shownInfo.c_str(),COL_VAL);
    std::string shownFolder=fittedText(g_font_sm,gamedirLabel?gamedirLabel:"",SW-52);
    drawTextC(g_font_sm,SW/2,bandH-TTF_FontHeight(g_font_sm)-12,shownFolder.c_str(),COL_DIM);
  } else {
    int lh=bandH-12;
    if(g_logo){SDL_Rect ld={26,(bandH-lh)/2,lh,lh};SDL_RenderCopy(g_ren,g_logo,nullptr,&ld);}
    drawTextC(g_font,SW/2,(bandH-TTF_FontHeight(g_font))/2,pinfo,COL_VAL);
    int pinfoRight=SW/2+textW(g_font,pinfo)/2;
    int folderMaxW=(SW-34)-(pinfoRight+24);
    drawScrollTextR(g_font_sm,SW-34,(bandH-TTF_FontHeight(g_font_sm))/2,folderMaxW,gamedirLabel,COL_DIM);
  }

  int rowStride=L.chh+(L.titleH?L.titleH+8:0)+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c;
    if(idx>=n) continue;
    Game&g=*g_visibleGames[idx];
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    bool cur=(idx==sel);
    ensureCover(g,true);
    fillRect(x+4,y+6,L.cw,L.chh,(SDL_Color){0,0,0,55});
    fillRect(x+2,y+3,L.cw,L.chh,(SDL_Color){0,0,0,70});
    if(g.cover){
      Uint32 el=SDL_GetTicks()-g.coverAt; Uint8 fa=!g_uiAnimations?255:(el<180?(Uint8)(255*el/180):255);
      SDL_SetTextureAlphaMod(g.cover,fa);
      SDL_SetTextureColorMod(g.cover,cur?255:150,cur?255:150,cur?255:150);
      SDL_Rect d={x,y,L.cw,L.chh}; SDL_RenderCopy(g_ren,g.cover,nullptr,&d);
    }
    else { fillRect(x,y,L.cw,L.chh,COL_CARD); const std::string_view noCover=LauncherLocalization::Translate("NO COVER");
      const std::string shown=fittedText(g_font_sm,std::string(noCover),L.cw-12);drawTextC(g_font_sm,x+L.cw/2,y+L.chh/2-8,shown.c_str(),COL_DIM); }
    border(x,y,L.cw,L.chh,1,(SDL_Color){12,13,18,255});
    fillRect(x,y,L.cw,1,(SDL_Color){255,255,255,26});
    if(cur){ const int G=6;
      for(int i=G;i>=1;i--){ Uint8 a=(Uint8)(150*(G-i+1)/G); border(x-2-i,y-2-i,L.cw+4+2*i,L.chh+4+2*i,1,(SDL_Color){255,170,0,a}); }
      border(x-2,y-2,L.cw+4,L.chh+4,2,COL_SEL);
    }
    if(g_showRegionFlags && g.region>0 && g_flag[g.region]){
      int fw=L.cw*26/100; if(fw>30)fw=30; if(fw<16)fw=16; int fh=fw*2/3;
      SDL_Rect fd={x+6,y+6,fw,fh}; SDL_RenderCopy(g_ren,g_flag[g.region],nullptr,&fd);
      border(x+6,y+6,fw,fh,1,(SDL_Color){10,12,18,255});
    }
    if(g_showCustomSettingsBadges && g.hasCfg){ int ds=L.cw/11<12?12:L.cw/11; fillRect(x+L.cw-ds-8,y+8,ds,ds,COL_SEL); border(x+L.cw-ds-8,y+8,ds,ds,2,(SDL_Color){10,12,18,255}); }
    if(g_showCompatBadges && compatdb_loaded()){
      const int compat=compatdb_state(g.title_id);
      if(compat!=COMPAT_UNKNOWN){
        unsigned char rgb[3]; compatdb_state_color(compat,rgb);
        const int ds=L.cw/11<12?12:L.cw/11, bx=x+8, by=y+L.chh-ds-8;
        fillRect(bx,by,ds,ds,(SDL_Color){rgb[0],rgb[1],rgb[2],255});
        border(bx,by,ds,ds,2,(SDL_Color){10,12,18,255});
      }
    }
    if(g_showGameTitles) drawTitleCell(x+L.cw/2,L.cw,y+L.chh+6,g.title,cur,cur?COL_VAL:COL_DIM);
  }
  if(Game *selected=visibleGame(sel))ensureCover(*selected,true);
  const int prefetchStart=(pageIndex+1)*per;
  for(int index=prefetchStart;index<std::min(n,prefetchStart+per);index++)
    ensureCover(*g_visibleGames[index]);
  if(n==0){
    const char *message=!g_libraryScan.complete.load(std::memory_order_acquire)?"Loading library...":
      (g_games.empty()?"No Vita games found -- install a game from Settings":"No games match this view");
    drawTextC(g_font,SW/2,SH/2,LauncherLocalization::Translate(message).data(),COL_DIM);
  }
  FootItem foot[] = {
    { g_gA, "Launch", FA_LAUNCH }, { g_gY, "Sort", FA_SORT },
    { g_gX, "Settings", FA_SETTINGS }, { g_gPlus, "Game Menu", FA_OPTIONS },
    { g_gMinus, "Filter", FA_FILTER }, { g_gL, "", FA_PAGEL }, { g_gR, "Page", FA_PAGER }, { g_gB, "Quit", FA_QUIT },
  };
  drawUpdateNotification();
  drawFooterHints(foot, 8, SH-26);
  presentUi();
}

// Horizontal edges turn whole grid pages; vertical movement stays within a page.
static int gridNav(int sel,int dx,int dy,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, page=sel/per, pos=sel%per, cr=pos/cols, cc=pos%cols;
  auto clamp=[&](int i){ return i>=n? n-1 : (i<0?0:i); };
  if(dx>0){ // right: next cell, or turn to the next page (same row, first col)
    if(cc<cols-1 && page*per+cr*cols+cc+1 < n) return page*per+cr*cols+cc+1;
    if((page+1)*per < n) return clamp((page+1)*per + cr*cols);
    return sel;
  }
  if(dx<0){ // left: prev cell, or previous page (same row, last col)
    if(cc>0) return sel-1;
    if(page>0) return clamp((page-1)*per + cr*cols + (cols-1));
    return sel;
  }
  if(dy>0){ // down: within the page only (no page change on the vertical edges)
    if(cr<rows-1 && page*per+(cr+1)*cols+cc < n) return page*per+(cr+1)*cols+cc;
    return sel;
  }
  if(dy<0){ // up: within the page only
    if(cr>0) return sel-cols;
    return sel;
  }
  return sel;
}

// Jump a whole page (L/R shoulders), keeping the cell position.
static int gridPage(int sel,int dir,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, pos=sel%per, maxpage=(n-1)/per;
  int np=sel/per + dir; if(np<0) np=0; if(np>maxpage) np=maxpage;
  int i=np*per+pos; return i>=n? n-1 : i;
}

// ---------------------------------------------------------------------------
// Self-contained release: the emulator is bundled inside the launcher's RomFS.
// It is copied lazily when a game or installer is launched so opening the
// library never waits for a large first-boot extraction.
// `detail` is a second, smaller line for the thing being worked on (a file name,
// which is routinely far wider than the screen). It is ellipsised to the bar
// width instead of running off both edges, and the cancel hint uses the launcher's
// glyph footer rather than being spliced into the text.
static void drawSetupProgress(int pct, const char *msg, const char *detail, bool cancellable) {
  clearUiBackground();
  const int bw = SW * 2 / 3, bx = (SW - bw) / 2, bh = 36;
  const bool hasDetail = detail && *detail;
  const int by = SH / 2 + (hasDetail ? 56 : 40);
  if (g_logo) { int s = 140; SDL_Rect ld = {(SW - s) / 2, SH / 2 - 180, s, s}; SDL_RenderCopy(g_ren, g_logo, nullptr, &ld); }
  drawTextC(g_font, SW / 2, SH / 2 - 14, fittedText(g_font, msg ? msg : "", bw).c_str(), COL_TXT);
  if (hasDetail)
    drawTextC(g_font_sm, SW / 2, SH / 2 + 22, fittedText(g_font_sm, detail, bw).c_str(), COL_DIM);
  border(bx, by, bw, bh, 2, COL_SEL);
  fillRect(bx + 3, by + 3, (bw - 6) * pct / 100, bh - 6, COL_HI);
  char t[16]; snprintf(t, sizeof(t), "%d%%", pct);
  drawTextC(g_font_sm, SW / 2, by + bh + 14, t, COL_DIM);
  if (cancellable) {
    FootItem footer[] = { { g_gB, "Cancel", FA_NONE } };
    drawFooterHints(footer, 1, SH - 26);
  }
  presentUi();
}

// ---------------------------------------------------------------------------
// Firmware setup: graphical download of the three PS Vita firmware PUPs, then
// hand off to the emulator installer (Vita3K.nro --install) via g_pendingInstall.
// ---------------------------------------------------------------------------
// firmware_download_all() progress hook: repaint the full-screen setup progress
// as the transfer ticks (drain input so the applet stays responsive). Shows the
// live speed and downloaded/total size the downloader now reports.
static void fwProgressCb(int idx, int total, int pct, const char *label,
                         double mbps, long long dlnow, long long dltotal) {
  if (!beginUiFrame()) return;
  SDL_Event e; while (pollUiEvent(e)) { /* discard: keep the queue drained */ }
  const double dlMB = (double)dlnow / (1024.0 * 1024.0);
  char msg[220];
  if (dltotal > 0) {
    const double totMB = (double)dltotal / (1024.0 * 1024.0);
    snprintf(msg, sizeof(msg), "%s  (%d/%d)    %.0f / %.0f MB    %.1f MB/s", label, idx, total, dlMB, totMB, mbps);
  } else {
    snprintf(msg, sizeof(msg), "%s  (%d/%d)    %.0f MB    %.1f MB/s", label, idx, total, dlMB, mbps);
  }
  drawSetupProgress(pct, msg);
}

// Firmware setup screen: shows install status + three actions -- "Download &
// install" (fetch the 3 PUPs), the full file manager, and "Install staged
// files" for PUPs already copied into install/. All end at the same chainload via g_pendingInstall; the
// actual crypto install runs in the emulator. Reachable from Settings and the
// first-start prompt; returns when the user backs out or an install starts.
static void firmwareSetupFlow() {
  bool installed = firmware_is_installed();
  const int cy = SH >= 1080 ? 200 : 138;
  const int bw = SW * 2 / 3, bh = 64, bx = (SW - bw) / 2;
  const int by0 = cy + 168;              // "Download & install" button
  const int by1 = by0 + bh + 14;         // "Import from storage" button
  const int by2 = by1 + bh + 14;         // "Install staged files" button
  int sel = 0;
  beginScreenFx();

  // Fetch all 3 PUPs, then hand off to the emulator installer. Returns true when
  // the flow should return to the caller (install pending), false to stay open.
  auto doDownload = [&]() -> bool {
    if(!g_networkReady){
      modalMessageStatic("Network unavailable",{"Firmware download needs an initialized network connection.",
        "Use Library & storage > File manager instead."});beginScreenFx();return false;
    }
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    int rc = firmware_download_all(fwProgressCb);
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if(rc==0){
      toastStatic("Firmware downloaded - installing...");
      g_pendingInstall = true;           // main() chainloads Vita3K.nro --install
      return true;
    }
    const char *failure = rc==1?"Could not download the pre-install firmware.":
      rc==2?"Could not download the system firmware.":
      rc==3?"Could not download the firmware system data.":"Could not download a firmware file.";
    modalMessage("Firmware download failed", {
      uiText(failure),
      "",
      uiText("Make sure the console is online and DNS is reachable."),
      uiText("Try again, or copy the PUP files from SD, USB, or SMB instead.") });
    installed = firmware_is_installed();
    beginScreenFx();
    return false;
  };
  auto doImport = [&]() -> bool {
    const bool ready=runFileManager();
    installed=firmware_is_installed();
    beginScreenFx();
    return ready;
  };
  // Skip the download: verify the 3 PUPs are already in install/, then chainload
  // the same emulator installer. If any are missing, tell the user what to copy.
  auto doLocal = [&]() -> bool {
    std::vector<std::string> missing;
    if(firmware_local_files_present(&missing)){
      toastStatic("Installing firmware from local files...");
      g_pendingInstall = true;           // main() chainloads Vita3K.nro --install
      return true;
    }
    std::vector<std::string> lines = {
      "Copy the PS Vita firmware PUP files into:",
      "sdmc:/switch/vita3k/install/",
      "",
      "Missing or invalid:" };
    for(auto &m : missing) lines.push_back("   " + m);
    modalMessage("Firmware files not found", lines);
    beginScreenFx();
    return false;
  };

  for (;;) {
    if (!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_TAP){
          if(ty>=SH-40){ return; }                                   // bottom band = back
          if(tx>=bx && tx<bx+bw && ty>=by0 && ty<by0+bh){ sel=0; if(doDownload()) return; continue; }
          if(tx>=bx && tx<bx+bw && ty>=by1 && ty<by1+bh){ sel=1; if(doImport())   return; continue; }
          if(tx>=bx && tx<bx+bw && ty>=by2 && ty<by2+bh){ sel=2; if(doLocal())    return; continue; }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
          sel=(sel+2)%3;break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%3;break;
        case BTN_CONFIRM:
          if(sel==0){ if(doDownload()) return; }
          else if(sel==1){if(doImport())return;}
          else {if(doLocal())return;}
          break;
        case BTN_CANCEL: return;
      }
    }
    clearUiBackground();
    drawLocalizedHeader("Firmware setup", nullptr);
    SDL_Color sc = installed ? (SDL_Color){120,215,130,255} : (SDL_Color){240,160,95,255};
    char st[96]; snprintf(st,sizeof(st),"Firmware: %s", installed?"Installed":"Not installed");
    drawTextC(g_font_big, SW/2, cy, st, sc);
    drawTextC(g_font_sm, SW/2, cy+58, "The PS Vita firmware provides system fonts, video", COL_DIM);
    drawTextC(g_font_sm, SW/2, cy+84, "playback and the on-screen dialogs games rely on.", COL_DIM);
    // two action buttons; the selected one gets the amber accent border.
    { bool cur=sel==0;
      fillRect(bx,by0,bw,bh, cur?(SDL_Color){52,100,52,245}:(SDL_Color){38,64,40,215});
      border(bx,by0,bw,bh,2, cur?COL_SEL:COL_DIM);
      drawTextC(g_font, SW/2, by0+(bh-TTF_FontHeight(g_font))/2, "Download & install firmware  (~350 MB)", cur?COL_VAL:COL_TXT); }
    { bool cur=sel==1;
      fillRect(bx,by1,bw,bh, cur?(SDL_Color){52,100,52,245}:(SDL_Color){38,64,40,215});
      border(bx,by1,bw,bh,2, cur?COL_SEL:COL_DIM);
      drawTextC(g_font, SW/2, by1+(bh-TTF_FontHeight(g_font))/2, "Open file manager for PUP files", cur?COL_VAL:COL_TXT); }
    { bool cur=sel==2;
      fillRect(bx,by2,bw,bh, cur?(SDL_Color){52,100,52,245}:(SDL_Color){38,64,40,215});
      border(bx,by2,bw,bh,2, cur?COL_SEL:COL_DIM);
      drawTextC(g_font, SW/2, by2+(bh-TTF_FontHeight(g_font))/2, "Install staged PUP files", cur?COL_VAL:COL_TXT); }
    drawTextC(g_font_sm, SW/2, SH-40, "A: Select      B: Back", COL_DIM);
    drawFadeIn();
    presentUi();
    waitForNextFrame();
  }
}

// Extract the bundled emulator to the SD so it can be chainloaded (the Switch can't chainload a
// file embedded in another .nro's romfs). Runs once; skipped by a build stamp on later launches.
static bool validNro(const std::string &path, long long expectedSize) {
  struct stat fileStat{};
  if (stat(path.c_str(), &fileStat) != 0 || fileStat.st_size <= 0 ||
      (expectedSize >= 0 && fileStat.st_size != expectedSize)) return false;
  FILE *file = fopen(path.c_str(), "rb");
  if (!file) return false;
  NroStart start{};
  NroHeader header{};
  bool ok = fread(&start, 1, sizeof(start), file) == sizeof(start) &&
            fread(&header, 1, sizeof(header), file) == sizeof(header);
  fclose(file);
  if (!ok || header.magic != NROHEADER_MAGIC ||
      header.size < sizeof(start) + sizeof(header) || header.size > (u64)fileStat.st_size)
    return false;
  for (const auto &segment : header.segments)
    if (segment.file_off > header.size || segment.size > header.size - segment.file_off)
      return false;
  return true;
}

static bool readEmbeddedEmulatorHash(std::array<u8, SHA256_HASH_SIZE> &hash,
                                     std::string &text) {
  FILE *file=fopen(EMU_HASH_SRC,"rb");
  if(!file) return false;
  char buffer[128];
  const size_t size=fread(buffer,1,sizeof(buffer),file);
  const bool ok=!ferror(file) && feof(file);
  fclose(file);
  if(!ok) return false;
  std::string value=trim(std::string(buffer,size));
  if(value.size()!=SHA256_HASH_SIZE*2) return false;
  auto hex=[](char c)->int {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
  };
  static const char digits[]="0123456789abcdef";
  text.clear(); text.reserve(SHA256_HASH_SIZE*2);
  for(size_t i=0;i<SHA256_HASH_SIZE;i++){
    int high=hex(value[i*2]),low=hex(value[i*2+1]);
    if(high<0||low<0) return false;
    hash[i]=(u8)((high<<4)|low);
    text+=digits[high]; text+=digits[low];
  }
  return true;
}

static bool ensureEmu() {
  std::string marker = std::string(EMU_HOST_DIR) + "/.core_build";
  const std::string tmp = std::string(EMU_NRO) + ".tmp";
  const std::string old = std::string(EMU_NRO) + ".old";
  bool destinationExists=false,oldExists=false,tmpExists=false;
  if(!queryRegularFile(EMU_NRO,destinationExists) || !queryRegularFile(old,oldExists) ||
     !queryRegularFile(tmp,tmpExists)) return false;
  if (!destinationExists && oldExists) {
    if (validNro(old,-1)) {
      if(rename(old.c_str(),EMU_NRO)!=0) return false;
      destinationExists=true;
    } else if(remove(old.c_str())!=0) {
      return false;
    }
    oldExists=false;
    fsdevCommitDevice("sdmc");
  } else if (destinationExists && oldExists) {
    if (!validNro(EMU_NRO, -1) && validNro(old, -1)) {
      if (remove(EMU_NRO) != 0 || rename(old.c_str(), EMU_NRO) != 0) return false;
    } else {
      if(remove(old.c_str())!=0) return false;
    }
    fsdevCommitDevice("sdmc");
  }
  if(tmpExists && remove(tmp.c_str())!=0) return false;

  std::array<u8,SHA256_HASH_SIZE> expectedHash{};
  std::string expectedHashText;
  if(!readEmbeddedEmulatorHash(expectedHash,expectedHashText) || !recoverAtomicFile(marker)) return false;

  struct stat sourceStat{};
  if (stat(EMU_NRO_SRC, &sourceStat) != 0 || sourceStat.st_size <= 0 || !S_ISREG(sourceStat.st_mode)) return false;
  char cur[80] = {0};
  if (FILE *mf = fopen(marker.c_str(), "r")) { if (!fgets(cur, sizeof(cur), mf)) cur[0] = 0; fclose(mf); }
  struct stat destinationStat{};
  if (trim(cur) == expectedHashText && stat(EMU_NRO, &destinationStat) == 0 &&
      destinationStat.st_size == sourceStat.st_size && validNro(EMU_NRO, sourceStat.st_size))
    return true;

  FILE *in = fopen(EMU_NRO_SRC, "rb"), *out = fopen(tmp.c_str(), "wb");
  if (!in || !out) {
    if (in) fclose(in);
    if (out) fclose(out);
    remove(tmp.c_str());
    return false;
  }
  static char buf[1 << 16]; size_t n; bool ok = true; long long written = 0; int lastPct = -1;
  const std::string progressText=uiText("Preparing Vita3K...");
  Sha256Context hashContext;
  sha256ContextCreate(&hashContext);
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    sha256ContextUpdate(&hashContext,buf,n);
    written += (long long)n;
    int pct = (int)((unsigned __int128)written * 100 / sourceStat.st_size);
    if (pct != lastPct) {
      if(g_directForwarderBoot){
        if(!appletMainLoop()){ok=false;break;}
      }else{
        if(!beginUiFrame()){ ok=false; break; }
        SDL_Event event; while(pollUiEvent(event)) {}
        if(g_exitRequested){ ok=false; break; }
        drawSetupProgress(pct, progressText.c_str());
      }
      lastPct = pct;
    }
  }
  if (ferror(in)) ok = false;
  if (fclose(in) != 0) ok = false;
  if (fflush(out) != 0 || fsync(fileno(out)) != 0) ok = false;
  if (fclose(out) != 0) ok = false;
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  std::array<u8,SHA256_HASH_SIZE> actualHash{};
  sha256ContextGetHash(&hashContext,actualHash.data());
  if (!ok || stat(tmp.c_str(), &destinationStat) != 0 ||
      destinationStat.st_size != sourceStat.st_size || actualHash != expectedHash ||
      !validNro(tmp, sourceStat.st_size)) {
    remove(tmp.c_str());
    return false;
  }
  if (!replaceAtomic(EMU_NRO, tmp)) {
    remove(tmp.c_str());
    return false;
  }
  return writeAtomicText(marker, expectedHashText + "\n");
}

static bool ensureDirectory(const char *path) {
  if (mkdir(path, 0777) == 0) return true;
  if (errno != EEXIST) return false;
  struct stat st{};
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void cleanupLauncher() {
  stopGameScan();
  stopCoverDecodeWorker();
  stopStorageWorker();
  LauncherUpdate_Shutdown();
  // Import jobs and browser directory handles have unwound before cleanup.
  // Retire SMB/USB while sockets are still alive, then tear networking down last.
  Vita3KLauncher::Storage::Shutdown();
  g_usbReady.store(false,std::memory_order_release);
  for (auto &game : g_games) {
    if (game.cover) SDL_DestroyTexture(game.cover);
    game.cover = nullptr;
  }
  clearTextCaches();
  for (int i=1;i<4;i++) {
    if (g_flag[i]) SDL_DestroyTexture(g_flag[i]);
    g_flag[i]=nullptr;
  }
  SDL_Texture **glyphs[] = { &g_gA, &g_gB, &g_gX, &g_gY, &g_gPlus, &g_gMinus,
                             &g_gLeft, &g_gRight, &g_gL, &g_gR };
  for (SDL_Texture **glyph : glyphs) {
    if (*glyph) SDL_DestroyTexture(*glyph);
    *glyph=nullptr;
  }
  if (g_logo) SDL_DestroyTexture(g_logo);
  if (g_glowTexture) SDL_DestroyTexture(g_glowTexture);
  g_logo=nullptr; g_glowTexture=nullptr;

  if(g_ren) SDL_SetRenderTarget(g_ren,nullptr);
  if(g_uiTarget) SDL_DestroyTexture(g_uiTarget);
  g_uiTarget=nullptr;

  if (g_font) TTF_CloseFont(g_font);
  if (g_font_sm) TTF_CloseFont(g_font_sm);
  if (g_font_big) TTF_CloseFont(g_font_big);
  g_font=g_font_sm=g_font_big=nullptr;
  if (g_plReady) plExit();
  g_plReady=false;

  uiAudioShutdown();
  closeController();
  if (g_ren) SDL_DestroyRenderer(g_ren);
  if (g_win) SDL_DestroyWindow(g_win);
  g_ren=nullptr; g_win=nullptr;
  if (g_imgReady) IMG_Quit();
  if (g_ttfReady) TTF_Quit();
  if (g_sdlReady) SDL_Quit();
  g_imgReady=g_ttfReady=g_sdlReady=false;
  if (g_griddbReady) griddb_global_exit();
  g_griddbReady=false;
  if(g_networkReady){ curl_global_cleanup(); socketExit(); }
  g_networkReady=false;
  if (g_romfsReady) romfsExit();
  g_romfsReady=false;
}

static int startupFailure(const char *message) {
  if (g_sdlReady) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Vita3K Launcher",message,g_win);
  cleanupLauncher();
  return 0;
}

static int earlyStartupFailure(const char *message,Result result){
  consoleInit(nullptr);
  printf("Vita3K-nx - early startup failure\n\n%s\n\n",message);
  printf("Result: 0x%08X (module %u, description %u)\n\n",result,R_MODULE(result),R_DESCRIPTION(result));
  printf("Press A to exit.");consoleUpdate(nullptr);
  PadState failurePad;padConfigureInput(1,HidNpadStyleSet_NpadStandard);padInitializeDefault(&failurePad);
  while(appletMainLoop()){
    padUpdate(&failurePad);
    if(padGetButtonsDown(&failurePad)&HidNpadButton_A)break;
    svcSleepThread(16000000ULL);
  }
  consoleExit(nullptr);
  return 0;
}


static bool isAppletMode(){
  const AppletType type=appletGetAppletType();
  return type!=AppletType_Application&&type!=AppletType_SystemApplication;
}

static void runAppletInstaller(){
  enum class State{Ready,Installing,Installed,Failed};
  State state=State::Ready;std::string error,workerError;bool workerOk=false;
  std::atomic_bool complete{false};std::thread worker;
  auto finishWorker=[&]{if(worker.joinable())worker.join();};
  auto install=[&]{
    if(state==State::Installing)return;
    finishWorker();complete.store(false,std::memory_order_release);error.clear();state=State::Installing;
    worker=std::thread([&]{char message[512]{};workerOk=forwarder_create_launcher("romfs:/logo.png",message,sizeof(message));
      workerError=workerOk?std::string{}:(message[0]?message:"Unknown installation error");
      complete.store(true,std::memory_order_release);
      SDL_Event wake{};wake.type=USB_STATUS_EVENT;SDL_PushEvent(&wake);});
  };
  beginScreenFx();
  while(beginUiFrame()){
    if(complete.exchange(false,std::memory_order_acq_rel)){finishWorker();error=std::move(workerError);state=workerOk?State::Installed:State::Failed;}
    const int panelWidth=std::min(980,SW-120),panelHeight=std::min(SH>=1080?560:450,SH-150);
    const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2+20;
    const int buttonWidth=std::min(700,panelWidth-100),buttonHeight=SH>=1080?112:86;
    const int buttonX=(SW-buttonWidth)/2,buttonY=panelY+panelHeight-buttonHeight-(SH>=1080?72:55);
    SDL_Event event;while(pollUiEvent(event)){pumpStick(event);int x=0,y=0;const TouchKind touch=touchFeed(event,&x,&y);
      if(touch==TOUCH_TAP&&x>=buttonX&&x<buttonX+buttonWidth&&y>=buttonY&&y<buttonY+buttonHeight&&state!=State::Installing&&state!=State::Installed)install();
      if(event.type==SDL_CONTROLLERBUTTONDOWN){
        if(event.cbutton.button==BTN_CONFIRM&&state!=State::Installing&&state!=State::Installed)install();
        else if(event.cbutton.button==BTN_CANCEL&&state!=State::Installing){finishWorker();return;}
      }}
    clearUiBackground();drawLocalizedHeader("Applet mode installer",nullptr);glassPanel(panelX,panelY,panelWidth,panelHeight);border(panelX,panelY,panelWidth,panelHeight,2,COL_SEL);
    const int textWidth=panelWidth-(SH>=1080?160:100),lineHeight=SH>=1080?42:32;
    std::vector<std::string> messages;
    auto localized=[](const char *source){return std::string(LauncherLocalization::Translate(source));};
    if(state==State::Installed)messages={localized("Installed on the HOME Menu."),"You can close this installer and launch Vita3K from HOME."};
    else if(state==State::Failed)messages={localized("Installation failed"),error};
    else if(state==State::Installing)messages={localized("Installing HOME Menu shortcut..."),"Please wait while the shortcut is committed safely."};
    else messages={localized("Vita3K is running in applet mode."),localized("Applet mode has limited memory and is not suitable for emulation."),localized("Install a HOME Menu shortcut to use full memory and normal performance.")};
    int textY=panelY+42;
    for(const std::string &message:messages){const auto lines=wrapTextLines(g_font,message,textWidth);
      for(const std::string &line:lines){drawTextC(g_font,SW/2,textY,line.c_str(),state==State::Failed?SDL_Color{255,155,155,255}:COL_TXT);textY+=lineHeight;}textY+=12;}
    const bool installed=state==State::Installed,failed=state==State::Failed;
    fillRect(buttonX,buttonY,buttonWidth,buttonHeight,installed?SDL_Color{30,92,58,240}:failed?SDL_Color{105,48,48,240}:COL_FOCUS);
    border(buttonX,buttonY,buttonWidth,buttonHeight,3,installed?SDL_Color{100,225,145,255}:failed?SDL_Color{235,125,125,255}:COL_SEL);
    const char *button=installed?"Installed":state==State::Installing?"Installing HOME Menu shortcut...":failed?"Try again":"Install to HOME Menu";
    TTF_Font *buttonFont=textW(g_font_big,LauncherLocalization::Translate(button).data())<=buttonWidth-48?g_font_big:g_font;
    drawTextC(buttonFont,SW/2,buttonY+(buttonHeight-TTF_FontHeight(buttonFont))/2,LauncherLocalization::Translate(button).data(),COL_VAL);
    if(state==State::Installed){FootItem footer[]={{g_gB,"Exit",FA_NONE}};drawFooterHints(footer,1,SH-26);}
    else if(state!=State::Installing){FootItem footer[]={{g_gA,failed?"Try again":"Install",FA_NONE},{g_gB,"Exit",FA_NONE}};drawFooterHints(footer,2,SH-26);}
    drawFadeIn();presentUi();waitForNextFrame();
  }
  finishWorker();
}

int main(int argc, char **argv){
  std::string positionalForwarderArgument,directGameArgument;
  if(argc>=2&&argv[1]&&argv[1][0]&&argv[1][0]!='-')
    positionalForwarderArgument=argv[1];
  for(int index=1;index+1<argc;index++) if(!strcmp(argv[index],"-g")){
    directGameArgument=argv[index+1];
    break;
  }
  g_directForwarderBoot=!positionalForwarderArgument.empty()||!directGameArgument.empty();

  // Remember where this launcher was actually launched from, so HOME forwarders chainload the
  // real launcher location instead of a hardcoded path.
  extern std::string g_forwarderSelfPath;
  if(argc>=1 && argv[0] && argv[0][0]) g_forwarderSelfPath=argv[0];
  g_launcherNroPath=LauncherUpdate_ResolveLauncherPath(g_forwarderSelfPath);
  std::string updateRecoveryError;
  const bool updateRecoveryOk=LauncherUpdate_RecoverInstallation(g_launcherNroPath,updateRecoveryError);
  const Result romfsResult=romfsInit();
  if(R_FAILED(romfsResult)) return earlyStartupFailure("Could not mount the embedded launcher RomFS.",romfsResult);
  g_romfsReady=true;
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"linear");
  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_AUDIO)!=0){ return startupFailure("SDL initialization failed."); }
  g_sdlReady=true;
  // Opening audio before applet focus can stall audren and hang SDL_CloseAudioDevice.
  for(int i=0;i<200&&appletGetFocusState()!=AppletFocusState_InFocus;i++){
    if(!appletMainLoop())break;
    svcSleepThread(10000000ULL);
  }
  uiAudioInit();
  if(TTF_Init()!=0) return startupFailure("Font initialization failed.");
  g_ttfReady=true;
  const int imageFlags=IMG_INIT_PNG|IMG_INIT_JPG;
  if((IMG_Init(imageFlags)&imageFlags)!=imageFlags) return startupFailure("Image initialization failed.");
  g_imgReady=true;
  if(appletGetOperationMode()==AppletOperationMode_Console){ SW=1920; SH=1080; }
  g_win=SDL_CreateWindow("Vita3K",0,0,SW,SH,SDL_WINDOW_FULLSCREEN);
  if(!g_win) return startupFailure("Could not create the launcher window.");
  g_ren=SDL_CreateRenderer(g_win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if(!g_ren) return startupFailure("Could not create the launcher renderer.");
  SDL_SetRenderDrawBlendMode(g_ren,SDL_BLENDMODE_BLEND);
  if(SDL_GetRendererOutputSize(g_ren,&SW,&SH)!=0) return startupFailure("Could not query the display size.");
  if(g_directForwarderBoot){
    SDL_SetRenderDrawColor(g_ren,0,0,0,255);
    SDL_RenderClear(g_ren);
    SDL_RenderPresent(g_ren);
  }
  g_outputW=SW;g_outputH=SH;
  { SDL_Surface *ls=IMG_Load("romfs:/logo.png"); if(ls){ g_logo=SDL_CreateTextureFromSurface(g_ren,ls); SDL_FreeSurface(ls); } }
  makeFlags();   // region-flag badges (rendered once, reused per game)

  for(int i=0;i<SDL_NumJoysticks();i++) if(SDL_IsGameController(i)){ openController(i); break; }

  if(R_FAILED(plInitialize(PlServiceType_User))) return startupFailure("System font service initialization failed.");
  g_plReady=true;
  PlFontData fd{};
  if(R_FAILED(plGetSharedFontByType(&fd,PlSharedFontType_Standard))||!fd.address||!fd.size||fd.size>INT_MAX)
    return startupFailure("Could not load the system font.");
  int sc=SH>=1080?1:0;
  auto openFont=[&](int size)->TTF_Font* {
    SDL_RWops *rw=SDL_RWFromConstMem(fd.address,(int)fd.size);
    return rw?TTF_OpenFontRW(rw,1,size):nullptr;
  };
  g_font_sm=openFont(sc?26:20);
  g_font=openFont(sc?32:26);
  g_font_big=openFont(sc?52:40);
  if(!g_font_sm||!g_font||!g_font_big) return startupFailure("Could not open the system font.");
  makeGlyphs();   // button-icon textures for the control hints (needs g_font_sm)
  if(isAppletMode()){
    LauncherLocalization::SetLanguage("system");applyLauncherAppearance();runAppletInstaller();cleanupLauncher();return 0;
  }
  if(R_SUCCEEDED(socketInitializeDefault())){
    if(curl_global_init(CURL_GLOBAL_ALL)==CURLE_OK) g_networkReady=true;
    else socketExit();
  }
  g_griddbReady=g_networkReady&&griddb_global_init();
  Vita3KLauncher::Storage::SetUsbStatusCallback(storageStatusWake,nullptr);
  LauncherUpdate_SetWakeCallback(storageStatusWake,nullptr);

  // First-launch bootstrap: create and validate the SD folder skeleton.
  const char *directories[]={
    "sdmc:/switch",DATA_DIR,COVERS_DIR,GAMECFG_DIR,
    "sdmc:/switch/vita3k/vita","sdmc:/switch/vita3k/vita/ux0",APP_DIR,
    CACHE_DIR,INSTALL_DIR,LSFG_DIR,EMU_HOST_DIR
  };
  for(const char *directory:directories)
    if(!ensureDirectory(directory)) return startupFailure("Could not create the Vita3K data directories.");
  compatdb_load(CACHE_DIR);
  std::string importRecoveryError;
  const bool importRecoveryOk=recoverInterruptedImports(importRecoveryError);

  struct stat bst;
  recoverAtomicFile(LAUNCHER_INI);
  bool firstRun = (stat(LAUNCHER_INI, &bst) != 0);
  storeLoad(g_global, LAUNCHER_INI);
  storeLoad(g_titles, TITLES_INI);
  storeLoad(g_recent, RECENT_INI);
  { int sm = atoi(storeGet(g_global,"Wrapper/SortMode","0")); if(sm>=0 && sm<SORT_COUNT) g_sort = sm; }
  if (firstRun) {
    // Seed a fresh launcher.ini with defaults + a blank SteamGridDB key line.
    // First launch only - never rewrite an existing config (would clobber a set key).
    g_active = &g_global;
    storeSet(g_global, "Wrapper/SteamGridDBKey", "");
    storeSet(g_global, "Wrapper/UiSounds", "true");
    storeSet(g_global, "Wrapper/Language", "system");
    storeSet(g_global, "Wrapper/Theme", "xmb");
    storeSet(g_global, "Wrapper/LauncherRotation", "0");
    storeSet(g_global, "Wrapper/GridColumns", "5");
    storeSet(g_global, "Wrapper/GridRows", "2");
    storeSet(g_global, "Wrapper/ShowGameTitles", "true");
    storeSet(g_global, "Wrapper/ShowRegionFlags", "true");
    storeSet(g_global, "Wrapper/ShowCustomSettingsBadges", "true");
    storeSet(g_global, "Wrapper/UiAnimations", "true");
    storeSet(g_global, "Wrapper/CheckUpdatesOnStartup", "true");
    storeSet(g_global, "Storage/SmbCount", "0");
    storeSet(g_global, "Browser/FavoriteCount", "0");
    commitAll();                        // write every managed setting at its default
    storeSave(g_global, LAUNCHER_INI);  // create launcher.ini immediately
  } else {
    bool changed=false;
    if(storeHas(g_global,"Wrapper/LauncherPortrait")&&!storeHas(g_global,"Wrapper/LauncherRotation")){
      storeSet(g_global,"Wrapper/LauncherRotation",!strcmp(storeGet(g_global,"Wrapper/LauncherPortrait","false"),"true")?"1":"0");
      storeRemove(g_global,"Wrapper/LauncherPortrait");changed=true;
    }
    // Migrate the old one-off SMB fields into the reusable Library & storage
    // share list, then remove them from the Launcher settings namespace.
    if(!storeHas(g_global,"Storage/SmbCount")){
      const std::string server=trim(storeGet(g_global,"Wrapper/SmbServer",""));
      const std::string share=trim(storeGet(g_global,"Wrapper/SmbShare",""));
      if(!server.empty()&&!share.empty()){
        storeSet(g_global,"Storage/SmbCount","1");
        storeSet(g_global,"Storage/Smb0Id","legacy");
        storeSet(g_global,"Storage/Smb0Name","Network share");
        storeSet(g_global,"Storage/Smb0Server",server.c_str());
        storeSet(g_global,"Storage/Smb0Share",share.c_str());
        storeSet(g_global,"Storage/Smb0Path",storeGet(g_global,"Wrapper/SmbPath",""));
        storeSet(g_global,"Storage/Smb0User",storeGet(g_global,"Wrapper/SmbUser",""));
        storeSet(g_global,"Storage/Smb0Password",storeGet(g_global,"Wrapper/SmbPassword",""));
        storeSet(g_global,"Storage/Smb0Domain",storeGet(g_global,"Wrapper/SmbDomain",""));
        storeSet(g_global,"Storage/Smb0AutoMount",!strcmp(storeGet(g_global,"Wrapper/SmbEnabled","false"),"true")?"true":"false");
      } else storeSet(g_global,"Storage/SmbCount","0");
      changed=true;
    }
    const char *legacySmbKeys[]={"Wrapper/SmbEnabled","Wrapper/SmbServer","Wrapper/SmbShare","Wrapper/SmbPath","Wrapper/SmbUser","Wrapper/SmbPassword","Wrapper/SmbDomain"};
    for(const char *key:legacySmbKeys)if(storeHas(g_global,key)){storeRemove(g_global,key);changed=true;}
    const struct { const char *key; const char *value; } launcherDefaults[]={
      {"Wrapper/UiSounds","true"},{"Wrapper/Language","system"},{"Wrapper/Theme","xmb"},
      {"Wrapper/LauncherRotation","0"},
      {"Wrapper/GridColumns","5"},{"Wrapper/GridRows","2"},
      {"Wrapper/ShowGameTitles","true"},{"Wrapper/ShowRegionFlags","true"},
      {"Wrapper/ShowCustomSettingsBadges","true"},{"Wrapper/UiAnimations","true"},
      {"Wrapper/CheckUpdatesOnStartup","true"},{"Browser/FavoriteCount","0"}
    };
    for(const auto &entry:launcherDefaults) if(!storeHas(g_global,entry.key)){
      storeSet(g_global,entry.key,entry.value); changed=true;
    }
    int columns=atoi(storeGet(g_global,"Wrapper/GridColumns","5"));
    int rows=atoi(storeGet(g_global,"Wrapper/GridRows","2"));
    int rotation=atoi(storeGet(g_global,"Wrapper/LauncherRotation","0"));
    if(columns<3||columns>8){ storeSet(g_global,"Wrapper/GridColumns","5"); changed=true; }
    if(rows<1||rows>3){ storeSet(g_global,"Wrapper/GridRows","2"); changed=true; }
    if(rotation<0||rotation>3){storeSet(g_global,"Wrapper/LauncherRotation","0");changed=true;}
    if(changed) storeSave(g_global,LAUNCHER_INI);
  }
  if(LauncherLocalization::FindLanguage(storeGet(g_global,"Wrapper/Language","system"))<0)
    storeSet(g_global,"Wrapper/Language","system");
  LauncherLocalization::SetLanguage(storeGet(g_global,"Wrapper/Language","system"));
  applyLauncherAppearance();
  uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  FrontendRequest frontendRequest=consumeFrontendRequest();
  if(frontendRequest.present){
    positionalForwarderArgument.clear();
    directGameArgument.clear();
    g_directForwarderBoot=false;
  }
  std::string directGameKey;
  if(!directGameArgument.empty()) directGameKey=forwarderTitleId(directGameArgument);
  else if(!positionalForwarderArgument.empty()) directGameKey=forwarderTitleId(positionalForwarderArgument);
  const bool directGameRequested=!directGameArgument.empty()||!positionalForwarderArgument.empty();
  if(directGameRequested&&directGameKey.empty()){
    modalMessageStatic("Game not found",{
      "The shortcut does not point to an installed Vita title.",
      "Reinstall the game or recreate the HOME shortcut."});
    cleanupLauncher();
    return 1;
  }
  if(!directGameRequested){
    startCoverDecodeWorker();
    startStorageWorker();
  }
  if(!updateRecoveryOk) modalMessage("Update recovery failed",{updateRecoveryError,uiText("The installed launcher was left unchanged where possible.")});
  if(!importRecoveryOk) modalMessage("Import recovery failed",{importRecoveryError,uiText("No new installer job was started.")});
  loadLibraryOrganization();
  if(!directGameRequested) startGameScan();

  int sel=0, top=0, rows=1;
  bool running=true, launch=false;
  std::string launchKey;   // key of the game being launched (== title id; per-game cfg)
  std::string launchTid;   // Vita title id, passed to the emulator as "-r <TITLEID>"

  if(!directGameKey.empty()){
    recordPlayed(directGameKey);
    launchKey=directGameKey;
    launchTid=directGameKey;
    launch=true;
    running=false;
  }
  if(!launch&&g_griddbReady&&strcmp(storeGet(g_global,"Wrapper/CheckUpdatesOnStartup","true"),"false")!=0)
    LauncherUpdate_StartCheck(installedReleaseTag());


  // First-start firmware nudge (once): games need the PS Vita firmware for fonts,
  // video and dialogs. Skipped on a forwarder headless boot (launch already set).
  if(!launch && !firmware_is_installed() && strcmp(storeGet(g_global,"Wrapper/FwPromptShown","0"),"1")!=0){
    storeSet(g_global,"Wrapper/FwPromptShown","1"); storeSave(g_global,LAUNCHER_INI);   // nag only once
    if(confirmBoxStatic("PS Vita firmware not installed", {
          "Vita games need the PS Vita firmware installed for",
          "fonts, video playback and system dialogs to work.",
          "",
          "Set it up now?  (downloads ~350 MB)" })){
      firmwareSetupFlow();
      if(g_pendingInstall) running=false;   // download done -> skip the grid, chainload Vita3K --install
    }
  }

  while(running && beginUiFrame()){
    std::string selectedKey;
    if(Game *selected=visibleGame(sel))selectedKey=selected->key;
    pumpGameScan();
    if(!selectedKey.empty())sel=visibleIndexForKey(selectedKey);
    else if(sel>=(int)g_visibleGames.size())sel=std::max(0,(int)g_visibleGames.size()-1);
    GLay L=gridLayout();
    int cols=L.cols; rows=L.rows;

    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0,n=(int)g_visibleGames.size(); TouchKind tk=touchFeed(e,&tx,&ty);   // touchscreen
        if(tk==TOUCH_SWIPE_L||tk==TOUCH_SWIPE_R){ sel=gridPage(sel,tk==TOUCH_SWIPE_L?+1:-1,cols,rows,n); top=n?(sel/(cols*rows))*rows:0; continue; }
        if(tk==TOUCH_TAP){
          int fa=footTapAct(tx,ty);
          if(fa==FA_NONE){ int hit=gridHitTest(tx,ty,top);
            if(hit>=0){
              if(hit==sel && n){Game *game=visibleGame(sel);recordPlayed(game->key);launchKey=game->key;launchTid=game->title_id;launch=true;running=false; }
              else sel=hit;                       // first tap selects, second launches
            }
          } else {                                 // footer glyph tapped -> reuse the button handler
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN;
            switch(fa){
              case FA_LAUNCH:   a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break;
              case FA_SORT:     a.cbutton.button=SDL_CONTROLLER_BUTTON_X; SDL_PushEvent(&a); break;
              case FA_OPTIONS:  a.cbutton.button=SDL_CONTROLLER_BUTTON_START; SDL_PushEvent(&a); break;
              case FA_SETTINGS: a.cbutton.button=BTN_SETTINGS; SDL_PushEvent(&a); break;
              case FA_FILTER:   a.cbutton.button=SDL_CONTROLLER_BUTTON_BACK; SDL_PushEvent(&a); break;
              case FA_PAGEL:    sel=gridPage(sel,-1,cols,rows,n); break;
              case FA_PAGER:    sel=gridPage(sel,+1,cols,rows,n); break;
              case FA_QUIT:     a.cbutton.button=BTN_CANCEL; SDL_PushEvent(&a); break;
            }
          }
          top=n?(sel/(cols*rows))*rows:0;
          if(!running) break;
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      int n=(int)g_visibleGames.size();
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=gridNav(sel,-1,0,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: sel=gridNav(sel,+1,0,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=gridNav(sel,0,-1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=gridNav(sel,0,+1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  sel=gridPage(sel,-1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: sel=gridPage(sel,+1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_X:              // Y (Nintendo): cycle listing order
          if(n){
            std::string keep=visibleGame(sel)->key;
            g_sort=(g_sort+1)%SORT_COUNT;
            char sb[8]; snprintf(sb,sizeof(sb),"%d",g_sort);
            storeSet(g_global,"Wrapper/SortMode",sb); storeSave(g_global,LAUNCHER_INI);
            applySort();
            sel=visibleIndexForKey(keep);
          }
          break;
        case SDL_CONTROLLER_BUTTON_BACK:
          libraryFilterMenu();sel=0;top=0;beginScreenFx();break;
        case BTN_CONFIRM:
          if(Game *game=visibleGame(sel)){ recordPlayed(game->key); launchKey=game->key; launchTid=game->title_id; launch=true; running=false; }
          break;
        case SDL_CONTROLLER_BUTTON_START:
          if(Game *game=visibleGame(sel)){ int r=perGameMenu(*game,g_pad);
            if(r==1){ recordPlayed(game->key); launchKey=game->key; launchTid=game->title_id; launch=true; running=false; }
            else if(r==2){ startGameScan(); sel=0; top=0; } }   // game deleted -> re-scan
          break;
        case BTN_SETTINGS: {                       // X: settings
          g_active=&g_global; runSettingsRoot(g_pad,nullptr);
          storeSave(g_global,LAUNCHER_INI);        // persist now -- don't rely on a clean exit
          if(g_updateInstallExitRequested){running=false;break;}
          { GLay updated=gridLayout(); cols=updated.cols; rows=updated.rows;
            int n=(int)g_visibleGames.size(); top=n?(sel/(cols*rows))*rows:0; }
          if(g_pendingInstall){ running=false; break; }   // Install row -> chainload Vita3K --install
          if(g_rescanAfterSettings){ startGameScan(); sel=0; top=0; g_rescanAfterSettings=false; }
          break;
        }
        case BTN_CANCEL:
          if(confirmBox("Exit Vita3K?",{std::string(LauncherLocalization::Translate("Return to the HOME Menu?")),
             std::string(LauncherLocalization::Translate("Active background operations will be cancelled safely."))}))running=false;
          beginScreenFx();break;
      }
      top = n ? (sel/(cols*rows))*rows : 0; // page-aligned: a page turn swaps both rows
    }
    pollUpdateNotification();
    renderGrid(sel,top,APP_DIR);
    waitForNextFrame();
  }

  // persist the launcher's global store (the emulator never touches launcher.ini).
  g_active=&g_global;
  if(launch) commitAll();
  storeSave(g_global, LAUNCHER_INI);

  const bool installLauncherUpdate=LauncherUpdate_ConsumeInstallationRequest();
  const LauncherUpdateSnapshot updateToInstall=LauncherUpdate_GetSnapshot();
  if(installLauncherUpdate){launch=false;g_pendingInstall=false;}

  // Prepare the chainload while SDL is still up. The bundled core is extracted
  // only on demand, keeping first boot and ordinary launcher-only use instant.
  bool willChain = false, willInstall = false;
  if((launch || g_pendingInstall) && envHasNextLoad()){
    const bool haveEmu=ensureEmu();
    bool configOk=true;
    if(launch){
      appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
      configOk=writeConfigYml(buildEffectiveSettings(launchKey));   // global + per-game -> config.yml
      appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    }
    if(haveEmu&&configOk){ willInstall = g_pendingInstall; willChain = launch && !g_pendingInstall; }
    else if(!haveEmu){ modalMessageStatic("Emulator setup failed",{
      "Could not prepare the bundled Vita3K core.",
      "The SD card may be full or write-protected.",
      "","Free up space and try again."}); }
    else { modalMessageStatic("Launch configuration failed",{
      "Vita3K configuration could not be updated safely.",
      "The previous settings were preserved.",
      "Check free SD space and file permissions."
    }); }
  }

  cleanupLauncher();

  if(installLauncherUpdate){
    const bool installed=LauncherUpdate_InstallDownloaded(g_launcherNroPath);
    if(installed){
      if(!updateToInstall.release.tag.empty()){
        storeSet(g_global,"Wrapper/InstalledReleaseTag",updateToInstall.release.tag.c_str());
        storeSave(g_global,LAUNCHER_INI);
      }
        return 0;
    }
    LauncherUpdateSnapshot failure=LauncherUpdate_GetSnapshot();
    return 0;
  }

  // envSetNextLoad(path, argv): the emulator parses "-r <TITLEID>" / "--install" from argv.
  // Pass "--return <this launcher>" so the emulator boots us back after installing.
  if(willInstall){ std::string a=std::string(EMU_NRO)+" --install"; if(!g_forwarderSelfPath.empty()) a+=" --return "+g_forwarderSelfPath; envSetNextLoad(EMU_NRO, a.c_str()); }
  else if(willChain){ std::string a=std::string(EMU_NRO)+" -r "+launchTid; if(!g_forwarderSelfPath.empty()) a+=" --return "+g_forwarderSelfPath; envSetNextLoad(EMU_NRO, a.c_str()); }
  return 0;
}
