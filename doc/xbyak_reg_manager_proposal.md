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
---

## Implementation Status

- [x] 1. Bug Fix: XCR0 Operator Precedence
- [ ] 2. Performance: Replace `std::set` with Bitmask Pools
- [x] 3. Pool Management: `add_to_vec_pool()` and `add_to_opmask_pool()` *(REJECTED — see §3)*
- [x] 4. Register Reservation: `mark_unavailable()` / `mark_available()`
- [x] 5. Pinned Registers: `pin()` and `alloc_pinned()` *(REJECTED — see §5)*
- [x] 6. Code Emission Coupling: `set_code_generator()`
- [x] 7. Register Spill / Restore: `spill()` and `restore()`
- [x] 8. Stack Frame Management: `StackFrame` RAII Helper
- [x] 9. Preserved Register Tracking: `emit_prologue()` / `emit_epilogue()`
- [x] 10. End-of-JIT Validation: `assert_all_free()`
- [x] 11. Error Handling: Align with `XBYAK_THROW` / `Xbyak::Error`
- [x] 12. Named-Register `alloc()` Overload
- [ ] 13. Remove `used_*` Sets
- [x] 14. In-Use Volatile / Preserved Getters
- [ ] 15. ABI Configuration: Windows x64 vs SysV
- [ ] 16. Manager `reset()` to Complement `CodeGenerator::reset()`
- [ ] 17. Accept External `Xbyak::util::Cpu` Reference
- [x] 18. `emit_call()` — ABI-correct Outgoing Calls (Shadow Space + Alignment)

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

---

## 15. ABI Configuration: Windows x64 vs SysV

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
// func_ptr       — address of the target function (loaded into a scratch register
//                  by the manager; rax is used as a scratch register internally).
// extra_pushes   — number of manual push instructions the caller emitted that
//                  the manager has not tracked (default: 0).  Used to compute
//                  the correct alignment and shadow space adjustment.
//
// What emit_call() emits (Win64):
//   sub  rsp, adj        ; adj = 32 + (8 if alignment pad needed)
//   mov  rax, func_ptr
//   call rax
//   add  rsp, adj
//
// What emit_call() emits (SysV):
//   [sub rsp, 8]         ; only if alignment pad needed
//   mov  rax, func_ptr
//   call rax
//   [add rsp, 8]         ; matching restore
//
// Returns: nothing.  The caller reads the return value from rax as usual.
//
// Throws ERR_RM_NO_CG if cg_ is null.
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

- `emit_call()` uses `rax` as a scratch register to load the function address
  (matching the pattern the tests already use). Since `rax` is volatile on both
  ABIs, this is always safe — the callee is free to clobber it anyway.
- If §9 (`emit_prologue`) is in use, the manager already knows how many callee-save
  pushes it emitted; `managed_push_count_` tracks this. The `extra_pushes` parameter
  covers any additional manual pushes the caller emits outside of manager control
  (e.g. `push(rbx)` to stash a return value).
- On SysV, the most common call sites after `emit_prologue()` will have
  `extra_pushes == 0` and an odd `managed_push_count_`, meaning no adjustment is
  needed at all and `emit_call()` reduces to a plain `call rax` with no `sub`/`add`.
- The function pointer is loaded via `mov rax, imm64` even on SysV. For
  position-independent code compiled with `-fPIC`, the target function must be
  reachable via an absolute address; this is always the case for statically-linked
  helper functions and PLT-resolved library symbols whose abs address is known at
  JIT construction time.

---

## Summary Table

| # | Change | API Impact | New Data Members | Requires §6 |
|---|---|---|---|---|
| 1 | XCR0 precedence fix | None (bug fix) | None | No |
| 2 | Bitmask pools | None (internal) | `uint32_t` bitmasks | No |
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
| 13 | Remove `used_*` sets | 3 methods removed (breaking) | 3 sets removed | No |
| 14 | In-use volatile/preserved getters | 6 new methods | 6 `const` initial masks | No |
| 15 | ABI configuration (Windows / SysV) | Constructor parameter | `abi_` enum member | No |
| 16 | Manager `reset()` | 1 new method | None | No |
| 17 | External `Cpu` constructor overload | 1 new constructor overload | None | No |
| 18 | `emit_call()` — ABI-correct outgoing calls | 1 new method (+ template overload) | `managed_push_count_` | Yes (§6) |

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
11. §2 (bitmask optimisation — internal only, do last when API is stable)
12. §12 (named-register alloc — pure convenience, no dependencies, add anytime)
13. §13 (remove `used_*` — breaking removal, do after §9 is in place so callers have a replacement)
14. §15 (ABI configuration — must precede §14; `populate_pools(abi)` determines which registers are volatile vs preserved, so §14's `const` initial masks must be captured after this is in place)
15. §17 (external `Cpu` overload — alongside §15; both converge on the same `init_from_cpu()` helper)
16. §14 (in-use volatile/preserved getters — comes after §15 so the `const` initial masks correctly reflect ABI-aware pool composition)
17. §16 (manager `reset()` — add last; clears all additions from §4, §5, §7, §9 and re-invokes `populate_pools(abi_)` from §15)

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
