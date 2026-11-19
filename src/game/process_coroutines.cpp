// process_coroutines.cpp

#include "recomp.h"
#include "librecomp/helpers.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <array>
#include <vector>
#include "bm64_debug.h"
#include "bm64_state.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// ============================================================================
// Configuration
// ============================================================================

// Use minicoro as the coroutine backend
#define MINICORO_IMPL
#include "minicoro.h"

// Maximum number of concurrent processes (matches game's process pool size)
static constexpr size_t MAX_PROCESSES = 0x50;

// Minimum native stack size for coroutines (native code needs more than MIPS)
static constexpr size_t MIN_NATIVE_STACK = 0x80000;  // 512KB

// Enable debug logging
//#define PROCESS_DEBUG

// ============================================================================
// Yield reasons (must match MIPS side in process_funcs.h)
// ============================================================================

static constexpr int32_t YIELD_NORMAL     = 1;
static constexpr int32_t YIELD_STACKCHECK = 2;
static constexpr int32_t YIELD_TERMINATE  = 3;

// ============================================================================
// Debug logging
// ============================================================================

#ifdef PROCESS_DEBUG
static void process_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[Process] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(args);
}
#else
#define process_log(...) ((void)0)
#endif

// ============================================================================
// Process slot state
// ============================================================================

enum class ProcessState {
    Free,       // Slot is available
    Created,    // Coroutine created but not yet started
    Running,    // Coroutine is actively executing
    Suspended,  // Coroutine yielded, waiting to resume
    Dead        // Coroutine finished, awaiting cleanup
};

struct ProcessSlot {
    ProcessState state = ProcessState::Free;
    uint32_t process_id = 0;
    uint32_t entry_func = 0;        // MIPS VRAM address of entry function
    uint32_t mips_sp = 0;           // Initial MIPS stack pointer
    mco_coro* coro = nullptr;       // Native coroutine handle
    recomp_context ctx;             // MIPS register context for this process
    
    void release_coro() {
        if (coro == nullptr) {
            return;
        }
        mco_state coro_state = mco_status(coro);
        if (coro_state == MCO_SUSPENDED || coro_state == MCO_DEAD) {
            mco_uninit(coro);
        }
        coro = nullptr;
    }

    void reset() {
        release_coro();
        state = ProcessState::Free;
        process_id = 0;
        entry_func = 0;
        mips_sp = 0;
        std::memset(&ctx, 0, sizeof(ctx));
    }
};

struct SlotBlock {
    void* mem = nullptr;
    size_t size = 0;
    uintptr_t stack_base = 0;
};

static SlotBlock g_blocks[MAX_PROCESSES];
static size_t g_block_size = 0;
static void* g_pool = nullptr;
static bool g_fixed_base = false;
static bool g_stack_moved = false;

static void process_coro_entry(mco_coro* co);

static void* pool_alloc(size_t size, void* data) {
    auto* block = static_cast<SlotBlock*>(data);
    if (block->mem == nullptr || size > block->size) {
        return nullptr;
    }
    return block->mem;
}

static void pool_free(void* ptr, size_t size, void* data) {
    (void)ptr;
    (void)size;
    (void)data;
}

