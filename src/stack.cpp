// The stack of a handled error, captured in the process: return addresses, the module each is in
// with the debug ID its .sym file is found by, and a name where the module exports one. Zharp's
// backend names the frames from the .sym files `zharp debug-files upload` sent, as it does for
// crashes.
#include "internal.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
#else
#  include <cxxabi.h>
#  include <dlfcn.h>
#  include <unwind.h>
#  include <cstdlib>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <mach-o/loader.h>
#  else
#    include <elf.h>
#    include <link.h>
#  endif
#endif

namespace zharp {

std::string debug_id_from_bytes(const unsigned char *bytes, size_t length, bool swap_guid_fields) {
    unsigned char guid[16] = {0};
    memcpy(guid, bytes, length < 16 ? length : 16);
    char out[40];
    if (swap_guid_fields) {
        // The first three GUID fields read little-endian, as Breakpad's FileID does.
        snprintf(out, sizeof out, "%02X%02X%02X%02X%02X%02X%02X%02X", guid[3], guid[2], guid[1], guid[0], guid[5], guid[4], guid[7], guid[6]);
    } else {
        snprintf(out, sizeof out, "%02X%02X%02X%02X%02X%02X%02X%02X", guid[0], guid[1], guid[2], guid[3], guid[4], guid[5], guid[6], guid[7]);
    }
    std::string id = out;
    for (int i = 8; i < 16; i++) {
        snprintf(out, sizeof out, "%02X", guid[i]);
        id += out;
    }
    return id + "0";
}

// ── Loaded modules ─────────────────────────────────────────────────────────

#if defined(_WIN32)

struct CvInfoPdb70 {
    DWORD signature;  // "RSDS"
    GUID guid;
    DWORD age;
    char pdb_file_name[1];
};

static std::vector<Module> loaded_modules() {
    std::vector<Module> modules;
    HANDLE process = GetCurrentProcess();
    HMODULE handles[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(process, handles, sizeof handles, &needed)) return modules;
    size_t count = needed / sizeof(HMODULE);
    if (count > 1024) count = 1024;
    for (size_t i = 0; i < count; i++) {
        MODULEINFO info;
        if (!GetModuleInformation(process, handles[i], &info, sizeof info)) continue;
        Module module;
        module.base = (uint64_t)(uintptr_t)info.lpBaseOfDll;
        module.size = info.SizeOfImage;
        wchar_t path[MAX_PATH * 4];
        DWORD n = GetModuleFileNameW(handles[i], path, MAX_PATH * 4);
        if (n) {
            int bytes = WideCharToMultiByte(CP_UTF8, 0, path, (int)n, nullptr, 0, nullptr, nullptr);
            std::string narrow(bytes, '\0');
            WideCharToMultiByte(CP_UTF8, 0, path, (int)n, &narrow[0], bytes, nullptr, nullptr);
            module.name = basename_of(narrow);
        }
        // The PE's CodeView record: the PDB's GUID and age, which is its debug ID.
        const unsigned char *image = (const unsigned char *)info.lpBaseOfDll;
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)image;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(image + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
                const IMAGE_DEBUG_DIRECTORY *entries = (const IMAGE_DEBUG_DIRECTORY *)(image + dir.VirtualAddress);
                size_t entry_count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
                for (size_t e = 0; dir.VirtualAddress && e < entry_count; e++) {
                    if (entries[e].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !entries[e].AddressOfRawData) continue;
                    const CvInfoPdb70 *cv = (const CvInfoPdb70 *)(image + entries[e].AddressOfRawData);
                    if (cv->signature != 0x53445352) continue;  // RSDS
                    char id[64];
                    snprintf(id, sizeof id, "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX", (unsigned long)cv->guid.Data1, cv->guid.Data2, cv->guid.Data3,
                             cv->guid.Data4[0], cv->guid.Data4[1], cv->guid.Data4[2], cv->guid.Data4[3], cv->guid.Data4[4], cv->guid.Data4[5],
                             cv->guid.Data4[6], cv->guid.Data4[7], (unsigned long)cv->age);
                    module.debug_id = id;
                    module.debug_file = basename_of(std::string(cv->pdb_file_name, strnlen(cv->pdb_file_name, 1024)));
                    break;
                }
            }
        }
        if (module.debug_file.empty()) module.debug_file = module.name;
        modules.push_back(module);
    }
    return modules;
}

