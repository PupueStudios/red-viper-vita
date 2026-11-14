#include <psp2/kernel/sysmem.h>
#include <stddef.h>
#include <stdint.h>

#include "utils.h"
#include "vita.h"

static SceUID code_block = -1;
static uint8_t *code_base = NULL;
static size_t code_size = 0;
static int unlock_depth = 0;

void *vita_code_alloc(size_t size) {
    size_t aligned = (size + 0xFFFFF) & ~(size_t)0xFFFFF;
    code_block = sceKernelAllocMemBlockForVM("RedViperDRC", aligned);
    if (code_block < 0) return NULL;
    void *base = NULL;
    if (sceKernelGetMemBlockBase(code_block, &base) < 0) {
        sceKernelFreeMemBlock(code_block);
        code_block = -1;
        return NULL;
    }
    code_base = base;
    code_size = aligned;
    return base;
}

void vita_code_free(void *ptr) {
    (void)ptr;
    if (code_block >= 0) {
        while (unlock_depth > 0) vita_code_lock();
        sceKernelFreeMemBlock(code_block);
    }
    code_block = -1;
    code_base = NULL;
    code_size = 0;
}

void vita_code_unlock(void) {
    if (unlock_depth++ == 0) sceKernelOpenVMDomain();
}

void vita_code_lock(void) {
    if (unlock_depth <= 0) return;
    if (--unlock_depth == 0) sceKernelCloseVMDomain();
}

void FlushInvalidateCache(void *addr, size_t len) {
    if (code_block < 0 || len == 0) return;
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)31;
    uintptr_t end = ((uintptr_t)addr + len + 31) & ~(uintptr_t)31;
    sceKernelSyncVMDomain(code_block, (void *)start, end - start);
}

int32_t ReprotectMemory(void *addr, uint32_t pages, uint32_t mode) {
    (void)addr;
    (void)pages;
    (void)mode;
    return 0;
}

bool jit_probe(void) {
    SceUID probe = sceKernelAllocMemBlockForVM("RedViperProbe", 0x100000);
    if (probe < 0) return false;
    sceKernelFreeMemBlock(probe);
    return true;
}