static void ensure_pool() {
    if (g_pool != nullptr) {
        return;
    }
    mco_desc desc = mco_desc_init(process_coro_entry, MIN_NATIVE_STACK);
    g_block_size = desc.coro_size;
    size_t bytes = g_block_size * MAX_PROCESSES;
#ifdef _WIN32
    g_pool = VirtualAlloc(reinterpret_cast<LPVOID>(static_cast<uintptr_t>(0x200000000ull)), bytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_fixed_base = g_pool != nullptr;
    if (g_pool == nullptr) {
        g_pool = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
#endif
    if (g_pool == nullptr) {
        g_pool = std::malloc(bytes);
        g_fixed_base = false;
    }
    if (g_pool == nullptr) {
        fprintf(stderr, "[Process] FATAL: could not allocate the coroutine stack pool\n");
        return;
    }
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        g_blocks[i].mem = static_cast<uint8_t*>(g_pool) + i * g_block_size;
        g_blocks[i].size = g_block_size;
    }
    printf("[Process] Fixed coroutine stacks: %zu slots x %zu bytes at %p%s\n",
        MAX_PROCESSES, g_block_size, g_pool, g_fixed_base ? " (fixed base)" : "");
}

// ============================================================================
// Global state
// ============================================================================

// Fixed array of process slots - stable memory addresses
static std::array<ProcessSlot, MAX_PROCESSES> g_slots;

// Currently executing process slot (nullptr when in scheduler)
static ProcessSlot* g_current_slot = nullptr;

// Communication between scheduler and processes
static int32_t g_yield_reason = 0;
static int32_t g_resume_value = 0;

// Entry context for new coroutines
static uint8_t* g_entry_rdram = nullptr;

// System initialization flag
static bool g_initialized = false;

// ============================================================================
// Slot management
// ============================================================================

static ProcessSlot* find_slot_by_id(uint32_t process_id) {
    for (auto& slot : g_slots) {
        if (slot.state != ProcessState::Free && slot.process_id == process_id) {
            return &slot;
        }
    }
    return nullptr;
}

static ProcessSlot* find_free_slot() {
    for (auto& slot : g_slots) {
        if (slot.state == ProcessState::Free) {
            return &slot;
        }
    }
    return nullptr;
}

// ============================================================================
// Coroutine entry point
// ============================================================================

static void process_coro_entry(mco_coro* co) {
    (void)co;
    
    // Get our slot from the global pointer (set before mco_resume)
    ProcessSlot* slot = g_current_slot;
    if (slot == nullptr) {
        fprintf(stderr, "[Process] FATAL: process_coro_entry called with null g_current_slot\n");
        g_yield_reason = YIELD_TERMINATE;
        mco_yield(mco_running());
        return;
    }
    
    uint8_t* rdram = g_entry_rdram;
    if (rdram == nullptr) {
        fprintf(stderr, "[Process] FATAL: process_coro_entry called with null g_entry_rdram\n");
        g_yield_reason = YIELD_TERMINATE;
        mco_yield(mco_running());
        return;
    }
    
    // Look up the entry function
    recomp_func_t* entry_func = get_function(static_cast<int32_t>(slot->entry_func));
    if (entry_func == nullptr) {
        fprintf(stderr, "[Process] ERROR: Could not find function at 0x%08X for process %u\n",
                slot->entry_func, slot->process_id);
        g_yield_reason = YIELD_TERMINATE;
        mco_yield(mco_running());
        return;
    }
    
    // Initialize MIPS context
    slot->ctx.r29 = slot->mips_sp;  // Stack pointer
    slot->ctx.mips3_float_mode = 0;
    slot->ctx.f_odd = &slot->ctx.f0.u32h;
    
    process_log("Process %u starting at 0x%08X (sp=0x%08X)", 
                slot->process_id, slot->entry_func, slot->mips_sp);
    
    slot->state = ProcessState::Running;
    
    // Call the process entry function
    // This will run until the process yields or terminates
    entry_func(rdram, &slot->ctx);
    
    // Process returned without explicitly terminating
    process_log("Process %u returned normally", slot->process_id);
    slot->state = ProcessState::Dead;
    g_yield_reason = YIELD_TERMINATE;
    mco_yield(mco_running());
    
    // Safety: should never reach here
    while (1) {
        mco_yield(mco_running());
    }
}

// ============================================================================
// API: Initialize process system
// ============================================================================

extern "C" void recomp_process_init(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    
    process_log("Initializing process system");
    ensure_pool();
    
    // Reset all slots
    for (auto& slot : g_slots) {
        slot.reset();
    }
    
    g_current_slot = nullptr;
    g_yield_reason = 0;
    g_resume_value = 0;
    g_entry_rdram = nullptr;
    g_initialized = true;
    
    process_log("Process system initialized with %zu slots", MAX_PROCESSES);
}

// ============================================================================
// API: Create a coroutine for a process
// ============================================================================

extern "C" void recomp_process_coro_create(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    
    uint32_t process_id = _arg<0, uint32_t>(rdram, ctx);
    uint32_t entry_func = _arg<1, uint32_t>(rdram, ctx);
    uint32_t stack_size = _arg<2, uint32_t>(rdram, ctx);
    uint32_t mips_sp = _arg<3, uint32_t>(rdram, ctx);
    
    process_log("Creating coroutine: pid=%u entry=0x%08X stack=%u mips_sp=0x%08X",
                process_id, entry_func, stack_size, mips_sp);
    
    // Check if this process already has a slot
    ProcessSlot* slot = find_slot_by_id(process_id);
    
    if (slot != nullptr) {
        // Reuse existing slot. The native stack stays at this slot's fixed address.
        process_log("Reusing slot for process %u (old state=%d)", 
                    process_id, static_cast<int>(slot->state));
        slot->release_coro();
    } else {
        // Allocate a new slot
        slot = find_free_slot();
        if (slot == nullptr) {
            fprintf(stderr, "[Process] ERROR: No free slots for process %u\n", process_id);
            return;
        }
    }

    ensure_pool();
    size_t index = static_cast<size_t>(slot - g_slots.data());
    if (g_blocks[index].mem == nullptr) {
        fprintf(stderr, "[Process] ERROR: stack pool is not ready\n");
        return;
    }
    (void)stack_size;

    // Every slot uses the same preallocated stack so its address never changes.
    mco_desc desc = mco_desc_init(process_coro_entry, MIN_NATIVE_STACK);
    desc.alloc_cb = pool_alloc;
    desc.dealloc_cb = pool_free;
    desc.allocator_data = &g_blocks[index];
    mco_result res = mco_create(&slot->coro, &desc);
    
    if (res != MCO_SUCCESS) {
        fprintf(stderr, "[Process] ERROR: Failed to create coroutine for process %u: %s\n",
                process_id, mco_result_description(res));
        slot->reset();
        return;
    }
    
    // Initialize slot
    slot->state = ProcessState::Created;
    slot->process_id = process_id;
    slot->entry_func = entry_func;
    slot->mips_sp = mips_sp;
    std::memset(&slot->ctx, 0, sizeof(slot->ctx));

    uintptr_t base = reinterpret_cast<uintptr_t>(slot->coro->stack_base);
    if (g_blocks[index].stack_base == 0) {
        g_blocks[index].stack_base = base;
    }
    else if (g_blocks[index].stack_base != base) {
        g_stack_moved = true;
        fprintf(stderr, "[Process] Stack moved for slot %zu: %p -> %p\n",
            index, reinterpret_cast<void*>(g_blocks[index].stack_base), reinterpret_cast<void*>(base));
    }
    
    process_log("Coroutine created for process %u in slot %zu", 
                process_id, slot - g_slots.data());
}

// ============================================================================
// API: Destroy a process's coroutine
// ============================================================================

extern "C" void recomp_process_coro_destroy(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    
    uint32_t process_id = _arg<0, uint32_t>(rdram, ctx);
    
    process_log("Destroying coroutine for process %u", process_id);
    
    ProcessSlot* slot = find_slot_by_id(process_id);
    if (slot == nullptr) {
        process_log("No slot found for process %u", process_id);
        return;
    }
    
    // Don't destroy the currently running coroutine
    if (slot == g_current_slot) {
        process_log("Cannot destroy currently running process %u", process_id);
        return;
    }
    
    slot->reset();
    process_log("Process %u destroyed", process_id);
}

// ============================================================================
// API: Yield from current process back to scheduler
// ============================================================================

extern "C" void recomp_process_yield(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    
    int32_t reason = _arg<0, int32_t>(rdram, ctx);
    
    ProcessSlot* slot = g_current_slot;
    if (slot == nullptr) {
        fprintf(stderr, "[Process] ERROR: yield called with no current process\n");
        _return(ctx, static_cast<int32_t>(-1));
        return;
    }
    
    process_log("Process %u yielding with reason %d", slot->process_id, reason);
    
    // Update state
    if (reason == YIELD_TERMINATE) {
        slot->state = ProcessState::Dead;
    } else {
        slot->state = ProcessState::Suspended;
    }
    
    // Store yield reason for scheduler
    g_yield_reason = reason;
    
    // Yield back to scheduler
    mco_yield(mco_running());
    
    // Resumed by scheduler
    slot->state = ProcessState::Running;
    
    process_log("Process %u resumed with value %d", slot->process_id, g_resume_value);
    
    _return(ctx, g_resume_value);
}

// ============================================================================
// API: Switch from scheduler to a process
// ============================================================================

extern "C" void recomp_process_switch_to(uint8_t* rdram, recomp_context* ctx) {
    uint32_t process_id = _arg<0, uint32_t>(rdram, ctx);
    int32_t resume_value = _arg<1, int32_t>(rdram, ctx);
    
    ProcessSlot* slot = find_slot_by_id(process_id);
    if (slot == nullptr) {
        fprintf(stderr, "[Process] ERROR: switch_to called for unknown process %u\n", process_id);
        _return(ctx, static_cast<int32_t>(YIELD_TERMINATE));
        return;
    }
    
    if (slot->coro == nullptr) {
        fprintf(stderr, "[Process] ERROR: process %u has no coroutine\n", process_id);
        _return(ctx, static_cast<int32_t>(YIELD_TERMINATE));
        return;
    }
    
    // Check coroutine state
    mco_state coro_state = mco_status(slot->coro);
    if (coro_state == MCO_DEAD) {
        process_log("Process %u coroutine is dead", process_id);
        _return(ctx, static_cast<int32_t>(YIELD_TERMINATE));
        return;
    }
    
    process_log("Switching to process %u with value %d", process_id, resume_value);
    
    // Set up context for the process
    g_current_slot = slot;
    g_entry_rdram = rdram;
    g_resume_value = resume_value;
    g_yield_reason = 0;
    
    // Resume the coroutine
    mco_result res = mco_resume(slot->coro);
    
    // Clear current slot (we're back in scheduler)
    g_current_slot = nullptr;
    
    if (res != MCO_SUCCESS) {
        fprintf(stderr, "[Process] ERROR: mco_resume failed for process %u: %s\n",
                process_id, mco_result_description(res));
        slot->state = ProcessState::Dead;
        _return(ctx, static_cast<int32_t>(YIELD_TERMINATE));
        return;
    }
    
    process_log("Process %u yielded with reason %d", process_id, g_yield_reason);
    
    // Return the yield reason to the scheduler
    _return(ctx, g_yield_reason);
}

// ============================================================================
// API: Check if a process coroutine has been started
// ============================================================================

extern "C" void recomp_process_is_started(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    
    uint32_t process_id = _arg<0, uint32_t>(rdram, ctx);
    
    ProcessSlot* slot = find_slot_by_id(process_id);
    if (slot == nullptr) {
        _return(ctx, static_cast<int32_t>(0));
        return;
    }
    
    // Process is "started" if it's past the Created state
    int32_t started = (slot->state != ProcessState::Free && 
                       slot->state != ProcessState::Created) ? 1 : 0;
    _return(ctx, started);
}

// ============================================================================
// API: Mark a process coroutine as started  
// ============================================================================

extern "C" void recomp_process_set_started(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    
    uint32_t process_id = _arg<0, uint32_t>(rdram, ctx);
    
    ProcessSlot* slot = find_slot_by_id(process_id);
    if (slot != nullptr && slot->state == ProcessState::Created) {
        slot->state = ProcessState::Suspended;
    }
}

// ============================================================================
// API: Get process context pointer
// Note: This returns a native pointer in r2/r3 (lo/hi halves) since
// MIPS only supports 32-bit return values.
// ============================================================================

extern "C" void recomp_process_get_ctx(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    
    uint32_t process_id = _arg<0, uint32_t>(rdram, ctx);
    
    ProcessSlot* slot = find_slot_by_id(process_id);
    if (slot == nullptr) {
        ctx->r2 = 0;
        ctx->r3 = 0;
        return;
    }
    
    // Return native pointer as two 32-bit halves
    uintptr_t ptr = reinterpret_cast<uintptr_t>(&slot->ctx);
    ctx->r2 = static_cast<uint64_t>(ptr & 0xFFFFFFFF);         // Low 32 bits
    ctx->r3 = static_cast<uint64_t>((ptr >> 32) & 0xFFFFFFFF); // High 32 bits
}

// ============================================================================
// Stub implementations for setjmp/longjmp
// These are never actually called in the recompiled code but may be
// referenced by function tables.
// ============================================================================

/*
extern "C" void longjmp_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    fprintf(stderr, "[Process] ERROR: longjmp_recomp called - this should not happen!\n");
}
*/

extern "C" void setjmp_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    fprintf(stderr, "[Process] ERROR: setjmp_recomp called - this should not happen!\n");
}

