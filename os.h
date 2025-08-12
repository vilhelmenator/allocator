

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
#include <sys/resource.h>
#else
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/resource.h>
#endif

#if defined(__cplusplus)
#include <atomic>
#define memory_order_relaxed std::memory_order_relaxed
#define memory_order_consume std::memory_order_consume
#define memory_order_acquire std::memory_order_acquire
#define memory_order_release std::memory_order_release
#define memory_order_acq_rel std::memory_order_acq_rel
#define memory_order_seq_cst std::memory_order_seq_cst
#define _Atomic(tp) std::atomic<tp>
#elif defined(_MSC_VER)
#define _Atomic(tp) tp
#define ATOMIC_VAR_INIT(x) x
#else
#include <stdatomic.h>
#endif

typedef void* (*thrd_start_t)(void *);
typedef void (*tls_dtor_t)(void *);
enum { thrd_success, thrd_nomem, thrd_timedout, thrd_busy, thrd_error };

#if defined(PLATFORM_WINDOWS)
#include <fibersapi.h>
#include <process.h>
#include <windows.h>

#if !defined(__cplusplus)
#define thread_local __declspec(thread);
#endif

typedef HANDLE thrd_t;
typedef DWORD tls_t;
typedef CRITICAL_SECTION mutex_t;

#define thrd_current() (thrd_t) GetCurrentThreadId()
#define thrd_equal(a, b) a == b
#define thrd_yield() (void)SwitchToThread()
#define thrd_exit(res) _endthreadex((unsigned)(size_t)res)

static inline int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    uintptr_t handle;
    if (!thr)
        return thrd_error;
    handle = _beginthread(func 0, arg);
    if (handle == 0) {
        if (errno == EAGAIN || errno == EACCES)
            return thrd_nomem;
        return thrd_error;
    }
    *thr = (thrd_t)handle;
    return thrd_success;
}

static inline int thrd_sleep(const struct timespec *ts_in)
{
    Sleep((DWORD)((ts_in->tv_sec * 1000u) + (ts_in->tv_nsec / 1000000)));
    return 0;
}

static int thrd_join(thrd_t *thread, int *res)
{
    DWORD code;
    DWORD w = WaitForSingleObject(thread, INFINITE);
    if (w != WAIT_OBJECT_0)
        return thrd_error;
    if (res) {
        if (!GetExitCodeThread(thr, &code)) {
            CloseHandle(thr);
            return thrd_error;
        }
        *res = (int)code;
    }
    CloseHandle(thr);
    return thrd_success;
}

static int thrd_detach(thrd_t *thread, int *res)
{
    if (CloseHandle(*thread)) {
        return thrd_success;
    }
    return thrd_error;
}

static int tls_create(tls_t *key, tls_dtor_t dtor)
{
    if (!key)
        return thrd_error;
    *key = FlsAlloc(dtor);
    return (*key != 0xFFFFFFFF) ? thrd_success : thrd_error;
}

static void tls_delete(tls_t key) { FlsFree(key); }

static void *tls_get(tls_t key) { return FlsGetValue(key); }

static int tls_set(tls_t key, void *val)
{
    return FlsSetValue(key, val) ? thrd_success : thrd_error;
}
#else
#include <errno.h>
#include <pthread.h>
#include <sched.h>

#if !defined(__cplusplus)
#define thread_local __thread
#endif

typedef pthread_t thrd_t;
typedef pthread_key_t tls_t;
typedef pthread_mutex_t mutex_t;

#define thrd_current() pthread_self()
#define thrd_equal(a, b) pthread_equal(a, b)
#define thrd_yield() sched_yield();
#define thrd_exit(res) pthread_exit((void *)(size_t)res)
#define thrd_detach(thr) pthread_detach(thr) == 0 ? thrd_success : thrd_error

static inline int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    if (!thr)
        return thrd_error;
    if (pthread_create(thr, NULL, (void *(*)(void *))func, arg) != 0) {
        return thrd_error;
    }
    return thrd_success;
}

static inline int thrd_join(thrd_t thr, int *res)
{
    int64_t *code;
    if (pthread_join(thr, (void **)&code) != 0)
        return thrd_error;
    if (res)
        *res = (int32_t)*code;
    return thrd_success;
}

static inline int thrd_sleep(const struct timespec *ts_in)
{
    if (nanosleep(ts_in, NULL) < 0) {
        if (errno == EINTR)
            return -1;
        return -2;
    }
    return 0;
}

