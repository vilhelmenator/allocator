//
//  os.inl
//  MemPoolTests
//
//  Created by Vilhelm Sævarsson on 23.6.2022.
//  Copyright © 2022 Vilhelm Sævarsson. All rights reserved.
//

#ifndef OS_H
#define OS_H
#include "callocator.h"
#if defined(_MSC_VER)
#define WINDOWS
#endif
#if defined(WINDOWS)
#include <intrin.h>
#include <memoryapi.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <errno.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

bool commit_memory(void *base, size_t size);

bool decommit_memory(void *base, size_t size);

bool free_memory(void *ptr, size_t size);
bool release_memory(void *ptr, size_t size, bool commit);

void *alloc_memory(void *base, size_t size, bool commit);

bool reset_memory(void *base, size_t size);

bool protect_memory(void *addr, size_t size, bool protect);

bool remap_memory(void *old_addr, void *new_addr, size_t size);

static inline size_t get_os_page_size(void)
{
#ifdef WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return sysconf(_SC_PAGESIZE);
#endif
}

static inline uintptr_t get_thread_id(void)
{
#if defined(WINDOWS)
    return (uintptr_t)NtCurrentTeb();
#elif defined(__GNUC__)
    void *res;
    const size_t ofs = 0;
#if defined(__APPLE__)
#if defined(__x86_64__)
    __asm__("movq %%gs:%1, %0" : "=r"(res) : "m"(*((void **)ofs)) :);
#elif defined(__aarch64__)
    void **tcb;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tcb));
    tcb = (void **)((uintptr_t)tcb & ~0x07UL);
    res = *tcb;
#endif
#elif defined(__x86_64__)
    __asm__("movq %%fs:%1, %0" : "=r"(res) : "m"(*((void **)ofs)) :);
#endif
    return (uintptr_t)res;
#else
    return (uintptr_t)&thread_instance;
#endif
}

#endif
