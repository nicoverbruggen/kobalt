#include "address_hook.h"
#include "support/diagnostics.h"
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" void kobalt_usb_workflow(void *manager);
extern "C" void kobalt_continue_using(void *manager);

namespace kobalt::nickel {
int redirectAddressReferences(void *library, const char *symbol, void *target)
{
    link_map *map = nullptr;
    void *original = library ? dlsym(library, symbol) : nullptr;
    if (!original || !target || dlinfo(library, RTLD_DI_LINKMAP, &map) != 0 || !map)
        return 0;
    const ElfW(Rel) *relocations = nullptr;
    size_t size = 0, entry = 0;
    const ElfW(Sym) *symbols = nullptr;
    const char *names = nullptr;
    for (const auto *dynamic = map->l_ld; dynamic->d_tag != DT_NULL; ++dynamic) {
        switch (dynamic->d_tag) {
        case DT_REL:
            relocations = reinterpret_cast<const ElfW(Rel) *>(dynamic->d_un.d_ptr);
            break;
        case DT_RELSZ:
            size = dynamic->d_un.d_val;
            break;
        case DT_RELENT:
            entry = dynamic->d_un.d_val;
            break;
        case DT_SYMTAB:
            symbols = reinterpret_cast<const ElfW(Sym) *>(dynamic->d_un.d_ptr);
            break;
        case DT_STRTAB:
            names = reinterpret_cast<const char *>(dynamic->d_un.d_ptr);
            break;
        }
    }
    // Nickel is 32-bit ARM, which uses REL relocations only.
    if (!relocations || !symbols || !names || entry != sizeof(ElfW(Rel)))
        return 0;
    const long page = sysconf(_SC_PAGESIZE);
    int redirected = 0;
    for (size_t i = 0; i < size / entry; ++i) {
        const auto &relocation = relocations[i];
        if (ELF32_R_TYPE(relocation.r_info) != R_ARM_GLOB_DAT
            || std::strcmp(names + symbols[ELF32_R_SYM(relocation.r_info)].st_name, symbol) != 0)
            continue;
        auto **slot = reinterpret_cast<void **>(map->l_addr + relocation.r_offset);
        if (*slot != original)
            continue;
        // The GOT can be read-only after relocation. NickelHook leaves its
        // pages writable for the same reason: lazy binding writes there later.
        auto *start = reinterpret_cast<void *>(reinterpret_cast<size_t>(slot) & ~size_t(page - 1));
        if (page <= 0 || mprotect(start, size_t(page), PROT_READ | PROT_WRITE) != 0)
            continue;
        *slot = target;
        ++redirected;
    }
    return redirected;
}

void redirectUsbDialog()
{
    void *nickel = dlopen("/usr/local/Kobo/libnickel.so.1.0.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!nickel) {
        diagnostic("usb-dialog-hooks", { { "available", false } }, DiagnosticSeverity::Warning);
        return;
    }
    const int connect =
            redirectAddressReferences(nickel, "_ZN19PlugWorkflowManager18onCancelAndConnectEv",
                                      reinterpret_cast<void *>(kobalt_usb_workflow));
    const int cancel =
            redirectAddressReferences(nickel, "_ZN19PlugWorkflowManager15onContinueUsingEv",
                                      reinterpret_cast<void *>(kobalt_continue_using));
    dlclose(nickel);
    diagnostic("usb-dialog-hooks",
               { { "available", true }, { "connect", connect }, { "cancel", cancel } },
               connect && cancel ? DiagnosticSeverity::Activity : DiagnosticSeverity::Warning);
}
} // namespace kobalt::nickel
