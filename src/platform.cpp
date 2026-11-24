// Files, paths, the OS, time and randomness, for Windows, macOS, Linux and Android.
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#else
#  include <dirent.h>
#  include <pwd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/utsname.h>
#  include <unistd.h>
#  if defined(__ANDROID__)
#    include <android/log.h>
#  endif
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <sys/sysctl.h>
#  endif
#endif

namespace zharp {

bool g_debug = false;

static void vlog(const char *format, va_list args) {
    char buffer[2048];
    vsnprintf(buffer, sizeof buffer, format, args);
    fprintf(stderr, "[zharp] %s\n", buffer);
}

void debug_log(const char *format, ...) {
    if (!g_debug) return;
    va_list args;
    va_start(args, format);
    vlog(format, args);
    va_end(args);
}

void warn(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vlog(format, args);
    va_end(args);
}

#if defined(_WIN32)
static std::wstring widen(const std::string &text) {
    if (text.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &out[0], n);
    return out;
}

static std::string narrow(const std::wstring &text) {
    if (text.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &out[0], n, nullptr, nullptr);
    return out;
}
#endif

std::string env(const char *name) {
#if defined(_WIN32)
    wchar_t buffer[4096];
    DWORD n = GetEnvironmentVariableW(widen(name).c_str(), buffer, 4096);
    if (n == 0 || n >= 4096) return std::string();
    return narrow(std::wstring(buffer, n));
#else
    const char *value = getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

std::string executable_path() {
#if defined(_WIN32)
    wchar_t buffer[32768];
    DWORD n = GetModuleFileNameW(nullptr, buffer, 32768);
    return n ? narrow(std::wstring(buffer, n)) : std::string();
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) != 0) return std::string();
    char resolved[PATH_MAX];
    return realpath(buffer, resolved) ? std::string(resolved) : std::string(buffer);
#else
    char buffer[4096];
    ssize_t n = readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    return n > 0 ? std::string(buffer, (size_t)n) : std::string();
#endif
}

static bool is_separator(char c) {
#if defined(_WIN32)
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

std::string dirname_of(const std::string &path) {
    size_t i = path.size();
    while (i > 0 && !is_separator(path[i - 1])) i--;
    if (i == 0) return ".";
    return path.substr(0, i > 1 ? i - 1 : 1);
}

std::string basename_of(const std::string &path) {
    size_t i = path.size();
    while (i > 0 && !is_separator(path[i - 1]) && path[i - 1] != '/' && path[i - 1] != '\\') i--;
    return path.substr(i);
}

std::string join_path(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    return is_separator(a.back()) ? a + b : a + sep + b;
}

std::string cache_dir() {
#if defined(_WIN32)
    std::string base = env("LOCALAPPDATA");
    if (base.empty()) base = env("TEMP");
    return base;
#elif defined(__APPLE__)
    std::string home = env("HOME");
    if (home.empty()) {
        if (struct passwd *pw = getpwuid(getuid())) home = pw->pw_dir;
    }
    return home.empty() ? "/tmp" : join_path(home, "Library/Caches");
#else
    std::string xdg = env("XDG_CACHE_HOME");
    if (!xdg.empty()) return xdg;
    std::string home = env("HOME");
    if (home.empty()) {
        if (struct passwd *pw = getpwuid(getuid())) home = pw->pw_dir;
    }
    return home.empty() ? "/tmp" : join_path(home, ".cache");
#endif
}

bool make_dirs(const std::string &path) {
    if (path.empty()) return false;
    std::string partial;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || (is_separator(path[i]) && i > 0)) {
            partial = path.substr(0, i);
#if defined(_WIN32)
            if (partial.size() == 2 && partial[1] == ':') continue;
            CreateDirectoryW(widen(partial).c_str(), nullptr);
#else
            mkdir(partial.c_str(), 0700);
#endif
        }
    }
    return file_exists(path);
}

bool file_exists(const std::string &path) {
#if defined(_WIN32)
    return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

bool read_file(const std::string &path, std::string &out) {
#if defined(_WIN32)
    FILE *file = _wfopen(widen(path).c_str(), L"rb");
#else
    FILE *file = fopen(path.c_str(), "rb");
#endif
    if (!file) return false;
    out.clear();
    char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, file)) > 0) out.append(buffer, n);
    fclose(file);
    return true;
}

bool write_file(const std::string &path, const std::string &data) {
    // Written under a temporary name, then renamed: a reader never sees half a file.
    std::string temporary = path + ".tmp";
#if defined(_WIN32)
    FILE *file = _wfopen(widen(temporary).c_str(), L"wb");
#else
    FILE *file = fopen(temporary.c_str(), "wb");
#endif
    if (!file) return false;
    bool ok = fwrite(data.data(), 1, data.size(), file) == data.size();
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        remove_file(temporary);
        return false;
    }
#if defined(_WIN32)
    return MoveFileExW(widen(temporary).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(temporary.c_str(), path.c_str()) == 0;
#endif
}

bool remove_file(const std::string &path) {
#if defined(_WIN32)
    return DeleteFileW(widen(path).c_str()) != 0;
#else
    return unlink(path.c_str()) == 0;
#endif
}

