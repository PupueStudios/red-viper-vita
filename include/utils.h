#ifndef _UTILS_H
#define _UTILS_H

#include "vb_types.h"


void FlushInvalidateCache(void *addr, size_t len);
int32_t ReprotectMemory(void* addr, uint32_t pages, uint32_t mode);

void *vita_code_alloc(size_t size);
void vita_code_free(void *ptr);
void vita_code_unlock(void);
void vita_code_lock(void);
#define DRC_CODE_UNLOCK() vita_code_unlock()
#define DRC_CODE_LOCK() vita_code_lock()

#endif // _UTILS_H
