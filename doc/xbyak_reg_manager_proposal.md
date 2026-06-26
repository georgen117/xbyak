# `xbyak_reg_manager` — Proposed Improvements

This document expands on each proposed improvement to `RegPoolManager` from `xbyak_reg_manager.hpp`.
For each item the following is provided:

- **Motivation** — why the change is needed.
- **Proposed API** — exact function signatures to add or modify.
- **Internal state changes** — any new data members required.
- **Usage example** — a minimal code snippet.
- **Notes / interactions** — cross-cutting concerns.

All new public API lives inside `class RegPoolManager` in `namespace Xbyak` unless stated otherwise.
The existing `alloc<T>()`, `free()`, `makeScoped()`, `reg_in_use()` etc. are unchanged.

---

## Table of Contents

1. [Bug Fix: XCR0 Operator Precedence](#1-bug-fix-xcr0-operator-precedence)
2. [Performance: Replace `std::set` with Bitmask Pools](#2-performance-replace-stdset-with-bitmask-pools)
3. [Pool Management: `add_to_vec_pool()` and `add_to_opmask_pool()`](#3-pool-management-add_to_vec_pool-and-add_to_opmask_pool)
4. [Register Reservation: `mark_unavailable()` / `mark_available()`](#4-register-reservation-mark_unavailable--mark_available)
5. [Pinned Registers: `pin()` and `alloc_pinned()`](#5-pinned-registers-pin-and-alloc_pinned)
6. [Code Emission Coupling: `set_code_generator()`](#6-code-emission-coupling-set_code_generator)
7. [Register Spill / Restore: `spill()` and `restore()`](#7-register-spill--restore-spill-and-restore)
8. [Stack Frame Management: `StackFrame` RAII Helper](#8-stack-frame-management-stackframe-raii-helper)
9. [Preserved Register Tracking: `emit_prologue()` / `emit_epilogue()`](#9-preserved-register-tracking-emit_prologue--emit_epilogue)
10. [End-of-JIT Validation: `assert_all_free()`](#10-end-of-jit-validation-assert_all_free)
11. [Debug vs. Release Error Handling](#11-debug-vs-release-error-handling)
12. [Named-Register `alloc()` Overload](#12-named-register-alloc-overload)
13. [Remove `used_*` Sets](#13-remove-used_-sets)
14. [In-Use Volatile / Preserved Getters](#14-in-use-volatile--preserved-getters)
15. [ABI Configuration: Windows x64 vs SysV](#15-abi-configuration-windows-x64-vs-sysv)
16. [Manager `reset()` to Complement `CodeGenerator::reset()`](#16-manager-reset-to-complement-codegeneratorreset)
17. [Accept External `Xbyak::util::Cpu` Reference](#17-accept-external-xbyakutilcpu-reference)
18. [`emit_call()` — ABI-correct Outgoing Calls (Shadow Space + Alignment)](#18-emit_call--abi-correct-outgoing-calls-shadow-space--alignment)
19. [Stack Integrity Checks: `clean_stack()` / `assert_clean_stack()` / `spill_stack_empty()` / `assert_spill_stack_empty()`](#19-stack-integrity-checks)
20. [Save/Restore Live Volatile Registers: `save_volatiles()` / `restore_volatiles()`](#20-saverestore-live-volatile-registers-savevolatiles--restorevolatiles)
21. [Pool Count Queries: `free_gp_count()`, `free_vec_count()`, etc.](#21-pool-count-queries-free_gp_count-free_vec_count-etc)
22. [Rename `in_use` to `live` in Getter Names](#22-rename-in_use-to-live-in-getter-names)
23. [Unified Stack Layout: `StackFrameBuilder` / `StackFrame` (Replaces §7 and §8)](#23-unified-stack-layout-stackframebuilder--stackframe-replaces-7-and-8)
24. [Open Task: ABI-portable argument register mapping (Future Work)](#24-open-task-abi-portable-argument-register-mapping-future-work)
25. [Stack-Overflow Arguments: `with_outgoing_args()` / `StackFrame::emit_call()`](#25-stack-overflow-arguments-with_outgoing_args--stackframeemit_call)
26. [Named-Alias Register Lifecycle: `declare_alias()` / `ManagedAlias`](#26-named-alias-register-lifecycle-declare_alias--managedalias)
27. [Allow `make_stack_frame().build()` with no slots (empty layout)](#27-allow-make_stack_framebuild-with-no-slots-empty-layout)
28. [RAII Wrapper for `ManagedAlias`: `ScopedAlias`](#28-raii-wrapper-for-managedalias-scopedalias)
29. [Post-`build()` `declare_alias()` Detection](#29-post-build-declare_alias-detection)
30. [Exclusive Sequential Alias: `declare_alias(reg, AliasMode::no_slot)`](#30-exclusive-sequential-alias-declare_aliasreg-aliasmodenostlot)
31. [`ManagedAlias` for Vector Registers: `ManagedVecAlias`](#31-managedalias-for-vector-registers-managedvecalias)
---

## Implementation Status
[x] - Completed
[o] - Rejected (Should have a reason for rejection or point to an alternate proposal)
[ ] - Yet to be implemeted / still under consideration

- [x] 1. Bug Fix: XCR0 Operator Precedence
- [o] 2. Performance: Replace `std::set` with Bitmask Pools *(REJECTED — see §2)*
- [o] 3. Pool Management: `add_to_vec_pool()` and `add_to_opmask_pool()` *(REJECTED — see §3)*
- [x] 4. Register Reservation: `mark_unavailable()` / `mark_available()`
- [o] 5. Pinned Registers: `pin()` and `alloc_pinned()` *(REJECTED — see §5)*
- [x] 6. Code Emission Coupling: `set_code_generator()`
- [o] 7. Register Spill / Restore: `spill()` and `restore()` *(REVIEW NOTES — see §7)* *(REJECTED in favor of §23)*
  - [o] 7a. Resolve GP-only limitation: extend `spill()` to Vec, or remove in favour of a unified approach
  - [o] 7b. Guard against `spill()` being called while a `StackFrame` is active
- [o] 8. Stack Frame Management: `StackFrame` RAII Helper *(REVIEW NOTES — see §8)* *(REJECTED in favor of §23)*
  - [o] 8a. Fix `put_on_stack(RegT &reg)` destructive-free behaviour: remove or rename to make intent obvious
- [x] 9. Preserved Register Tracking: `emit_prologue()` / `emit_epilogue()`
  - [o] 9a. Add debug assertion: fire if preserved register is promoted by `alloc()` before `emit_prologue()` has been called *(REJECTED — see §9)*
- [x] 10. End-of-JIT Validation: `assert_all_free()`
- [x] 11. Error Handling: Align with `XBYAK_THROW` / `Xbyak::Error`
- [x] 12. Named-Register `alloc()` Overload
- [x] 13. Remove `used_*` Sets
- [x] 14. In-Use Volatile / Preserved Getters *(REVIEW NOTE — see §14)*
  - [x] 14a. Remove `get_in_use_volatile_opmasks()` (identical to `get_in_use_opmasks()`)
  - [x] 14b. Remove `get_in_use_volatile_tiles()` (identical to `get_in_use_tiles()`)
  - [x] 14c. Remove per-family index helpers: `gp_idx_in_use()`, `vec_idx_in_use()`, `opmask_idx_in_use()`, `tile_idx_in_use()`
  - [ ] 14d. Remove `_stack_pointer()`, `_base_pointer()`, `_opmask_k0()` helpers
- [x] 15. ABI Configuration: Windows x64 vs SysV *(REJECTED — see §15)*
- [x] 16. Manager `reset()` to Complement `CodeGenerator::reset()`
- [x] 17. Accept External `Xbyak::util::Cpu` Reference
- [x] 18. `emit_call()` -- ABI-correct Outgoing Calls (Shadow Space + Alignment)
  - [o] 18a. Debug assertion for rax liveness at `emit_call()` *(REJECTED -- false positive; see §18)*
  - [o] 18b. Deprecate `extra_pushes` once `save_volatiles()` (§20) is implemented *(REJECTED -- see §18)*
- [x] 19. Stack Integrity Checks: `clean_stack()` / `assert_clean_stack()` / `spill_stack_empty()` / `assert_spill_stack_empty()`
- [x] 20. Save/Restore Live Volatile Registers: `save_volatiles()` / `restore_volatiles()`
- [x] 21. Pool Count Queries: `free_gp_count()`, `free_vec_count()`, etc. *(REJECTED — see §21)*
- [x] 22. Rename `in_use` to `live` in Getter Names
- [x] 23. Unified Stack Layout: `StackFrameBuilder` / `StackFrame`
- [ ] 24. Open Task: ABI-portable argument register mapping (Future Work)
- [x] 25. Stack-Overflow Arguments: `with_outgoing_args()` / `StackFrame::emit_call()`
- [x] 26. Named-Alias Register Lifecycle: `declare_alias()` / `ManagedAlias`
- [x] 27. Allow `make_stack_frame().build()` with no slots (empty layout)
- [ ] 28. RAII Wrapper for `ManagedAlias`: `ScopedAlias`
- [x] 29. Post-`build()` `declare_alias()` Detection
- [ ] 30. Exclusive Sequential Alias: `declare_alias(reg, AliasMode::no_slot)`
- [ ] 31. `ManagedAlias` for Vector Registers: `ManagedVecAlias`

---

## 1. Bug Fix: XCR0 Operator Precedence

### Motivation

**Resolved This was an issue in the x64 implementation of the xbyak_reg_manager**

In C++, `&` has lower precedence than `==`. The current check:

```cpp
has_apx_ = (xcr0 >> 19) & 1 == 1;
```

parses as:

```cpp
has_apx_ = (xcr0 >> 19) & (1 == 1);  // (1 == 1) is always 1, happens to work
```

This accidentally produces the correct result today, but is a latent bug if the check is
ever adapted (e.g. `!= 0`, `>= 1`). It will also trigger `-Wparentheses` on stricter builds.

### Fix

```cpp
// Before:
has_apx_   = (xcr0 >> 19) & 1 == 1;   // broken precedence
has_avx512_ = (xcr0 >> 7) & 1 == 1;   // broken precedence

// After:
has_apx_    = ((xcr0 >> 19) & 1) == 1;
has_avx512_ = ((xcr0 >> 7)  & 1) == 1;
```

No API change. No new data members.

---

## 2. Performance: Replace `std::set` with Bitmask Pools

> **REJECTED — Do not implement.**
>
> For small, fixed pool sizes (n ≤ 32) O(1) and O(log n) are practically
> indistinguishable in wall-clock time. Replacing `std::set` with bitmasks would also
> introduce non-standard compiler built-ins (`__builtin_ctz`, `_BitScanForward`),
> reducing portability. Retaining `std::set`-based containers keeps the implementation
> maximally compatible with standard C++. This optimisation may be revisited in the
> future if profiling identifies the pools as a genuine bottleneck.

### Motivation

All pool operations (`alloc`, `free`, `reg_in_use`) use `std::set<int>` which is O(log n)
and pointer-chasing through a red-black tree. For a maximum pool size of 32 registers a
`uint32_t` bitmask is O(1) on all operations, uses 4 bytes vs. ~480 bytes per set, and is
entirely cache-resident.

### Proposed Internal Representation

This is a **pure internal implementation change**. The public API is unchanged.

```cpp
// Replace each std::set<int> pair with a uint32_t bitmask.
// Bit i set = register index i is in that pool.

// Current (slow):
std::set<int> free_gp_regs;
std::set<int> in_use_gp;
std::set<int> preserved_gp;
// used_gp removed — see §13

// Proposed (fast):
uint32_t free_gp_mask_      = 0;  // volatile (caller-saved) regs available
uint32_t preserved_gp_mask_ = 0;  // callee-saved regs available
uint32_t in_use_gp_mask_    = 0;  // currently allocated
// used_gp_mask_ removed — see §13
// Same pattern for vec and opmask.
```

**Key primitives:**

```cpp
// Next free bit (lowest available index):
inline int next_set_bit(uint32_t mask) {
    return __builtin_ctz(mask);   // GCC/Clang; _BitScanForward on MSVC
}

// Allocate from bitmask pool:
inline int alloc_from(uint32_t &pool) {
    if (pool == 0) throw std::runtime_error("No registers available");
    const int idx = __builtin_ctz(pool);
    pool &= pool - 1;  // clear lowest set bit
    return idx;
}

// Release back to pool:
inline void release_to(uint32_t &pool, int idx) {
    pool |= (1u << idx);
}

// Check membership:
inline bool in_pool(uint32_t mask, int idx) {
    return (mask >> idx) & 1u;
}
```

**Getter methods** can be updated to materialise a `std::vector<int>` on demand (same
signatures, just different internals):

```cpp
std::vector<int> get_free_gps() const {
    return bits_to_vector(free_gp_mask_);
}

// Helper:
static std::vector<int> bits_to_vector(uint32_t mask) {
    std::vector<int> v;
    v.reserve(__builtin_popcount(mask));
    while (mask) {
        v.push_back(__builtin_ctz(mask));
        mask &= mask - 1;
    }
    return v;
}
```

No usage changes required anywhere.

---

## 3. Pool Management: `add_to_vec_pool()` and `add_to_opmask_pool()`

> **REJECTED — Do not implement.**
>
> The sole justification for `add_to_gp_pool()` is that rsp (4) and rbp (5) are
> intentionally excluded from the allocation pool because they have fixed architectural
> roles, and a caller may legitimately want to reclaim one after it is no longer needed
> as a frame pointer. No equivalent excluded register exists in the Vec or Opmask
> families:
>
> - **Vec:** The constructor pre-populates *all* of xmm0–xmm15 into either
>   `free_vec_regs` or `preserved_vec`, and adds xmm16–xmm31 automatically when
>   AVX-512 is detected. Every allocatable vector register is already in the pool.
>   There is nothing to add.
>
> - **Opmask (k0):** k0 is excluded not because it is scarce, but because x86-64
>   hardwires it to mean "unmasked". When k0 is used as a writemask in any
>   EVEX-encoded instruction the masking behaviour is suppressed entirely —
>   `vadd zmm0{k0}, zmm1, zmm2` is identical to `vadd zmm0, zmm1, zmm2`.
>   Writing a mask value into k0 via `kmov` has no effect on subsequent vector
>   instructions. Adding it to the allocator pool would allow code to allocate it,
>   store a predicate, and then silently ignore that predicate — a correctness trap
>   with no upside.
>
> `add_to_gp_pool()` should remain as-is. This section is kept for reference only.

### Motivation

`add_to_gp_pool()` already exists to let callers add registers that are not normally
in the allocation pool (e.g. rsp/rbp after a function finishes needing them). No
equivalent exists for the Vec or Opmask families, creating an inconsistency.

### Proposed API

```cpp
// Vec family
void add_to_vec_pool(const Xmm &reg) { add_to_vec_pool(reg.getIdx()); }
void add_to_vec_pool(int idx) {
    if (idx < 0 || idx > max_vec_reg_idx_)
        throw std::runtime_error("Vec register index out of range");
    if (free_vec_regs.count(idx) || preserved_vec.count(idx) || in_use_vec.count(idx))
        throw std::runtime_error("Vec register already tracked");
    free_vec_regs.insert(idx);
}

// Opmask family
void add_to_opmask_pool(const Opmask &reg) { add_to_opmask_pool(reg.getIdx()); }
void add_to_opmask_pool(int idx) {
    if (idx < 0 || idx > 7)
        throw std::runtime_error("Opmask register index out of range");
    if (free_opmask_regs.count(idx) || in_use_opmask.count(idx))
        throw std::runtime_error("Opmask register already tracked");
    free_opmask_regs.insert(idx);
}
```

No new data members.

### Usage Example

```cpp
RegPoolManager rm;

// k0 is special (means "unmasked") and excluded from auto-allocation.
// If a kernel legitimately needs to allocate k0 for a non-masking purpose,
// it can be manually added:
rm.add_to_opmask_pool(0);
auto my_k0 = rm.alloc<Opmask>();  // will now return k0
rm.free(my_k0);

// Similarly for a scratch vector register that was previously special-cased:
rm.add_to_vec_pool(16);   // zmm16, if not added by AVX-512 auto-detect
auto scratch = rm.alloc<Zmm>();
rm.free(scratch);
```

---

## 4. Register Reservation: `mark_unavailable()` / `mark_available()`

### Motivation

At JIT ABI boundaries specific hardware registers carry defined roles: `rdi` is the first
integer argument (SysV), `rcx` holds a loop counter, etc. Currently there is no way to
inform the manager that a specific register is in use by convention without calling
`alloc<Reg64>(idx)` — which implies the caller must also call `free()` later, and adds
the register to the `used` set. `mark_register_unavailable()` covers this cleanly.

### Design Note

Registers marked unavailable via this API are tracked in a separate set
`reserved_gp` (not `in_use_gp`) so they can be distinguished from registers
handed out through `alloc()`. They are invisible to `get_in_use_gps()` to avoid
confusing ABI-reserved registers with allocator-managed ones.

### Internal State Changes

```cpp
// New private members:
std::set<int> reserved_gp;    // externally reserved, not managed by alloc/free
std::set<int> reserved_vec;
std::set<int> reserved_opmask;
```

### Proposed API

```cpp
// Reserve a specific hardware register, removing it from the free/preserved pool.
// Throws if it is already in_use, already reserved, or already allocated.
template <class RegT>
void mark_unavailable(int idx);

// Return a previously mark_unavailable'd register back to its original pool.
template <class RegT>
void mark_available(int idx);

// Query whether a register is currently reserved (not alloc'd, just reserved).
template <class RegT>
bool is_reserved(int idx) const;
```

Concrete specialisations (or dispatch via `reg_family<>` switch):

```cpp
void mark_unavailable_gp(int idx);   // removes from free/preserved, adds to reserved_gp
void mark_available_gp(int idx);     // returns to whichever pool it came from
bool is_reserved_gp(int idx) const;
// Same for _vec and _opmask variants.
```

### Usage Example

```cpp
// SysV ABI: rdi (idx=7) holds the kernel params pointer on entry.
// Reserve it so the allocator won't hand it out by accident.
rm.mark_unavailable<Reg64>(static_cast<int>(Xbyak::Operand::RDI)); // idx = 7

// Use rdi directly in generated code to read the param struct:
Reg64 rdi_param = Reg64(7);
mov(reg_a, ptr[rdi_param + offsetof(kernel_params_t, src)]);

// Once done with the ABI register, release it back to the pool:
rm.mark_available<Reg64>(7);

// The allocator can now hand out rdi as a general scratch register.
auto scratch = rm.alloc<Reg64>();  // may return rdi
rm.free(scratch);
```

---

## 5. Pinned Registers: `pin()` and `alloc_pinned()`

> **REJECTED — Do not implement.**
>
> The sole purpose of `pin()` is to suppress false positives from `assert_all_free()`
> for registers that are intentionally kept in-use for the lifetime of a kernel. However
> `assert_all_free()` is a debug-only developer aid that compiles to nothing in release
> builds. Adding an entire new API surface (`pin`, `alloc_pinned`, `unpin`, `is_pinned`,
> three new `pinned_*` sets, and modifications to `assert_all_free`) to silence a
> debug-only assertion is not justified when the same result is trivially achieved by
> calling `free()` on long-lived registers before invoking `assert_all_free()`. The
> emitted JIT code is unaffected either way.
>
> Long-lived registers that truly outlive any scope are simply left allocated until
> kernel emission ends; `assert_all_free()` should be called after those registers have
> been released, or not called at all in kernels that intentionally keep registers live.
> This section is kept for reference only.

### Motivation

Many JIT kernels assign a register to a long-lived pointer (e.g. input tensor base
address) that persists for the entire kernel. These registers are never released during
JIT emission. The end-of-JIT validation (see §10) would incorrectly flag them as leaks.
Pinning provides first-class support for this pattern: a register allocated for the full
kernel lifetime is marked pinned so validation skips it.

### Design

Pinned registers remain in `in_use_*` but are additionally added to a `pinned_gp` set.
The `assert_all_free()` call skips any index present in the pinned set. Pinned
registers still participate in introspection (`get_in_use_gps()` returns them).

### Internal State Changes

```cpp
std::set<int> pinned_gp;    // subset of in_use_gp, excluded from leak detection
std::set<int> pinned_vec;
std::set<int> pinned_opmask;
```

### Proposed API

```cpp
// Pin a register that is currently in_use. It will no longer be flagged as a
// leak by assert_all_free(). The register is still in_use and cannot be alloc'd.
template <class RegT>
void pin(const RegT &reg);

// Convenience: allocate and immediately pin in one call.
template <class RegT>
RegT alloc_pinned() {
    RegT r = alloc<RegT>();
    pin(r);
    return r;
}

template <class RegT>
RegT alloc_pinned(int idx) {
    RegT r = alloc<RegT>(idx);
    pin(r);
    return r;
}

// Query: is this register pinned?
template <class RegT>
bool is_pinned(const RegT &reg) const;

// Unpin without freeing (returns it to normal in_use tracking).
template <class RegT>
void unpin(const RegT &reg);
```

### Usage Example

```cpp
// Allocate the weights base address pointer for the lifetime of the kernel.
// rdi is the first argument (SysV), allocate the next free GP for the pointer copy.
Reg64 weights_base = rm.alloc_pinned<Reg64>();
mov(weights_base, ptr[rdi + offsetof(kernel_args_t, weights)]);

// ... entire kernel body uses weights_base freely ...
// No need for: rm.free(weights_base);

// assert_all_free() will not complain about weights_base at kernel end.
rm.assert_all_free();  // only checks non-pinned registers
```

---

## 6. Code Emission Coupling: `set_code_generator()`

### Motivation

Features in §7 (spill/restore) and §8 (stack frame management) require the register
manager to emit actual x86 instructions (`push`, `pop`, `sub rsp`, `add rsp`, `mov
[rsp+off], reg`, etc.). The manager currently has no reference to a `CodeGenerator`.

### Design Options

| Option | Approach | Tradeoff |
|---|---|---|
| **A (recommended)** | Store an optional `CodeGenerator*`, set at construction or lazily | Zero overhead when not used; backward-compatible |
| B | Template `RegPoolManager<JitT>` on JIT type | Type-safe but breaks existing usage |
| C | Pass `CodeGenerator&` to each spill/restore call | Verbose at each call site |

**Option A** is recommended. Passing `nullptr` (the default) means the manager operates
in tracking-only mode (existing behaviour). Callers that need spill/restore pass `this`
at construction from within their `CodeGenerator` subclass.

### Proposed API

```cpp
// At construction (preferred for JIT kernel subclasses):
explicit RegPoolManager(Xbyak::CodeGenerator *cg = nullptr) : cg_(cg) { /* ... */ }

// Or lazy injection after construction:
void set_code_generator(Xbyak::CodeGenerator *cg) { cg_ = cg; }

// Query:
bool has_code_generator() const { return cg_ != nullptr; }
```

### Internal State Change

```cpp
Xbyak::CodeGenerator *cg_ = nullptr;
```

### Usage Example

```cpp
class MyKernel : public Xbyak::CodeGenerator {
    Xbyak::RegPoolManager rm_;
public:
    MyKernel() : Xbyak::CodeGenerator(4096), rm_(this) {
        // rm_ can now emit instructions.
        auto r1 = rm_.alloc<Reg64>();
        auto r2 = rm_.alloc<Reg64>();
        // ... use r1, r2 ...
        rm_.free(r1);
        rm_.free(r2);
    }
};
```

Alternatively, for kernels that inherit `RegPoolManager` directly (removing the `rm_` prefix entirely):

```cpp
class MyKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
public:
    MyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(this) {
        // alloc/free/spill called directly without rm_ prefix.
        auto r1 = alloc<Reg64>();
        // ...
        free(r1);
    }
};
```

---

## 7. Register Spill / Restore: `spill()` and `restore()`

### Motivation

When all volatile GP registers are exhausted mid-kernel, the only recourse is to push
one or more live register values to the stack and reclaim them temporarily. Currently
the manager has no mechanism for this; callers must manually write `push`/`pop`
sequences and reason about stack alignment themselves.

**Prerequisite:** §6 (code emission coupling) must be in place.

### Design

- `spill(reg)` emits `push reg` into the code stream, removes `reg` from `in_use_gp`,
  and adds it to a `spilled_gp` stack (a `std::vector` acting as a LIFO).
- `restore(reg)` finds `reg` in `spilled_gp` (by index), emits `pop reg`, removes it
  from `spilled_gp`, and re-adds it to `in_use_gp`.
- `restore()` (no argument) restores the most-recently-spilled register (LIFO order),
  matching the natural `push`/`pop` stack discipline.
- Spilling more than one register at once (`spill(count)`) is supported via a
  convenience overload.

### Internal State Changes

```cpp
// LIFO stack of GP register indices that have been push'd onto the hardware stack.
std::vector<int> spill_stack_gp_;
```

### Proposed API

```cpp
// Spill a single register: emits push reg; marks it unavailable.
// Throws if reg is not in_use or if cg_ is null.
void spill(const Reg64 &reg);

// Spill 'count' registers chosen from in_use_gp, excluding those in 'exclude'.
// Returns the list of spilled registers (in spill order) for use with restore().
// Throws if count > in_use_gp.size().
std::vector<Reg64> spill(int count,
                         const std::vector<Reg64> &exclude = {});

// Restore the most-recently-spilled register.
// Emits pop into the most recently spilled reg; re-adds it to in_use_gp.
Reg64 restore();

// Restore a specific register (must be in spill_stack_gp_).
// Emits pop directly into reg.
// Note: restoring out of LIFO order requires a temporary register; see Notes.
void restore(const Reg64 &reg);

// Restore all registers from the spill list returned by spill(count).
// Order is automatically reversed to match the push sequence.
void restore(const std::vector<Reg64> &spilled_regs);
```

### Usage Example — Basic Single Spill

```cpp
class MyKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
public:
    MyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(this) {

        // Allocate all immediately available volatile registers (SysV: 9 total).
        auto r0  = alloc<Reg64>(); // rax
        auto r1  = alloc<Reg64>(); // rcx
        auto r2  = alloc<Reg64>(); // rdx
        // ... up to r8 ...

        // free_gp_regs is now empty. Need one more temporary register.
        // Spill r0 (push rax): frees rax for reuse.
        spill(r0);  // emits: push rax

        auto r_tmp = alloc<Reg64>(); // gets rax
        // use r_tmp for whatever was needed ...
        free(r_tmp);

        // Restore r0 (pop rax): rax has its previous value back.
        restore(r0);  // emits: pop rax

        // Continue using r0 ...
        free(r0);
        free(r1);
        free(r2);
        // ...
    }
};
```

### Usage Example — Multi-Spill

```cpp
// Need 3 more registers but volatile pool is exhausted.
auto spilled = spill(3, /*exclude=*/{r_important});  // emits 3 x push

auto t0 = alloc<Reg64>();
auto t1 = alloc<Reg64>();
auto t2 = alloc<Reg64>();

// use t0, t1, t2 ...

free(t0);
free(t1);
free(t2);

restore(spilled);  // emits 3 x pop in reverse order
```

### Notes

- Spilling/restoring out of LIFO order is not directly supported because x86 `pop`
  always targets the top of the stack. To restore out of order a temporary register
  must be used as an intermediary, which quickly becomes complex. LIFO ordering should
  be enforced and documented clearly.
- Stack alignment: each `push` decrements `rsp` by 8. The caller is responsible for
  ensuring the stack remains 16-byte aligned before any `call` instruction. The
  `StackFrame` helper (§8) integrates with this.
- Only `Reg64` is spill-able. `Xmm`/`Ymm`/`Zmm` registers require `vmovdqu`/`vmovaps`
  for save/restore and are addressed separately in the `StackFrame` helper (§8).

> **REVIEW NOTE §7a / §7b — GP-Only Limitation and `StackFrame` Interaction**
>
> 1. `spill()` works only for `Reg64`. SIMD-heavy kernels exhaust ZMM registers before
>    GP registers. `StackFrame` handles vector saves but with a completely different flow
>    (alloc frame first, explicit offsets, different ownership semantics). There is no
>    unified “park this register temporarily” API that spans all register families. A
>    developer cannot use `spill()` for the register type they most commonly run out of.
>    Consider extending `spill()` to Vec registers, or removing it in favour of a unified
>    approach.
>
> 2. `spill()` and an active `StackFrame` are silently incompatible. `spill()` emits
>    `push reg`, decrementing `rsp` by 8. Every open `StackFrame`’s slot addresses
>    (`[rsp + offset]`) then shift by 8 relative to the values stored at frame
>    construction time. `check_offset()` cannot detect this — it has no knowledge of
>    pushes that happen after the frame is opened. The API provides no guard against
>    interleaving the two. Until this interaction is resolved, users must not call
>    `spill()` while any `StackFrame` is active.

---

## 8. Stack Frame Management: `StackFrame` RAII Helper

### Motivation

Complex JIT kernels (e.g. brgemm, matmul post-ops) require long-lived values parked in
the stack and read back via explicit offsets from `rsp`. Currently the developer must
manually emit `sub rsp, N` / `mov [rsp+off], reg` / `mov reg, [rsp+off]` / `add rsp, N`
with no assistance from the manager. This is both error-prone and tightly coupled to
manual offset arithmetic.

**Prerequisite:** §6 (code emission coupling) must be in place.

### Design

A `StackFrame` is an RAII object returned by `make_stack_frame(size)`. Its constructor
emits `sub rsp, size` and its destructor emits `add rsp, size`. The frame exposes
`put_on_stack()` / `read_from_stack()` that emit `mov [rsp+off], reg` and
`mov reg, [rsp+off]` respectively, and call `rm_.free()` / `rm_.alloc()` on the
underlying register accordingly.

All offset arithmetic is the caller's responsibility. The frame validates that offsets remain within the allocated space.

### Internal State Changes (on `RegPoolManager`)

```cpp
// Track currently allocated stack frame sizes for validation.
ptrdiff_t allocated_stack_space_ = 0;
```

### Register Type Coverage

The `StackFrame` must handle all allocatable register types. The correct instruction and
slot size varies by type:

| Register type | Slot size (bytes) | Store instruction | Load instruction |
|---|---|---|---|
| `Reg64` | 8 | `mov [rsp+off], reg64` | `mov reg64, [rsp+off]` |
| `Reg32` | 4 | `mov [rsp+off], reg32` | `mov reg32, [rsp+off]` |
| `Reg16` | 2 | `mov [rsp+off], reg16` | `mov reg16, [rsp+off]` |
| `Xmm` | 16 | `vmovdqu [rsp+off], xmm` | `vmovdqu xmm, [rsp+off]` |
| `Ymm` | 32 | `vmovdqu [rsp+off], ymm` | `vmovdqu ymm, [rsp+off]` |
| `Zmm` | 64 | `vmovdqu32 [rsp+off], zmm` | `vmovdqu32 zmm, [rsp+off]` |

**Key points:**

- **`Reg32` and `Reg16`**: These share the same physical register as their `Reg64`
  counterpart (tracked at the same index). You can write a 32-bit value to the stack
  and read it back into a 32-bit register. The slot only needs to be 4 or 2 bytes wide,
  but it is good practice to align slots to their natural size and pad to 8 bytes per
  slot for simplicity.
- **`Xmm` vs `Ymm` vs `Zmm`**: All three alias the same physical register (index-based
  tracking). A value stored with `Ymm` must be loaded back with `Ymm` (or `Zmm`), not
  `Xmm` — the upper lanes would be lost. The `read_from_stack` overload for vector
  registers returns the same width type that was stored by tracking the slot width via
  the offset metadata.
- **`vmovdqu` vs `vmovaps`**: `vmovdqu` handles unaligned accesses and is preferred
  unless the kernel guarantees 16/32/64-byte stack alignment at the slot. After a
  `sub rsp, N` the alignment relative to the original 16-byte aligned `rsp` is
  determined by `N mod 16`, so alignment is the caller's responsibility. The API uses
  `vmovdqu`/`vmovdqu32` by default for safety.
- **`Reg8`**: Not supported for stack storage. An 8-bit value that needs to be
  preserved should be zero-extended into a `Reg32` or `Reg64` before storing.

### Proposed API (Preferred: Template Form)

`put_on_stack` and `read_from_stack` are templated on the register type, exactly as
`alloc<T>()` and `free()` are in `RegPoolManager`. `reg_family<T>` handles pool
bookkeeping uniformly. Instruction selection uses `if constexpr` on the exact type
within private `emit_store<T>` / `emit_load<T>` helpers.

```cpp
// Inner RAII class that owns a stack frame.
class StackFrame {
public:
    // Emits: sub rsp, size
    StackFrame(RegPoolManager &rm, ptrdiff_t size);

    // Emits: add rsp, size
    ~StackFrame();

    // Move-only (same as Scoped<T>).
    StackFrame(const StackFrame &) = delete;
    StackFrame &operator=(const StackFrame &) = delete;
    StackFrame(StackFrame &&) noexcept;

    // Write a register's value to [rsp + offset] and free it from the allocator.
    // RegT may be: Reg64, Reg32, Reg16, Xmm, Ymm, Zmm.
    // Reg8 is not supported — zero-extend to Reg32 first.
    // Throws if offset + slot_size(RegT) > frame_size.
    template <class RegT>
    void put_on_stack(RegT &reg, ptrdiff_t offset) {
        emit_store(reg, offset);  // emits correct instruction for RegT
        rm_->free(reg);           // reg_family<RegT> dispatches correctly
    }

    // Overwrite an existing slot without freeing the register.
    template <class RegT>
    void put_on_stack(const RegT &reg, ptrdiff_t offset) {
        emit_store(reg, offset);
    }

    // Allocate a register of type RegT, load [rsp + offset] into it, and return it.
    // Caller is responsible for freeing the returned register.
    // Usage: auto r = frame.read_from_stack<Reg64>(off_src);
    //        auto z = frame.read_from_stack<Zmm>(off_zmm_const);
    template <class RegT>
    RegT read_from_stack(ptrdiff_t offset) {
        RegT reg = rm_->alloc<RegT>();
        emit_load(reg, offset);   // emits correct instruction for RegT
        return reg;
    }

    ptrdiff_t size() const { return size_; }

private:
    RegPoolManager *rm_;
    ptrdiff_t       size_;

    // Instruction dispatch — resolved at compile time via if constexpr.
    // Emitted instructions per type (see Register Type Coverage table above):
    template <class RegT>
    void emit_store(const RegT &reg, ptrdiff_t offset);

    template <class RegT>
    void emit_load(RegT &reg, ptrdiff_t offset);
};

// Factory method on RegPoolManager:
StackFrame make_stack_frame(ptrdiff_t size) {
    return StackFrame(*this, size);
}
```

The `emit_store` / `emit_load` private helpers contain the `if constexpr` branching,
keeping all complexity internal and leaving the public API surface clean:

```cpp
template <class RegT>
void StackFrame::emit_store(const RegT &reg, ptrdiff_t offset) {
    auto rsp = Xbyak::Reg64(4);
    if constexpr (std::is_same_v<RegT, Reg64> || std::is_same_v<RegT, Reg32>
               || std::is_same_v<RegT, Reg16>) {
        rm_->cg_->mov(rm_->cg_->ptr[rsp + offset], reg);
    } else if constexpr (std::is_same_v<RegT, Xmm> || std::is_same_v<RegT, Ymm>) {
        rm_->cg_->vmovdqu(rm_->cg_->ptr[rsp + offset], reg);
    } else if constexpr (std::is_same_v<RegT, Zmm>) {
        rm_->cg_->vmovdqu32(rm_->cg_->ptr[rsp + offset], reg);
    } else {
        static_assert(false, "Unsupported register type for put_on_stack");
    }
}
```

### Usage Example — GP Registers (Reg64 and Reg32)

```cpp
class MyKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
public:
    MyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(this) {

        // Stack layout: two 64-bit pointers + one 32-bit loop bound.
        // Pad the 32-bit slot to 8 bytes for alignment simplicity.
        constexpr ptrdiff_t off_src_ptr  = 0;  // 8 bytes: src base address (Reg64)
        constexpr ptrdiff_t off_dst_ptr  = 8;  // 8 bytes: dst base address (Reg64)
        constexpr ptrdiff_t off_loop_end = 16; // 8 bytes: Reg32 value, padded to 8
        constexpr ptrdiff_t stack_size   = 24;

        // Emits: sub rsp, 24
        auto frame = make_stack_frame(stack_size);

        Reg64 rdi_arg = Reg64(7); // SysV ABI arg 0

        auto r_src = alloc<Reg64>();
        mov(r_src, ptr[rdi_arg + offsetof(kernel_args_t, src)]);
        frame.put_on_stack(r_src, off_src_ptr); // emits: mov [rsp+0], r_src; frees r_src

        auto r_dst = alloc<Reg64>();
        mov(r_dst, ptr[rdi_arg + offsetof(kernel_args_t, dst)]);
        frame.put_on_stack(r_dst, off_dst_ptr); // emits: mov [rsp+8], r_dst; frees r_dst

        auto r_len = alloc<Reg32>();
        mov(r_len, ptr[rdi_arg + offsetof(kernel_args_t, length)]);
        frame.put_on_stack(r_len, off_loop_end); // emits: mov [rsp+16], r_len; frees r_len

        // --- kernel body ---
        auto r_tmp_src = frame.read_from_stack<Reg64>(off_src_ptr); // emits: mov r_tmp, [rsp+0]
        add(r_tmp_src, 64);
        free(r_tmp_src);

        auto r_tmp_len = frame.read_from_stack<Reg32>(off_loop_end); // emits: mov r_tmp, [rsp+16]
        // use r_tmp_len for loop control ...
        free(r_tmp_len);

        // frame destructor: emits add rsp, 24
        ret();
    }
};
```

### Usage Example — Vector Registers (Ymm and Zmm)

```cpp
class MyKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
public:
    MyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(this) {

        // Stack layout: ZMM slot first (largest, 64 bytes), then YMM (32 bytes).
        // Total: 96 bytes (multiple of 16, no padding needed).
        constexpr ptrdiff_t off_zmm_const = 0;  // 64 bytes
        constexpr ptrdiff_t off_ymm_bias  = 64; // 32 bytes
        constexpr ptrdiff_t stack_size    = 96;

        auto frame = make_stack_frame(stack_size); // emits: sub rsp, 96

        auto zmm_scale = alloc<Zmm>();
        vbroadcastss(zmm_scale, ptr[rdi]);
        frame.put_on_stack(zmm_scale, off_zmm_const); // emits: vmovdqu32 [rsp+0], zmm; frees zmm_scale

        auto ymm_bias = alloc<Ymm>();
        vmovdqu(ymm_bias, ptr[rdi + 8]);
        frame.put_on_stack(ymm_bias, off_ymm_bias);   // emits: vmovdqu [rsp+64], ymm; frees ymm_bias

        // --- inner loop ---
        auto zmm_tmp = frame.read_from_stack<Zmm>(off_zmm_const); // emits: vmovdqu32 zmm, [rsp+0]
        // use zmm_tmp ...
        free(zmm_tmp);

        auto ymm_tmp = frame.read_from_stack<Ymm>(off_ymm_bias);  // emits: vmovdqu ymm, [rsp+64]
        // use ymm_tmp ...
        free(ymm_tmp);

        // frame destructor: emits add rsp, 96
        ret();
    }
};
```

### Usage Example — XMM Save/Restore (Windows ABI)

```cpp
// Windows-only: preserve xmm6 and xmm7 (callee-saved).
constexpr ptrdiff_t off_xmm6_save = 0;  // 16 bytes
constexpr ptrdiff_t off_xmm7_save = 16; // 16 bytes
constexpr ptrdiff_t stack_size    = 32;

auto frame = make_stack_frame(stack_size); // emits: sub rsp, 32

auto xmm6 = alloc<Xmm>(6);
frame.put_on_stack(xmm6, off_xmm6_save); // emits: vmovdqu [rsp+0], xmm6; frees xmm6

auto xmm7 = alloc<Xmm>(7);
frame.put_on_stack(xmm7, off_xmm7_save); // emits: vmovdqu [rsp+16], xmm7; frees xmm7

// ... use xmm6/xmm7 freely ...

auto xmm6_r = frame.read_from_stack<Xmm>(off_xmm6_save); // emits: vmovdqu xmm6, [rsp+0]
auto xmm7_r = frame.read_from_stack<Xmm>(off_xmm7_save); // emits: vmovdqu xmm7, [rsp+16]
free(xmm6_r);
free(xmm7_r);
// frame dtor: emits add rsp, 32
```

### Alternate API (Not Recommended): Named Per-Width Methods

This variant replaces the template `read_from_stack<T>` with explicitly named methods.
It avoids `if constexpr` in the implementation but produces an API that is inconsistent
with `alloc<T>()` / `free()` and forces the caller to use a different naming convention
for each width. It is documented here only for completeness.

```cpp
// put_on_stack is still overloaded but takes concrete types rather than a template.
void put_on_stack(Reg64 &reg, ptrdiff_t offset);
void put_on_stack(Reg32 &reg, ptrdiff_t offset);
void put_on_stack(Reg16 &reg, ptrdiff_t offset);
void put_on_stack(const Xmm &reg, ptrdiff_t offset);
void put_on_stack(const Ymm &reg, ptrdiff_t offset);
void put_on_stack(const Zmm &reg, ptrdiff_t offset);

// read_from_stack is split into named methods because return types differ.
Reg64 read_from_stack_gp64(ptrdiff_t offset);
Reg32 read_from_stack_gp32(ptrdiff_t offset);
Reg16 read_from_stack_gp16(ptrdiff_t offset);
Xmm   read_from_stack_xmm(ptrdiff_t offset);
Ymm   read_from_stack_ymm(ptrdiff_t offset);
Zmm   read_from_stack_zmm(ptrdiff_t offset);
```

Caller code with the alternate API:

```cpp
// Alternate (not recommended):
auto r_tmp = frame.read_from_stack_gp64(off_src_ptr);  // verbose, inconsistent suffix
auto z_tmp = frame.read_from_stack_zmm(off_zmm_const); // different suffix per family
```

vs. the preferred template form:

```cpp
// Preferred:
auto r_tmp = frame.read_from_stack<Reg64>(off_src_ptr); // consistent with alloc<Reg64>()
auto z_tmp = frame.read_from_stack<Zmm>(off_zmm_const); // consistent with alloc<Zmm>()
```

### Notes

- The `StackFrame` destructor emitting `add rsp, N` only fires if the `StackFrame`
  object itself goes out of scope. In kernels where the frame is a structural member,
  the developer must ensure `~StackFrame()` runs before `ret`.
- Stack alignment: `size` should be rounded up to the next multiple of 16 to maintain
  16-byte alignment. The constructor can do this automatically (with a warning if the
  raw size is not already aligned).
- The `make_stack_frame()` factory should throw (or assert) if `cg_` is `nullptr`.

> **REVIEW NOTE §8a — `put_on_stack(RegT &reg)` Destroys the Register Variable**
>
> The non-const overload `put_on_stack(RegT &reg, ptrdiff_t offset)` emits the store
> **and** calls `rm_.free(reg)`. After the call the developer’s `reg` variable refers to
> a freed register. When the value is needed back, `read_from_stack<T>(offset)` allocates
> *whatever next register is free* — which is not guaranteed to be the same index.
> The stale variable and the newly allocated register share the same C++ name, making the
> error silent. Either remove the free-on-store entirely and let the caller call `free()`
> explicitly, or rename it to `put_on_stack_and_free()` so the destructive intent is
> obvious at the call site.

> **NOTE — Vec-Width Aliasing Is Correctly Handled**
>
> `Xmm`, `Ymm`, and `Zmm` all share `reg_family<T>::value == RegFamily::Vec` and all
> dispatch through `vec_reg(idx)`, which tracks all widths via the single `in_use_vec`
> set. Calling `alloc<Xmm>(5)` followed by `alloc<Ymm>(5)` correctly throws
> `ERR_RM_VEC_IN_USE` — the same physical register cannot be allocated twice under
> different width names. This is the expected behaviour, matching GP aliasing where
> `eax` and `rax` share index 0 and are mutually exclusive.

---

## 9. Preserved Register Tracking: `emit_prologue()` / `emit_epilogue()`

### Motivation

When `alloc()` exhausts the volatile (caller-saved) pool and promotes a register from
`preserved_gp`, it silently marks that register as in-use — but emits **no code** to
save its original value. The JIT kernel is now ABI-incorrect: the function clobbers a
callee-saved register without saving it. The caller's value in e.g. `rbx` is corrupted.

Tracking which preserved registers were consumed and auto-generating the correct
`push`/`pop` pairs fixes this with minimal developer effort.

### Design

`alloc()` already moves preserved registers from `preserved_gp` into `in_use_gp`.
We add a parallel `allocated_preserved_gp_` set that records each such promotion.
`emit_prologue()` then iterates this set (sorted ascending by index) and emits one
`push` per entry. `emit_epilogue()` emits the matching `pop`s in reverse order.

The developer calls `emit_prologue()` near the start of the JIT function body (before
any use of preserved registers) and `emit_epilogue()` just before `ret`.

### Internal State Changes

```cpp
std::vector<int> allocated_preserved_gp_;   // ordered by allocation time for correct pop order
std::vector<int> allocated_preserved_vec_;  // for Windows vMM save/restore
```

The internal `gp_reg()` helper that currently just calls
`in_use_gp.insert(idx); preserved_gp.erase(pres_it)` would additionally append to
`allocated_preserved_gp_` when the register came from `preserved_gp`.

### Proposed API

```cpp
// Emit push for each callee-saved GP that has been alloc()'d.
// Call this once near the top of the JIT function body.
// Throws if cg_ is null.
void emit_prologue();

// Emit pop for each callee-saved GP in reverse allocation order.
// Call this once just before ret.
void emit_epilogue();

// Query which preserved registers have been promoted:
std::vector<int> get_allocated_preserved_gps() const;
std::vector<int> get_allocated_preserved_vecs() const; // Windows: XMM save slots
```

### Usage Example

```cpp
class MyKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
public:
    MyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(this) {

        // Emit push for any callee-saved registers we are about to use.
        // This is empty if we only use volatile registers.
        emit_prologue(); // e.g. emits: push rbx (if rbx was alloc'd)

        auto r1 = alloc<Reg64>(); // volatile: e.g. rax
        auto r2 = alloc<Reg64>(); // volatile: e.g. rcx
        // ... fill all 9 volatile SysV registers ...
        auto r_extra = alloc<Reg64>(); // causes alloc from preserved_gp -> rbx
                                       // emit_prologue must have run already, or
                                       // re-run emit_prologue here (see Note)

        // ... kernel body ...

        free(r1);
        // ... free others ...
        free(r_extra);

        // Emit pop in reverse order of push:
        emit_epilogue(); // emits: pop rbx

        ret();
    }
};
```

### Notes

- The cleanest pattern is a **two-pass design**: on the first pass (a "dry run"), call
  `alloc()` for all registers you intend to use without emitting code. Inspect
  `get_allocated_preserved_gps()` to know what to save. On the second pass, call
  `emit_prologue()` first, then emit the actual kernel. This requires the kernel to
  be structured as two separate methods.
- A simpler one-pass alternative: call `emit_prologue()` last (after all `alloc()`
  calls are done) but then patch it into the code stream at a known label at the top.
  This matches how OneDNN kernels already use `postamble()` + label patching.
- `emit_prologue()` should be idempotent / safe to call multiple times (emit only the
  newly-promoted registers since the last call).

> **REVIEW NOTE §9a — Prologue Timing Cannot Be Enforced**
>
> `alloc()` silently promotes a preserved register from `preserved_gp` to `in_use_gp`
> without emitting any code. If any JIT instruction writes to that register *before*
> `emit_prologue()` is called, the function is already ABI-incorrect — the caller’s
> saved value is overwritten with no error or warning. The Notes above document a
> two-pass workaround, but the fundamental problem — that the API cannot detect or
> enforce the required ordering — remains. A debug-build assertion that fires if a
> preserved register is promoted by `alloc()` before `emit_prologue()` has ever been
> called would surface these errors during development.
>
> **§9a — REJECTED:** The assertion fires on promotion, not on the first code write to
> the register. It cannot distinguish the correct pattern (alloc-then-emit_prologue)
> from the buggy pattern (alloc-write-then-emit_prologue), so it enforces a stylistic
> rule without protecting against the actual bug. The natural alloc-all-then-emit_prologue
> pattern is valid and is broken by this assertion.

---

## 10. End-of-JIT Validation: `assert_all_free()`

### Motivation

If a developer accidentally omits a `free()` call there is currently no detection
mechanism. This is a logic error in the kernel author's bookkeeping, not a runtime
correctness problem — the emitted machine code bytes are unaffected and the kernel
is perfectly executable. However the unintended imbalance in the manager's tracking
sets can mask later bugs (e.g. a subsequent `alloc()` in the same manager instance may
return a register the developer believes is free but which is still silently tracked
as in-use).

`assert_all_free()` is therefore a **developer correctness aid for debug builds only**.
It has no place in release builds and carries no implication about whether the emitted
JIT code is valid or runnable.

### Design

`assert_all_free()` checks that `in_use_gp`, `in_use_vec`, and `in_use_opmask` are all
empty after excluding any pinned registers (§5). It fires `assert()` in debug builds
with a diagnostic message listing the un-freed register indices. In release builds it
compiles to nothing — no flag, no return value, no side effects.

No new data members are required.

### Proposed API

```cpp
// Debug-only check: assert that all non-pinned registers have been freed.
// Fires assert() in debug builds listing any still-in-use register indices.
// Compiles to nothing (no-op) in release builds.
void assert_all_free() const;
```

A companion check for the spill stack (§7) can be added alongside it:

```cpp
// Debug-only check: assert that no registers are currently spilled.
// A non-empty spill stack at the end of JIT construction indicates a missing
// restore() call — this IS a real correctness problem (unbalanced push/pop).
void assert_spill_stack_empty() const;
```

Note the intentional distinction:
- **`assert_all_free()`** — bookkeeping hygiene, debug only, JIT code is still valid.
- **`assert_spill_stack_empty()`** — genuine ABI bug, the hardware stack is unbalanced.

### Usage Example

```cpp
class MyKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
public:
    MyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(this) {

        auto r1 = alloc<Reg64>();
        auto r2 = alloc<Reg64>();

        // ... kernel body ...

        free(r1);
        // BUG: forgot free(r2) — the kernel still runs correctly, but
        // assert_all_free() will catch the bookkeeping error during development.

        ret();

        // Debug build: assertion fires, listing r2's index as un-freed.
        // Release build: compiled out entirely.
        assert_all_free();
        assert_spill_stack_empty(); // also verify no dangling spills
    }
};
```

---

## 11. Error Handling: Align with Xbyak's `XBYAK_NO_EXCEPTION` Model

### Motivation

Currently every error path in `RegPoolManager` throws `std::runtime_error`. This is
inconsistent with the error handling model already built into Xbyak itself. Xbyak
provides two compile-time modes controlled by the `XBYAK_NO_EXCEPTION` macro:

**Exception mode (default — `XBYAK_NO_EXCEPTION` not defined):**
```cpp
// Xbyak throws a typed exception carrying an ErrorList enum value.
#define XBYAK_THROW(err)        { throw Xbyak::Error(err); }
#define XBYAK_THROW_RET(err, r) { throw Xbyak::Error(err); }
```

**No-exception mode (`XBYAK_NO_EXCEPTION` defined):**
```cpp
// Xbyak records the first error in a TLS int and returns silently.
// The caller polls with Xbyak::GetError() and clears with Xbyak::ClearError().
#define XBYAK_THROW(err)        { Xbyak::local::SetError(err); return; }
#define XBYAK_THROW_RET(err, r) { Xbyak::local::SetError(err); return r; }
```

`RegPoolManager` currently ignores this compiled-in choice and always throws
`std::runtime_error`. This means:
- In `XBYAK_NO_EXCEPTION` builds the manager throws when Xbyak itself never would,
  surprising callers who expect error-code polling.
- The manager uses a different exception type (`std::runtime_error`) from Xbyak's own
  typed `Xbyak::Error`, making it harder to write a single catch handler.

### Design

Replace `std::runtime_error` throws and the `ErrorMode` enum entirely. Instead:

1. **Extend `Xbyak::ErrorList`** with new register-manager-specific error codes so that
   register manager errors are first-class `Xbyak::Error` values.
2. **Use `XBYAK_THROW` / `XBYAK_THROW_RET`** for all fatal error paths so that the
   manager automatically inherits whichever mode was compiled in.
3. **Use `reg_warn()`** (debug stderr only) for non-fatal warnings, unchanged.

This gives zero overhead in `XBYAK_NO_EXCEPTION` builds and typed, catchable
`Xbyak::Error` exceptions in normal builds — exactly matching what Xbyak already does.

### Proposed Error Codes

Add the following entries to `Xbyak::ErrorList` immediately before `ERR_INTERNAL`:

```cpp
// Register manager errors (add before ERR_INTERNAL):
ERR_RM_GP_IN_USE,          // alloc'd a GP register that is already in use
ERR_RM_GP_NOT_IN_USE,      // freed a GP register that is not in use
ERR_RM_GP_NOT_AVAILABLE,   // requested GP register not in free/preserved pool
ERR_RM_NO_FREE_GP,         // no free GP registers available
ERR_RM_VEC_IN_USE,         // same for Vec family
ERR_RM_VEC_NOT_IN_USE,
ERR_RM_VEC_NOT_AVAILABLE,
ERR_RM_NO_FREE_VEC,
ERR_RM_OPMASK_IN_USE,      // same for Opmask family
ERR_RM_OPMASK_NOT_IN_USE,
ERR_RM_OPMASK_NOT_AVAILABLE,
ERR_RM_NO_FREE_OPMASK,
ERR_RM_REG_IDX_OUT_OF_RANGE,  // index outside valid range for family
ERR_RM_REG_ALREADY_TRACKED,   // add_to_gp_pool: register already in a pool
ERR_RM_SCOPED_REG_NOT_IN_USE, // makeScoped called on a register not in use
ERR_RM_SPILL_NOT_IN_USE,      // spill called on a register not in use
ERR_RM_NO_CG,                 // spill/restore/StackFrame called with no CodeGenerator
ERR_RM_STACK_FRAME_OFFSET_OOB,// put_on_stack offset outside allocated frame
```

Add matching strings to `ConvertErrorToString`'s `errTbl` array.

### Proposed API Change

No new public API methods on `RegPoolManager`. Remove `ErrorMode`, the constructor
parameter, and `set_error_mode()`. Replace all `throw std::runtime_error(...)` calls
with `XBYAK_THROW(ERR_RM_*)` or `XBYAK_THROW_RET(ERR_RM_*, return_val)`.

The `reg_warn()` helper is unchanged (debug stderr, non-fatal):

```cpp
// Non-fatal warning — output only in debug builds, never stops execution.
void reg_warn(const char *msg) const {
#ifndef NDEBUG
    std::fprintf(stderr, "[RegPoolManager WARNING] %s\n", msg);
#endif
}
```

Internal fatal error helper (private, replaces all `throw std::runtime_error`):

```cpp
// Use XBYAK_THROW so the calling method can return void (no [[noreturn]] needed
// in XBYAK_NO_EXCEPTION mode where XBYAK_THROW does a plain return).
// Example usage inside RegPoolManager methods:
void gp_reg(int idx) {
    if (reg_in_use_idx(idx, RegFamily::GP))
        XBYAK_THROW(ERR_RM_GP_IN_USE)  // no semicolon — macro ends with }
    // ...
}
```

For methods that return a value, use `XBYAK_THROW_RET`:

```cpp
int next_gp_idx() const {
    if (!free_gp_regs.empty()) return *free_gp_regs.begin();
    if (!preserved_gp.empty()) return *preserved_gp.begin();
    XBYAK_THROW_RET(ERR_RM_NO_FREE_GP, 0)  // returns 0 in no-exception mode
}
```

### No New Data Members

The `XBYAK_NO_EXCEPTION` path uses Xbyak's existing TLS `local::GetErrorRef()`.
No new members are needed on `RegPoolManager`.

### Usage Example — Exception Mode (default)

```cpp
// Catch register manager errors alongside Xbyak's own errors using one handler.
try {
    MyKernel kernel;
} catch (const Xbyak::Error &e) {
    // e is an int-like value; ConvertErrorToString(e) gives a readable message.
    std::fprintf(stderr, "JIT error: %s\n", Xbyak::ConvertErrorToString(e));
}
```

### Usage Example — No-Exception Mode (`XBYAK_NO_EXCEPTION` defined)

```cpp
// Build with -DXBYAK_NO_EXCEPTION.
// Both Xbyak and the register manager now use error-code polling.

Xbyak::ClearError();

MyKernel kernel;  // constructor runs; any error is stored in TLS

if (int err = Xbyak::GetError()) {
    std::fprintf(stderr, "JIT error: %s\n", Xbyak::ConvertErrorToString(err));
    Xbyak::ClearError();
    // handle error ...
}
```

### Why Not the Custom `ErrorMode` Enum?

The original `ErrorMode` proposal provided three modes (kThrow, kAssert, kSilent)
selected at construction time. Xbyak's model is simpler and better:

| Concern | `ErrorMode` approach | Xbyak approach |
|---|---|---||
| Mode selection | Runtime constructor parameter | Compile-time macro (`XBYAK_NO_EXCEPTION`) |
| Error identity | Free-form string | Typed `ErrorList` enum, human-readable via `ConvertErrorToString` |
| Catch granularity | Can't distinguish error types | Caller can switch on `(int)err` |
| Integration | Separate from Xbyak errors | Single `catch (Xbyak::Error)` catches both |
| No-exception builds | Needs `kSilent` mode explicitly | Automatic via `XBYAK_THROW` macro |
| Existing code compat | Adds new constructor signature | Drop-in; all `XBYAK_THROW` sites unchanged |

The `kAssert` mode (assert in debug, silently ignore in release) is intentionally not
replicated here. If an assert-on-error behaviour is needed for oneDNN integration, the
caller can wrap the kernel constructor in a thin debug-mode helper that calls
`assert(Xbyak::GetError() == 0)` after construction.

---

## 12. Named-Register `alloc()` Overload

### Motivation

The existing `alloc<Reg64>(int idx)` overload requires callers to know the internal
Xbyak integer encoding for each register (`rdx` = 2, `rsi` = 6, `rdi` = 7, etc.).
These indices are not part of everyday x86-64 programming vocabulary — developers
think and read documentation in terms of register *names*, not encoding numbers.
A typo like `alloc<Reg64>(6)` vs. `alloc<Reg64>(7)` silently allocates the wrong
register with no compiler warning.

Xbyak already provides named register objects (`rax`, `rcx`, `rdx`, `xmm3`, `ymm4`,
`k2`, etc.) as `const` members of `CodeGenerator`. Every register object exposes its
index via `.getIdx()`. Adding an `alloc(const RegT &reg)` overload that accepts one
of these objects directly eliminates the magic number entirely, at zero runtime cost.

**Relation to §2 (bitmask pools):** Because the overload immediately calls
`alloc<RegT>(reg.getIdx())` and the internal representation remains `uint32_t`
bitmasks, there is no conflict. The named-register overload is pure syntactic sugar
layered on top of the existing index-based path.

### Proposed API

A single template overload is sufficient. `RegT` is deduced from the argument, so
no explicit template parameter is ever needed at the call site:

```cpp
// Allocate a specific register identified by its Xbyak name.
// RegT is deduced — no explicit template argument required.
// Equivalent to alloc<RegT>(reg.getIdx()) but reads like familiar register names.
// Supported types: Reg64, Reg32, Reg16, Xmm, Ymm, Zmm, Opmask.
template <class RegT>
RegT alloc(const RegT &reg) {
    return alloc<RegT>(reg.getIdx());
}
```

No new data members. No internal state changes. The existing `alloc<RegT>(int idx)`
method (and all its error handling, pool logic, and §2 bitmask internals) is unchanged.

### Register Name to Index Mapping

For reference, the x86-64 hardware encoding that Xbyak uses:

| Name | Index | Name | Index |
|---|---|---|---|
| `rax` | 0 | `r8` | 8 |
| `rcx` | 1 | `r9` | 9 |
| `rdx` | 2 | `r10` | 10 |
| `rbx` | 3 | `r11` | 11 |
| `rsp` | 4 | `r12` | 12 |
| `rbp` | 5 | `r13` | 13 |
| `rsi` | 6 | `r14` | 14 |
| `rdi` | 7 | `r15` | 15 |

Vector and opmask registers follow a straightforward sequential numbering: `xmm0`/`ymm0`/`zmm0` = 0,
`xmm1`/`ymm1`/`zmm1` = 1, ..., `zmm31` = 31; `k1` = 1, ..., `k7` = 7.

### Usage Example

```cpp
class MyKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
public:
    MyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(this) {

        // Before: requires knowing that rdx=2 and rsi=6 in Xbyak's encoding.
        // auto p_src  = alloc<Reg64>(2);  // magic number — is this rdx or rcx?
        // auto p_dst  = alloc<Reg64>(6);  // magic number — rsi

        // After: register names match documentation and assembly listings directly.
        auto p_src  = alloc(rdx);   // clearly rdx; deduces Reg64 automatically
        auto p_dst  = alloc(rsi);   // clearly rsi
        auto loop_k = alloc(rcx);   // rcx is the conventional LOOP counter register

        // Vector and opmask registers work identically:
        auto scale  = alloc(zmm0);  // pin zmm0 as the scale vector
        auto mask   = alloc(k1);    // k1 as the predicate mask

        // The returned objects are the same Xbyak register types as always.
        // Existing usage is unchanged:
        mov(p_src, ptr[rdi + offsetof(kernel_args_t, src)]);
        mov(p_dst, ptr[rdi + offsetof(kernel_args_t, dst)]);
        xor_(loop_k, loop_k);

        free(p_src);
        free(p_dst);
        free(loop_k);
        free(scale);
        free(mask);

        ret();
    }
};
```

### Notes

- The overload requires that the named register is currently free (in `free_*` or
  `preserved_*` pool). If it is already in use, `XBYAK_THROW(ERR_RM_*_NOT_AVAILABLE)`
  fires just as it does for `alloc<RegT>(int idx)` — the error handling path is shared.
- Within a `CodeGenerator` subclass `rax`, `rcx`, `rdx` etc. resolve to `this->rax`
  etc. (the `const Reg64` members inherited from `CodeGenerator`). Outside a
  `CodeGenerator` subclass the fully-qualified forms `Xbyak::util::reg_table` names
  or locally-constructed `Reg64(2)` objects can be passed instead.
- `Reg32` / `Reg16` variants work by passing `eax` / `ax` etc. which are
  `Reg32` / `Reg16` objects in Xbyak, so `RegT` is deduced to the correct width.
  The pool lookup uses the same index (e.g. `edx.getIdx() == 2`, same physical
  register as `rdx`).
- This overload does **not** conflict with the existing `alloc<RegT>()` (no-argument)
  or `alloc<RegT>(int idx)` overloads. Overload resolution selects correctly based
  on whether an argument is present and whether it is an `int` or a register object.

---

## 13. Remove `used_*` Sets

### Motivation

Three data members accumulate every register index that passes through `alloc()`:

```cpp
std::set<int> used_gp;      // grows on every gp_reg() call, never cleared
std::set<int> used_vec;     // grows on every vec_reg() call, never cleared
std::set<int> used_opmask;  // grows on every opmask_reg() call, never cleared
```

And the corresponding public getters:

```cpp
std::vector<int> get_used_gps()     const;
std::vector<int> get_used_vecs()    const;
std::vector<int> get_used_opmasks() const;
```

These sets have two compounding problems that make them both misleading and useless.

**Problem 1 — The name implies "currently in use" but `in_use_*` already does that.**
`in_use_gp` correctly tracks which registers are currently allocated. `used_gp` tracks
registers that have *ever* been handed out by `alloc()` and never removes entries when
`free()` is called. The semantic gap between the name and the behaviour is a silent
source of confusion.

**Problem 2 — The only plausible use case (prologue generation) fails.**
A developer might reach for `get_used_gps()` to determine which callee-saved registers
need to be preserved in the function prologue. This does not work:

- A volatile register (e.g. `rax`) that was `alloc()`d and `free()`d during
  initialisation code appears in `used_gp` permanently — but `rax` is caller-saved
  and needs no prologue save at all.
- `used_gp` is only complete *after* all allocations have happened, but the prologue
  must be emitted *before* any preserved registers are actually used.
- `allocated_preserved_gp_` from §9 correctly records only the callee-saved registers
  that were promoted from `preserved_gp`, providing exactly the right information for
  `emit_prologue()` / `emit_epilogue()`. `used_*` is therefore fully superseded.

**Problem 3 — The constructor pre-populates `used_*` with registers that were never
allocated.** At construction, `rsp` (4) and `rbp` (5) are inserted into `used_gp`, and
`k0` (0) is inserted into `used_opmask`. These registers are excluded from the pool
because they have fixed architectural roles — they were never "used" in any meaningful
sense. This pre-population further corrupts any attempt to use `used_*` for real
bookkeeping.

### Proposed Change

Remove the three data members and their public getters entirely. Remove all
`used_*.insert(idx)` calls from `gp_reg()`, `vec_reg()`, `opmask_reg()`, and the
constructor.

**Data members removed:**
```cpp
std::set<int> used_gp;      // deleted
std::set<int> used_vec;     // deleted
std::set<int> used_opmask;  // deleted
```

**Public methods removed:**
```cpp
std::vector<int> get_used_gps()     const;  // deleted
std::vector<int> get_used_vecs()    const;  // deleted
std::vector<int> get_used_opmasks() const;  // deleted
```

This is a **breaking public API change**. Any callers of `get_used_*()` must be
updated. The replacement for the prologue use case is `get_allocated_preserved_gps()`
from §9, which provides the correct and complete information.

### Interaction with §2

The `used_gp_mask_` bitmask proposed in §2 is dropped along with `used_gp`. The §2
bitmask representation needs only three masks per family — `free_*`, `preserved_*`,
and `in_use_*` — not four. This is already reflected in the §2 code snippet.

### Usage Example — Migration

```cpp
// Before: using used_* for prologue decisions (incorrect but valid attempt)
void my_kernel_prologue() {
    for (int idx : rm.get_used_gps()) {
        // BUG: includes volatile regs (rax etc.) and registers already freed;
        //      also incomplete if called before all allocs have been made.
        if (is_callee_saved(idx)) push(Reg64(idx));
    }
}

// After: use get_allocated_preserved_gps() from §9 (correct)
void my_kernel_prologue() {
    // Returns only callee-saved registers that were actually promoted by alloc().
    // Information is complete because emit_prologue() knows the full allocation plan.
    emit_prologue();  // handles push sequence correctly and completely
}
```

---

## 14. In-Use Volatile / Preserved Getters

### Motivation

`get_in_use_gps()` returns all currently allocated registers regardless of whether
they are volatile (caller-saved) or preserved (callee-saved). When a JIT kernel needs
to call out to an external function (e.g. a library routine), the developer must
manually filter this list against their knowledge of the ABI to determine which
registers are at risk. This is error-prone and duplicates logic that the manager
already implicitly holds.

Two complementary getters clarify the distinction:

- **`get_in_use_volatile_gps()`** — registers that are currently allocated *and*
  caller-saved. The JIT kernel **must** push these before a `call` instruction if their
  values are needed after the call returns, because the callee is free to clobber them.
- **`get_in_use_preserved_gps()`** — registers that are currently allocated *and*
  callee-saved. The callee will save and restore these, so the kernel does **not** need
  to push them before a `call`. (This also provides a fast path: if this set is empty,
  no function prologue is needed.)

### Distinction from §9's `get_allocated_preserved_gps()`

These getters are complementary to, not a replacement for, `get_allocated_preserved_gps()`
from §9.

| Method | Returns | Primary use case |
|---|---|---|
| `get_allocated_preserved_gps()` | Ordered list of preserved registers promoted by `alloc()` | Prologue/epilogue generation — order matters for LIFO push/pop pairing |
| `get_in_use_preserved_gps()` | Unordered snapshot of currently in-use callee-saved registers | Introspection — "do I need a prologue at all?", optimisation decisions |
| `get_in_use_volatile_gps()` | Unordered snapshot of currently in-use caller-saved registers | Saving live registers before a `call` to an external function |

### Interaction with §2 (Bitmask Pools)

The implementation requires knowing which register indices are originally volatile vs
preserved. With the §2 bitmask representation this is trivial: capture two `const`
bitmasks at construction time recording the initial pool membership, then intersect
with `in_use_gp_mask_` to answer each query in a single bitwise AND.

### Internal State Changes

```cpp
// Const masks captured once at construction; never modified afterwards.
const uint32_t initial_volatile_gp_mask_;   // indices originally in free_gp_regs
const uint32_t initial_preserved_gp_mask_;  // indices originally in preserved_gp
// Same pair for vec and opmask families.
const uint32_t initial_volatile_vec_mask_;
const uint32_t initial_preserved_vec_mask_;
const uint32_t initial_volatile_opmask_mask_;
const uint32_t initial_preserved_opmask_mask_;
```

With the current `std::set`-based representation (pre-§2), the equivalent is two
`const std::set<int>` copies of the initial pools captured in the constructor
initialiser list.

### Proposed API

```cpp
// GP family
std::vector<int> get_in_use_volatile_gps()   const; // in_use ∩ initial_volatile
std::vector<int> get_in_use_preserved_gps()  const; // in_use ∩ initial_preserved

// Vec family
std::vector<int> get_in_use_volatile_vecs()  const;
std::vector<int> get_in_use_preserved_vecs() const;

// Opmask family
std::vector<int> get_in_use_volatile_opmasks()  const;
std::vector<int> get_in_use_preserved_opmasks() const;
```

With §2 bitmasks the implementation of each getter is O(k) where k is the number
of set bits (number of currently allocated registers of that class), and the
classification is a single `&`:

```cpp
std::vector<int> get_in_use_volatile_gps() const {
    return bits_to_vector(in_use_gp_mask_ & initial_volatile_gp_mask_);
}
std::vector<int> get_in_use_preserved_gps() const {
    return bits_to_vector(in_use_gp_mask_ & initial_preserved_gp_mask_);
}
```

### Usage Example — Saving Caller-Saved Registers Before a `call`

```cpp
class MyKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
public:
    MyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(this) {

        auto r1 = alloc<Reg64>(); // volatile: e.g. rax
        auto r2 = alloc<Reg64>(); // volatile: e.g. rcx
        auto r3 = alloc<Reg64>(); // may be preserved: e.g. rbx
        auto r4 = alloc<Reg64>(); // may be preserved: e.g. r12

        // --- need to call an external C function ---

        // Only push registers that the callee is free to clobber.
        // Preserved registers (rbx, r12 etc.) will be saved by the callee — no push needed.
        auto volatile_live = get_in_use_volatile_gps();
        for (int idx : volatile_live)
            push(Reg64(idx));  // save only what is actually at risk

        // Optional fast check: if no preserved registers are in use we can skip
        // the prologue entirely in a dynamically-generated kernel variant.
        if (get_in_use_preserved_gps().empty()) {
            // No callee-saved regs touched — prologue is a no-op.
        }

        // ... set up call arguments ...
        call(reinterpret_cast<void*>(some_c_function));

        // Restore in reverse order.
        for (auto it = volatile_live.rbegin(); it != volatile_live.rend(); ++it)
            pop(Reg64(*it));

        free(r1);
        free(r2);
        free(r3);
        free(r4);
        ret();
    }
};
```

### Notes

- The SysV x86-64 ABI designates `rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r8`–`r11` as
  volatile (caller-saved). `rbx`, `rbp`, `r12`–`r15` are preserved (callee-saved).
  On Windows, `rcx`, `rdx`, `r8`, `r9` are volatile and `rdi`, `rsi`, `rbx`, `rbp`,
  `r12`–`r15` are preserved. The initial pool contents in `RegPoolManager` already
  encode this ABI split — the const masks just capture it once at construction.
- For the Vec family on SysV Linux, all XMM/YMM/ZMM registers are volatile. On
  Windows, `xmm6`–`xmm15` are callee-saved. `get_in_use_preserved_vecs()` is
  therefore a no-op on Linux SysV but meaningful on Windows.
- These getters return the *current in-use snapshot*, not a historical record.
  If a volatile register was `alloc()`d and then `free()`d before this call, it
  will not appear in the result — which is the correct behaviour (its value is no
  longer live).

> **REVIEW NOTE §14a / §14b / §14c / §14d — Redundant Getters and Helpers to Remove**
>
> Several methods are redundant by construction and should be removed:
>
> - `get_in_use_volatile_opmasks()` is always identical to `get_in_use_opmasks()` —
>   all opmask registers are volatile on every supported platform. One should be removed.
> - `get_in_use_volatile_tiles()` is always identical to `get_in_use_tiles()` — same
>   reasoning. One should be removed.
> - `gp_idx_in_use(idx)`, `vec_idx_in_use(idx)`, `opmask_idx_in_use(idx)`,
>   `tile_idx_in_use(idx)` duplicate `reg_in_use(reg)`. A caller who has only an index
>   can construct `Reg64(idx)` at zero cost. The four per-family index variants add
>   API surface without adding capability. Remove them in favour of `reg_in_use(reg)`.
>
> After §13 removed the `used_*` sets, three special-register helpers now trivially
> return constant values and carry no semantic weight:
>
> - `_stack_pointer()` returns `Reg64(4)`.
> - `_base_pointer()` returns `Reg64(5)`.
> - `_opmask_k0()` returns `Opmask(0)`.
>
> Any developer using this manager knows these indices. These helpers should be removed.
> If discoverable names are still desired, `static constexpr int kRspIdx = 4` style
> constants are preferable to zero-body methods.

---

## 15. ABI Configuration: Windows x64 vs SysV

> **REJECTED — Do not implement.**
>
> The `#ifdef _WIN32` guards already present in the header are both correct and
> sufficient. JIT kernels are always called by the surrounding C++ code that was
> compiled for the same host ABI, so the ABI of the JIT target and the host
> binary are always identical — a runtime `Abi` parameter cannot diverge from
> the compile-time `#ifdef` in any supported use case. Replacing compile-time
> constants with a runtime enum member adds API surface, an `abi_` data member,
> a `populate_pools()` refactor, and five `if (abi_ == kWin64)` branches for
> no practical gain. The `#ifdef` approach is simpler, zero-overhead, and
> impossible to misconfigure.

### Motivation

The constructor currently hardcodes the SysV x86-64 ABI register classification into
the initial pool composition — volatile GP registers (`rax`, `rcx`, `rdx`, `rsi`,
`rdi`, `r8`–`r11`) go into `free_gp_regs`; callee-saved registers (`rbx`, `rbp`,
`r12`–`r15`) go into `preserved_gp`. All vector registers go into `free_vec_regs`
because SysV does not preserve any SIMD registers.

This is wrong for **Windows x64**, which has a materially different calling convention:

| Register class | SysV volatile | Win64 volatile | SysV preserved | Win64 preserved |
|---|---|---|---|---|
| GP | rax,rcx,rdx,rsi,rdi,r8–r11 | rax,rcx,rdx,r8–r11 | rbx,rbp,r12–r15 | rbx,rbp,**rdi,rsi**,r12–r15 |
| XMM | all (0–15) | xmm0–xmm5 | none | **xmm6–xmm15** |
| YMM/ZMM upper | all | ymm0–ymm5 (lower 128 match) | none | ymm6-15 lower (xmm6-15 rule) |

Because §9 (`emit_prologue`/`emit_epilogue`) and §14 (`get_in_use_volatile_*` /
`get_in_use_preserved_*`) derive their answers entirely from the initial pool
classification, a single hardcoded layout makes both features incorrect on Windows.

### Design

Add an `Abi` enum and make it a constructor parameter with `kSysV` as the default.
No other public API changes are required — every ABI-sensitive feature (§9, §14)
already reads from the initial pool contents.

### Proposed API

```cpp
enum class Abi {
    kSysV,   // Linux / macOS x86-64 System V ABI (default)
    kWin64,  // Windows x64 ABI
};

// Abi parameter added to constructors (default = kSysV for backward compatibility):
explicit RegPoolManager(Xbyak::CodeGenerator *cg = nullptr,
                        Abi abi = Abi::kSysV);

explicit RegPoolManager(const Xbyak::util::Cpu &cpu,  // see §17
                        Xbyak::CodeGenerator *cg = nullptr,
                        Abi abi = Abi::kSysV);

// Query the configured ABI:
Abi abi() const { return abi_; }
```

### Internal State Changes

```cpp
Abi abi_ = Abi::kSysV;  // stored so reset() (§16) can re-apply the same ABI
```

The constructor pool-population block becomes ABI-conditional:

```cpp
void populate_pools(Abi abi) {
    // GP volatile pool
    const std::initializer_list<int> sysv_volatile_gp  = {0,1,2,6,7,8,9,10,11};
    const std::initializer_list<int> win64_volatile_gp = {0,1,2,8,9,10,11};
    const std::initializer_list<int> sysv_preserved_gp = {3,12,13,14,15};  // rsp(4),rbp(5) excluded
    const std::initializer_list<int> win64_preserved_gp = {3,6,7,12,13,14,15}; // rdi,rsi added

    auto &vgp = (abi == Abi::kWin64) ? win64_volatile_gp : sysv_volatile_gp;
    auto &pgp = (abi == Abi::kWin64) ? win64_preserved_gp : sysv_preserved_gp;
    for (int idx : vgp) free_gp_regs.insert(idx);
    for (int idx : pgp) preserved_gp.insert(idx);

    // Vec: SysV — all volatile; Win64 — xmm0-5 volatile, xmm6-15 preserved
    for (int i = 0; i <= max_vec_reg_idx_; ++i) {
        if (abi == Abi::kWin64 && i >= 6 && i <= 15)
            preserved_vec.insert(i);
        else
            free_vec_regs.insert(i);
    }
    // Opmask: same on both ABIs
    for (int i = 1; i <= 7; ++i) free_opmask_regs.insert(i);
}
```

### Usage Example

```cpp
// Linux kernel — SysV default, no change needed:
class LinuxKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
    LinuxKernel() : Xbyak::CodeGenerator(4096),
                    Xbyak::RegPoolManager(this /* , Abi::kSysV implicit */) {}
};

// Windows kernel — Win64 ABI:
class WindowsKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
    WindowsKernel() : Xbyak::CodeGenerator(4096),
                      Xbyak::RegPoolManager(this, Abi::kWin64) {
        // preserved_vec now pre-populated with xmm6-xmm15.
        // emit_prologue() will save them if any are alloc()'d.
        // get_in_use_preserved_vecs() correctly identifies xmm6-xmm15.
        emit_prologue();
        // ... kernel body ...
        emit_epilogue();
        ret();
    }
};
```

### Notes

- `Abi::kSysV` is the default, so all existing code compiles unchanged.
- On Windows, `xmm6`–`xmm15` are callee-saved for their lower 128 bits. The full
  `ymm`/`zmm` upper lanes are Intel's responsibility if the kernel uses `vzeroupper`.
  The manager's responsibility is only the 128-bit save/restore handled by §9 +
  `StackFrame` (§8).
- The `Abi` enum belongs in `namespace Xbyak` alongside `RegPoolManager`.

---

## 16. Manager `reset()` to Complement `CodeGenerator::reset()`

### Motivation

`Xbyak::CodeGenerator` provides a `reset()` method that resets the write pointer to
the start of the code buffer, allowing a new kernel to be emitted into the same
memory. Some kernel generation patterns — variant selection, A/B emission, or kernel
families that share a single statically-allocated `CodeGenerator` — call `reset()` to
re-emit code without constructing a new object.

The register manager has no equivalent. After a `reset()` call on the associated
`CodeGenerator`, the manager still thinks the old allocation state is current —
pinned registers from the previous emission remain pinned, `allocated_preserved_gp_`
still holds the previous kernel's promotion history, and `spill_stack_gp_` may be
non-empty. The developer must construct a new `RegPoolManager`, losing the associated
`cg_` pointer and capability flags.

`reset()` restores the manager to the state it had immediately after construction,
preserving only the static properties: `cg_`, `abi_`, `has_apx_`, `has_avx512_`,
`max_gp_reg_idx_`, `max_vec_reg_idx_`, and the initial pool masks — everything that
depends on hardware and configuration rather than on the emission in progress.

### Proposed API

```cpp
// Reset all allocation state to the post-construction baseline.
// The code generator pointer, ABI, and ISA capability flags are preserved.
// After this call the manager is ready to track a new code generation pass.
void reset();
```

### What `reset()` Clears

```cpp
void RegPoolManager::reset() {
    // Restore pools to initial composition (re-run populate_pools):
    free_gp_regs.clear(); in_use_gp.clear(); preserved_gp.clear();
    free_vec_regs.clear(); in_use_vec.clear(); preserved_vec.clear();
    free_opmask_regs.clear(); in_use_opmask.clear(); preserved_opmask.clear();
    populate_pools(abi_);  // resets free/preserved from ABI (§15)

    // Clear all additions from §4, §5, §7, §9:
    reserved_gp.clear();  reserved_vec.clear();  reserved_opmask.clear();
    pinned_gp.clear();    pinned_vec.clear();    pinned_opmask.clear();
    spill_stack_gp_.clear();
    allocated_preserved_gp_.clear();
    allocated_preserved_vec_.clear();
    allocated_stack_space_ = 0;

    // NOT reset: cg_, abi_, has_apx_, has_avx512_,
    //            max_gp_reg_idx_, max_vec_reg_idx_,
    //            initial_volatile_*_mask_, initial_preserved_*_mask_
}
```

### Usage Example

```cpp
class KernelFamily : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
public:
    KernelFamily() : Xbyak::CodeGenerator(65536), Xbyak::RegPoolManager(this) {}

    void emit_variant_a() {
        reset();                    // reset CodeGenerator write pointer
        RegPoolManager::reset();    // reset allocation tracking
        emit_prologue();
        auto r = alloc<Reg64>();
        // ... emit variant A ...
        free(r);
        emit_epilogue();
        ret();
    }

    void emit_variant_b() {
        reset();
        RegPoolManager::reset();
        emit_prologue();
        auto r1 = alloc<Reg64>();
        auto r2 = alloc<Reg64>();
        // ... emit variant B (uses more registers) ...
        free(r1); free(r2);
        emit_epilogue();
        ret();
    }
};
```

### Notes

- The method is named `reset()` to mirror `CodeGenerator::reset()` exactly. Since
  `RegPoolManager` and `CodeGenerator` are separate classes (even when used together
  via multiple inheritance), there is no name clash — `reset()` on the manager and
  `CodeGenerator::reset()` are distinct calls.
- Calling `RegPoolManager::reset()` without also calling `CodeGenerator::reset()` is
  valid in cases where only the tracking state needs to be refreshed (e.g. running
  `assert_all_free()` after a dry-run pass and then clearing for the real pass).

---

## 17. Accept External `Xbyak::util::Cpu` Reference

### Motivation

The `RegPoolManager` constructor currently instantiates a local `Xbyak::util::Cpu`
object to query CPUID for the AVX-512 and APX capability flags:

```cpp
RegPoolManager::RegPoolManager(...) {
    Xbyak::util::Cpu cpu;   // queries CPUID here — a serializing instruction
    // ...
}
```

In oneDNN and similar projects, the kernel class already holds or has access to a
`Cpu` object constructed earlier in the dispatch path. The manager's internal
construction therefore runs CPUID a second time. `CPUID` is a serializing instruction
with a latency of 100–1000 cycles; more importantly it serialises the pipeline.
For kernels instantiated on the hot path this is a measurable cost.

The fix is to add a constructor overload that accepts a `const Xbyak::util::Cpu &`
reference and skips internal CPUID detection entirely.

### Proposed API

```cpp
// New overload: caller provides an existing Cpu object.
// No CPUID instruction is run inside RegPoolManager.
explicit RegPoolManager(const Xbyak::util::Cpu &cpu,
                        Xbyak::CodeGenerator *cg = nullptr,
                        Abi abi = Abi::kSysV);

// Existing constructor unchanged — still works for standalone use:
explicit RegPoolManager(Xbyak::CodeGenerator *cg = nullptr,
                        Abi abi = Abi::kSysV);  // constructs internal Cpu
```

Both constructors converge on the same private helper:

```cpp
void init_from_cpu(const Xbyak::util::Cpu &cpu, Abi abi) {
    if (cpu.has(Xbyak::util::Cpu::tOSXSAVE)) {
        uint64_t xcr0 = cpu.getXfeature();
        has_apx_    = ((xcr0 >> 19) & 1) == 1;   // §1 precedence fix applied
        has_avx512_ = ((xcr0 >> 7)  & 1) == 1;
    }
    if (cpu.has(Xbyak::util::Cpu::tAPX_F))   max_gp_reg_idx_  = has_apx_    ? 31 : 15;
    if (cpu.has(Xbyak::util::Cpu::tAVX512F)) max_vec_reg_idx_ = has_avx512_ ? 31 : 15;
    populate_pools(abi);
}
```

### Internal State Changes

None beyond what §15 already adds (`abi_`). No new data members.

### Usage Example

```cpp
// oneDNN-style kernel dispatch — Cpu is already available:
class MyDispatcher {
    Xbyak::util::Cpu cpu_;

    void dispatch() {
        if (cpu_.has(Xbyak::util::Cpu::tAVX512F)) {
            // Pass cpu_ to RegPoolManager — no second CPUID query.
            MyAvx512Kernel kernel(cpu_);
            kernel.run();
        }
    }
};

class MyAvx512Kernel : public Xbyak::CodeGenerator,
                       public Xbyak::RegPoolManager {
public:
    explicit MyAvx512Kernel(const Xbyak::util::Cpu &cpu)
        : Xbyak::CodeGenerator(4096)
        , Xbyak::RegPoolManager(cpu, this, Abi::kSysV) {
        // AVX-512 and APX detection used cpu directly — CPUID run only once.
        emit_prologue();
        auto zmm = alloc<Zmm>();
        // ...
        free(zmm);
        emit_epilogue();
        ret();
    }
};
```

### Notes

- The `const Xbyak::util::Cpu &` overload is an additive change with no impact on
  existing code. The existing no-argument constructor continues to run CPUID
  internally and remains the correct choice for standalone usage outside a larger
  dispatch context.
- This overload is the natural companion for §15's `Abi` parameter: callers that
  already have a `Cpu` are also most likely to need a specific ABI configuration.

---

## 18. `emit_call()` — ABI-correct Outgoing Calls (Shadow Space + Alignment)

### Motivation

When JIT code calls a real C function, two platform-specific requirements must be
satisfied at the `call` instruction site:

**Windows x64 (Win64 ABI):**
1. **32-byte shadow space** — the caller must subtract 32 from `rsp` before any
   `call` instruction, unconditionally, even if the callee takes no arguments.
   This space is for the callee's own use (e.g. home area for register arguments)
   and must be provided by every caller. Omitting it causes the callee to corrupt
   whatever happens to live just below the caller's stack frame.
2. **16-byte stack alignment** — `rsp` must be 16-byte aligned *at* the `call`
   instruction (i.e. `rsp % 16 == 0` before `call` pushes the return address,
   so the callee receives `rsp % 16 == 8`). If the total number of `push`
   instructions since JIT function entry is even, an extra 8-byte pad is needed
   before the shadow space allocation.

The combined adjustment is:

```
total_pushes = all push instructions from JIT entry to call site
needs_pad    = (total_pushes % 2) == 0   // true when rsp would be misaligned
adj          = 32 + (needs_pad ? 8 : 0)  // 32 shadow + optional 8-byte pad

sub rsp, adj
call target
add rsp, adj
```

**Linux / macOS (SysV AMD64 ABI):**

SysV has **no shadow space** requirement — the callee owns only what it allocates
itself. However the 16-byte alignment rule still applies: `rsp % 16 == 0` at the
`call` instruction. The adjustment is therefore just:

```
needs_pad = (total_pushes % 2) == 0
adj       = needs_pad ? 8 : 0   // only align; no shadow space

// if adj > 0: sub rsp, adj / call target / add rsp, adj
// if adj == 0: call target  (already aligned)
```

In practice many SysV JIT kernels that call leaf functions already happen to be
aligned and need no adjustment at all. The requirement is real but the common case
is zero cost.

Without this abstraction, callers must reason about push-count parity and
platform differences every time they emit a `call`. Both the `dynamicSaveRestore`
and `volatileGPCallerSave` tests in `test/reg_manager_test.cpp` currently contain
`#ifdef _WIN32` blocks that manually compute and apply this adjustment — exactly
the kind of boilerplate that `emit_call()` eliminates.

**Prerequisite:** §6 (`set_code_generator()`) must be in place so the manager has
a `CodeGenerator*` with which to emit `sub`/`add rsp` instructions.

### Design

The manager tracks the number of `push`-equivalent stack moves it has observed
since function entry or the last `reset()`. This count is incremented by:

- `emit_prologue()` (§9) — one per callee-saved GP pushed
- `spill()` (§7) — one per register spilled
- `StackFrame` construction (§8) — equivalent to `size / 8` slots (fractional
  pushes are accounted for in the alignment calculation as bytes, not push units)

For the common case where the caller also pushes registers manually before calling
`emit_call()`, an optional `extra_pushes` parameter allows the caller to declare
any additional pushes the manager has not seen.

### Proposed API

```cpp
// Emit an ABI-correct call to a runtime C function.
//
// func_ptr     -- address of the target function.
// extra_pushes -- number of manual push instructions the caller emitted that
//                 the manager has not tracked (default: 0).  Used to compute
//                 the correct alignment and shadow space adjustment.
//
// Near-call path (target within +-2 GB, or auto-grow mode):
//   [sub rsp, adj]     ; only if alignment pad / shadow space needed
//   call rel32         ; direct relative call -- no register consumed
//   [add rsp, adj]
//
// Far-call path (target > 2 GB away, fixed-size buffer only):
//   [sub rsp, adj]
//   mov  rax, func_ptr   ; rax holds the target address for the indirect call
//   call rax
//   [add rsp, adj]
//
// Returns: nothing.  The caller reads the return value from rax as usual.
//
// Throws RmError::NO_CG if cg_ is null.
void emit_call(uint64_t func_ptr, size_t extra_pushes = 0);

// Convenience overload: accept a typed function pointer directly.
template <typename FuncT>
void emit_call(FuncT *func_ptr, size_t extra_pushes = 0) {
    emit_call(reinterpret_cast<uint64_t>(func_ptr), extra_pushes);
}
```

### Internal State Changes

```cpp
// Running count of push-equivalent 8-byte slots emitted by the manager
// (incremented by emit_prologue, spill, and StackFrame construction).
// Used by emit_call() to compute alignment.
size_t managed_push_count_ = 0;  // added alongside §7 / §9 state
```

### Usage Example — Before and After

**Before `emit_call()` (current state in tests):**

```cpp
// Save volatile registers.
auto vol = rm.get_in_use_volatile_gps();
for (int idx : vol) push(Reg64(idx));          // manual push, count = vol.size()

#ifdef _WIN32
// Must manually compute shadow space + alignment pad.
size_t total_pushes = 1 + vol.size();          // 1 = earlier push(rbx)
bool needs_pad = (total_pushes % 2) == 0;
int adj = needs_pad ? 40 : 32;
sub(rsp, adj);
#endif
mov(rax, reinterpret_cast<uint64_t>(&my_func));
call(rax);
#ifdef _WIN32
add(rsp, adj);
#endif

for (auto it = vol.rbegin(); it != vol.rend(); ++it) pop(Reg64(*it));
```

**After `emit_call()` (target state once §6 + §18 are implemented):**

```cpp
// Save volatile registers.
auto vol = rm.get_in_use_volatile_gps();
for (int idx : vol) push(Reg64(idx));

// emit_call handles shadow space, alignment, and load — on all platforms.
rm.emit_call(&my_func, /*extra_pushes=*/1 + vol.size());
// or equivalently, if the manager tracks all pushes itself via emit_prologue/spill:
// rm.emit_call(&my_func);

for (auto it = vol.rbegin(); it != vol.rend(); ++it) pop(Reg64(*it));
```

### Removing the `#ifdef _WIN32` Blocks From Tests

Once `emit_call()` is implemented, the manual `#ifdef _WIN32` adjustment blocks in
`test/reg_manager_test.cpp` must be removed and replaced with `emit_call()` calls.
The affected tests are:

- `dynamicSaveRestore` — `gen_caller_saves_all()` contains an `#ifdef _WIN32`
  block that computes `adj = needs_pad ? 40 : 32` and emits `sub`/`add rsp`.
- `volatileGPCallerSave` — `OptimalCallerJit::gen()` contains an equivalent
  `#ifdef _WIN32` block.

Both blocks are intentional workarounds for the missing `emit_call()` abstraction
and are marked with comments referencing this proposal item. Deleting them in favour
of `emit_call()` is the acceptance criterion for completing §18.

### Notes

- The near-call path emits `call rel32` (the same encoding compilers produce for
  `call my_func`) -- no register is consumed and `rax` is left untouched.
  The far-call path (target > 2 GB from the JIT buffer, fixed-size buffers only)
  loads `func_ptr` into `rax` then calls it; `rax` is volatile and always clobbered
  by the return value in both SysV and Win64, so this is always safe.
- If §9 (`emit_prologue`) is in use, the manager already knows how many callee-save
  pushes it emitted; `managed_push_count_` tracks this. The `extra_pushes` parameter
  covers any additional manual pushes the caller emits outside of manager control
  (e.g. `push(rbx)` to stash a return value).
- On SysV, the most common call sites after `emit_prologue()` will have
  `extra_pushes == 0` and an odd `managed_push_count_`, meaning no adjustment is
  needed at all and `emit_call()` reduces to a plain `call rel32` with no `sub`/`add`.

> **REVIEW NOTES §18a / §18b**
>
> **§18a -- Debug assertion for rax liveness (REJECTED -- false positive):**
> Proposed: add a debug-build assertion that `rax` is not currently live when
> `emit_call()` is invoked, since both ABIs guarantee the return value clobbers
> `rax`. Attempted. Fires a false positive in `dynamicSaveRestore`, where `rax`
> is validly allocated by the manager, pushed to the stack inside the caller's
> save loop, then `emit_call()` is invoked. The manager cannot distinguish
> "live and unsaved (bug)" from "live and saved on the stack (valid)" because
> `extra_pushes` is a count, not a list of which registers were pushed.
> Assertion removed.
>
> **§18b -- `extra_pushes` deprecation (REJECTED):**
>    `extra_pushes` cannot be deprecated because raw `CodeGenerator::push()` calls are
>    always invisible to the manager. `extra_pushes` is a permanent escape hatch. Normal
>    usage should route all pushes through `spill()`, `emit_prologue()`, or `save_volatiles()`
>    and pass `extra_pushes=0`. The `extra_pushes` doc comment has been updated to reflect this.

---

## Summary Table

| # | Change | API Impact | New Data Members | Requires §6 |
|---|---|---|---|---|
| 1 | XCR0 precedence fix | None (bug fix) | None | No |
| 2 | Bitmask pools | None (internal) | `uint32_t` bitmasks | No | *(REJECTED)*
| 3 | `add_to_vec/opmask_pool()` | 4 new methods | None | No |
| 4 | `mark_unavailable/available()` | 4 new methods | 3 `reserved_*` sets | No |
| 5 | `pin()` / `alloc_pinned()` | 6 new methods | 3 `pinned_*` sets | No |
| 6 | `set_code_generator()` | Constructor change + 1 method | `cg_` pointer | — |
| 7 | `spill()` / `restore()` | 5 new methods | `spill_stack_gp_` vector | Yes |
| 8 | `StackFrame` RAII | New inner class + 1 factory | `allocated_stack_space_` | Yes |
| 9 | `emit_prologue/epilogue()` | 3 new methods | 2 `allocated_preserved_*` vectors | Yes |
| 10 | `assert_all_free()` | 2 new methods (debug-only) | None | No |
| 11 | Align errors with `XBYAK_THROW` / `Xbyak::Error` | New `ERR_RM_*` enum values + `ConvertErrorToString` strings | None | No |
| 12 | Named-register `alloc(reg)` overload | 1 new template overload | None | No |
| 13 | Remove `used_*` sets | 4 getters removed (breaking) | 4 sets removed | No |
| 14 | In-use volatile/preserved getters | 6 new methods | 6 `const` initial masks | No |
| 15 | ABI configuration (Windows / SysV) | Constructor parameter | `abi_` enum member | No | *(REJECTED)* |
| 16 | Manager `reset()` | 1 new method | None | No |
| 17 | External `Cpu` constructor overload | 1 new constructor overload | None | No |
| 18 | `emit_call()` — ABI-correct outgoing calls | 1 new method (+ template overload) | `managed_push_count_` | Yes (§6) |
| 19 | Stack integrity checks | 4 new methods | None | No |
| 20 | `save_volatiles()` / `restore_volatiles()` | 2 new methods | `saved_volatile_gp_` vector | Yes |
| 21 | Pool count queries: `free_gp_count()` etc. | 5 new methods | None | No | *(REJECTED)*
| 22 | Rename `in_use` → `live` in getter names | API rename | None | No |

### Recommended Implementation Order

1. §1 (bug fix — zero risk, immediate value)
2. §11 (error handling — aligns XBYAK_THROW usage; needed by everything that follows)
3. §3 (pool parity — trivial, fills obvious gap) *(REJECTED — see §3)*
4. §4 (mark_unavailable — no code gen dependency)
5. §5 (pinned registers — no code gen dependency) *(REJECTED — see §5)*
6. §10 (`assert_all_free()` only — `assert_spill_stack_empty()` companion must wait until step 9 when `spill_stack_gp_` exists)
7. §6 (code generator coupling — prerequisite for §7, §8, §9)
8. §9 (prologue/epilogue tracking — can be done before spill; establishes `managed_push_count_`)
9. §18 (`emit_call()` — builds directly on §6 + §9; removes all `#ifdef _WIN32` shadow-space blocks from tests once implemented)
10. §7 (spill/restore — builds on §9 for alignment accounting; add `assert_spill_stack_empty()` companion here)
11. §8 (StackFrame — builds on §7 and §9)
11. §2 (bitmask optimisation — internal only, do last when API is stable) *(REJECTED — see §2)*
12. §12 (named-register alloc — pure convenience, no dependencies, add anytime)
13. §13 (remove `used_*` — breaking removal, do after §9 is in place so callers have a replacement)
14. §15 (ABI configuration) *(REJECTED — see §15)*
15. §17 (external `Cpu` overload)
16. §14 (in-use volatile/preserved getters)
17. §16 (manager `reset()`)
18. §20 (`save_volatiles()` / `restore_volatiles()` — depends on §6; eliminates `extra_pushes` footgun in §18)
19. §21 (pool count queries) *(REJECTED — see §21)*
20. §22 (rename `in_use` → `live` — breaking API rename; do after §14 getters are stabilised)

---

## 19. Stack Integrity Checks

### Motivation

`assert_all_free()` (§10) verifies that every register handed out by `alloc()` has been returned with `free()`. The stack has two independent sources of imbalance that it does not detect:

1. **Unrestored spills** — `spill()` pushes a register value onto the hardware stack. If a matching `restore()` is never called, `rsp` is permanently displaced and the `ret` instruction will pop the wrong address.
2. **Open `StackFrame` objects** — `make_stack_frame(N)` emits `sub rsp, N`. If the returned `StackFrame` is never destroyed (e.g. it outlives its intended scope), `add rsp, N` is never emitted and `rsp` is again incorrect at `ret`.

Both errors produce silent wrong-address returns rather than a compile error or an obvious fault, making them hard to diagnose.

### API

```cpp
// Returns true if no spill() calls are awaiting a matching restore().
bool spill_stack_empty() const;

// Debug-build assertion: triggers if any spill() is unmatched.
// Compiles to nothing in release builds.
void assert_spill_stack_empty() const;

// Returns true if both the spill stack is empty and no StackFrame is open.
bool clean_stack() const;

// Debug-build assertion: triggers if either condition above is violated.
// Compiles to nothing in release builds.
void assert_clean_stack() const;
```

### Usage

Call the assertions at the end of JIT kernel construction alongside `assert_all_free()`:

```cpp
rm.assert_all_free();      // no leaked registers
rm.assert_clean_stack();   // no unrestored spills, no open StackFrames
ret();
```

For programmatic checks (e.g. unit tests):

```cpp
ASSERT(rm.spill_stack_empty());
ASSERT(rm.clean_stack());
```

### Notes

- `clean_stack()` is a strict superset of `spill_stack_empty()`: `clean_stack()` returns `false` whenever `spill_stack_empty()` does, and also when an open `StackFrame` exists. Calling `assert_clean_stack()` is sufficient to catch both problems.
- No new data members are required. `spill_stack_gp_` (added in §7) and `allocated_stack_space_` (added in §8) already carry the necessary state.
- Neither check requires a `CodeGenerator` — the state is tracked purely by the manager.

---

## Discussion: `alloc` / `free` Naming

### The Question

The names `alloc` and `free` are borrowed from dynamic memory management. Registers are
not dynamic memory — they are a **fixed hardware pool** of 16–32 resources that exist
whether or not any code references them. The concern is that the memory-allocation
framing may create a misleading mental model, particularly for readers (or tools) that
associate `alloc`/`free` with heap allocation.

Two alternative pairs worth considering are `claim`/`release` and `obtain`/`release`.

### Recommendation: Keep `alloc` / `free`

For the target audience — developers writing Xbyak JIT kernels — `alloc`/`free` is the
right choice for the following reasons:

**The audience already has the correct mental model.** Anyone working with a JIT
register manager understands that hardware registers are a fixed resource. The name
`alloc` communicates *exclusive ownership from a pool*, which is precisely what
happens. The heap-allocation connotation does not apply because there is no heap, no
size argument, and no pointer arithmetic — the context disambiguates immediately.

**It is the established terminology in the field.** LLVM, GCC's register allocator, and
essentially every compiler infrastructure project uses "allocate"/"free" for register
management. Adopting different terminology introduces friction for developers familiar
with that literature.

**`free` is unambiguous at the call site.** `rm.free(r)` reads clearly as "I am done
with this register". Alternative verbs like `release` can carry connotations of
synchronisation primitives (`std::memory_order_release`, `std::mutex::unlock`),
which is a worse mismatch than the memory one.

### If a Rename Is Desired

Should a future style guide or API review require a change, the strongest candidate is
`claim` / `release`:

| Method | Current | Alternative |
|---|---|---|
| Obtain any free register | `alloc<T>()` | `claim<T>()` |
| Obtain a specific register | `alloc<T>(idx)` / `alloc(reg)` | `claim<T>(idx)` / `claim(reg)` |
| Return a register to the pool | `free(reg)` | `release(reg)` |
| Obtain and pin | `alloc_pinned<T>()` | `claim_pinned<T>()` |

`claim` implies staking ownership of one item from a constrained fixed set, with no
memory connotation. `release` as the counterpart is unambiguous in this context
(no mutex or memory-order association when paired with `claim`).

Other candidates considered and their drawbacks:

| Pair | Drawback |
|---|---|
| `obtain` / `release` | Correct but verbose; `obtain` is rarely used in C++ APIs |
| `acquire` / `release` | Direct clash with `std::memory_order_acquire/release` |
| `checkout` / `checkin` | Informal tone, unexpected in a low-level JIT API |
| `borrow` / `return` | `return` as a method name visually conflicts with the `return` keyword |

A rename is a significant API churn cost with marginal benefit for the primary
audience. It is worth revisiting only if the manager is ever exposed as a public API
in a broader library where the user base is less homogeneous.

---

## 20. Save/Restore Live Volatile Registers: `save_volatiles()` / `restore_volatiles()`

### Motivation

The most common operation before emitting a `call` instruction is saving all live
volatile registers so the callee cannot corrupt them. Currently the developer must:

1. Call `get_in_use_volatile_gps()` to get the live volatile list.
2. Manually emit a `push` for each entry.
3. Call `emit_call()` with an accurate `extra_pushes` count that includes those pushes.
4. Manually emit a `pop` for each entry in reverse order.

Steps 2 and 4 are boilerplate repeated in every kernel that makes an external call.
Step 3 requires an accurate count of pushes the manager did not observe — the primary
reason `emit_call()`'s `extra_pushes` parameter exists (see §18 review notes). A
`save_volatiles()` / `restore_volatiles()` pair eliminates all of this and allows
`emit_call()` to drop `extra_pushes` entirely.

**Prerequisite:** §6 (code emission coupling) must be in place.

### Design

- `save_volatiles()` calls `get_in_use_volatile_gps()`, emits `push` for each live
  volatile GP in a consistent order, increments `managed_push_count_` by the count,
  and records the pushed list internally so `restore_volatiles()` knows what to pop
  and in what order.
- `restore_volatiles()` emits `pop` for each saved register in reverse order and
  decrements `managed_push_count_`.
- Both operations go through the manager, so `managed_push_count_` is always accurate
  and `emit_call()` no longer needs `extra_pushes`.

### Internal State Changes

```cpp
// Registers saved by the most recent save_volatiles(), in push order.
// restore_volatiles() pops them in reverse.
std::vector<int> saved_volatile_gp_;
```

### Proposed API

```cpp
// Push all currently in-use volatile GP registers onto the hardware stack.
// Increments managed_push_count_ accordingly so emit_call() alignment is correct.
// Records the pushed registers so restore_volatiles() can reverse the sequence.
// Throws ERR_RM_NO_CG if no CodeGenerator is attached.
void save_volatiles();

// Pop the registers saved by the most recent save_volatiles(), in reverse order.
// Throws if called without a preceding save_volatiles(), or if no CodeGenerator
// is attached.
void restore_volatiles();
```

### Usage Example

```cpp
class MyKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
public:
    MyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(this) {
        emit_prologue();

        auto r_src = alloc<Reg64>();    // volatile: e.g. rdi
        auto r_cnt = alloc<Reg64>();    // volatile: e.g. rsi
        auto r_acc = alloc<Reg64>();    // volatile: e.g. rdx

        // --- need to call a C helper ---
        save_volatiles();               // emits: push rdi; push rsi; push rdx
                                        //        managed_push_count_ updated automatically

        // emit_call() needs no extra_pushes — the manager knows the full push count.
        emit_call(&some_c_helper);

        restore_volatiles();            // emits: pop rdx; pop rsi; pop rdi

        // r_src, r_cnt, r_acc are valid again.
        free(r_src);
        free(r_cnt);
        free(r_acc);

        emit_epilogue();
        ret();
    }
};
```

### Notes

- `save_volatiles()` and `restore_volatiles()` are strictly paired. Calling
  `restore_volatiles()` without a preceding `save_volatiles()` throws.
- Only GP volatile registers are saved. `Xmm`/`Ymm`/`Zmm` save/restore requires
  `StackFrame` (§8) due to the `vmovdqu` instruction requirement.
- Once `save_volatiles()` is implemented, the `extra_pushes` parameter on `emit_call()`
  should be deprecated. Any remaining manual pushes outside manager APIs indicate code
  that should be converted to use `save_volatiles()`.
- On SysV after `emit_prologue()`, the stack alignment state is already tracked by
  `managed_push_count_`. The combined `save_volatiles()` + `emit_call()` sequence is
  always correctly aligned with no extra input from the developer.

---

## 21. Pool Count Queries: `free_gp_count()`, `free_vec_count()`, etc.

> **REJECTED — Do not implement.**
>
> The only scenario where these methods provide value over the existing
> `get_free_vecs().size()` approach is a branch decision where a count is needed
> *without* immediately allocating — avoiding the transient `std::vector` heap
> allocation. In practice this is not a meaningful concern: JIT kernel constructors
> are called infrequently, and the allocation is O(n) on a set of at most 32
> elements. More importantly, the pattern that actually appears in real kernels is
> "check count, then allocate all available registers", which requires calling
> `get_free_vecs()` anyway to obtain the indices — making the count-only query
> redundant in the same call sequence. Adding five new methods (`free_gp_count()`,
> `preserved_gp_count()`, `free_vec_count()`, `free_opmask_count()`,
> `free_tile_count()`) for this marginal benefit is not justified. Callers should
> use `(int)rm.get_free_vecs().size()` and equivalents directly.
> This section is kept for reference only.

### Motivation

JIT kernel generators frequently need to size themselves based on how many registers
are available. The only current way to answer "how many free ZMM registers are there?"
is:

```cpp
int n = (int)rm.get_free_vecs().size();
```

This materialises a full `std::vector<int>` — a heap allocation — just to get a count.
In a JIT loop that sizes itself dynamically (e.g. "use as many ZMMs as available, at
least 4") this is called on every kernel instantiation. A `free_vec_count()` query
that returns an `int` directly is both cleaner at the call site and avoids the
allocation entirely.

### Proposed API

```cpp
// Returns the number of GP registers currently in the free (volatile) pool.
int free_gp_count()      const;

// Returns the number of GP registers currently in the preserved (callee-saved) pool.
int preserved_gp_count() const;

// Returns the number of Vec registers currently in the free pool.
int free_vec_count()     const;

// Returns the number of Opmask registers currently in the free pool.
int free_opmask_count()  const;

// Returns the number of Tile registers currently in the free pool.
int free_tile_count()    const;
```

With the current `std::set` representation each call is O(1) (`.size()` on a set).
After §2 (bitmask pools) each call becomes a single `__builtin_popcount()`.

No new data members are required.

### Usage Example

```cpp
// Dynamically size the kernel based on available ZMM registers.
const int n_zmm = rm.free_vec_count();
if (n_zmm < 4) {
    // Not enough vector registers for the vectorised path — fall back.
    return generate_scalar_kernel();
}

// Claim all available ZMM registers.
std::vector<Zmm> accumulators;
for (int i = 0; i < n_zmm; ++i)
    accumulators.push_back(rm.alloc<Zmm>());

// ... kernel body ...

for (auto &z : accumulators) rm.free(z);
```

### Notes

- These queries return the count at the moment of the call. The count decreases after
  each `alloc()` and increases after each `free()` — they are snapshots.
- `free_gp_count()` counts only the volatile (caller-saved) free pool. To include
  preserved registers that `alloc()` can also draw from, sum
  `free_gp_count() + preserved_gp_count()`.
- These methods intentionally do not materialise a vector, unlike `get_free_gps()`.
  The getter methods remain useful when the caller needs the actual indices, not just
  the count.

---

## 22. Rename `in_use` to `live` in Getter Names (IMPLEMENTED)

### Motivation

The current getter family uses the phrase `in_use` to describe registers that are
currently allocated (i.e. handed out by `alloc()` and not yet returned via `free()`):

```cpp
get_in_use_gps()
get_in_use_volatile_gps()
get_in_use_preserved_gps()
get_in_use_vecs()
get_in_use_volatile_vecs()
get_in_use_opmasks()
get_in_use_volatile_opmasks()   // (slated for removal — §14a)
get_in_use_tiles()
get_in_use_volatile_tiles()     // (slated for removal — §14b)
```

`in_use` is correct but verbose. In compiler and register-allocator literature the
standard term for a register that holds a value needed by future instructions is
**live**. A register is *live* at a program point if its current value may be read
before the next write. For the register manager, "currently allocated" and "live" are
equivalent: `alloc()` marks a register live; `free()` marks it dead.

Adopting `live` produces shorter, idiomatic names that align with standard terminology:

| Current name | Proposed name |
|---|---|
| `get_in_use_gps()` | `get_live_gps()` |
| `get_in_use_volatile_gps()` | `get_live_volatile_gps()` |
| `get_in_use_preserved_gps()` | `get_live_preserved_gps()` |
| `get_in_use_vecs()` | `get_live_vecs()` |
| `get_in_use_volatile_vecs()` | `get_live_volatile_vecs()` |
| `get_in_use_opmasks()` | `get_live_opmasks()` |
| `get_in_use_tiles()` | `get_live_tiles()` |

The methods slated for removal in §14 (`get_in_use_volatile_opmasks()`,
`get_in_use_volatile_tiles()`, and the per-family index helpers) need not be renamed —
they should be removed directly.

### Proposed API

No new methods are added. Existing `get_in_use_*` methods are renamed to
`get_live_*`. The signatures are otherwise unchanged:

```cpp
// Renamed from get_in_use_gps()
std::vector<int> get_live_gps() const;

// Renamed from get_in_use_volatile_gps()
std::vector<int> get_live_volatile_gps() const;

// Renamed from get_in_use_preserved_gps()
std::vector<int> get_live_preserved_gps() const;

// Renamed from get_in_use_vecs()
std::vector<int> get_live_vecs() const;

// Renamed from get_in_use_volatile_vecs()
std::vector<int> get_live_volatile_vecs() const;

// Renamed from get_in_use_opmasks()
std::vector<int> get_live_opmasks() const;

// Renamed from get_in_use_tiles()
std::vector<int> get_live_tiles() const;
```

### Internal State Changes

None. The underlying data members (`in_use_gp_`, `in_use_vec_`, etc.) should also be
renamed to `live_gp_`, `live_vec_`, etc. for consistency in the same commit.

### Usage Example

```cpp
// Before:
auto live = rm.get_in_use_volatile_gps();

// After:
auto live = rm.get_live_volatile_gps();
```

### Notes

- The rename is orthogonal to all other sections — it does not change behaviour,
  data members, or control flow.
- Perform the rename after §14 cleanups are complete so that only the methods that
  will survive long-term are renamed.
- Rename the internal data members (`in_use_gp_`, etc.) to `live_gp_`, etc. in the
  same commit for consistency.

---

## 23. Unified Stack Layout: `StackFrameBuilder` / `StackFrame` (Replaces §7 and §8)

### Motivation

The combination of `spill()`/`restore()` (§7) and `StackFrame` (§8) is the most
architecturally fragile part of the current design. Each was designed independently and
they are fundamentally incompatible:

- `spill()` emits `push reg`, which moves `rsp` downward by 8 bytes.
- `StackFrame` uses fixed `[rsp + offset]` addressing computed at frame-open time.
- Any `spill()` emitted after a `StackFrame` is opened silently shifts every slot
  address by 8 bytes, corrupting reads and writes without any diagnostic.

The §7 review note acknowledges this with: *"users must not call `spill()` while any
`StackFrame` is active"*. This is an unenforceable documentation contract that will
be violated.

The root cause is a mismatch between two models of stack management:

| Model | How rsp moves | Address calculation |
|---|---|---|  
| `spill()` / `push` | Every push shifts rsp by −8 | Implicit; pop restores by LIFO |
| `StackFrame` / fixed slots | Once, at frame open | Explicit: `[rsp + fixed_offset]` |

These models cannot coexist in the same function without drift-corrected offset
arithmetic that the user is expected to perform manually. Real compilers never mix
them: they choose one `sub rsp, total` at function entry and use fixed offsets
everywhere, or they use no local frame at all. This design should follow that model.

The proposed `StackFrameBuilder` / `StackFrame` pair replaces both §7 and §8 with a
two-phase design that eliminates the incompatibility by construction.

### Why This Is Better Than §7 (`spill()`/`restore()`)

1. **No silent address drift.** `spill()` shifts every open `StackFrame`'s slot
   addresses invisibly. `StackFrame` fixes all offsets at `build()` and never
   moves rsp again until `destroy()`. There is nothing to drift.

2. **All register families supported.** `spill()` is GP-only (§7 review note §7a).
   `StackFrame::park<T>()` accepts `Reg64`, `Ymm`, `Zmm`, and any other type
   with a `do_store` overload — the same set `StackFrame` already supports.

3. **No LIFO ordering constraint.** `restore()` must be called in exact reverse
   spill order because `pop` targets the top of the physical hardware stack.
   `StackFrame` uses `mov [rsp + fixed]` / `mov reg, [rsp + fixed]`, so slots
   can be parked and reloaded in any order at any time.

4. **No interleaving hazard.** The §7 / §8 incompatibility is resolved by removing
   the concept of push-based spill entirely. There is only one model: a single
   `sub rsp` at layout commit, fixed slot offsets, a single `add rsp` at destroy.

### Why This Is Better Than §8 (`StackFrame`)

1. **Fixed offsets require no mental arithmetic.** `StackFrame` exposes raw `ptrdiff_t`
   offsets. The caller must manually compute and maintain slot positions, ensure slots
   do not overlap, and keep the total within `size`. `StackFrameBuilder` issues typed
   `ParkSlot` handles at declaration time that encapsulate their own fixed offset.
   Off-by-8 mistakes are impossible.

2. **Volatile-save integration.** `StackFrame` has no knowledge of volatile registers.
   `save_gp_volatiles()` (§20) and `save_vec_volatiles()` still use push/sub rsp,
   which conflicts with an open `StackFrame` for the same reason `spill()` does.
   `StackFrameBuilder` pre-declares volatile-save slots at build time so the emit methods
   use fixed offsets rather than additional rsp movement.

3. **Single rsp movement enforced by design.** Because `StackFrame` is the *only*
   way to interact with the stack frame, `managed_push_count_` becomes stable after
   `emit_prologue()` and `build()`. `emit_call()` alignment is then trivially correct;
   the `extra_pushes` escape hatch (§18) can be removed entirely.

4. **Aligned total computed automatically.** `StackFrame` requires the caller to pass
   an already-aligned size. `StackFrameBuilder` accumulates declared slot requirements and
   rounds the total up to the nearest 16 bytes at `build()` time, preventing
   misalignment silently.

### Proposed API

#### Phase 1 — Declaration (`StackFrameBuilder` builder, no code emitted)

```cpp
class StackFrameBuilder {
public:
    // Reserve n GP-sized (8-byte) named park slots.
    // Returns *this for chaining.
    StackFrameBuilder &gp_parks(int n);

    // Reserve n vector-sized park slots.
    // Slot size is 64 bytes (ZMM) when AVX-512 is present, 32 bytes (YMM) otherwise.
    // Returns *this for chaining.
    StackFrameBuilder &vec_parks(int n);

    // Reserve a raw scratch area of 'bytes' bytes.
    // Must be a positive multiple of 8.
    // Returns *this for chaining.
    StackFrameBuilder &scratch(ptrdiff_t bytes);

    // Reserve fixed-offset stack slots for every ABI-volatile GP and vector
    // register.  save_volatiles() on the StackFrame will store only those
    // that are live at the time of the call; restore_volatiles() reloads exactly
    // that set.  The slot map is independent of which registers are allocated at
    // build() time, so registration order does not matter.
    // Returns *this for chaining.
    StackFrameBuilder &with_volatile_save();

    // Emit sub rsp, <aligned_total> and return a StackFrame owning the frame.
    // After this call rsp will not move until StackFrame::destroy() or its
    // destructor. Throws if no CodeGenerator has been provided.
    StackFrame build();
};

// Factory on RegPoolManager:
StackFrameBuilder make_stack_frame();
```

#### Phase 2 — Committed layout (`StackFrame`, owns the stack space)

```cpp
class StackFrame {
public:
    // Move-only; destructor emits add rsp, total if destroy() was not called.
    StackFrame(const StackFrame &) = delete;
    StackFrame &operator=(const StackFrame &) = delete;
    StackFrame(StackFrame &&) noexcept;
    ~StackFrame() noexcept;

    // Emits add rsp, total immediately and disarms the destructor.
    void destroy();

    // Store reg at GP park slot 'idx' and free it from the allocator.
    // Emits: mov [rsp + gp_base + idx*8], reg
    // RegT may be Reg64, Reg32, Reg16.
    template <class RegT>
    void park(RegT &reg, int slot_idx);

    // Store reg at GP park slot without freeing it.
    template <class RegT>
    void park(const RegT &reg, int slot_idx);

    // Allocate a new register of type RegT, load GP park slot 'idx' into it.
    // Caller is responsible for freeing the returned register.
    template <class RegT>
    RegT reload(int slot_idx);

    // Vector equivalents (slot size 32 or 64 bytes depending on AVX-512).
    template <class VecT>
    void park_vec(VecT &reg, int slot_idx);

    template <class VecT>
    void park_vec(const VecT &reg, int slot_idx);

    template <class VecT>
    VecT reload_vec(int slot_idx);

    // Emit fixed-offset stores for each volatile register that is currently
    // live (allocated) at the time of the call.  Uses fixed offsets — does not
    // move rsp.  The saved set is recorded and passed to restore_volatiles().
    void save_volatiles();

    // Reload the registers saved by the preceding save_volatiles() call,
    // in reverse order.  Only the registers that were actually stored are reloaded.
    void restore_volatiles();

    // Return a stack address suitable for use as a memory operand.
    // Equivalent to [rsp + scratch_base + byte_offset].
    Xbyak::Address scratch_addr(ptrdiff_t byte_offset = 0) const;

    // Total bytes allocated by this layout.
    ptrdiff_t total_size() const;
};
```

#### Removed API (superseded by §23)

```cpp
// Removed from RegPoolManager:
void spill(const Reg64 &reg);               // §7 — push-based, incompatible with frames
std::vector<Reg64> spill(int count, ...);  // §7
Reg64 restore();                            // §7
void restore(const Reg64 &reg);             // §7
void restore(const std::vector<Reg64> &);  // §7
bool spill_stack_empty() const;             // §19 — only meaningful for push spills
void assert_spill_stack_empty() const;      // §19
StackFrame make_stack_frame(ptrdiff_t);     // §8 — replaced by make_stack_frame()
void save_gp_volatiles();                   // §20 — absorbed into StackFrame
void restore_gp_volatiles();                // §20
void save_vec_volatiles();                  // §20
void restore_vec_volatiles();               // §20
void save_volatiles();                      // §20 — absorbed into StackFrame
void restore_volatiles();                   // §20
```

`emit_prologue()`, `emit_epilogue()`, `emit_call()`, `alloc()`, `free()`, `Scoped<T>`,
`mark_unavailable()`, and all pool/getter methods are **unchanged**.

### Internal State Changes

```cpp
// Replaces: spill_stack_gp_, saved_volatile_gp_, saved_volatile_vec_,
//           saved_volatile_vec_bytes_, saved_volatiles_armed_, allocated_stack_space_

// managed_push_count_ is now only modified by emit_prologue() and
// StackFrame::build() / destroy(). It is never modified mid-kernel.
```

The `StackFrameBuilder` builder is a lightweight value type that accumulates sizing data
(counts and byte totals) in local variables before `build()` is called. No new
persistent data members are needed on `RegPoolManager` beyond tracking whether a
`StackFrame` is currently active (for debug assertions).

### Usage Example — Typical oneDNN Kernel Pattern

The most common pattern in oneDNN kernels is: receive pointer arguments in ABI
registers, park them to the stack immediately, then use those hardware registers as
scatch throughout the compute loop.

```cpp
class MyKernel : public Xbyak::CodeGenerator,
                 public Xbyak::RegPoolManager {
public:
    MyKernel(const Xbyak::util::Cpu &cpu)
        : Xbyak::CodeGenerator(4096),
          Xbyak::RegPoolManager(cpu, this) {}

    void generate() {
        // Step 1: allocate callee-saved registers upfront so the prologue is complete
        auto reg_src  = alloc<Reg64>();   // likely rax (volatile) or rbx (preserved)
        auto reg_dst  = alloc<Reg64>();
        auto reg_len  = alloc<Reg64>();
        auto zmm_bias = alloc<Zmm>();

        emit_prologue();  // push rbx etc. if any preserved regs were promoted

        // Step 2: declare all stack needs upfront — nothing is emitted yet
        auto cl = make_stack_frame()
            .gp_parks(3)       // 3 x 8-byte slots for the pointer/length arguments
            .vec_parks(1)      // 1 x 64-byte slot for zmm_bias (AVX-512 kernel)
            .with_volatile_save()  // snapshot live volatiles for pre-call save
            .build();          // emits ONCE: sub rsp, 112  (48 + 64, rounded to 16)

        // Step 3: park ABI argument registers to free up hardware registers
        // emits: mov [rsp+0],  rdi
        cl.park(alloc<Reg64>(7 /*rdi*/), 0);  // src pointer
        // emits: mov [rsp+8],  rsi
        cl.park(alloc<Reg64>(6 /*rsi*/), 1);  // dst pointer
        // emits: mov [rsp+16], rdx
        cl.park(alloc<Reg64>(2 /*rdx*/), 2);  // length

        // load and park zmm broadcast constant
        vbroadcastss(zmm_bias, ptr[rdi + offsetof(args_t, bias)]);
        cl.park_vec(zmm_bias, 0);  // emits: vmovdqu32 [rsp+48], zmm; frees zmm_bias

        // rdi, rsi, rdx, zmm_bias are all free now — use them as scratch
        // without consulting any offset table

        // --- compute loop ---
        auto r_src = cl.reload<Reg64>(0);    // emits: mov r_src, [rsp+0]
        auto r_dst = cl.reload<Reg64>(1);    // emits: mov r_dst, [rsp+8]
        auto z     = cl.reload_vec<Zmm>(0);  // emits: vmovdqu32 z, [rsp+48]
        // ... loop body using r_src, r_dst, z ...
        free(r_src); free(r_dst); free(z);

        // Step 4: tear down in reverse order
        cl.destroy();        // emits: add rsp, 112
        emit_epilogue();     // pops callee-saved registers in reverse order
        ret();
    }
};
```

### Usage Example — Calling a C Runtime Function

```cpp
// save_volatiles() stores each volatile register that is live at that moment
// to its pre-reserved fixed slot — no rsp movement.
cl.save_volatiles();        // emits: mov [rsp+slot_N], reg for each live volatile
emit_call(&my_c_function);  // alignment is trivially correct: managed_push_count_ stable
cl.restore_volatiles();     // emits: mov reg, [rsp+slot_N] in reverse order
```

### Notes / Interactions

- **`with_volatile_save()` slot sizing:** `build()` reserves a slot for every
  register in the ABI-volatile set (e.g., 9 GP slots + 16 or 32 vector slots on
  Linux), regardless of which registers are allocated at that moment.
  `save_volatiles()` and `restore_volatiles()` then inspect the live set at
  emission time and only emit stores/loads for registers that are actually
  allocated.  This avoids a footgun where a volatile register allocated after
  `build()` would silently have no slot under a live-at-build-time snapshot
  design, leading to data corruption on the next call.
  The trade-off is that the frame is somewhat larger when few volatile registers
  are in use, but for a JIT kernel this is generally acceptable.
- **Ordering constraint:** `emit_prologue()` must be called before `build()` so that
  `managed_push_count_` is stable at the moment the alignment for `emit_call()` is
  computed. The epilogue mirrors this: `destroy()` before `emit_epilogue()`.
- **`emit_call()` simplification:** because `managed_push_count_` is now stable between
  `emit_prologue()` and `emit_epilogue()`, the `extra_pushes` parameter becomes
  unnecessary and can be removed.
- **`assert_clean_stack()`** can be simplified: it only needs to check that no
  `StackFrame` is currently open, rather than also checking `spill_stack_gp_`.
- **No impact on prologue/epilogue:** `emit_prologue()` / `emit_epilogue()` operate
  on callee-saved push/pop sequences which are fully outside and orthogonal to any
  `StackFrame`. A kernel that only uses callee-saved GP registers with no stack
  parking needs neither `StackFrameBuilder` nor any of the removed APIs.
- **Migration path:** The `StackFrame`, `spill()`, and `save_volatiles()` family can
  be kept as deprecated thin wrappers during a transition period, each calling the
  new `StackFrameBuilder` / `StackFrame` primitives internally, before being removed
  in a subsequent release.

---

## Open Issue: `Reg8`/`Reg16` in `reg_family<>` but no `do_store`/`do_load` overloads 

`reg_family<Reg8>` and `reg_family<Reg16>` are defined (mapping to `RegFamily::GP`),
so `alloc<Reg8>()`, `free(Reg8(...))`, and `mark_unavailable<Reg8>(idx)` all compile
and function correctly for pool tracking purposes.

However, `StackFrame::park()` and `reload()` dispatch via `do_store`/`do_load`,
which have overloads for `Reg64`, `Reg32`, and `Reg16` but **not** `Reg8`.  Calling
`cl.park(Reg8(...), slot)` therefore fails to compile.

### Options

1. **Add `do_store`/`do_load` overloads for `Reg8`** — emit `mov byte [rsp+off], r8`
   / `mov r8, byte [rsp+off]`.  Architecturally sound; completes the matrix.

2. **Remove `reg_family<Reg8>` (and `reg_family<Reg16>`)** — narrow the public API
   surface to the widths that are practically useful for JIT kernels (`Reg32`,
   `Reg64`, and the vector/opmask/tile families).  `Reg8`/`Reg16` can still be
   constructed manually from a `Reg32`/`Reg64` index when needed.

3. **Keep current state with a `static_assert` guard** — add a `static_assert` inside
   `park()`/`reload()` that fires a clear error message when `Reg8` is passed,
   preventing the confusing linker/template error.

The recommended path is **option 1**: it is consistent, low-risk, and closes the gap
without removing already-working tracking functionality.

---

## 24. Open Task: ABI-portable argument register mapping (Future Work)

### Motivation

JIT kernels that accept incoming function arguments must know which registers
hold those arguments at function entry.  This mapping is ABI-specific:

| Arg position | SysV AMD64 (Linux/macOS) | Windows x64        |
|--------------|--------------------------|--------------------|
| GP arg 0     | rdi (index 7)            | rcx (index 1)      |
| GP arg 1     | rsi (index 6)            | rdx (index 2)      |
| GP arg 2     | rdx (index 2)            | r8  (index 8)      |
| GP arg 3     | rcx (index 1)            | r9  (index 9)      |
| GP arg 4     | r8  (index 8)            | stack              |
| GP arg 5     | r9  (index 9)            | stack              |
| FP arg 0     | xmm0                     | xmm0               |
| FP arg 1     | xmm1                     | xmm1               |
| …            | xmm0–xmm7 (8 regs)       | xmm0–xmm3 (4 regs) |

Today every call site must `#ifdef` around the register index:

```cpp
#ifdef _WIN32
    auto arg0 = alloc<Reg64>(1);   // rcx
#else
    auto arg0 = alloc<Reg64>(7);   // rdi
#endif
```

This is verbose, error-prone, and defeats the goal of writing platform-portable
JIT kernels with a single code path.

### Fundamental ABI asymmetry

The two ABIs count GP and FP slots **differently**, which means a single
`arg_reg_index(n)` function is insufficient for mixed-type signatures.

On **SysV**, GP and FP argument counters are **independent**:

```
void f(int a, double b, int c)
      rdi       xmm0    rsi
```

On **Windows x64**, all arguments share a **single slot counter** and the register
type follows the argument type at that position:

```
void f(int a, double b, int c)
      rcx       xmm1    r8
       0         1       2   ← slot position
```

A JIT kernel generating code to process its own incoming arguments can handle
this with two separate index queries (one for GP, one for FP).  Generating a JIT
*caller* that passes mixed-type arguments to a C function is inherently
platform-dependent at the slot-counting level and may require a higher-level
abstraction (e.g. an `ArgBuilder` that tracks both counters).

### Proposed API (static helpers only — no new member state)

```cpp
// Returns the register index of the n-th integer/pointer argument register.
// n is the GP argument count (0-based), independent of any FP arguments.
//
// SysV:    n=0→rdi(7), n=1→rsi(6), n=2→rdx(2), n=3→rcx(1), n=4→r8(8), n=5→r9(9)
// Windows: n=0→rcx(1), n=1→rdx(2), n=2→r8(8),  n=3→r9(9)
//
// Throws ERR_RM_REG_IDX_OUT_OF_RANGE if n >= gp_arg_reg_count().
static int gp_arg_reg_index(int n);

// Returns the register index of the n-th FP/vector argument register.
// n is the FP argument count (0-based), independent of any GP arguments on SysV.
// On Windows, n is the slot position (must account for preceding GP args).
//
// SysV/Windows: n=0→xmm0, n=1→xmm1, ..., up to xmm7 (SysV) / xmm3 (Windows).
//
// Throws ERR_RM_REG_IDX_OUT_OF_RANGE if n >= fp_arg_reg_count().
static int fp_arg_reg_index(int n);

// Returns the number of GP argument registers available in registers.
// SysV: 6  (rdi, rsi, rdx, rcx, r8, r9)
// Windows: 4  (rcx, rdx, r8, r9)
static int gp_arg_reg_count();

// Returns the number of FP argument registers.
// SysV: 8  (xmm0–xmm7)
// Windows: 4  (xmm0–xmm3)
static int fp_arg_reg_count();
```

### Stack-passed arguments

Arguments beyond `gp_arg_reg_count()` (GP) or `fp_arg_reg_count()` (FP) are
passed on the stack.  The manager has no visibility into the caller's frame layout
at the point of the JIT entry, so stack-passed arguments are **out of scope** for
this API.  Callers that need to access them must do so manually, using the known
ABI layout:

- **SysV**: 7th GP arg at `[rsp+8]` (before any prologue push), then `[rsp+16]`, etc.
- **Windows**: 5th arg at `[rsp+40]` (after 32-byte shadow space + return address).

`gp_arg_reg_index()` and `fp_arg_reg_index()` should throw when `n` exceeds the
register count so call sites receive a clear error rather than silently using a
wrong index.

### Usage example (after implementation)

```cpp
// Platform-portable: park the first two integer arguments
auto arg0 = alloc<Reg64>(gp_arg_reg_index(0));   // rdi / rcx
auto arg1 = alloc<Reg64>(gp_arg_reg_index(1));   // rsi / rdx

// Platform-portable: park the first FP argument
auto fp0  = alloc<Xmm>(fp_arg_reg_index(0));     // xmm0 on both ABIs

emit_prologue();
auto cl = make_stack_frame().gp_parks(2).build();
cl.park(arg0, 0);
cl.park(arg1, 1);
// ... kernel body ...
cl.destroy();
emit_epilogue();
ret();
```

Compared to the current boilerplate:

```cpp
// Before: required #ifdef at every call site
#ifdef _WIN32
    auto arg0 = alloc<Reg64>(1);   // rcx
    auto arg1 = alloc<Reg64>(2);   // rdx
#else
    auto arg0 = alloc<Reg64>(7);   // rdi
    auto arg1 = alloc<Reg64>(6);   // rsi
#endif
```

### Remaining open question

For kernels that both **receive** mixed-type arguments (as callee) and **emit
calls** to mixed-signature C functions (as caller), the two ABIs' different
slot-counting rules mean a second, higher-level abstraction may be needed —
an `ArgDescriptor` or `CallBuilder` that accumulates argument types in order and
resolves both the GP and FP register assignments simultaneously.  This is deferred
until concrete use cases drive the design.

### Internal state changes

None.  All four functions are `static` — they encode only compile-time ABI
constants.  No new member variables or constructor changes are required.

---

## 25. Stack-Overflow Arguments: `with_outgoing_args()` / `StackFrame::emit_call()`

### Motivation

`RegPoolManager::emit_call()` (§18) handles the common case: a call whose
arguments all fit in ABI registers.  Its `sub rsp, adj; call; add rsp, adj`
sequence is correct when rsp only needs to move for alignment and Win64 shadow
space — it emits that movement at call time and immediately undoes it.

This model breaks down for functions whose argument count exceeds the ABI GP
register limit:

| ABI     | GP regs for args       | Stack args start at |
|---------|------------------------|---------------------|
| SysV    | 6 (rdi,rsi,rdx,rcx,r8,r9) | arg 7+           |
| Win64   | 4 (rcx,rdx,r8,r9)     | arg 5+              |

Stack-overflow arguments must be written to specific `[rsp + offset]` slots
**before** the `call` instruction.  The callee reads them at fixed positions
relative to the rsp it sees on entry:

- SysV:  7th arg at `[rsp + 0]`, 8th at `[rsp + 8]`, …
- Win64: 5th arg at `[rsp + 32]`, 6th at `[rsp + 40]`, …  (above the 32-byte
  shadow space the caller already owns)

`emit_call()` emits `sub rsp, adj` immediately before the `call`.  Any
stack-overflow args written to `[rsp + X]` before that `sub` are shifted to
`[rsp + X + adj]` at call time — the callee reads the wrong values with no
diagnostic of any kind.

The fundamental mismatch is:

| Method | When rsp moves | Stack-arg writes |
|--------|---------------|-----------------|
| `emit_call()` | At call, transiently | Must be *after* the sub |
| `StackFrame` | Once at `build()`, stable | Can be written at *any time* before the call |

§23's commitment — that rsp does not move between `build()` and `destroy()` —
provides exactly the stability needed for fixed-offset pre-call writes.  The
missing piece is a way to (a) declare the overflow-arg slots inside the
`StackFrameBuilder` builder so they are placed at the correct ABI position, and (b)
make `build()` absorb the required alignment adjustment into the frame total so
the call itself needs no further rsp movement.

### Design

Two additions are required:

**1. `StackFrameBuilder::with_outgoing_args(n)`** — declares `n` stack-overflow argument
slots.  The builder records this count; `build()` places the slots at the
ABI-correct base:

- SysV:  slots at `[rsp + 0]`, `[rsp + 8]`, … (`outgoing_arg_base = 0`)
- Win64: 32 bytes of shadow space at `[rsp + 0..31]`, then slots at
  `[rsp + 32]`, `[rsp + 40]`, … (`outgoing_arg_base = 32`)

Both shadow space and overflow slots are laid out by `build_layout()` **below**
any GP/Vec park slots, scratch area, or volatile-save slots, so they sit at the
lowest rsp offsets as the ABI requires.

**2. Alignment-aware `build_layout()` total.**  When `with_outgoing_args(n)` is
declared, `emit_layout_call()` (see below) emits a bare `call` with no sub/add.
For rsp to be 16-aligned at that `call` instruction the frame total must satisfy:

```
rsp_at_call = rsp_entry − 8·P − total
rsp_entry   = 16k − 8  (the C caller's call instruction pushed an 8-byte return address)

Alignment constraint: (8 + 8·P + total) % 16 == 0

P even → total ≡  8 (mod 16)   ← round cursor up to the nearest value ≡ 8 (mod 16)
P odd  → total ≡  0 (mod 16)   ← standard nearest-16 rounding (same as default path)
```

where `P = managed_push_count_` at the time `build()` is called (i.e. after
`emit_prologue()`).

**3. `StackFrame::emit_call()` — unified call method.**  Rather than
introducing a separate `emit_layout_call()` method that callers must remember to
use, `StackFrame` gains its own `emit_call()` that automatically selects the
correct behaviour:

- `with_outgoing_args(n)` was declared → bare `mov rax, func; call rax`.  rsp is
  already aligned; no sub/add emitted.
- Not declared → delegate to `RegPoolManager::emit_call()`, which performs the
  standard `sub rsp, adj; call; add rsp, adj` sequence.

From the caller's perspective there is exactly **one call method** inside a
`StackFrame`.  The same `cl.emit_call(&func)` works for both register-only
calls and overflow-arg calls in the same layout.

### Proposed API

#### On `StackFrameBuilder` (builder additions)

```cpp
// Reserve n outgoing stack-overflow argument slots.
//
// Slot positions after build():
//   SysV:  [rsp + n*8]          (n = 0-based overflow arg index)
//   Win64: [rsp + 32 + n*8]     (preceded by 32-byte shadow space; total Win64
//                                 stack arg base = rsp + 32, as the ABI requires)
//
// build() adjusts the frame total so that rsp is 16-aligned at the subsequent
// emit_call() instruction, absorbing alignment compensation into the sub rsp.
// Returns *this for chaining.
StackFrameBuilder &with_outgoing_args(int n);
```

#### On `StackFrame` (new methods)

```cpp
// Returns the number of outgoing stack-overflow argument slots declared
// with with_outgoing_args().  Zero if not declared.
int outgoing_arg_count() const;

// Returns an address operand for outgoing stack-overflow argument slot n.
//
//   SysV:  [rsp + n*8]
//   Win64: [rsp + 32 + n*8]   (above the 32-byte shadow space)
//
// Write each overflow argument here before calling emit_call().
// n must be in [0, outgoing_arg_count()).
// Throws ERR_RM_LAYOUT_SLOT_OOB if n is out of range.
Xbyak::Address outgoing_arg_addr(int n) const;

// Emit a call to func_ptr with automatic ABI correctness.
//
// Behaviour depends on whether with_outgoing_args() was declared:
//
//   with_outgoing_args(n) declared:
//     Emits only "mov rax, func; call rax" — no sub/add rsp.
//     build() has already absorbed the required alignment into the frame total.
//     Win64 shadow space is included in the frame.
//     Write overflow arguments to outgoing_arg_addr(0..n-1) and load register
//     arguments before calling.
//
//   with_outgoing_args() not declared:
//     Delegates to RegPoolManager::emit_call(), which emits the standard
//     "sub rsp, adj; mov rax, func; call rax; add rsp, adj" sequence.
//     Use this form when all arguments fit in registers.
//
// rax is clobbered in both cases.
// Throws ERR_RM_NO_CG if no CodeGenerator is attached.
void emit_call(uint64_t func_ptr);

template <typename FuncT>
void emit_call(FuncT *func_ptr);
```

### Internal State Changes

```cpp
// Added to StackFrame:
ptrdiff_t outgoing_arg_base_;   // rsp-relative base of the first overflow-arg slot
int       n_outgoing_args_;     // 0 if with_outgoing_args() was not declared
```

`build_layout()` gains an `outgoing_args` parameter (default `0`) and a second
block in the total-rounding logic that selects between `≡ 8 (mod 16)` and
`≡ 0 (mod 16)` depending on `managed_push_count_` parity.

No new persistent state is required on `RegPoolManager` itself.

### Usage Example — 8-argument function (4 register + 4 stack on SysV; 4 register + 4 stack on Win64)

```cpp
// C function to call:
//   SysV:  a-f in regs (rdi,rsi,rdx,rcx,r8,r9); g,h on stack
//   Win64: a-d in regs (rcx,rdx,r8,r9); e-h on stack
extern "C" uint64_t sum8(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                         uint64_t e, uint64_t f, uint64_t g, uint64_t h);

struct MyKernel : CodeGenerator, RegPoolManager {
    MyKernel(const util::Cpu &cpu)
        : CodeGenerator(4096), RegPoolManager(cpu, this) {}

    void build() {
        emit_prologue();  // no callee-saved regs used here — no-op

        // Declare overflow slots.  build() reserves Win64 shadow space
        // automatically and aligns the frame total for a bare call.
#ifdef _WIN32
        auto cl = make_stack_frame().with_outgoing_args(4).build(); // e,f,g,h
#else
        auto cl = make_stack_frame().with_outgoing_args(2).build(); // g,h
#endif

        // Write stack-overflow arguments to their pre-reserved fixed slots.
        // rsp is stable — offsets are correct at the call instruction.
        auto r_tmp = alloc<Reg64>();
#ifdef _WIN32
        mov(r_tmp, 5); mov(cl.outgoing_arg_addr(0), r_tmp); // e → [rsp+32]
        mov(r_tmp, 6); mov(cl.outgoing_arg_addr(1), r_tmp); // f → [rsp+40]
        mov(r_tmp, 7); mov(cl.outgoing_arg_addr(2), r_tmp); // g → [rsp+48]
        mov(r_tmp, 8); mov(cl.outgoing_arg_addr(3), r_tmp); // h → [rsp+56]
        free(r_tmp);
        // Load register arguments.
        mov(rcx, 1); mov(rdx, 2); mov(r8, 3); mov(r9, 4);
#else
        mov(r_tmp, 7); mov(cl.outgoing_arg_addr(0), r_tmp); // g → [rsp+0]
        mov(r_tmp, 8); mov(cl.outgoing_arg_addr(1), r_tmp); // h → [rsp+8]
        free(r_tmp);
        mov(rdi, 1); mov(rsi, 2); mov(rdx, 3);
        mov(rcx, 4); mov(r8,  5); mov(r9,  6);
#endif
        // Bare call — rsp is already 16-aligned; no sub/add emitted.
        cl.emit_call(&sum8); // rax = 1+2+3+4+5+6+7+8 = 36

        cl.destroy();
        emit_epilogue();
        ret();
    }
};
```

### Usage Example — Mixed calls in one layout

The same `with_outgoing_args` layout can serve both a register-only call and an
overflow-arg call.  `cl.emit_call()` selects the bare-call path for both; the
overflow slots simply go unwritten before the register-only call.

```cpp
auto r_pres = alloc<Reg64>(3);  // rbx — survives every call
emit_prologue();                 // push rbx → P = 1 (odd)

#ifdef _WIN32
auto cl = make_stack_frame().with_outgoing_args(4).build();
#else
auto cl = make_stack_frame().with_outgoing_args(2).build();
#endif

// Call 1: register-only.  Overflow slots untouched.
cl.emit_call(&no_args_func);    // rax = ...
mov(r_pres, rax);               // stash in callee-saved register

// Call 2: overflow-arg call.
auto r_tmp = alloc<Reg64>();
// ... write outgoing_arg_addr slots and register args ...
cl.emit_call(&sum8);            // rax = sum
free(r_tmp);

add(rax, r_pres);               // combine results

free(r_pres);
cl.destroy();
emit_epilogue();
ret();
```

### Notes / Interactions

- **`emit_call()` vs `RegPoolManager::emit_call()`** — inside a layout the user
  always calls `cl.emit_call()`.  `RegPoolManager::emit_call()` should generally
  not be used inside an active `StackFrame`; in debug builds an assertion
  could guard against this, though the current implementation does not require it.
- **Overflow slot count must be declared upfront** — the count is fixed at
  `build()` time and reflects the worst case across all calls made inside the
  layout.  If different calls within the same layout have different overflow counts,
  declare the maximum.
- **Win64 shadow space is non-optional when `with_outgoing_args(n)` is used** —
  `build_layout()` always reserves the full 32 bytes on Win64 so that register-only
  calls within the same layout also have shadow space available.
- **`emit_layout_call()` (interim name)** — during development a separate method
  `emit_layout_call()` was briefly used for the bare-call path before being merged
  into `StackFrame::emit_call()`.  The unified single-method design is the
  intended final form.

---

## 26. Named-Alias Register Lifecycle: `declare_alias()` / `ManagedAlias`

### Motivation

Many JIT kernels hold several named values simultaneously -- input pointers,
output pointers, loop bounds, scale factors -- where not all values can live in
hardware registers at the same time.  The standard pattern is to spill a value
to the stack and reload it when needed.  With only `alloc<Reg64>()` / `free()`
/ `park()` / `reload()` this requires the programmer to:

1. Manually allocate a stack slot via `StackFrame`.
2. Track which slot holds which named value.
3. Manually call `park(reg, slot)` and `reload<Reg64>(slot)` at each use site.
4. Remember which register currently holds a given value.

There is no abstraction that ties a named logical value to its stack slot and
its current register.  Mistakes are silent: a slot written by one value can be
overwritten by another if the programmer uses the wrong slot index, and there
is no collision detection when two names try to activate the same physical
register simultaneously.

`ManagedAlias` binds a logical name, a stack slot, and a hardware register
together under a five-operation lifecycle.  The register manager enforces
correct use, detects simultaneous-activation collisions, and emits the
load/store instructions automatically.

**Additional motivation for APX kernels:** On APX-capable hardware, registers
r16-r31 are not caller- or callee-saved and require no stack slot at all.
`declare_alias(rax, r22)` expresses "use r22 on APX (no stack slot needed),
fall back to rax with a stack slot otherwise."  The same kernel source compiles
and runs correctly on both APX and non-APX hardware with no conditional
compilation at the call sites.

### Design

Each `ManagedAlias` is always in one of two states:

```
  DORMANT                                      ACTIVE
  (no register allocated;      ---------->    (register live in live_gp_;
   slot may hold a value)      <----------     reg() is valid)
```

Transitions from DORMANT to ACTIVE:

| Operation | Register action | Stack slot action | When to use |
|-----------|-----------------|-------------------|-------------|
| `prime()` | `alloc<Reg64>()` | no load emitted | First activation. Caller writes the initial value after calling prime(). |
| `restore(cl)` | `alloc<Reg64>()` | emit `mov reg, [slot]` | Retrieve a value that was previously saved. |

Transitions from ACTIVE to DORMANT:

| Operation | Register action | Stack slot action | When to use |
|-----------|-----------------|-------------------|-------------|
| `save(cl)` | `free(reg)` | emit `mov [slot], reg` | Value was modified and must be persisted to the slot. |
| `release()` | `free(reg)` | nothing | Value is unchanged since the last `save()`; slot is still valid; extra store is avoided. |

`free()` is a terminal operation callable from either DORMANT or ACTIVE state.
It frees the register if currently held and ends the alias lifetime.  It is
symmetric with calling `rm.free(reg)` on a directly allocated register.

For aliases with `needs_slot_ == false` (`AliasMode::no_slot` path, including the
APX extended-register pattern from `declare_alias(primary, alt, has_apx())`): the
register is NOT pre-allocated at `declare_alias` time.  It enters `live_gp_` when
`prime()` is called, just like any slotted alias.  `prime()` throws `GP_IN_USE` if
the register is already live.  `save`, `restore`, and `release()` on the slotted
behavior do not apply: `save`/`restore` are no-ops; `release()` frees the register
back to the pool.  `free()` is NOT a no-op -- it is the required terminal call to
return the register to the pool before `assert_all_free()`.

### Internal State Changes

**Additions to `RegPoolManager`:**

```cpp
struct AliasPendingDecl {
    bool  needs_slot;
    int   desired_idx;  // -1: anonymous (manager picks); >= 0: named register index
};

std::vector<AliasPendingDecl> pending_aliases_;

// Returns true if register idx is currently in free_gp_regs.
// Used by ManagedAlias::prime() to distinguish "pool owns it" from
// "we own it in live_gp_".
bool is_available_gp(int idx) const;
```

`pending_aliases_` is cleared by `reset()` along with all other manager state.

**`StackFrame` additions:**

```cpp
// Emit: mov [rsp + alias_slot_offset(alias_id)], reg
void alias_store(int alias_id, const Xbyak::Reg64 &reg);

// Emit: mov reg, [rsp + alias_slot_offset(alias_id)]
void alias_load(int alias_id, Xbyak::Reg64 &reg);
```

**`ManagedAlias` internal fields:**

```cpp
int            alias_id_;     // index into StackFrame alias offset table
int            desired_idx_;  // -1 for anonymous
bool           needs_slot_;
bool           is_active_;
Xbyak::Reg64   reg_;          // valid only when is_active_ == true
RegPoolManager *rm_;          // back-pointer set at declare_alias time
```

`reg_` is mutable: for anonymous aliases it is updated on each `restore` call
because the manager may assign a different physical register depending on what
is currently free.

### Proposed API

#### `declare_alias` overloads (on `RegPoolManager`)

```cpp
// Named, always backed by a stack slot (default).
// Register is not allocated at declaration time.
// Collision detected at the first prime() or restore() call.
// Equivalent to declare_alias(reg, AliasMode::slotted).
ManagedAlias declare_alias(Xbyak::Reg64 reg);

// Named, with explicit slot/no-slot control.  See §30 for AliasMode definition.
//
// AliasMode::slotted (default): 8-byte stack slot assigned at build();
//   save()/restore() emit the load/store.  Same as the single-arg overload.
//
// AliasMode::no_slot: no stack slot; register allocated at prime() and freed
//   at release()/free(); prime() throws GP_IN_USE if the register is already
//   live, enforcing mutual exclusion.  save() and restore() are no-ops.
//   Primary use cases:
//     1. Sequential exclusive aliases (§30): two names for the same physical
//        register used in non-overlapping phases.
//     2. APX permanent hold: declare_alias(rax, r22, has_apx()) -- see the
//        three-argument overload below.
ManagedAlias declare_alias(Xbyak::Reg64 reg, AliasMode mode);

// Named, conditional: use_alt==true picks alt_reg (AliasMode::no_slot);
//                     use_alt==false picks primary_reg (AliasMode::slotted).
// Primary use case -- APX-portable alias:
//   declare_alias(rax, r22, has_apx())
//   On APX: r22 allocated at prime(), permanent-hold pattern, free() at end.
//   On non-APX: rax with a stack slot, full save/restore lifecycle.
// The two-argument shorthand declare_alias(primary, alt) that hard-coded
// has_apx() internally is intentionally omitted: the explicit has_apx() call
// at the call site is self-documenting and adds no verbosity.
ManagedAlias declare_alias(Xbyak::Reg64 primary_reg,
                           Xbyak::Reg64 alt_reg,
                           bool         use_alt);

// Anonymous, always backed by a stack slot.
// Manager picks the first free GP register at prime()/restore() time.
// Required when multiple aliases must share the same physical register
// in a time-multiplexed pattern (one active at a time, rest dormant).
template <class RegT>
ManagedAlias declare_alias();
```

All overloads append an `AliasPendingDecl` to `pending_aliases_` and return a
`ManagedAlias`.  Stack slots are assigned when `make_stack_frame().build()` is
called, exactly as with other `StackFrame` resources.

No-slot overloads (`AliasMode::no_slot` or `use_alt=true`) do NOT allocate the
register at declaration time.  The register enters `live_gp_` only when `prime()`
is called.  This makes the call-site behavior uniform: `prime()` always does
work on both slotted and no-slot paths.

#### `ManagedAlias` class

```cpp
class ManagedAlias {
public:
    // Returns the currently active register.
    // Precondition: is_active() == true. Asserts or throws otherwise.
    const Xbyak::Reg64 &reg() const;

    // True when a stack slot was reserved for this alias.
    // False on the APX no-slot path.
    bool has_stack_slot() const;

    // True when a register is currently allocated (ACTIVE state).
    bool is_active() const;

    // DORMANT -> ACTIVE: allocate register, emit no load.
    // Use for first-time initialization; write the value to reg() after calling.
    //
    // Slotted path: allocates desired_idx_ (or any GP if anonymous).
    //   Throws GP_IN_USE if the named register is not available.
    //
    // No-slot path (AliasMode::no_slot): allocates desired_idx_.
    //   Throws GP_IN_USE if the register is already held in live_gp_ by any
    //   other allocation -- this is the mutual-exclusion guarantee.
    //   Re-prime after release() re-allocates the register (it returned to
    //   the pool on release()).
    //   Re-prime while already active is a silent no-op.
    void alloc();

    // DORMANT -> ACTIVE: allocate register and load value from slot.
    // Use to retrieve a value that was previously saved.
    // No-op when has_stack_slot() == false.
    void restore(StackFrame &cl);

    // ACTIVE -> DORMANT: emit store to slot and free register.
    // Use when the value in the register was modified and must be persisted.
    // No-op when has_stack_slot() == false.
    void save(StackFrame &cl);

    // ACTIVE -> DORMANT: free register without emitting a store.
    // Slotted: slot retains the value from the last save(); extra store avoided.
    // No-slot: frees the register back to the pool.
    void free();

    // Terminal: free register (if currently active) and end alias lifetime.
    // Symmetric with rm.free(reg) for directly allocated registers.
    // Callable from ACTIVE or DORMANT state.
    // After free(), the alias must not be used without first re-calling prime().
    // No-slot aliases (has_stack_slot() == false) MUST call free() before
    // rm.assert_all_free() since their register stays in live_gp_ until
    // explicitly freed here.
    void free();
};
```

### Implementation

#### `prime` pseudocode

Named aliases (both slotted and no-slot) use `is_available_gp()` as the branch
condition rather than `needs_slot_`.  This handles re-prime after `free()` for
no-slot aliases, where the register has been returned to `free_gp_regs` and
must be re-allocated.

```cpp
void ManagedAlias::prime() {
    if (desired_idx_ >= 0) {
        // Named path: use pool membership to determine action.
        if (rm_->is_available_gp(desired_idx_)) {
            // Register is free -- allocate it.
            // Covers: normal slotted first-use, and re-prime after free().
            reg_       = rm_->alloc<Xbyak::Reg64>(desired_idx_);
            is_active_ = true;
            return;
        }
        // Register is not in the free pool.
        if (!needs_slot_) return;  // no-slot: we own it in live_gp_ -- no-op
        // Named slotted and register is live: double-prime or conflict.
        RM_THROW(REG_IN_USE);
    } else {
        // Anonymous (always slotted): pick next free register.
        assert(!is_active_);
        reg_       = rm_->alloc<Xbyak::Reg64>();
        is_active_ = true;
        // No load emitted -- caller writes the initial value.
    }
}
```

Three outcomes for the named path:
1. Register in free pool: allocate -- normal slotted first-use or re-prime
   after `free()`.
2. Register not in free pool, `needs_slot_=false`: the alias owns it in
   `live_gp_` from `declare_alias` time -- no-op.
3. Register not in free pool, `needs_slot_=true`: conflict -- throw.

#### `restore` pseudocode

```cpp
void ManagedAlias::restore(StackFrame &cl) {
    if (!needs_slot_) return;
    assert(!is_active_);
    if (desired_idx_ >= 0)
        reg_ = rm_->alloc<Xbyak::Reg64>(desired_idx_);
    else
        reg_ = rm_->alloc<Xbyak::Reg64>();
    is_active_ = true;
    cl.alias_load(alias_id_, reg_);   // emit: mov reg_, [rsp+offset]
}
```

#### `save` pseudocode

```cpp
void ManagedAlias::save(StackFrame &cl) {
    if (!needs_slot_) return;
    assert(is_active_);
    cl.alias_store(alias_id_, reg_);  // emit: mov [rsp+offset], reg_
    rm_->free(reg_);
    is_active_ = false;
}
```

#### `release` pseudocode

```cpp
void ManagedAlias::release() {
    if (!needs_slot_) return;
    assert(is_active_);
    rm_->free(reg_);
    is_active_ = false;
    // No store emitted. Slot retains the value from the last save().
}
```

#### `free` pseudocode

```cpp
void ManagedAlias::free() {
    if (is_active_) {
        rm_->free(reg_);
        is_active_ = false;
    }
    // Slot content, if any, is now undefined.
    // Re-prime is possible but not the expected usage pattern.
}
```

For no-slot aliases `is_active_` is always true after `prime()`, so the
`rm_->free()` call always executes, returning the register to `free_gp_regs`.
For slotted aliases in DORMANT state the `if (is_active_)` guard short-circuits
since the register is already back in the free pool.

#### `build_layout` / `StackFrame` interaction

`pending_aliases_` is iterated during `make_stack_frame().build()`.  Each
entry with `needs_slot == true` gets an 8-byte slot in the frame, placed before
anonymous `gp_park` slots:

```
[rsp + 0  ]  alias slot 0          (first needs_slot==true alias)
[rsp + 8  ]  alias slot 1
              ...
[rsp + N  ]  gp park slot 0        (park / reload)
[rsp + N+8]  gp park slot 1
              ...
[rsp + M  ]  volatile GP saves     (with_volatile_save)
[rsp + P  ]  volatile vec saves
[rsp + Q  ]  scratch               (.scratch(n))
```

Aliases with `needs_slot==false` do not consume frame space.

### Usage Examples

#### Example 1: time-multiplexed pointer aliases

A kernel loads several buffer addresses from a parameter struct.  Not all can
live in registers simultaneously; each is used in a distinct phase.

```cpp
class MultiBufferKernel : public CodeGenerator, public RegPoolManager {
    ManagedAlias src_ptr_   = declare_alias<Reg64>();
    ManagedAlias dst_ptr_   = declare_alias<Reg64>();
    ManagedAlias scale_ptr_ = declare_alias<Reg64>();
    ManagedAlias bias_ptr_  = declare_alias<Reg64>();

    void generate() {
        auto layout = make_stack_frame().build();
        emit_prologue();

        auto param = alloc<Reg64>();
        // param holds the pointer to the params struct (e.g. from ABI arg reg).

        // Load all named values once into their slots.
        src_ptr_.alloc();
        mov(src_ptr_.reg(), ptr[param + 0]);
        src_ptr_.save(layout);        // store to slot, free register

        dst_ptr_.alloc();
        mov(dst_ptr_.reg(), ptr[param + 8]);
        dst_ptr_.save(layout);

        scale_ptr_.alloc();
        mov(scale_ptr_.reg(), ptr[param + 16]);
        scale_ptr_.save(layout);

        bias_ptr_.alloc();
        mov(bias_ptr_.reg(), ptr[param + 24]);
        bias_ptr_.save(layout);

        free(param);

        // Phase 1: process with src and scale.
        scale_ptr_.restore(layout);
        src_ptr_.restore(layout);
        // ... use src_ptr_.reg() and scale_ptr_.reg() ...
        src_ptr_.free();    // read-only: slot still valid, no store needed
        scale_ptr_.free();

        // Phase 2: write output using dst and bias.
        bias_ptr_.restore(layout);
        dst_ptr_.restore(layout);
        // ... write output using dst_ptr_.reg() and bias_ptr_.reg() ...
        dst_ptr_.save(layout);   // modified: must persist
        bias_ptr_.free();

        // End-of-kernel cleanup.
        src_ptr_.free();
        dst_ptr_.free();
        scale_ptr_.free();
        bias_ptr_.free();
        layout.destroy();
        emit_epilogue();
        ret();
        assert_all_free();
    }
};
```

#### Example 2: APX extended-register alias

On APX-capable hardware use a dedicated extended register (no stack slot,
zero save/restore overhead).  On non-APX, fall back to a named GP register
with a stack slot.  The use-site code is identical for both platforms.

```cpp
// Declaration (class member or local):
ManagedAlias out_ptr = declare_alias(rax, r22);
//   APX path:     r22 allocated at declare time; all lifecycle ops are no-ops
//                 except free().
//   non-APX path: rax used with a stack slot; full lifecycle applies.

auto layout = make_stack_frame().build();

// Initialization -- identical code on both paths.
out_ptr.alloc();
mov(out_ptr.reg(), ptr[rdi + 0]);
out_ptr.save(layout);   // APX: no-op; non-APX: mov [slot], rax; free rax

// Use site -- identical code on both paths.
out_ptr.restore(layout);          // APX: no-op; non-APX: alloc rax, load slot
vmovaps(ptr[out_ptr.reg()], zmm0);
out_ptr.free();                // APX: no-op; non-APX: free rax

// End of kernel.
out_ptr.free();           // APX: frees r22; non-APX: no-op (already dormant)
layout.destroy();
assert_all_free();
```

#### Example 3: conditional no-slot register

Use an unconstrained register (rbp, when not needed as a frame pointer) on
some code paths; fall back to a slot-backed register otherwise.  The use-site
code is uniform across both paths.

```cpp
bool const use_rbp = !needs_frame_pointer();
ManagedAlias loop_var = declare_alias(rcx, rbp, use_rbp);

auto layout = make_stack_frame().build();
loop_var.alloc();
xor_(loop_var.reg(), loop_var.reg());    // initialize counter to 0

loop_var.save(layout);  // rbp path: no-op; rcx path: store slot, free rcx

L("loop_top");
// ... body that uses rcx for other work ...
loop_var.restore(layout);
inc(loop_var.reg());
cmp(loop_var.reg(), trip_count);
loop_var.save(layout);
jl("loop_top");

loop_var.free();
```

#### Example 4: read-once value with `release()` optimisation

A configuration value loaded once at kernel start, read multiple times,
never modified.  `release()` avoids the redundant store that `save()` would
emit on every read-only use.

```cpp
ManagedAlias cfg = declare_alias<Reg64>();

auto layout = make_stack_frame().build();
cfg.alloc();
mov(cfg.reg(), ptr[rdi + 8]);    // load config pointer from params
cfg.save(layout);                // store to slot, free register

// Read-only use 1.
cfg.restore(layout);             // alloc reg, load from slot
mov(rax, ptr[cfg.reg() + 0]);   // read first field
cfg.free();                   // free reg; NO store emitted -- slot still valid

// Read-only use 2, later in the kernel.
cfg.restore(layout);
vmovaps(zmm0, ptr[cfg.reg() + 64]);
cfg.free();                   // again: free without store

// Each restore/release pair costs one load and zero stores.
// restore/save would emit a redundant store on every read-only use.
cfg.free();
```

### Notes / Interactions

- **`assert_all_free()` and no-slot aliases** -- no-slot aliases hold their
  register in `live_gp_` for the full kernel duration.  `assert_all_free()`
  checks `live_gp_` for leaks.  Every no-slot alias must therefore have
  `free()` called before `assert_all_free()`.  Slotted aliases in DORMANT
  state have already freed their register via `save()` or `release()` and
  need no extra call.

- **`release()` vs `save()`** -- use `release()` when the register value has
  not changed since the last `save()`, to avoid a redundant store.  Using
  `release()` when the value WAS modified will silently discard the change;
  the programmer is responsible for choosing correctly.

- **Re-prime after `free()`** -- calling `prime()` after `free()` re-allocates
  the register.  For named aliases this re-allocates the same index if it is
  still free; if another allocation has taken it, `prime()` throws.  This is
  not the intended usage pattern; `free()` is an end-of-lifetime call.

- **`StackFrame` dependency** -- `save()` and `restore()` require the
  `StackFrame` built from the same `make_stack_frame()` invocation that
  the alias was registered under.

- **Anonymous aliases and physical register identity** -- for anonymous aliases,
  `reg()` may return a different physical register on each `restore()` call
  depending on what is currently free.  Code that requires a stable register
  index (e.g. constructing addresses from a known base) must use a named
  overload instead.

- **Collision detection timing** -- named slotted aliases detect conflicts at
  `prime()`/`restore()` time.  Named no-slot aliases detect conflicts at
  `declare_alias` time (the second call attempts to `alloc()` an already-live
  register).

---

### Not Implemented: Anonymous APX-aware Allocation

A fifth `declare_alias` overload was considered during design:

```cpp
// NOT IMPLEMENTED
template <class RegT>
ManagedAlias declare_alias(bool prefer_extended);
```

When `prefer_extended=true` and APX is available, this would allocate from
r16-r31 and set `needs_slot=false`.  On non-APX it would fall back to a
slot-backed allocation.

**Why it was not included:**

1. The anonymous overload exists specifically for time-multiplexing -- aliases
   that are dormant some of the time and borrow a register only when active.
   A slotless anonymous alias (permanently live) defeats this purpose.

2. For the permanently-live APX case, `alloc<Reg64>()` already provides the
   same outcome with less API surface.

3. The `needs_slot_=false && desired_idx_==-1` internal state combination it
   would require does not arise from any other overload, adding a special
   branch for a single narrow case.

4. Named APX-aware aliases (`declare_alias(rax, r22)`) cover the important
   real-world case: existing code that hardcodes a legacy register but should
   use a specific APX register when available.

**When to revisit:** if a kernel needs many APX-dedicated aliases and the
specific extended register indices are not important (the kernel wants the
manager to assign r16, r17, r18 automatically), this overload would eliminate
the need to manually assign specific indices.  Add it then, together with an
`alloc_extended<RegT>()` primitive on `RegPoolManager`.

## 27. Allow `make_stack_frame().build()` with no slots (empty layout)

**Status:** Done

**Problem:**

`build()` currently throws `LAYOUT_SLOT_OOB` when the computed frame size is
zero (i.e. no GP park, vec park, scratch, outgoing-args, or alias slots were
declared).  This means callers must always add at least one slot just to get a
valid `StackFrame` -- even when the only purpose of the layout is, for
example, to use `emit_call()` with correct alignment, or to hold a
`ManagedAlias` slot that was the sole reason for calling `build()`.

The `managedAliasReset` test hits this directly: after `reset()` clears
`pending_aliases_`, the test needs a valid layout to verify that no stale alias
data survives into the new build.  It must add `.gp_parks(1)` purely to satisfy
the non-zero check, not because the test logic requires a park slot.

**Proposed change:**

Remove the `if (total == 0)` guard that throws, and instead allow an empty
layout with `total = 0` and a no-op `StackFrame`.  `destroy()` on an
empty layout should emit no `add rsp` instruction (nothing was subtracted).

**Impact on existing tests:**

The `stackLayoutNegativeArgs` test includes `build_empty()` and asserts it
throws.  That assertion would need to be removed or changed to
`CYBOZU_TEST_NO_EXCEPTION` once empty layouts are allowed.

**Considerations:**

- `emit_call()` on a zero-size layout must still emit correct alignment (the
  existing `managed_push_count_`-based logic already handles this independently
  of the frame size).
- `scratch_addr()`, `park()`, `reload()`, `outgoing_arg_addr()`,
  `save_volatiles()`, `restore_volatiles()` on an empty layout should continue
  to throw (slot count is 0, so any index is OOB).
- `clean_stack()` should return `true` on an empty layout (nothing was pushed).

---

## 28. RAII Wrapper for `ManagedAlias`: `ScopedAlias`

### Motivation

`Scoped<RegT>` (returned by `makeScoped()`) calls `rm_.free(reg)` in its destructor
for plain allocated registers.  No equivalent exists for `ManagedAlias`.  Without
one, every `prime()` call requires a matching `release()` or `free()` somewhere in
the code, including on every error path.  The pattern

```cpp
L("error_exit");
alias_a.free();
alias_b.free();
```

is both boilerplate and fragile: adding a third alias to the kernel silently misses
the error-path cleanup unless every such label is updated by hand.

In Phase 3c evaluation, Bug 4 required `alias_aux_A_vpad_bottom.free()` to be
added unconditionally before `store_accumulators()` to ensure correctness on the
FWD_I path.  Idempotent `free()` resolved the safety concern, but a `ScopedAlias`
would have made the ownership intent self-documenting and independent of whether the
alias was actually active at the exit point.

### Proposed API

`ScopedAlias` is a move-only RAII guard that calls `alias.free()` in its destructor
if the alias is still active.  It is constructed by `ManagedAlias::scoped()`, which
also calls `prime()` so that declaration and activation are a single step.

```cpp
// Move-only RAII owner for a ManagedAlias in ACTIVE state.
// Calls alias.free() in its destructor if the alias is still active.
class ScopedAlias {
public:
    explicit ScopedAlias(ManagedAlias &alias) : alias_(&alias) {}

    ~ScopedAlias() {
        if (alias_ && alias_->is_active()) alias_->release();
    }

    ScopedAlias(const ScopedAlias &)            = delete;
    ScopedAlias &operator=(const ScopedAlias &) = delete;
    ScopedAlias(ScopedAlias &&other) noexcept : alias_(other.alias_) {
        other.alias_ = nullptr;
    }

    // Explicit early release -- disarms the destructor.
    // No-op if the alias is already inactive.
    void free() {
        if (alias_ && alias_->is_active()) alias_->release();
        alias_ = nullptr;
    }

private:
    ManagedAlias *alias_;
};

// Factory on ManagedAlias -- prime() the alias and return a RAII guard.
// After this call alias.is_active() == true and the guard owns the release.
ScopedAlias ManagedAlias::scoped();
```

### Internal State Changes

None on `RegPoolManager` or `StackFrame`.  `ScopedAlias` is a thin wrapper around
`ManagedAlias &` with no additional manager state.

### Usage Example -- Error-path cleanup

```cpp
// Without ScopedAlias: every exit path must list all active aliases manually.
alias_src.alloc();
alias_dst.alloc();
if (error_condition) {
    alias_src.free();  // easy to miss when a new alias is added later
    alias_dst.free();
    return;
}
use(alias_src.reg(), alias_dst.reg());
alias_src.free();
alias_dst.free();

// With ScopedAlias: destructor handles free on all paths.
auto g_src = alias_src.scoped();   // alloc() + RAII guard
auto g_dst = alias_dst.scoped();
if (error_condition) return;       // both freed automatically by destructors
use(alias_src.reg(), alias_dst.reg());
// both released when g_src and g_dst go out of scope
```

### Usage Example -- Explicit early release at a save point

```cpp
auto g = alias_binary_params.scoped();  // alloc(); guard armed

// ... use alias_binary_params.reg() to load the binary params pointer ...

// Must save before the register is needed by another alias.
// Call save() explicitly, then disarm the guard (register is already free).
alias_binary_params.save(*sf_main_);  // ACTIVE -> DORMANT; free(reg)
g.free();                          // disarm: alias already inactive, no double-free
```

### Notes / Interactions

- `ScopedAlias` calls `free()`, not `save()`.  It is appropriate only for aliases
  whose value does not need to be persisted at scope exit.  Aliases that must be saved
  before releasing should call `save()` explicitly first; the guard's `free()` then
  becomes a no-op because `is_active()` is already false after `save()`.
- `scoped()` calls `alloc()`.  If the alias is a no-slot alias (§30), `alloc()` throws
  `GP_IN_USE` when another alias holds the same register; the `ScopedAlias` object is
  never returned.  No cleanup is needed in the throwing path.
- For slotted aliases that require `restore()` rather than `alloc()` at activation,
  `ScopedAlias` is not directly applicable.  A `RestoredAlias` variant that calls
  `restore(cl)` in its constructor could be added as a companion, but its destructor
  semantics are less clear (should it `save()` or `release()`?).  This is left as a
  future extension.
- `ScopedVecAlias` (analogous wrapper for `ManagedVecAlias`, §31) should be added
  alongside `ScopedAlias` for consistency.

---

## 29. Post-`build()` `declare_alias()` Detection

### Motivation

All `declare_alias()` calls must appear before `make_stack_frame().build()` because
`build_layout()` assigns stack offsets to pending aliases at build time.  If
`declare_alias()` is called after `build()` -- for example, in a refactoring that adds
a new alias to an existing kernel -- the new alias receives no stack slot.  Subsequent
`save()` and `restore()` calls on that alias either emit code at a wrong address or do
nothing, depending on the implementation.  There is no diagnostic and no runtime
assertion catches the mistake.

Phase 3c evaluation noted this ordering constraint explicitly: all `declare_alias()`
calls must appear before `make_stack_frame().build()` because `build_layout()` assigns
stack offsets to pending aliases at build time.  The current implementation relies on
the developer remembering this rule.

### Proposed Change

No new public API.  The fix is a guard inside all `declare_alias()` overloads that
throws when `build()` has already been called on the current layout.

**New error code** (add to `Xbyak::ErrorList` alongside the other `ERR_RM_*` entries
from §11):

```
ERR_RM_ALIAS_AFTER_BUILD,  // declare_alias() called after make_stack_frame().build()
```

Add the matching string to `ConvertErrorToString`'s `errTbl`:

```
"declare_alias called after StackFrame build -- alias has no stack slot",
```

**Internal state change** (on `RegPoolManager`):

```cpp
// Set to true by StackFrame::build(); cleared by reset().
// Guards all declare_alias() overloads from accepting registrations after
// the layout has been committed.
bool build_done_ = false;
```

**Guard inside all `declare_alias()` overloads** (including `declare_alias(reg, AliasMode::no_slot)`, §30):

```cpp
if (build_done_)
    XBYAK_THROW(ERR_RM_ALIAS_AFTER_BUILD)
pending_aliases_.push_back(...);
```

`reset()` clears `build_done_ = false` along with `pending_aliases_` so that a new
`make_stack_frame().build()` cycle after `reset()` starts clean.

No-slot aliases (§30) do not consume frame space, but they are still subject to the
guard for consistency: allowing some overloads after `build()` while rejecting others
creates a confusing partial contract that is harder to document and test.

### Usage Example -- Incorrect code that the guard catches

```cpp
auto layout = make_stack_frame().gp_parks(2).build();  // build_done_ = true

// Refactoring adds a new alias here -- WRONG ordering.
ManagedAlias alias_new = declare_alias(rax);  // throws ERR_RM_ALIAS_AFTER_BUILD
```

The fix is to move `declare_alias(rax)` before the `make_stack_frame()` call.

### Notes / Interactions

- This guard fires in debug builds (via `XBYAK_THROW`) and is silently swallowed as a
  TLS error in `XBYAK_NO_EXCEPTION` builds.  In release builds, the erroneous alias
  simply has no slot and `save()`/`restore()` produce wrong code -- the same as
  without the guard.  This is the standard debug-only protection model used throughout
  the manager.
- `declare_alias()` overloads that set `needs_slot_ = false` (the `AliasMode::no_slot`
  path) do not modify `pending_aliases_`, so strictly speaking they could be exempt.
  The guard is applied uniformly regardless to keep the rule simple: all alias
  declarations precede `build()`.
- `assert_clean_stack()` does not check `build_done_`.  Its job is to verify stack
  balance, not declaration ordering.  The post-build guard is a separate concern.

---

## 30. Exclusive Sequential Alias: `declare_alias(reg, AliasMode::no_slot)`

### Motivation

Several oneDNN kernels use multiple C++ names for the same physical register with
sequential (never concurrent) roles.  The pattern in `jit_brgemm_conv_comp_pad_kernel`
is representative: `reg_icb` and `reg_aux_comp_out` are both bound to r9 at class
definition time.  They are used in strictly non-overlapping phases and the developer
must mentally track that only one is ever live at a time.

The current single-alloc workaround -- allocate only the primary name, add a comment
-- documents the aliasing but cannot detect if both roles are accidentally activated
concurrently.  The Phase 2 evaluation design suggestion sketched an "alias group"
mechanism with dedicated group registration and a `dissolve_group()` teardown call.

Phase 3c evaluation demonstrated that this complexity is unnecessary.  Two no-slot
`ManagedAlias` objects on the same physical register already get mutual exclusion for
free from the pool's `live_gp_` tracking: when alias_a holds r9 in `live_gp_`, any
attempt to `alloc(r9)` for alias_b throws `GP_IN_USE`.  The only gap is a variant of
`declare_alias` that produces an alias with throw-on-conflict semantics at `prime()`,
distinct from the existing no-slot path which is a no-op when the register is in use
(correct for the APX permanent-hold case, wrong here).

### API Naming: Parameter vs. Separate Function Name

The new behavior could be exposed as either a separate function or a parameterized
overload of the existing `declare_alias` family.  The parameterized approach is
preferred because:

- The `declare_alias(reg)` and `declare_alias(reg, AliasMode::no_slot)` pair expresses
  that both create a single-register alias; the mode selects which backing strategy.
- The second parameter is unambiguously distinct from the existing 2-argument overload
  `declare_alias(primary, alt)`, where the second argument is always a `Reg64`.
  `AliasMode` and `Reg64` are different types; no overload collision occurs.
- The existing `declare_alias(primary, alt, bool)` can be reduced to a one-liner that
  delegates to the two primitives:

```cpp
ManagedAlias declare_alias(const Reg64 &primary, const Reg64 &alt, bool use_alt) {
    return use_alt ? declare_alias(alt,     AliasMode::no_slot)
                   : declare_alias(primary, AliasMode::slotted);
}
```

### `AliasMode` Enum

```cpp
// Controls whether declare_alias(reg, mode) reserves a stack slot.
//
// AliasMode::slotted  -- register is backed by an 8-byte slot in the StackFrame.
//                        Slot is assigned at make_stack_frame().build() time.
//                        save(sf) / restore(sf) emit the stack store/load.
//                        This is the default for declare_alias(reg).
//
// AliasMode::no_slot  -- no stack slot is reserved; no slot assigned at build().
//                        The register is acquired from the pool at prime() time
//                        and returned at release() / free() time.
//                        prime() throws GP_IN_USE if the register is already live,
//                        enforcing mutual exclusion between any two aliases on the
//                        same physical register.
//                        save() and restore() are no-ops.
enum class AliasMode { slotted, no_slot };
```

### Unification with the APX No-Slot Path

The APX pattern previously used distinct implementation semantics: the register was
pre-allocated at `declare_alias` time and held in `live_gp_` permanently, with
`prime()` and `release()` both no-ops.  With the introduction of `AliasMode`,
both the APX permanent-hold pattern and the sequential exclusive pattern are served
by the same `AliasMode::no_slot` implementation.

The 3-arg overload simplifies to a one-liner that delegates to the two primitives:

```cpp
// With AliasMode::no_slot covering both patterns, the 3-arg overload reduces to:
ManagedAlias declare_alias(const Reg64 &primary, const Reg64 &alt, bool use_alt) {
    return use_alt ? declare_alias(alt,     AliasMode::no_slot)
                   : declare_alias(primary, AliasMode::slotted);
}
```

The 2-arg APX convenience shorthand `declare_alias(primary, alt)` (which
hard-coded `has_apx()` internally) is intentionally removed.  The explicit form
`declare_alias(primary, alt, has_apx())` is equally terse and self-documenting.

The behavioral change for the APX path: the register is no longer pre-allocated at
`declare_alias` time.  The developer calls `prime()` once near the top of `generate()`
to acquire it, and `free()` at end-of-kernel before `assert_all_free()`.
For APX extended registers (r16-r31) this is always safe since those registers are
never contested by the slotted path.

The distinction between the two patterns is now purely a usage convention, not an
implementation difference:

| Pattern | prime() / release() usage | Stack slot |
|---|---|---|
| APX permanent hold | `prime()` once at start; `free()` at end-of-kernel | No |
| Sequential exclusive | `prime()` / `release()` per active window | No |

### Proposed API

```cpp
// Declare a named alias with explicit slot/no-slot control.
//
// AliasMode::slotted (default single-register overload behavior):
//   Equivalent to declare_alias(reg) -- backed by a stack slot.
//   Register is not allocated until prime() is called.
//
// AliasMode::no_slot:
//   No stack slot is reserved; make_stack_frame().build() ignores this alias.
//   Register is allocated by prime() and freed by release() or free().
//   prime() throws GP_IN_USE if the register is already held in live_gp_,
//   enforcing mutual exclusion between all aliases on the same physical register.
//   save() and restore() are no-ops.
//   The §29 post-build guard applies: declare before make_stack_frame().build().
ManagedAlias declare_alias(const Xbyak::Reg64 &reg, AliasMode mode = AliasMode::slotted);

// Existing single-register overload (unchanged behavior, calls the above with slotted).
// Kept for backward compatibility and as the idiomatic default.
// ManagedAlias declare_alias(const Xbyak::Reg64 &reg);  // equivalent to mode=slotted

// APX-aware overload now delegates to the two primitives:
ManagedAlias declare_alias(const Xbyak::Reg64 &primary, const Xbyak::Reg64 &alt,
                           bool use_alt) {
    return use_alt ? declare_alias(alt,     AliasMode::no_slot)
                   : declare_alias(primary, AliasMode::slotted);
}
```

### Internal State Changes

`AliasPendingDecl` carries the `RegFamily` field introduced in §31.  No
`exclusive_no_slot` flag is needed: all `AliasMode::no_slot` aliases share the
same `prime()`/`release()` semantics (allocate at prime, throw on conflict, free
at release):

```cpp
struct AliasPendingDecl {
    bool      needs_slot;   // false for AliasMode::no_slot
    int       desired_idx;  // -1: anonymous; >= 0: named register index
    RegFamily family;       // RegFamily::GP (ManagedVecAlias adds RegFamily::Vec)
};
```

`declare_alias(reg, AliasMode::no_slot)` sets `needs_slot = false`,
`desired_idx = reg.getIdx()`, `family = RegFamily::GP`.

### Implementation -- `prime()` for the no-slot path

With the unified `AliasMode::no_slot` semantics, the APX-specific no-op branch is
removed.  `prime()` always throws when the register is contested:

```cpp
void ManagedAlias::prime() {
    if (!needs_slot_) {
        if (!is_active_) {
            if (!rm_->is_available_gp(desired_idx_))
                XBYAK_THROW(ERR_RM_GP_IN_USE)  // mutual exclusion enforced
            reg_       = rm_->alloc<Xbyak::Reg64>(desired_idx_);
            is_active_ = true;
        }
        // else: already active on this alias -- re-prime is a no-op
        return;
    }
    // ... slotted path unchanged ...
}
```

### Implementation -- `release()` for the no-slot path

```cpp
void ManagedAlias::release() {
    if (!needs_slot_) {
        if (is_active_) {
            rm_->free(reg_);
            is_active_ = false;
        }
        return;
    }
    // ... slotted path ...
}
```

### Usage Example -- Sequential GP aliases in a comp-pad kernel

```cpp
// Declaration (class members in the .hpp; no StackFrame slot consumed).
ManagedAlias alias_icb      = declare_alias(r9, AliasMode::no_slot);
ManagedAlias alias_aux_comp = declare_alias(r9, AliasMode::no_slot);

// In icb_loop(): r9 serves as the icb loop counter.
alias_icb.alloc();                        // alloc r9 from pool
xor_(alias_icb.reg(), alias_icb.reg());  // initialize counter
L("icb_loop_start");
// ... loop body using alias_icb.reg() as r9 ...
dec(alias_icb.reg());
jnz("icb_loop_start");
alias_icb.free();                      // free r9 back to pool

// In store_accumulators(): r9 serves as an auxiliary output pointer.
alias_aux_comp.alloc();                   // alloc r9: safe, alias_icb released it
lea(alias_aux_comp.reg(), ptr[...]);
// ... use alias_aux_comp.reg() as r9 ...
alias_aux_comp.free();

// Bug caught automatically:
alias_icb.alloc();
alias_aux_comp.alloc();  // GP_IN_USE: r9 is in live_gp_ under alias_icb
```

End-of-kernel cleanup (before `assert_all_free()`):

```cpp
alias_icb.free();       // no-op if already released
alias_aux_comp.free();  // no-op if already released
assert_all_free();
```

### Notes / Interactions

- No "group" registration, no `dissolve_group()` call, and no new pool state are
  required.  The mutual exclusion derives entirely from `live_gp_` tracking that the
  pool already performs.  `declare_alias(reg, AliasMode::no_slot)` is the only new
  surface; no additional flag is added to `AliasPendingDecl` or `ManagedAlias`.
- Any number of `AliasMode::no_slot` aliases may be bound to the same physical
  register.  The pool enforces that at most one is active at any time.
- `assert_all_free()` catches a no-slot alias that was primed and never released,
  just as it catches any live GP register.
- `ScopedAlias` (§28) works with `AliasMode::no_slot` aliases: `scoped()` calls
  `prime()` which may throw; the destructor calls `release()` which frees the register.
- `make_stack_frame().build()` ignores `AliasMode::no_slot` aliases (no slot is
  reserved).  The §29 post-build guard still applies: declare before `build()`.
- The APX permanent-hold pattern (§26) now also uses `AliasMode::no_slot` internally
  via `declare_alias(primary, alt, has_apx())`.  The implementation is shared;
  the distinction is purely in usage (single prime/free per kernel vs. per-window
  prime/release cycles).

---

## 31. `ManagedAlias` for Vector Registers: `ManagedVecAlias`

### Motivation

`ManagedAlias` is currently GP-only.  In kernels that call `jit_uni_postops_injector`
with `preserve_vmm = true`, vector registers are saved and restored by the injector
via `register_preserve_guard_t` -- a push-based mechanism separate from
`RegPoolManager`.  Phase 3 evaluation confirmed that unifying those two systems
requires redesigning the injector interface and is out of scope for a register manager
change alone.

However, for kernels that do NOT use the injector (or that control their own vector
save/restore), there is no reason to exclude Zmm/Ymm from the same lifecycle-tracking
safety that GP aliases provide.  A typical scenario: a kernel broadcasts a scale
constant into a ZMM register early in `generate()`, needs to park it to the stack
during a clobber window, and restores it afterwards.  Without `ManagedVecAlias` the
developer must manage a raw `StackFrame` vec-park slot index manually.

### Design

`ManagedVecAlias` mirrors `ManagedAlias` with the same five-operation lifecycle but
operates on `Xbyak::Zmm` (the widest form; callers narrow to `Ymm` or `Xmm` at use
sites if needed) and uses `vmovdqu32` / `vmovdqu` for stack I/O, matching the
instruction selection already used in `StackFrame::park_vec()` and `reload_vec()`.

Slot sizing follows the same rule as `StackFrameBuilder::vec_parks()`: 64 bytes per
slot on AVX-512 hardware, 32 bytes otherwise.

### Proposed API

**New factory overloads on `RegPoolManager`:**

```cpp
// Declare a named vector alias backed by a vec park slot in the StackFrame.
// Slot size: 64 bytes (Zmm) when has_avx512() is true, 32 bytes (Ymm) otherwise.
// A narrower reload (Ymm or Xmm) into a Zmm slot is legal but only the lower
// lanes are meaningful -- the caller is responsible for lane-width consistency.
ManagedVecAlias declare_vec_alias(Xbyak::Zmm reg);
ManagedVecAlias declare_vec_alias(Xbyak::Ymm reg);
ManagedVecAlias declare_vec_alias(Xbyak::Xmm reg);
```

**`ManagedVecAlias` class:**

```cpp
class ManagedVecAlias {
public:
    // DORMANT -> ACTIVE: alloc register, no load.
    // Caller writes the initial value to reg() after calling prime().
    ManagedVecAlias &prime();

    // DORMANT -> ACTIVE: alloc register and load value from slot.
    // Emits vmovdqu32 reg, [rsp+slot]  (Zmm) or  vmovdqu reg, [rsp+slot]  (Ymm/Xmm)
    ManagedVecAlias &restore(StackFrame &cl);

    // ACTIVE -> DORMANT: emit store to slot and free register.
    // Emits vmovdqu32 [rsp+slot], reg  or  vmovdqu [rsp+slot], reg
    ManagedVecAlias &save(StackFrame &cl);

    // ACTIVE -> DORMANT: free register, no store.
    // Slot retains the value from the last save().  Avoids a redundant store on
    // read-only use sites.
    ManagedVecAlias &release();

    // Terminal: free register if active, end alias lifetime.
    // Callable from ACTIVE or DORMANT state.  Must be called before assert_all_free().
    void free();

    // Returns the currently active register (always Zmm; caller narrows as needed).
    // Precondition: is_active() == true.
    const Xbyak::Zmm &reg() const;

    bool is_active() const;
};
```

All lifecycle methods return `ManagedVecAlias &` for chaining, consistent with
`ManagedAlias` after the Phase 3c chainable-methods improvement.

**`ScopedVecAlias`** -- RAII companion (see §28):

```cpp
class ScopedVecAlias {
public:
    explicit ScopedVecAlias(ManagedVecAlias &alias) : alias_(&alias) {}
    ~ScopedVecAlias() { if (alias_ && alias_->is_active()) alias_->release(); }
    ScopedVecAlias(const ScopedVecAlias &)            = delete;
    ScopedVecAlias &operator=(const ScopedVecAlias &) = delete;
    ScopedVecAlias(ScopedVecAlias &&other) noexcept : alias_(other.alias_) {
        other.alias_ = nullptr;
    }
    void free() {
        if (alias_ && alias_->is_active()) alias_->release();
        alias_ = nullptr;
    }
private:
    ManagedVecAlias *alias_;
};

ScopedVecAlias ManagedVecAlias::scoped();  // prime() + RAII guard
```

### Internal State Changes

`AliasPendingDecl` (introduced in §26, extended in §30) carries the `RegFamily` field
that `build_layout()` reads to assign the correct slot size:

```cpp
struct AliasPendingDecl {
    bool      needs_slot;
    int       desired_idx;
    RegFamily family;   // RegFamily::GP or RegFamily::Vec
};
```

`StackFrame` internal layout after `build_layout()` processes both GP and Vec alias
declarations:

```
[rsp + 0       ]  GP alias slots     (8 bytes each; from declare_alias() calls)
[rsp + A       ]  Vec alias slots    (32 or 64 bytes each, 64-byte aligned)
[rsp + A+B     ]  GP park slots      (from gp_parks(n))
[rsp + A+B+C   ]  Vec park slots     (from vec_parks(n))
[rsp + ...     ]  volatile save area (from with_volatile_save())
[rsp + ...     ]  scratch            (from scratch(bytes))
[rsp + ...     ]  outgoing args      (from with_outgoing_args(n))
```

### Usage Example -- Scale constant parked across a clobber window

```cpp
class ScaleKernel : public Xbyak::CodeGenerator,
                    public Xbyak::RegPoolManager {
    ManagedVecAlias alias_scale;   // default-constructible class member

    void generate() {
        alias_scale = declare_vec_alias(zmm0);  // zmm0 with a 64-byte stack slot

        auto sf = make_stack_frame()
            .with_volatile_save()
            .build();              // alias_scale slot allocated inside build()

        emit_prologue();

        // Broadcast the scale constant from the params pointer.
        alias_scale.alloc();
        vbroadcastss(alias_scale.reg(), ptr[rdi + offsetof(params_t, scale)]);
        alias_scale.save(sf);      // store to slot; zmm0 free for clobber window

        // --- clobber window: zmm0 used freely for computation ---
        auto zmm_tmp = alloc<Zmm>();  // may return zmm0
        // ... use zmm_tmp ...
        free(zmm_tmp);

        // --- use the scale constant again ---
        alias_scale.restore(sf);   // reload zmm0 from slot
        // ... use alias_scale.reg() ...
        alias_scale.free();     // read-only: slot still valid, no store needed

        alias_scale.free();
        sf.destroy();
        emit_epilogue();
        ret();
        assert_all_free();
        assert_clean_stack();
    }
};
```

### Limitation: injector boundary

`ManagedVecAlias` does NOT address the `preserve_vmm = true` injector scenario.
That scenario requires the injector to participate in the pool protocol at the
call boundary.  `ManagedVecAlias` covers only kernels that own their own vector
save/restore path without relying on `register_preserve_guard_t`.  For
injector-using kernels, the existing `preserve_vmm = true` push/pop mechanism
remains the correct approach until the injector interface is redesigned.

### Notes / Interactions

- The store/load instruction emitted by `save()` and `restore()` matches the slot
  width selected at `declare_vec_alias()` time (Zmm -> `vmovdqu32`; Ymm/Xmm ->
  `vmovdqu`).  Accessing the slot with a narrower type than declared is legal but
  only the lower bits are meaningful; the caller is responsible for lane-width
  consistency.
- `assert_all_free()` checks `live_vec_` for leaks.  Every `ManagedVecAlias` that
  was primed must have `free()` called before `assert_all_free()`.
- Opmask registers (`k1`-`k7`, 8 bytes each) could follow the same pattern with a
  `ManagedOpmaskAlias` variant.  Their use in save/restore scenarios is rare enough
  that this extension is deferred until a concrete kernel use case emerges.
- `declare_vec_alias_no_slot()` -- an exclusive-sequential variant for vector registers
  analogous to §30 -- is not proposed here.  The pattern of two C++ names sharing one
  physical vector register with sequential roles has not appeared in evaluated kernels.
  Add if a concrete use case is identified.