void bm64::debug::get_process_stacks(std::vector<ProcessStack>& out) {
    out.clear();
    for (const auto& slot : g_slots) {
        if (slot.state == ProcessState::Suspended || slot.state == ProcessState::Created) {
            out.push_back({ slot.process_id, slot.mips_sp, static_cast<uint32_t>(slot.ctx.r29) });
        }
    }
}

namespace {

void append_u32(std::vector<uint8_t>& out, uint32_t v) {
    uint8_t b[4] = { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24) };
    out.insert(out.end(), b, b + 4);
}

void append_bytes(std::vector<uint8_t>& out, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

struct BlobReader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    uint32_t u32() {
        if (p + 4 > end) {
            ok = false;
            return 0;
        }
        uint32_t v = static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
        p += 4;
        return v;
    }
    void bytes(void* dst, size_t n) {
        if (p + n > end) {
            ok = false;
            return;
        }
        std::memcpy(dst, p, n);
        p += n;
    }
};

void* coro_rsp(mco_coro* co) {
    void* rsp = nullptr;
    if (co == nullptr || co->context == nullptr) {
        return nullptr;
    }
    std::memcpy(&rsp, static_cast<uint8_t*>(co->context) + sizeof(void*), sizeof(rsp));
    return rsp;
}

void fix_float_odd(recomp_context& ctx) {
    if (ctx.mips3_float_mode) {
        ctx.f_odd = &ctx.f1.u32l;
    }
    else {
        ctx.f_odd = &ctx.f0.u32h;
    }
}

} // namespace

