# Session Summary — reg_manager_test

## Overview

This session translated the demo/sample program
`xbyak/sample/test_xbyak_reg_manager.cpp` into a proper unit test using the
**Cybozu test framework** (the same framework used by all other tests in
`xbyak/test/`).  Along the way a CMake build script and `readme.md` were also
created for the entire test directory, and a bug in `xbyak_reg_manager.hpp`
was fixed.

---

## Files Created

### `xbyak/test/CMakeLists.txt` (new)
A CMake build script that mirrors the style of `xbyak/sample/CMakeLists.txt`.

Key features:
- Builds every test binary: `bad_address`, `misc`, `misc32`, `cvt_test`,
  `cvt_test32`, `noexception`, `jmp64`, `address64`, `apx`, `avx10_test`,
  `sf_test`, `cpumask_test`, `reg_manager_test`, `normalize_prefix`
  (placed directly in the source tree so shell scripts find it as
  `./normalize_prefix`), `detect_x32`, and `lib_run`.
- Registers all standalone binaries as **CTest** tests.
- Auto-detects `nasm`/`yasm`/`awk` and registers the shell-script
  assembler-comparison tests when those tools are present.
- Auto-detects `xed`/`python3` and registers per-dataset XED validation
  tests when available.
- `BUILD_32BIT_TARGETS=ON` option enables 32-bit `jmp`/`address` binaries
  (requires `g++-multilib` on Linux 64-bit).

### `xbyak/test/readme.md` (new)
Documentation styled after `xbyak/sample/readme.md`.  Covers:
- How the tests work (binary-level encoding comparison).
- **CMake** build and `ctest` workflow (new).
- **Make** workflow (existing Linux/macOS method).
- **Batch file** workflow (existing Windows method).
- Linux vs. Windows comparison table.
- Troubleshooting section.

### `xbyak/test/reg_manager_test.cpp` (new)
Unit test for `xbyak/xbyak/xbyak_reg_manager.hpp` using the Cybozu framework.

16 test modules covering the same ground as tests 1–16 of the sample program
(tests 17–21 from the sample, which require `get_in_use_volatile_*` /
`get_in_use_preserved_*` methods that are not yet in the `main` branch header,
were intentionally omitted):

| Module | What it tests |
|---|---|
| `basicAllocation` | `alloc<>()` / `free()` round-trip for GP registers |
| `specificAllocation` | Named-index `alloc<>(idx)` + duplicate-alloc exception |
| `scopedRegisters` | RAII `makeScoped()` — auto-free on scope exit |
| `vectorRegisters` | `alloc` / `free` of `Xmm` / `Ymm` / `Zmm` |
| `opmaskRegisters` | `alloc` / `free` of `Opmask` (k1-k7; k0 is the "unmasked" sentinel) |
| `apxSupport` | `max_gp_registers()` reflects APX CPU capability |
| `addToPool` | `_stack_pointer()`, `_base_pointer()`, `add_to_gp_pool()` exceptions |
| `registerExhaustion` | Exhausting the pool throws; correct count allocated |
| `mixedAllocation` | Mix of GP / Vec / Opmask in one manager |
| `regInUse` | `reg_in_use()` / `gp_idx_in_use()` helpers |
| `gpRegisterAliasing` | Reg64/Reg32/Reg16 share one physical-register index |
| `vectorRegisterAliasing` | Xmm/Ymm/Zmm share one physical-register index |
| `registerContentsViaJIT` | Write + read register values through JIT execution |
| `functionCallConvention` | ABI parameter passing + non-volatile register preservation across a JIT call |
| `realisticKernel` | JIT code generation using the manager as the register strategy |
| `dynamicSaveRestore` | Dynamic `push`/`pop` using `get_in_use_gps()` / `gp_idx_in_use()` around a real C function call |

---

## Files Modified

### `xbyak/xbyak/xbyak_reg_manager.hpp`
**Bug fix**: namespace typo in the `RegPoolManager` constructor.

The header referenced `Xbyak::Util::Cpu` (capital U) at three points, but
the actual namespace declared in `xbyak_util.h` is `Xbyak::util::Cpu`
(lowercase u).  This caused three `'Xbyak::Util' has not been declared`
compiler errors.

```cpp
// Before (wrong — capital U):
if (cpu.has(Xbyak::Util::Cpu::tOSXSAVE)) { ...
if (cpu.has(Xbyak::Util::Cpu::tAPX_F))   { ...
if (cpu.has(Xbyak::Util::Cpu::tAVX512F)) { ...

// After (correct — lowercase u):
if (cpu.has(Xbyak::util::Cpu::tOSXSAVE)) { ...
if (cpu.has(Xbyak::util::Cpu::tAPX_F))   { ...
if (cpu.has(Xbyak::util::Cpu::tAVX512F)) { ...
```

### `xbyak/test/CMakeLists.txt`
- Added `reg_manager_test` build target (64-bit only).
- Added `reg_manager_test` CTest registration.
- Removed `-DXBYAK64` from `reg_manager_test`'s compile flags —
  `xbyak_reg_manager.hpp` already does `#define XBYAK64` internally, so
  passing it on the command line caused a harmless but noisy redefinition
  warning.

### `xbyak/test/Makefile`
- Added `reg_manager_test` to the 64-bit `TARGET` list.
- Added build rule:
  ```makefile
  reg_manager_test: reg_manager_test.cpp $(XBYAK_INC)
      $(CXX) $(CFLAGS) $< -o $@
  ```
  `Makefile.win` was intentionally left unchanged — its `all` target only
  generates `xbyak_mnemonic.h`; it does not build standalone test binaries.

---

## Bugs Fixed in `reg_manager_test.cpp`

Three logic bugs were found and fixed during testing:

### 1. `addToPool` — wrong index for exception test
The test called `rm.add_to_gp_pool(4)` expecting a throw, reasoning that
`_stack_pointer()` had put index 4 into `used_gp`.  However,
`add_to_gp_pool()` only checks `free_gp_regs`, `preserved_gp`, and
`in_use_gp` — it does not check `used_gp`, so it did not throw.

**Fix**: Changed to indices that are always tracked in the expected sets:
- `add_to_gp_pool(0)` — rax is always in `free_gp_regs` (caller-saved on
  both Windows and Linux).
- `add_to_gp_pool(3)` — rbx is always in `preserved_gp` (callee-saved on
  both Windows and Linux).

### 2. `realisticKernel` — callee-saved register clobbered → segfault
`gen_kernel_with_manager()` allocated a callee-saved register (e.g. rbx or
r12 depending on platform) and wrote a value into it without saving the
original value first.  This corrupted the C++ runtime's view of that register
after the JIT function returned, causing a segfault.

**Fix**: Added `push(state_reg)` before writing to the preserved register and
`pop(state_reg)` before `ret()`.

### 3. `dynamicSaveRestore` — callee-saved register clobbered → segfault
`gen_caller_saves_all()` used `push rax` to stash the real function's return
value, then `pop rbx` to restore it.  This left rbx (a callee-saved register)
holding a garbage value, again corrupting the C++ runtime.

**Fix**:
- Added `push(rbx)` / `pop(rbx)` as a proper prologue/epilogue.
- Stash the function's return value in the now-saved rbx directly
  (`mov(rbx, rax)`) rather than using an extra stack slot, keeping the
  push/pop accounting balanced.

---

## Final Test Result

```
ctest:name=reg_manager_test, module=16, total=58, ok=58, ng=0, exception=0
```
