# Xbyak Test Suite

This directory contains the tests for the [Xbyak](https://github.com/herumi/xbyak) JIT assembler library.

## How the Tests Work

The core approach is **binary-level encoding verification**:

1. A code generator (`make_nm`, `make_512`, `address`) outputs an assembly listing for its instruction set.
2. A reference assembler (`nasm` or `yasm`) assembles that listing into binary and records the encoding.
3. Xbyak JIT generates the same instructions at runtime and records its encodings.
4. `diff` compares the two outputs — any mismatch is a failure.

Some tests (`bad_address`, `misc`, `jmp`, `apx`, etc.) are self-contained executables that run without an assembler.

---

## Test Categories

| Category | Scripts / Binaries | What it tests |
|---|---|---|
| **Mnemonic encoding** | `test_nm.sh` / `test_nm.bat` | All general instructions, 32/64-bit, nasm & yasm |
| **AVX encoding** | `test_avx.sh` / `test_avx.bat` | AVX instruction encodings |
| **AVX-512 encoding** | `test_avx512.sh` / `test_avx512.bat` | AVX-512 instruction encodings |
| **Address encoding** | `test_address.sh` / `test_address.bat` | Memory addressing modes |
| **Jump encoding** | `jmp` / `jmp64` | Short/near/far jump encodings |
| **Misc** | `misc` / `misc32` | Miscellaneous instruction cases |
| **Conversion** | `cvt_test` / `cvt_test32` | Conversion instruction encodings |
| **APX** | `apx` | Intel Advanced Performance Extensions |
| **AVX10** | `avx10_test` | AVX10 instructions |
| **XED validation** | `test_by_xed.sh` / `test_by_xed.bat` | Encoding validation via Intel XED |
| **Bad addressing** | `bad_address` | Error handling for invalid addresses |
| **No-exceptions** | `noexception` | Compile/run with exceptions disabled |
| **Scalable flags** | `sf_test` | Scalable feature flags (64-bit) |
| **CPU mask** | `cpumask_test` | CPU mask utilities (64-bit) |

---

## Building with CMake (Recommended)

CMake provides a unified build and test workflow for Linux, macOS, and Windows.

### Prerequisites

**Linux / macOS:**
```bash
# Compiler (64-bit; add g++-multilib for optional 32-bit tests)
sudo apt install g++

# Assemblers (needed for assembler-comparison tests)
sudo apt install nasm yasm

# Standard tools (usually pre-installed)
sudo apt install awk diffutils
```

**Windows (Developer Command Prompt — x64 or x86 Native Tools):**
- MSVC `cl.exe` — provided by Visual Studio
- `nasm.exe` / `yasm.exe` — [nasm.us](https://www.nasm.us), add to `PATH`
- `python3` — for XED-based tests
- `awk`, `diff` — from Git for Windows, GnuWin32, or Cygwin

### Basic Build

```bash
# From the xbyak/test directory:
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

Executables land in `build/bin/`.  `normalize_prefix` is placed directly in
`xbyak/test/` (the source directory) so that the shell-script tests can find it
as `./normalize_prefix` without any path changes.

### Run All Tests with CTest

```bash
cd build
ctest --output-on-failure
```

### Run Tests Verbosely

```bash
ctest -V
```

### Run a Specific Test or Group

```bash
# Run a single named test
ctest -R bad_address

# Run all mnemonic tests
ctest -R test_nm

# Run all assembler-comparison tests
ctest -R "test_nm|test_avx|test_address"

# Run only standalone binary tests (no assembler needed)
ctest -R "bad_address|misc|jmp|cvt_test|apx|avx10|sf_test|cpumask"
```

### Clean Build

```bash
# Incremental clean
cmake --build . --target clean

# Full clean (recommended when switching configurations)
cd ..
rm -rf build
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

### CMake Options

| Option | Default | Description |
|---|---|---|
| `BUILD_32BIT_TARGETS` | `OFF` | Build 32-bit `jmp` and `address` binaries (Linux 64-bit only, requires `g++-multilib`) |

```bash
# Example: enable 32-bit targets
cmake -DBUILD_32BIT_TARGETS=ON ..
cmake --build . --config Release
ctest
```

### Building Specific Targets

```bash
# Build only normalize_prefix and bad_address
cmake --build . --target normalize_prefix bad_address

# Build only the 64-bit standalone binaries
cmake --build . --target jmp64 apx avx10_test sf_test cpumask_test
```

### Windows CMake Build

Run from **x64 Native Tools Command Prompt** for Visual Studio:

```bat
mkdir build
cd build
cmake ..
cmake --build . --config Release
ctest -C Release --output-on-failure
```

Standalone binary tests run automatically via CTest.  Assembler-comparison
tests are skipped on Windows when using this CMake workflow — use the `.bat`
files described below for full Windows assembler testing.

---

## Building with Make (Linux / macOS — Existing Method)

The original `Makefile` is still fully supported:

```bash
# Install 32-bit multilib support (optional, Ubuntu/Debian)
sudo apt install g++-multilib

# Build all test binaries
make

# Run the full test suite
make test         # runs test_nm + test_avx + test_avx512

# Run individual test groups
make test_nm      # mnemonic encoding tests (all variants)
make test_avx     # AVX instruction tests
make test_avx512  # AVX-512 instruction tests
make test_avx10   # AVX10 (runs ./avx10_test directly)
make xed_test     # XED-based encoding validation (requires xed + python3)

# Clean all built files
make clean
```

### Run Individual Shell Scripts Directly

```bash
# Mnemonic tests
./test_nm.sh              # nasm, 32-bit
./test_nm.sh 64           # nasm, 64-bit
./test_nm.sh Y            # yasm, 32-bit
./test_nm.sh Y64          # yasm, 64-bit
./test_nm.sh noexcept     # nasm, 32-bit, XBYAK_NO_EXCEPTION
./test_nm.sh avx512       # nasm, 64-bit + AVX-512

# AVX tests
./test_avx.sh             # nasm, 32-bit
./test_avx.sh 64          # nasm, 64-bit
./test_avx.sh Y           # yasm, 32-bit
./test_avx.sh Y64         # yasm, 64-bit

# AVX-512 tests
./test_avx512.sh          # nasm, 32-bit
./test_avx512.sh 64       # nasm, 64-bit

# Address encoding tests
./test_address.sh         # 32-bit
./test_address.sh 64      # 64-bit

# XED validation (one dataset file at a time)
./test_by_xed.sh dataset/old.txt
./test_by_xed.sh dataset/comp.txt
```

### Run Standalone Binaries Directly

These require no external assembler:

```bash
./bad_address       # invalid address error handling
./misc              # miscellaneous (64-bit mode)
./misc32            # miscellaneous (32-bit mode)
./cvt_test          # conversion instructions (64-bit)
./cvt_test32        # conversion instructions (32-bit)
./jmp               # jump encoding (32-bit binary, needs multilib)
./jmp64             # jump encoding (64-bit)
./apx               # APX instructions (64-bit only)
./avx10_test        # AVX10 instructions (64-bit only)
./sf_test           # scalable flags (64-bit only)
./cpumask_test      # CPU mask utilities (64-bit only)
```

---

## Windows — Existing Batch File Method

Run from a **Visual Studio Developer Command Prompt** with `nasm.exe`/`yasm.exe` on `PATH`.

The batch files use `bmake -f Makefile.win` to generate the xbyak mnemonic header
before each test, and `cl.exe` to compile test sources.

### Run All Tests

```bat
cd xbyak\test
test_all.bat
```

`test_all.bat` runs in sequence:
1. `test_nm_all.bat` — all mnemonic variants (nasm 32/64, yasm 32/64) + all AVX
2. `test_address.bat` / `test_address.bat 64` — address encoding
3. `test_jmp.bat` — jump encoding
4. `test_misc.bat` with `misc`, `apx`, `avx10_test`

### Run Individual Batch Scripts

| Command | Description |
|---|---|
| `test_nm.bat` | nasm 32-bit mnemonic test |
| `test_nm.bat 64` | nasm 64-bit |
| `test_nm.bat Y` | yasm 32-bit |
| `test_nm.bat Y64` | yasm 64-bit |
| `test_nm.bat noexcept` | nasm 32-bit, no exceptions |
| `test_nm_all.bat` | All of the above + AVX variants |
| `test_avx.bat` / `test_avx.bat 64` | AVX encoding, 32/64-bit |
| `test_avx_all.bat` | All AVX + AVX-512 variants |
| `test_avx512.bat` / `test_avx512.bat 64` | AVX-512 encoding |
| `test_address.bat` / `test_address.bat 64` | Address encoding |
| `test_jmp.bat` | Jump encoding |
| `test_misc.bat` (with `set FILE=misc`) | Misc standalone test |
| `test_misc.bat` (with `set FILE=apx`) | APX standalone test |
| `test_misc.bat` (with `set FILE=avx10_test`) | AVX10 standalone test |

```bat
REM Example: run a specific misc test
set FILE=apx
call test_misc.bat
```

### XED tests on Windows

```bat
set FILE=dataset\old.txt
call test_by_xed.bat %FILE%
call test_by_xed_all.bat
```

---

## Linux vs. Windows Comparison

| Aspect | Linux / macOS | Windows |
|---|---|---|
| **Compiler** | `g++` (via `$CXX`) | `cl.exe` (MSVC) |
| **Build tool** | `make` or `cmake` | `bmake -f Makefile.win` or `cmake` |
| **Test driver** | `make test` or `ctest` | `test_all.bat` or `ctest` |
| **Assemblers** | `nasm`, `yasm` | `nasm.exe`, `yasm.exe` |
| **32-bit support** | Via `-m32` / `g++-multilib` | Via x86 Native Tools prompt |
| **normalize_prefix** | `./normalize_prefix` (built by cmake/make) | `normalize_prefix.exe` (built by bmake) |

---

## Troubleshooting

### CMake: assembler tests are skipped
Ensure `nasm` and `awk` are installed and on `PATH`:
```bash
which nasm awk
sudo apt install nasm
```

### CMake: `normalize_prefix` not found when running shell script tests
`normalize_prefix` must be built before running assembler tests.  It is
output to `xbyak/test/normalize_prefix` (the source directory).  Build it
explicitly if needed:
```bash
cmake --build . --target normalize_prefix
```

### Linux: 32-bit build errors (`cannot find -lstdc++` or `-m32` failure)
Install multilib support:
```bash
sudo apt install g++-multilib gcc-multilib
cmake -DBUILD_32BIT_TARGETS=ON ..
```

### Windows: `bmake` not found
`bmake` (BSD make) is required by the `.bat` test scripts.  Install it or
use the CMake workflow instead (which uses MSVC directly without `bmake`):
```bat
mkdir build && cd build
cmake ..
cmake --build . --config Release
ctest -C Release
```

### Windows: `awk`/`diff` not found
Install [Git for Windows](https://git-scm.com/download/win) (includes `awk`, `diff`, `cat`, `rm`)
or add [GnuWin32](http://gnuwin32.sourceforge.net/) to `PATH`.

### XED tests: `xed` command not found
Download the Intel XED tool from the
[Intel XED repository](https://github.com/intelxed/xed) and add it to `PATH`.
Set the `XED` environment variable to override the binary name:
```bash
XED=/path/to/xed ctest -R xed_test
# or
XED=/path/to/xed make xed_test
```

---

## Notes

- **Intermediate files**: The assembler comparison shell scripts produce
  temporary files (`a.asm`, `a.lst`, `ok.lst`, `x.lst`, `nm.cpp`, `nm_frame`)
  in the `xbyak/test/` source directory — the same behavior as `make test`.
  Run `make clean` to remove them.
- **normalize_prefix**: Built into the source directory (`xbyak/test/`) rather
  than `build/bin/` so that the unmodified shell scripts find it as
  `./normalize_prefix`.
- **64-bit vs. 32-bit defines**: `XBYAK64` / `XBYAK32` preprocessor macros
  control which instruction set the code generators emit; `misc32` and
  `cvt_test32` use `-DXBYAK32` but are compiled as 64-bit binaries (no `-m32`
  required).
- **macOS**: 32-bit binaries are not supported; only 64-bit targets are built.