#elif defined(__APPLE__)

static std::vector<Module> loaded_modules() {
    std::vector<Module> modules;
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        const struct mach_header *header = _dyld_get_image_header(i);
        const char *path = _dyld_get_image_name(i);
        if (!header) continue;
        Module module;
        module.name = basename_of(path ? path : "");
        module.debug_file = module.name;
        module.base = (uint64_t)(uintptr_t)header;
        bool is64 = header->magic == MH_MAGIC_64;
        const unsigned char *command = (const unsigned char *)header + (is64 ? sizeof(struct mach_header_64) : sizeof(struct mach_header));
        for (uint32_t c = 0; c < header->ncmds; c++) {
            const struct load_command *lc = (const struct load_command *)command;
            if (lc->cmd == LC_UUID) {
                const struct uuid_command *uuid = (const struct uuid_command *)lc;
                // dump_syms writes a Mach-O UUID as it is, age 0.
                module.debug_id = debug_id_from_bytes(uuid->uuid, 16, false);
            } else if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *segment = (const struct segment_command_64 *)lc;
                if (strcmp(segment->segname, "__TEXT") == 0) module.size = segment->vmsize;
            } else if (lc->cmd == LC_SEGMENT) {
                const struct segment_command *segment = (const struct segment_command *)lc;
                if (strcmp(segment->segname, "__TEXT") == 0) module.size = segment->vmsize;
            }
            command += lc->cmdsize;
        }
        modules.push_back(module);
    }
    return modules;
}

#else

static int add_module(struct dl_phdr_info *info, size_t, void *data) {
    auto *modules = static_cast<std::vector<Module> *>(data);
    uint64_t lowest = UINT64_MAX, highest = 0;
    std::string debug_id, code_id;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type == PT_LOAD) {
            if (phdr.p_vaddr < lowest) lowest = phdr.p_vaddr;
            if (phdr.p_vaddr + phdr.p_memsz > highest) highest = phdr.p_vaddr + phdr.p_memsz;
        } else if (phdr.p_type == PT_NOTE && debug_id.empty()) {
            // The GNU build ID note: what Breakpad's debug ID for an ELF file is made from.
            const unsigned char *note = (const unsigned char *)(info->dlpi_addr + phdr.p_vaddr);
            const unsigned char *end = note + phdr.p_memsz;
            while (note + sizeof(ElfW(Nhdr)) <= end) {
                const ElfW(Nhdr) *header = (const ElfW(Nhdr) *)note;
                size_t name_size = (header->n_namesz + 3) & ~(size_t)3;
                size_t desc_size = (header->n_descsz + 3) & ~(size_t)3;
                const unsigned char *name = note + sizeof(ElfW(Nhdr));
                const unsigned char *desc = name + name_size;
                if (desc + header->n_descsz > end) break;
                if (header->n_type == NT_GNU_BUILD_ID && header->n_namesz == 4 && memcmp(name, "GNU", 4) == 0) {
                    debug_id = debug_id_from_bytes(desc, header->n_descsz, true);
                    static const char hex[] = "0123456789abcdef";
                    for (size_t b = 0; b < header->n_descsz; b++) {
                        code_id += hex[desc[b] >> 4];
                        code_id += hex[desc[b] & 15];
                    }
                    break;
                }
                note = desc + desc_size;
            }
        }
    }
    if (lowest == UINT64_MAX) return 0;
    Module module;
    std::string path = info->dlpi_name ? info->dlpi_name : "";
    // The executable itself has no name here.
    module.name = basename_of(path.empty() ? executable_path() : path);
    module.debug_file = module.name;
    module.debug_id = debug_id;
    module.code_id = code_id;
    module.base = info->dlpi_addr + lowest;
    module.size = highest - lowest;
    modules->push_back(module);
    return 0;
}

static std::vector<Module> loaded_modules() {
    std::vector<Module> modules;
    dl_iterate_phdr(add_module, &modules);
    return modules;
}