std::vector<std::string> list_dir(const std::string &path) {
    std::vector<std::string> names;
#if defined(_WIN32)
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(widen(join_path(path, "*")).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return names;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(narrow(data.cFileName));
    } while (FindNextFileW(find, &data));
    FindClose(find);
#else
    DIR *dir = opendir(path.c_str());
    if (!dir) return names;
    while (struct dirent *entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        names.push_back(entry->d_name);
    }
    closedir(dir);
#endif
    std::sort(names.begin(), names.end());
    return names;
}

std::string host_name() {
#if defined(_WIN32)
    wchar_t buffer[256];
    DWORD size = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, buffer, &size)) return narrow(std::wstring(buffer, size));
    return std::string();
#else
    char buffer[256] = {0};
    if (gethostname(buffer, sizeof buffer - 1) != 0) return std::string();
    return buffer;
#endif
}

#if defined(__APPLE__)
static std::string sysctl_string(const char *name) {
    char buffer[256] = {0};
    size_t size = sizeof buffer - 1;
    if (sysctlbyname(name, buffer, &size, nullptr, 0) != 0) return std::string();
    return std::string(buffer);
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
/** PRETTY_NAME-less: the VERSION_ID of /etc/os-release, e.g. "24.04". */
static std::string os_release(const char *field) {
    std::string text;
    if (!read_file("/etc/os-release", text)) return std::string();
    std::istringstream lines(text);
    std::string line;
    std::string prefix = std::string(field) + "=";
    while (std::getline(lines, line)) {
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        std::string value = line.substr(prefix.size());
        if (value.size() >= 2 && (value[0] == '"' || value[0] == '\'')) value = value.substr(1, value.size() - 2);
        return value;
    }
    return std::string();
}
#endif

OsInfo os_info() {
    OsInfo info;
#if defined(_WIN32)
    info.name = "Windows";
    info.type = "windows";
    // RtlGetVersion tells the truth; GetVersionEx lies to unmanifested apps.
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW *);
    OSVERSIONINFOEXW version = {};
    version.dwOSVersionInfoSize = sizeof version;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn get = ntdll ? (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
    if (get && get(&version) == 0) {
        info.version = std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion) + "." + std::to_string(version.dwBuildNumber);
    }
    SYSTEM_INFO system;
    GetNativeSystemInfo(&system);
    info.arch = system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64"
              : system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "amd64" : "x86";
#else
    struct utsname uts;
    if (uname(&uts) == 0) {
        std::string machine = uts.machine;
        info.arch = machine == "x86_64" ? "amd64" : (machine == "aarch64" || machine == "arm64") ? "arm64" : machine;
#  if defined(__APPLE__)
        info.name = "macOS";
        info.type = "darwin";
        info.version = sysctl_string("kern.osproductversion");
        info.model = sysctl_string("hw.model");
#  else
        info.name = "Linux";
        info.type = "linux";
        info.version = uts.release;
        std::string distro = os_release("NAME");
        std::string version = os_release("VERSION_ID");
        if (!distro.empty()) info.model = distro + (version.empty() ? "" : " " + version);
#  endif
    }
#endif
    return info;
}

int process_id() {
#if defined(_WIN32)
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

uint64_t now_nanos() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

std::string iso_time(uint64_t nanos) {
    time_t seconds = (time_t)(nanos / 1000000000ull);
    unsigned millis = (unsigned)((nanos / 1000000ull) % 1000);
    struct tm utc;
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char buffer[64];
    snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
    return buffer;
}

static std::mutex g_random_mutex;

std::string random_hex(size_t bytes) {
    static std::mt19937_64 engine = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(), (unsigned)now_nanos(), (unsigned)process_id()};
        return std::mt19937_64(seed);
    }();
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    std::lock_guard<std::mutex> lock(g_random_mutex);
    for (size_t i = 0; i < bytes; i++) {
        unsigned byte = (unsigned)(engine() & 0xff);
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

std::string random_uuid() {
    std::string hex = random_hex(16);
    hex[12] = '4';
    hex[16] = "89ab"[hex[16] % 4];
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20);
}

// ── JSON ────────────────────────────────────────────────────────────────────

std::string json_string(const std::string &text) {
    std::string out;
    out.reserve(text.size() + 2);
    out += '"';
    const unsigned char *s = (const unsigned char *)text.data();
    size_t n = text.size();
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char escape[8];
                        snprintf(escape, sizeof escape, "\\u%04x", c);
                        out += escape;
                    } else {
                        out += (char)c;
                    }
            }
            i++;
            continue;
        }
        // Copy a valid UTF-8 sequence as it is; anything else becomes U+FFFD.
        size_t length = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        bool valid = length && i + length <= n;
        for (size_t k = 1; valid && k < length; k++) valid = (s[i + k] & 0xc0) == 0x80;
        if (valid) {
            out.append((const char *)s + i, length);
            i += length;
        } else {
            out += "\xef\xbf\xbd";
            i++;
        }
    }
    out += '"';
    return out;
}

std::string otlp_attributes(const std::vector<std::pair<std::string, std::string>> &attributes) {
    std::string out = "[";
    bool first = true;
    for (const auto &kv : attributes) {
        if (kv.second.empty()) continue;
        if (!first) out += ',';
        first = false;
        out += "{\"key\":" + json_string(kv.first) + ",\"value\":{\"stringValue\":" + json_string(kv.second) + "}}";
    }
    out += ']';
    return out;
}

}  // namespace zharp