static int tls_create(tls_t *key, tls_dtor_t dtor)
{
    if (!key)
        return thrd_error;
    return (pthread_key_create(key, dtor) == 0) ? thrd_success : thrd_error;
}

static void tls_delete(tls_t key) { pthread_key_delete(key); }
static void *tls_get(tls_t key) { return pthread_getspecific(key); }
static int tls_set(tls_t key, void *val)
{
    return (pthread_setspecific(key, val) == 0) ? thrd_success : thrd_error;
}
#endif

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



static inline bool reset_memory(void *base, size_t size) {
    if (!base || size == 0) return false;

#if defined(_WIN32)
    // Windows: MEM_RESET + explicit unlock
    void *p = VirtualAlloc(base, size, MEM_RESET, PAGE_READWRITE);
    if (p == base) {
        VirtualUnlock(base, size);  // Optional but recommended
        return true;
    }
#else
    // Linux/macOS: MADV_FREE fallback to MADV_DONTNEED
    int err = madvise(base, size, MADV_FREE);
    if (err == -1) {
        if (errno == EINVAL) {  // MADV_FREE not supported
            err = madvise(base, size, MADV_DONTNEED);
        }
        return (err == 0);
    }
    return true;
#endif
    return false;
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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <memoryapi.h>
#elif defined(__linux__)
#include <sys/mman.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/mman.h>
#endif

static inline bool remap_memory(void *old_addr, void *new_addr, size_t size) {
#if defined(_WIN32)
    // --- Windows Implementation ---
    // Try to reserve the new address first
    void *reserved = VirtualAlloc2(
        GetCurrentProcess(),
        new_addr,
        size,
        MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
        PAGE_NOACCESS,
        NULL,
        0
    );
    if (reserved != new_addr) {
        if (reserved) VirtualFree(reserved, 0, MEM_RELEASE);
        return false;
    }

    // Map the old memory to the new location
    void *result = VirtualAlloc2(
        GetCurrentProcess(),
        new_addr,
        size,
        MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
        PAGE_READWRITE,
        old_addr,
        0
    );

    if (result != new_addr) {
        VirtualFree(new_addr, 0, MEM_RELEASE);
        return false;
    }

    // Free the old memory
    VirtualFree(old_addr, 0, MEM_RELEASE);
    return true;

#elif defined(__linux__)
    // --- Linux Implementation (mremap) ---
    void *result = mremap(old_addr, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, new_addr);
    if (result == MAP_FAILED) {
        return false;
    }
    return true;

#elif defined(__APPLE__)
    // --- macOS Implementation (vm_remap) ---
    vm_prot_t cur_prot, max_prot;
    kern_return_t ret = vm_remap(
        mach_task_self(),
        (vm_address_t *)&new_addr,
        size,
        0,  // mask
        VM_FLAGS_FIXED,  // force new_addr
        mach_task_self(),
        (vm_address_t)old_addr,
        FALSE,  // copy (not needed since we unmap old)
        &cur_prot,
        &max_prot,
        VM_INHERIT_DEFAULT
    );

    if (ret != KERN_SUCCESS) {
        return false;
    }

    // Unmap the old memory
    munmap(old_addr, size);
    return true;

#else
    return false;
#endif
}

static inline size_t get_stack_limit(void)
{
    struct rlimit limit;

    getrlimit (RLIMIT_STACK, &limit);
    return limit.rlim_max;
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

extern __thread Allocator *thread_instance;

static inline uintptr_t get_thread_id(void) {
#if defined(_WIN32)
    return (uintptr_t)NtCurrentTeb();  // Windows
#elif defined(__linux__) && defined(__x86_64__)
    uintptr_t res;
    __asm__("movq %%fs:0, %0" : "=r" (res));  // Linux x86-64 (FS)
    return res;
#elif defined(__APPLE__) && defined(__aarch64__)
    uintptr_t res;
    __asm__ volatile ("mrs %0, tpidrro_el0" : "=r" (res));  // macOS ARM64
    return res;
#elif defined(__APPLE__) && defined(__x86_64__)
    uintptr_t res;
    __asm__("movq %%gs:0, %0" : "=r" (res));  // macOS x86-64 (GS)
    return res;
#else
    // Fallback: Use thread-local variable address as a unique identifier
    return (uintptr_t)&thread_instance;
#endif
}
#endif
