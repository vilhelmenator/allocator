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
#include <windows.h>
#include <intrin.h>
#include <memoryapi.h>

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

static inline bool commit_memory(void *base, size_t size)
{
#if defined(WINDOWS)
    return VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE) == base;
#else
    return (mprotect(base, size, (PROT_READ | PROT_WRITE)) == 0);
#endif
}

static inline bool decommit_memory(void *base, size_t size)
{
#if defined(WINDOWS)
    return VirtualFree(base, size, MEM_DECOMMIT);
#else
    return (mmap(base, size, PROT_NONE, (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE), -1, 0) == base);
#endif
}

static inline bool free_memory(void *ptr, size_t size)
{
#if defined(WINDOWS)
    MEMORY_BASIC_INFORMATION info = { 0 };
    VirtualQuery(ptr, &info, sizeof(info));
    if(info.AllocationBase < (uintptr_t)ptr)
    {
        return VirtualFree(info.AllocationBase, 0, MEM_RELEASE);
    }
    else
    {
        return VirtualFree(ptr, 0, MEM_RELEASE);
    }
#else
    return (munmap(ptr, size) == 0);
#endif
}

static inline bool release_memory(void *ptr, size_t size, bool commit)
{
    if (commit) {
        return decommit_memory(ptr, size);
    } else {
        return free_memory(ptr, size);
    }
}

static inline void *alloc_memory(void *base, size_t size, bool commit)
{
#if defined(WINDOWS)
    int flags = commit ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE;
    return VirtualAlloc(base, size, flags, PAGE_READWRITE);
#else
    int flags = commit ? (PROT_WRITE | PROT_READ) : PROT_NONE;
    return mmap(base, size, flags, (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);
#endif
}

static inline bool reset_memory(void *base, size_t size)
{
#if defined(WINDOWS)
    void *p = VirtualAlloc(base, size, MEM_RESET, PAGE_READWRITE);
    if (p == base && base != NULL) {
        VirtualUnlock(base, size);
    }
    return (p == base);
#else
    int err;
    while ((err = madvise(base, size, MADV_FREE)) != 0 && errno == EAGAIN) {
        errno = 0;
    };
    if (err != 0 && errno == EINVAL) {
        err = madvise(base, size, MADV_DONTNEED);
    }
    return (err != 0);
#endif
}

static inline bool protect_memory(void *addr, size_t size, bool protect)
{
#if defined(WINDOWS)
    DWORD prev_value = 0;
    return VirtualProtect(addr, size, protect ? PAGE_NOACCESS : PAGE_READWRITE, &prev_value) == 0;
#else
    return mprotect(addr, size, protect ? PROT_NONE : (PROT_READ | PROT_WRITE)) == 0;
#endif
}

static inline bool remap_memory(void *old_addr, void *new_addr, size_t size)
{
#if defined(WINDOWS)
    /*
    int32_t numberOfPages = size/os_page_size;
    int32_t numberOfPagesInitial = numberOfPages;
    if(AllocateUserPhysicalPages( GetCurrentProcess(),
                                         &numberOfPages,
                                         aPFNs ))
    {
        if(numberOfPagesInitial == numberOfPages)
        {
            if(MapUserPhysicalPages(new_addr, )
        }

    }*/

    return false;
#elif defined(__linux__)
    if (mremap(old_addr, size, MREMAP_FIXED | MREMAP_MAYMOVE, new_addr) != MAP_FAILED) {
        // I think linux unmaps the old address.
        return true;
    }
#elif defined(__APPLE__)

    vm_prot_t curProtection, maxProtection;
    if (vm_remap(mach_task_self(), (vm_address_t *)new_addr, size,
                 0, // mask
                 0, // anywhere
                 mach_task_self(), (vm_address_t)old_addr,
                 0, // copy
                 &curProtection, &maxProtection, VM_INHERIT_COPY) != KERN_NO_SPACE) {
        // do we need to unmap the old address
        return munmap(old_addr, size) == 0;
    }
#else
    //
#endif
    return false;
}

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
#if defined(__APPLE__)
#if defined(__x86_64__)
    const size_t ofs = 0;
    __asm__("movq %%gs:%1, %0" : "=r"(res) : "m"(*((void **)ofs)) :);
#elif defined(__aarch64__)
    void **tcb;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tcb));
    tcb = (void **)((uintptr_t)tcb & ~0x07UL);
    res = *tcb;
#endif
#elif defined(__x86_64__)
    const size_t ofs = 0;
    __asm__("movq %%fs:%1, %0" : "=r"(res) : "m"(*((void **)ofs)) :);
#endif
    return (uintptr_t)res;
#else
    return (uintptr_t)&thread_instance;
#endif
}

#endif