bool bm64::proc::stacks_moved() {
    return g_stack_moved;
}

bool bm64::proc::save(std::vector<uint8_t>& out) {
    if (g_current_slot != nullptr) {
        fprintf(stderr, "[Process] Refusing to save while a coroutine is running\n");
        return false;
    }
    out.clear();
    append_u32(out, 0x434F5250u);
    append_u32(out, 1);
    append_u32(out, static_cast<uint32_t>(MAX_PROCESSES));
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        const ProcessSlot& slot = g_slots[i];
        out.push_back(static_cast<uint8_t>(slot.state));
        append_u32(out, slot.process_id);
        append_u32(out, slot.entry_func);
        append_u32(out, slot.mips_sp);
        append_bytes(out, &slot.ctx, sizeof(slot.ctx));
        bool has = slot.coro != nullptr;
        out.push_back(has ? 1 : 0);
        if (!has) {
            continue;
        }
        auto* base = static_cast<uint8_t*>(slot.coro->stack_base);
        auto* start = reinterpret_cast<uint8_t*>(slot.coro);
        if (base < start) {
            fprintf(stderr, "[Process] Coroutine %zu has a stack below its header\n", i);
            return false;
        }
        uint32_t prefix = static_cast<uint32_t>(base - start);
        append_u32(out, prefix);
        append_bytes(out, start, prefix);
        auto* rsp = static_cast<uint8_t*>(coro_rsp(slot.coro));
        auto* top = base + slot.coro->stack_size;
        uint32_t used = 0;
        if (rsp != nullptr && rsp >= base && rsp <= top) {
            used = static_cast<uint32_t>(top - rsp);
        }
        append_u32(out, used);
        if (used > 0) {
            append_bytes(out, rsp, used);
        }
    }
    return true;
}