#endif

// ── Unwinding ──────────────────────────────────────────────────────────────

#if !defined(_WIN32)
struct UnwindState {
    uint64_t *addresses;
    int count;
    int max;
};

static _Unwind_Reason_Code unwind_one(struct _Unwind_Context *context, void *data) {
    auto *state = static_cast<UnwindState *>(data);
    uint64_t ip = (uint64_t)_Unwind_GetIP(context);
    if (!ip) return _URC_END_OF_STACK;
    if (state->count >= state->max) return _URC_END_OF_STACK;
    state->addresses[state->count++] = ip;
    return _URC_NO_REASON;
}

static std::string symbol_name(uint64_t address) {
    Dl_info info;
    // The call instruction, not the one after it.
    if (!dladdr((void *)(uintptr_t)(address - 1), &info) || !info.dli_sname || !info.dli_saddr) return std::string();
    int status = 0;
    char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
    std::string name = status == 0 && demangled ? demangled : info.dli_sname;
    free(demangled);
    return name;
}
#endif

Stack capture_stack(void *caller, int max) {
    const int limit = 128;
    uint64_t addresses[limit];
    int count = 0;
#if defined(_WIN32)
    void *raw[limit];
    count = (int)RtlCaptureStackBackTrace(0, limit, raw, nullptr);
    for (int i = 0; i < count; i++) addresses[i] = (uint64_t)(uintptr_t)raw[i];
#else
    UnwindState state = {addresses, 0, limit};
    _Unwind_Backtrace(unwind_one, &state);
    count = state.count;
#endif
    // Frames above the public function's caller are the SDK's own.
    int start = 0;
    uint64_t from = (uint64_t)(uintptr_t)caller;
    for (int i = 0; i < count; i++) {
        if (addresses[i] == from) {
            start = i;
            break;
        }
    }

    Stack stack;
    std::vector<Module> modules = loaded_modules();
    std::vector<int> used(modules.size(), -1);
    for (int i = start; i < count && (int)stack.frames.size() < max; i++) {
        Frame frame;
        frame.address = addresses[i];
        for (size_t m = 0; m < modules.size(); m++) {
            if (frame.address >= modules[m].base && frame.address < modules[m].base + modules[m].size) {
                if (used[m] < 0) {
                    used[m] = (int)stack.modules.size();
                    stack.modules.push_back(modules[m]);
                }
                frame.module = used[m];
                break;
            }
        }
#if !defined(_WIN32)
        frame.function = symbol_name(frame.address);
#endif
        stack.frames.push_back(frame);
    }
    return stack;
}

/** Brackets would end the frame's "[module +0x…]" early: "foo() [clone .cold]" → "foo() (clone .cold)". */
static std::string clean_name(std::string name) {
    for (char &c : name) {
        if (c == '[') c = '(';
        else if (c == ']') c = ')';
        else if (c == '\n' || c == '\r') c = ' ';
    }
    return name;
}

std::string format_stack(const Stack &stack) {
    std::string out;
    char line[64];
    for (size_t i = 0; i < stack.frames.size(); i++) {
        const Frame &frame = stack.frames[i];
        snprintf(line, sizeof line, "#%zu", i);
        std::string number = line;
        while (number.size() < 3) number += ' ';
        if (!out.empty()) out += '\n';
        if (frame.module < 0) {
            snprintf(line, sizeof line, " ??? +0x%llx", (unsigned long long)frame.address);
            out += number + line;
            continue;
        }
        const Module &module = stack.modules[(size_t)frame.module];
        snprintf(line, sizeof line, " +0x%llx", (unsigned long long)(frame.address - module.base));
        if (frame.function.empty()) {
            out += number + " " + module.name + line;
        } else {
            out += number + " " + clean_name(frame.function) + " [" + module.name + line + "]";
        }
    }
    return out;
}

std::string format_modules(const Stack &stack) {
    std::string out;
    for (const Module &module : stack.modules) {
        if (module.debug_id.empty()) continue;
        if (!out.empty()) out += '\n';
        out += module.name + "\t" + module.debug_id + "\t" + module.debug_file;
    }
    return out;
}

}  // namespace zharp