bool bm64::proc::load(const uint8_t* data, size_t size) {
    if (g_current_slot != nullptr) {
        fprintf(stderr, "[Process] Refusing to load while a coroutine is running\n");
        return false;
    }
    ensure_pool();
    BlobReader r{ data, data + size, true };
    if (r.u32() != 0x434F5250u || r.u32() != 1 || r.u32() != MAX_PROCESSES) {
        fprintf(stderr, "[Process] Coroutine snapshot header rejected\n");
        return false;
    }
    for (size_t i = 0; i < MAX_PROCESSES; i++) {
        ProcessSlot& slot = g_slots[i];
        uint8_t state_byte = 0;
        r.bytes(&state_byte, 1);
        slot.state = static_cast<ProcessState>(state_byte);
        slot.process_id = r.u32();
        slot.entry_func = r.u32();
        slot.mips_sp = r.u32();
        r.bytes(&slot.ctx, sizeof(slot.ctx));
        fix_float_odd(slot.ctx);
        uint8_t has = 0;
        r.bytes(&has, 1);
        if (!r.ok) {
            return false;
        }
        if (!has) {
            slot.coro = nullptr;
            continue;
        }
        uint32_t prefix = r.u32();
        auto* block = static_cast<uint8_t*>(g_blocks[i].mem);
        if (block == nullptr || prefix > g_blocks[i].size) {
            return false;
        }
        r.bytes(block, prefix);
        uint32_t used = r.u32();
        auto* restored = reinterpret_cast<mco_coro*>(block);
        auto* base = static_cast<uint8_t*>(restored->stack_base);
        auto* top = base + restored->stack_size;
        if (used > 0) {
            if (base == nullptr || used > restored->stack_size) {
                return false;
            }
            r.bytes(top - used, used);
        }
        slot.coro = restored;
        if (!r.ok) {
            return false;
        }
    }
    return r.ok;
}
