/*******************************************************************************
* Copyright 2026 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/
#ifndef XBYAK_REG_MANAGER_HPP
#define XBYAK_REG_MANAGER_HPP

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>
#include <type_traits>

#ifndef XBYAK64
#  define XBYAK64
#endif
#ifndef XBYAK_NO_OP_NAMES
#  define XBYAK_NO_OP_NAMES
#endif
#include "xbyak/xbyak.h"
#include "xbyak/xbyak_util.h"

namespace Xbyak {
// Static definitions for different types of registers in relation to which family they belong to.
enum class RegFamily { GP, Vec, Opmask, Tile };

template <class RegT>
struct reg_family;

// General Purpose register families (8-bit, 16-bit, 32-bit, 64-bit)
template <>
struct reg_family<Reg8> {
    static constexpr RegFamily value = RegFamily::GP;
};
template <>
struct reg_family<Reg16> {
    static constexpr RegFamily value = RegFamily::GP;
};
template <>
struct reg_family<Reg32> {
    static constexpr RegFamily value = RegFamily::GP;
};
template <>
struct reg_family<Reg64> {
    static constexpr RegFamily value = RegFamily::GP;
};

// Vector register families (XMM, YMM, ZMM)
template <>
struct reg_family<Xmm> {
    static constexpr RegFamily value = RegFamily::Vec;
};
template <>
struct reg_family<Ymm> {
    static constexpr RegFamily value = RegFamily::Vec;
};
template <>
struct reg_family<Zmm> {
    static constexpr RegFamily value = RegFamily::Vec;
};

// Opmask registers (k0-k7)
template <>
struct reg_family<Opmask> {
    static constexpr RegFamily value = RegFamily::Opmask;
};

// AMX tile registers (tmm0-tmm7)
template <>
struct reg_family<Tmm> {
    static constexpr RegFamily value = RegFamily::Tile;
};

class RegPoolManager {
public:
    // Constructs the manager and populates register pools based on the CPU features
    // (APX, AVX-512, AMX) reported by the provided Xbyak::util::Cpu object.
    //
    // cpu — CPU feature query object.  Construct it once per process and share it
    //       across all kernel instances to avoid redundant CPUID overhead:
    //         Xbyak::util::Cpu cpu;
    //         MyKernelA a(cpu);
    //         MyKernelB b(cpu);
    //
    // cg  — optional pointer to the CodeGenerator into which instructions will be
    //       emitted by emit_prologue(), emit_epilogue(), and make_stack_layout().
    //       Pass NULL (default) when only register tracking is
    //       required; the manager then operates with no emission overhead.
    //
    // Composition pattern (RegPoolManager as a member):
    //   class MyKernel : public Xbyak::CodeGenerator {
    //       Xbyak::RegPoolManager rm_;
    //   public:
    //       MyKernel(const Xbyak::util::Cpu &cpu)
    //           : Xbyak::CodeGenerator(4096), rm_(cpu, this) {}
    //   };
    //
    // Direct-inheritance pattern (RegPoolManager IS-A CodeGenerator):
    //   class MyKernel : public Xbyak::CodeGenerator,
    //                    public Xbyak::RegPoolManager {
    //   public:
    //       MyKernel(const Xbyak::util::Cpu &cpu)
    //           : Xbyak::CodeGenerator(4096),
    //             Xbyak::RegPoolManager(cpu, this) {}
    //   };
    explicit RegPoolManager(const Xbyak::util::Cpu &cpu,
            Xbyak::CodeGenerator *cg = NULL)
            : prologue_gp_cursor_(0),
              prologue_vec_cursor_(0),
              managed_push_count_(0),
              allocated_stack_space_(0),
              cg_(cg) {
        uint64_t xcr0 = 0;
        if (cpu.has(Xbyak::util::Cpu::tOSXSAVE)) {
            xcr0 = cpu.getXfeature();
        }

        // Detect APX for extended GP registers (r16-r31)
        if (cpu.has(Xbyak::util::Cpu::tAPX_F)) {
            has_apx_ = ((xcr0 >> 19) & 1) == 1; // Check if XCR0[19] is set for APX support
        }
        max_gp_reg_idx_ = has_apx_ ? 31 : 15;

        // Opmask registers (k1-k7) require AVX-512F and XCR0[5] (OPMASK state).
        // Full ZMM support (zmm0-31) additionally requires XCR0[6] (ZMM_Hi256)
        // and XCR0[7] (Hi16_ZMM = zmm16-31).
        if (cpu.has(Xbyak::util::Cpu::tAVX512F)) {
            has_opmask_ = ((xcr0 >> 5) & 1) == 1; // XCR0[5]: OPMASK state (k registers)
            has_avx512_ = ((xcr0 >> 5) & 7) == 7; // XCR0[5:7] all set for full ZMM support
        }
        max_vec_reg_idx_ = has_avx512_ ? 31 : 15;

        // If APX is available, add r16-r31 to the free pool
        // APX registers r16-r31 are caller-saved (call-clobbered)
        if (has_apx_) {
            for (int i = 16; i <= 31; ++i) {
                free_gp_regs.insert(i);
            }
        }

        // XCR0[1] = SSE state (XMM registers), XCR0[2] = AVX state (YMM upper half)
        // Both must be OS-enabled before vector registers can be safely used
        const bool has_vec_base = ((xcr0 >> 1) & 3) == 3;
        if (has_vec_base) {
            free_vec_regs = base_free_vec();
            preserved_vec = base_preserved_vec();
        }
        has_vec_base_ = has_vec_base;

        // If AVX-512 is available, add zmm16-zmm31 to the free pool
        // Extended vector registers are caller-saved (call-clobbered)
        if (has_avx512_) {
            for (int i = 16; i <= 31; ++i) {
                free_vec_regs.insert(i);
            }
        }

        // Opmask registers (k1-k7) require AVX-512F + XCR0[5] (OPMASK state).
        // This is independent of full ZMM (zmm16-31) support.
        if (has_opmask_) {
            free_opmask_regs = base_free_opmask();
            preserved_opmask = base_preserved_opmask();
        }

        // Detect AMX for tile registers (tmm0-tmm7)
        // Requires both XCR0[17] (XTILECFG) and XCR0[18] (XTILEDATA) to be OS-enabled
        if (cpu.has(Xbyak::util::Cpu::tAMX_TILE)) {
            has_amx_ = ((xcr0 >> 17) & 3) == 3;
        }

        // If AMX is available, add tmm0-tmm7 to the free pool
        // All tile registers are caller-saved (call-clobbered)
        if (has_amx_) {
            for (int i = 0; i <= 7; ++i) {
                free_tile_regs.insert(i);
            }
        }
    }

    RegPoolManager(const RegPoolManager &) = delete;
    RegPoolManager &operator=(const RegPoolManager &) = delete;
    RegPoolManager(RegPoolManager &&) = default;
    RegPoolManager &operator=(RegPoolManager &&) = default;

    // Usage:
    // Reg64 rax = rm.alloc<Reg64>(); // Allocate next available 64-bit register (freed by user)
    // Reg64 rdx = rm.alloc(rdx);     // Allocate a specific register by name (RegT deduced) (Must be in a CodeGenerator) (freed by user)
    // Reg64 r10 = rm.alloc<Reg64>(10); // Allocate a specific register by index (freed by user)
    // rm.free(rax);                  // Free rax
    // rm.free(rdx);                  // Free rdx
    // rm.free(r10);                  // Free r10

    // register allocation method - accepts int to specify an unused reg, or no arg to get next free reg
    template <class RegT>
    RegT alloc() {
        switch (reg_family<RegT>::value) {
            case RegFamily::GP: {
                const int idx = next_gp_idx();
                gp_reg(idx);
                return RegT(idx);
            }
            case RegFamily::Vec: {
                const int idx = next_vec_idx();
                vec_reg(idx);
                return RegT(idx);
            }
            case RegFamily::Opmask: {
                const int idx = next_opmask_idx();
                opmask_reg(idx);
                return RegT(idx);
            }
            case RegFamily::Tile: {
                const int idx = next_tile_idx();
                tile_reg(idx);
                return RegT(idx);
            }
            default: XBYAK_THROW_RET(ERR_INTERNAL, RegT(0))
        }
    }

    template <class RegT>
    RegT alloc(int idx) {
        switch (reg_family<RegT>::value) {
            case RegFamily::GP: gp_reg(idx); return RegT(idx);
            case RegFamily::Vec: vec_reg(idx); return RegT(idx);
            case RegFamily::Opmask: opmask_reg(idx); return RegT(idx);
            case RegFamily::Tile: tile_reg(idx); return RegT(idx);
            default: XBYAK_THROW_RET(ERR_INTERNAL, RegT(0))
        }
    }

    // Accepts an Xbyak register object directly.
    // RegT is deduced from the argument — no explicit template parameter needed.
    // Equivalent to alloc<RegT>(reg.getIdx()) but reads like familiar register names.
    // Example: alloc(rdx)  instead of  alloc<Reg64>(2)
    //
    // NOTE: Named register constants (rax, rdx, r10, xmm2, tmm0, k1, etc.) are
    // const members of Xbyak::CodeGenerator. This overload must therefore be
    // called from within a class that inherits Xbyak::CodeGenerator so that
    // those names are in scope. Outside a CodeGenerator subclass, use the
    // index-based overload alloc<RegT>(int idx) instead.
    template <class RegT>
    RegT alloc(const RegT &reg) {
        return alloc<RegT>(reg.getIdx());
    }

    // takes register object and moves it from in use to free set
    template <class RegT>
    void free(RegT reg) {
        const int idx = reg.getIdx();
        switch (reg_family<RegT>::value) {
            case RegFamily::GP: release_gp(idx); break;
            case RegFamily::Vec: release_vec(idx); break;
            case RegFamily::Opmask: release_opmask(idx); break;
            case RegFamily::Tile: release_tile(idx); break;
            default: XBYAK_THROW(ERR_INTERNAL)
        }
    }

    // getter methods - return vectors of register indices representing a set
    std::vector<int> get_free_gps() const {
        return make_index_vector(free_gp_regs);
    }
    std::vector<int> get_live_gps() const {
        return make_index_vector(live_gp_);
    }
    std::vector<int> get_preserved_gps() const {
        return make_index_vector(preserved_gp);
    }

    // Returns only in-use registers that are volatile (caller-saved)
    // These MUST be saved by the caller before making a function call
    std::vector<int> get_live_volatile_gps() const {
        std::vector<int> result;
        const auto& base_free = base_free_gp();
        for (int idx : live_gp_) {
            if (base_free.count(idx) > 0) {
                result.push_back(idx);
            }
        }
        return result;
    }

    // Returns only in-use registers that are preserved (callee-saved)
    // These will be saved by the callee if it uses them
    std::vector<int> get_live_preserved_gps() const {
        std::vector<int> result;
        const auto& base_preserved = base_preserved_gp();
        for (int idx : live_gp_) {
            if (base_preserved.count(idx) > 0) {
                result.push_back(idx);
            }
        }
        return result;
    }

    std::vector<int> get_free_vecs() const {
        return make_index_vector(free_vec_regs);
    }
    std::vector<int> get_live_vecs() const {
        return make_index_vector(live_vec_);
    }
    std::vector<int> get_preserved_vecs() const {
        return make_index_vector(preserved_vec);
    }

    // Returns only in-use vector registers that are volatile (caller-saved)
    // These MUST be saved by the caller before making a function call
    // IMPORTANT for cross-platform code: On Windows, xmm6-xmm15 are preserved (callee-saved),
    // but on Linux/macOS ALL vector registers are volatile (caller-saved).
    // Use this method to write portable code that avoids unnecessary saves on Windows.
    std::vector<int> get_live_volatile_vecs() const {
        std::vector<int> result;
        const auto& base_free = base_free_vec();
        for (int idx : live_vec_) {
            if (base_free.count(idx) > 0) {
                result.push_back(idx);
            }
        }
        return result;
    }

    // Returns only in-use vector registers that are preserved (callee-saved)
    // These will be saved by the callee if it uses them
    std::vector<int> get_live_preserved_vecs() const {
        std::vector<int> result;
        const auto& base_preserved = base_preserved_vec();
        for (int idx : live_vec_) {
            if (base_preserved.count(idx) > 0) {
                result.push_back(idx);
            }
        }
        return result;
    }

    std::vector<int> get_free_opmasks() const {
        return make_index_vector(free_opmask_regs);
    }
    std::vector<int> get_live_opmasks() const {
        return make_index_vector(live_opmask_);
    }
    std::vector<int> get_preserved_opmasks() const {
        return make_index_vector(preserved_opmask);
    }

    std::vector<int> get_free_tiles() const {
        return make_index_vector(free_tile_regs);
    }
    std::vector<int> get_live_tiles() const {
        return make_index_vector(live_tile_);
    }

    // Returns true if every register allocated with alloc() has been returned with free().
    // Returns false if any register is still currently allocated.
    //
    // Use this to inspect allocation state programmatically. For a hard stop in debug
    // builds, use assert_all_free() instead.
    bool all_free() const {
        return live_gp_.empty() && live_vec_.empty()
            && live_opmask_.empty() && live_tile_.empty();
    }

    // Checks that every register allocated with alloc() has been returned with free().
    //
    // Call this at the end of JIT kernel construction to confirm there are no
    // mismatched alloc/free pairs. A forgotten free() does not affect the correctness
    // of the emitted machine code, but leaves the manager in an unexpected state that
    // may cause incorrect behaviour in a subsequent alloc() on the same instance.
    //
    // In debug builds (NDEBUG not defined): triggers an assertion if any register is
    // still allocated. A message listing the leaked register indices by family is
    // printed to stderr before the assertion fires.
    //
    // In release builds (NDEBUG defined): compiles to nothing — no check, no overhead.
    void assert_all_free() const {
#ifndef NDEBUG
        if (!live_gp_.empty()) {
            fprintf(stderr, "assert_all_free: GP registers still allocated:");
            for (int idx : live_gp_) fprintf(stderr, " %d", idx);
            fprintf(stderr, "\n");
        }
        if (!live_vec_.empty()) {
            fprintf(stderr, "assert_all_free: Vec registers still allocated:");
            for (int idx : live_vec_) fprintf(stderr, " %d", idx);
            fprintf(stderr, "\n");
        }
        if (!live_opmask_.empty()) {
            fprintf(stderr, "assert_all_free: Opmask registers still allocated:");
            for (int idx : live_opmask_) fprintf(stderr, " %d", idx);
            fprintf(stderr, "\n");
        }
        if (!live_tile_.empty()) {
            fprintf(stderr, "assert_all_free: Tile registers still allocated:");
            for (int idx : live_tile_) fprintf(stderr, " %d", idx);
            fprintf(stderr, "\n");
        }
        assert(all_free() && "assert_all_free: registers are still allocated — missing free() call(s)");
#endif
    }

    // Returns true if no CommittedLayout is currently open.
    // Use this to inspect state programmatically.  For a hard stop in debug
    // builds, use assert_clean_stack() instead.
    bool clean_stack() const {
        return allocated_stack_space_ == 0;
    }

    // Checks that the hardware stack is fully balanced: no CommittedLayout is
    // currently open.
    //
    // Call this just before ret() to confirm every CommittedLayout opened with
    // make_stack_layout().build() has been destroyed.  An open layout leaves
    // rsp pointing into allocated frame space, making the return address
    // unreachable.
    //
    // In debug builds (NDEBUG not defined): prints diagnostic information to
    // stderr and triggers an assertion.
    //
    // In release builds (NDEBUG defined): compiles to nothing.
    void assert_clean_stack() const {
#ifndef NDEBUG
        if (allocated_stack_space_ != 0)
            fprintf(stderr,
                    "assert_clean_stack: CommittedLayout not destroyed (%td bytes still allocated)\n",
                    allocated_stack_space_);
        assert(clean_stack() &&
               "assert_clean_stack: unbalanced stack — open CommittedLayout");
#endif
    }

    // Prevents a register from being returned by alloc() without marking it as in-use.
    // Useful for protecting registers that must stay off-limits during code generation,
    // such as ABI argument registers or registers dedicated to a runtime helper.
    //
    // The register must not be currently allocated. Reserving an already-in-use register,
    // or calling mark_unavailable() twice on the same register, throws Xbyak::Error.
    // Reserved registers are not visible to get_live_gps() / get_live_vecs() etc.
    // Call mark_available() to return the register to the normal allocation pool.
    //
    // Named-register and index-based overloads are both available:
    //   rm.mark_unavailable(rdi);        // named (requires CodeGenerator subclass scope)
    //   rm.mark_unavailable<Reg64>(7);   // GP register by index
    //   rm.mark_unavailable<Xmm>(0);     // vector register by index
    //   rm.mark_unavailable<Opmask>(1);  // opmask register by index
    //   rm.mark_unavailable<Tmm>(0);     // tile register by index

    // Index-based overload — RegT must be specified explicitly (e.g. mark_unavailable<Reg64>(7)).
    template <class RegT>
    void mark_unavailable(int idx) {
        switch (reg_family<RegT>::value) {
            case RegFamily::GP:     reserve_reg_gp(idx);     return;
            case RegFamily::Vec:    reserve_reg_vec(idx);    return;
            case RegFamily::Opmask: reserve_reg_opmask(idx); return;
            case RegFamily::Tile:   reserve_reg_tile(idx);   return;
            default: XBYAK_THROW(ERR_INTERNAL)
        }
    }

    // Named-register overload — RegT is deduced from the argument.
    template <class RegT>
    void mark_unavailable(const RegT &reg) { mark_unavailable<RegT>(reg.getIdx()); }

    template <class RegT>
    void mark_available(int idx) {
        switch (reg_family<RegT>::value) {
            case RegFamily::GP:     unreserve_reg_gp(idx);     return;
            case RegFamily::Vec:    unreserve_reg_vec(idx);    return;
            case RegFamily::Opmask: unreserve_reg_opmask(idx); return;
            case RegFamily::Tile:   unreserve_reg_tile(idx);   return;
            default: XBYAK_THROW(ERR_INTERNAL)
        }
    }

    // Returns a previously reserved register to the allocation pool so that alloc() may
    // return it again. Throws Xbyak::Error if the register is not currently reserved.
    template <class RegT>
    void mark_available(const RegT &reg) { mark_available<RegT>(reg.getIdx()); }

    // Returns true if the register has been reserved with mark_unavailable().
    template <class RegT>
    bool is_reserved(const RegT &reg) const { return is_reserved<RegT>(reg.getIdx()); }

    template <class RegT>
    bool is_reserved(int idx) const {
        switch (reg_family<RegT>::value) {
            case RegFamily::GP:     return reserved_gp.count(idx) != 0;
            case RegFamily::Vec:    return reserved_vec.count(idx) != 0;
            case RegFamily::Opmask: return reserved_opmask.count(idx) != 0;
            case RegFamily::Tile:   return reserved_tile.count(idx) != 0;
            default: XBYAK_THROW_RET(ERR_INTERNAL, false)
        }
    }

    // Adds a GP register to the allocatable pool that was deliberately excluded
    // at construction time.
    //
    // The primary use case is rsp (index 4) and rbp (index 5): both are valid
    // hardware registers but are omitted from every pool by default because
    // using them as general allocatables would corrupt the stack frame or frame
    // pointer.  A kernel that manages its own frame layout — for example, a
    // leaf function that repurposes rbp as an extra GP scratch register — may
    // call this to opt one of them in:
    //
    //   rm.add_to_gp_pool(rm.base_ptr());  // opt rbp in as a scratch reg
    //   auto rbp_scratch = rm.alloc<Reg64>();    // may now return rbp (idx 5)
    //
    // No counterpart exists for Vec, Opmask, or Tile families because all
    // allocatable registers in those families are already in their respective
    // pools when the required ISA extension is OS-enabled.  (k0 is excluded
    // from the opmask pool but must never be allocated — using it as a write
    // mask silently disables masking.)
    //
    // Throws Xbyak::Error if the index is out of range, or if the register is
    // already tracked (free, preserved, in-use, or reserved).
    void add_to_gp_pool(const Reg64 &reg) { add_to_gp_pool(reg.getIdx()); }
    void add_to_gp_pool(int idx) {
        if (idx < 0 || idx > max_gp_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        const bool in_free = free_gp_regs.count(idx) != 0;
        const bool in_preserved = preserved_gp.count(idx) != 0;
        const bool in_use = live_gp_.count(idx) != 0;
        const bool in_reserved = reserved_gp.count(idx) != 0;
        if (in_free || in_preserved || in_use || in_reserved)
            XBYAK_THROW(ERR_RM_REG_ALREADY_TRACKED)
        free_gp_regs.insert(idx);
    }

    // helper function - returns true if a register object is currently in the used set of registers
    template <class RegT>
    bool reg_in_use(const RegT &reg) const {
        return reg_in_use_idx(reg.getIdx(), reg_family<RegT>::value);
    }

    // scoped register handling with RAII
    // usage: Reg64 r10 = rm.alloc<Reg64>(10);
    //        auto scoped = rm.makeScoped(r10);
    // or
    //        auto scoped_reg = rm.makeScoped(rm.alloc<Reg64>());
    // r10 & scoped_reg will free at end of scope when guards' dtors called.
    template <class Reg>
    class Scoped {
    public:
        explicit Scoped(RegPoolManager &rm, Reg r)
            // pointer to allocator, allocate scoped reg at construction & track, unowned = NULL
            : rm_(&rm), reg_(r), generation_(rm.generation_) {
            validate_scoped_reg(rm_, reg_);
        }

        // if object is owner of scoped reg and goes out of scope, deallocate
        ~Scoped() noexcept {
            if (!rm_) return;
            if (generation_ != rm_->generation_) return; // manager was reset; don't free
#if !defined(XBYAK_NO_EXCEPTION)
            try {
                rm_->free(reg_);
            } catch (...) {
#ifndef NDEBUG
                fprintf(stderr, "RegPoolManager::Scoped::~Scoped: free() threw; swallowed\n");
#endif
            }
#else
            rm_->free(reg_);
#endif
        }

        // disable copy - scoped regs are move only to avoid ownership/double free issues as per RAII
        Scoped(const Scoped &) = delete;
        Scoped &operator=(const Scoped &) = delete;

        // move constructor - used when scoped regs initialised from rvalue (incl. std::move)
        Scoped(Scoped &&other) noexcept : rm_(other.rm_), reg_(other.reg_), generation_(other.generation_) {
            other.rm_ = NULL; // set previous owner to no longer own
        }

        // expose underlying register for implicit use in JIT helpers
        operator const Reg &() const noexcept { return reg_; }
        const Reg &get() const noexcept { return reg_; }

        // forwarding accessors — avoids .get() when auto-type deduction
        // prevents the implicit conversion from firing
        int getIdx() const noexcept { return reg_.getIdx(); }
        int getBit() const noexcept { return reg_.getBit(); }

    private:
        RegPoolManager *rm_
                = NULL; // pointer to allocator, initialised as NULL
        Reg reg_ {};
        std::size_t generation_ = 0;
    };

    // helper factory - calls Scoped ctor
    template <class Reg>
    inline Scoped<Reg> makeScoped(Reg r) & {
        return Scoped<Reg>(*this, r);
    }

    // Allocates a register and wraps it in a Scoped guard in one call.
    // Equivalent to makeScoped(alloc<RegT>()) or makeScoped(alloc<RegT>(idx)).
    // The register is freed automatically when the returned Scoped goes out of scope.
    //
    // Usage:
    //   auto r  = rm.allocScoped<Reg64>();     // next available GP register
    //   auto r9 = rm.allocScoped<Reg64>(9);    // specific register by index
    template <class RegT>
    inline Scoped<RegT> allocScoped() & { return makeScoped(alloc<RegT>()); }

    template <class RegT>
    inline Scoped<RegT> allocScoped(int idx) & { return makeScoped(alloc<RegT>(idx)); }

    // -------------------------------------------------------------------------
    // StackLayout / CommittedLayout — two-phase unified stack management
    //
    // Guarantees rsp moves exactly once (at build()) and never again until
    // destroy().  All slot offsets are fixed at build() time, so park,
    // reload, and scratch operations remain valid regardless of what else
    // happens between build() and destroy().
    //
    // Required call order
    // -------------------
    //  1. alloc() all registers you need (determines which are callee-saved).
    //  2. emit_prologue() — pushes callee-saved GP registers onto the stack.
    //     Must come before build() so that stack alignment is correctly
    //     computed for any emit_call() within the layout.
    //  3. build() — emits sub rsp, <total> and opens the CommittedLayout.
    //     Must come after emit_prologue() (see above).
    //  4. ... use park/reload/save_volatiles/emit_call as needed ...
    //  5. destroy() (or let CommittedLayout go out of scope) — emits
    //     add rsp, <total>.  Must come before emit_epilogue() so that rsp
    //     points back to the return-address slot before the pop sequence.
    //  6. emit_epilogue() — pops callee-saved GP registers in reverse order.
    //     Must come after destroy().
    //  7. ret()
    //
    // Violating steps 2→3 or 5→6 corrupts rsp and produces an invalid frame.
    //
    // Typical usage:
    //
    //   emit_prologue();                        // push callee-saves first
    //
    //   auto cl = make_stack_layout()
    //       .gp_parks(2)                        // two 8-byte GP slots
    //       .vec_parks(1)                       // one ZMM/YMM slot
    //       .scratch(64)                        // 64 bytes of raw scratch
    //       .with_volatile_save()               // snapshot live volatiles now
    //       .build();                           // emits: sub rsp, <total>
    //
    //   cl.park(rdi, 0);                        // mov [rsp+0], rdi; free rdi
    //   cl.park_vec(zmm0, 0);                   // vmovdqu32 [rsp+N], zmm0; free zmm0
    //   cl.save_volatiles();                    // mov [rsp+..], live_vol_reg ...
    //   emit_call(&my_func);
    //   cl.restore_volatiles();
    //   auto r = cl.reload<Reg64>(0);           // mov r, [rsp+0]; alloc r
    //
    //   cl.destroy();                           // emits: add rsp, <total>
    //   emit_epilogue();
    //   ret();
    // -------------------------------------------------------------------------

    // Forward declarations for the builder and committed types.
    class CommittedLayout;

    // Builder — accumulates slot requirements before any code is emitted.
    // All methods return *this for chaining.  No machine code is emitted until
    // build() is called.
    class StackLayout {
    public:
        explicit StackLayout(RegPoolManager &rm)
                : rm_(&rm), gp_count_(0), vec_count_(0),
                  scratch_bytes_(0), with_volatile_save_(false),
                  outgoing_arg_count_(0) {}

        // Reserve n GP-sized (8-byte) park slots.
        StackLayout &gp_parks(int n) { gp_count_ = n; return *this; }

        // Reserve n vector park slots.
        // Each slot is 64 bytes when AVX-512 is present, 32 bytes otherwise.
        StackLayout &vec_parks(int n) { vec_count_ = n; return *this; }

        // Reserve a raw scratch area of at least 'bytes' bytes.
        // The value is rounded up to the nearest 8-byte boundary automatically.
        StackLayout &scratch(ptrdiff_t bytes) { scratch_bytes_ = bytes; return *this; }

        // Reserve fixed-offset slots for all ABI-volatile GP and vector registers.
        // save_volatiles() will store only those that are live at call time;
        // restore_volatiles() reloads exactly the same set.  Because slots are
        // sized for every possible volatile register, the set of live registers
        // at save_volatiles() time does not need to match the set at build() time.
        StackLayout &with_volatile_save() { with_volatile_save_ = true; return *this; }

        // Reserve n stack-overflow argument slots at [rsp+0] (SysV) or
        // [rsp+32] (Win64, above the 32-byte shadow space that is also reserved).
        //
        // Use this when calling a C function whose argument count exceeds the
        // ABI register limit (SysV: more than 6 GP args; Win64: more than 4 GP
        // args).  After build(), write each overflow argument to
        // outgoing_arg_addr(n) on the CommittedLayout, load the register
        // arguments, then call emit_call() on the CommittedLayout instead of
        // RegPoolManager::emit_call().
        //
        // CommittedLayout::emit_call() emits only "mov rax, func; call rax" — no sub/add
        // rsp — because build() adjusts the frame total so that rsp is already
        // 16-byte aligned (at the call instruction) without any extra adjustment.
        StackLayout &with_outgoing_args(int n) {
            outgoing_arg_count_ = n;
            return *this;
        }

        // Commit: compute the total frame size, emit sub rsp, <total>, and
        // return a CommittedLayout owning the frame.
        // Throws Xbyak::Error if no CodeGenerator has been provided, or if a
        // CommittedLayout is already active on this manager.
        CommittedLayout build() {
            return rm_->build_layout(gp_count_, vec_count_,
                                     scratch_bytes_, with_volatile_save_,
                                     outgoing_arg_count_);
        }

    private:
        RegPoolManager *rm_;
        int    gp_count_;
        int    vec_count_;
        ptrdiff_t scratch_bytes_;
        bool   with_volatile_save_;
        int    outgoing_arg_count_;
    };

    // Committed layout — owns the stack frame opened by StackLayout::build().
    // Construction emits sub rsp, <total>.
    // Destruction emits add rsp, <total> (unless destroy() was called first).
    // Move-only.
    class CommittedLayout {
    public:
        CommittedLayout(RegPoolManager &rm,
                        ptrdiff_t      gp_base,
                        int            gp_count,
                        ptrdiff_t      vec_base,
                        int            vec_count,
                        int            vec_slot_bytes,
                        ptrdiff_t      scratch_base,
                        ptrdiff_t      scratch_bytes,
                        ptrdiff_t      total,
                        std::vector<int> volatile_gps,
                        ptrdiff_t      vol_gp_base,
                        std::vector<int> volatile_vecs,
                        ptrdiff_t      vol_vec_base,
                        int            vol_vec_slot_bytes,
                        bool           vol_save_declared,
                ptrdiff_t      outgoing_arg_base = 0,
                int            n_outgoing_args = 0)
                : rm_(&rm), gp_base_(gp_base), gp_count_(gp_count),
                  vec_base_(vec_base), vec_count_(vec_count),
                  vec_slot_bytes_(vec_slot_bytes),
                  scratch_base_(scratch_base), scratch_bytes_(scratch_bytes),
                  total_(total),
                  volatile_gps_(volatile_gps), vol_gp_base_(vol_gp_base),
                  volatile_vecs_(volatile_vecs), vol_vec_base_(vol_vec_base),
                  vol_vec_slot_bytes_(vol_vec_slot_bytes),
                  vol_save_declared_(vol_save_declared),
                  vol_save_armed_(false),
                  outgoing_arg_base_(outgoing_arg_base),
                  n_outgoing_args_(n_outgoing_args) {
            if (!rm_->cg_) XBYAK_THROW(ERR_RM_NO_CG)
            rm_->cg_->sub(rm_->cg_->rsp, static_cast<uint32_t>(total_));
            rm_->managed_push_count_ += static_cast<size_t>(total_) / 8;
            rm_->allocated_stack_space_ += total_;
            rm_->layout_active_ = true;
        }

        ~CommittedLayout() noexcept { do_destroy(); }

        CommittedLayout(const CommittedLayout &) = delete;
        CommittedLayout &operator=(const CommittedLayout &) = delete;

        CommittedLayout(CommittedLayout &&other) noexcept
                : rm_(other.rm_), gp_base_(other.gp_base_),
                  gp_count_(other.gp_count_), vec_base_(other.vec_base_),
                  vec_count_(other.vec_count_),
                  vec_slot_bytes_(other.vec_slot_bytes_),
                  scratch_base_(other.scratch_base_),
                  scratch_bytes_(other.scratch_bytes_), total_(other.total_),
                  volatile_gps_(std::move(other.volatile_gps_)),
                  vol_gp_base_(other.vol_gp_base_),
                  volatile_vecs_(std::move(other.volatile_vecs_)),
                  vol_vec_base_(other.vol_vec_base_),
                  vol_vec_slot_bytes_(other.vol_vec_slot_bytes_),
                  vol_save_declared_(other.vol_save_declared_),
                  vol_save_armed_(other.vol_save_armed_),
                  outgoing_arg_base_(other.outgoing_arg_base_),
                  n_outgoing_args_(other.n_outgoing_args_),
                  actually_saved_gp_indices_(std::move(other.actually_saved_gp_indices_)),
                  actually_saved_vec_indices_(std::move(other.actually_saved_vec_indices_)) {
            other.rm_ = NULL;
        }

        // Emit add rsp, <total> immediately and disarm the destructor.
        void destroy() {
            do_destroy();
        }

        // Store reg at GP park slot slot_idx and free it from the allocator.
        // Emits: mov [rsp + gp_base + slot_idx*8], reg
        template <class RegT>
        void park(RegT &reg, int slot_idx) {
            check_gp_slot(slot_idx);
            do_store(rm_->cg_, reg,
                     gp_base_ + static_cast<ptrdiff_t>(slot_idx) * 8);
            rm_->free(reg);
        }

        // Store reg at GP park slot without freeing it.
        template <class RegT>
        void park(const RegT &reg, int slot_idx) {
            check_gp_slot(slot_idx);
            do_store(rm_->cg_, reg,
                     gp_base_ + static_cast<ptrdiff_t>(slot_idx) * 8);
        }

        // Allocate a new register of type RegT and load GP park slot slot_idx into it.
        // Caller is responsible for freeing the returned register.
        template <class RegT>
        RegT reload(int slot_idx) {
            check_gp_slot(slot_idx);
            RegT reg = rm_->alloc<RegT>();
            do_load(rm_->cg_, reg,
                    gp_base_ + static_cast<ptrdiff_t>(slot_idx) * 8);
            return reg;
        }

        // Store vec reg at vector park slot slot_idx and free it from the allocator.
        // Slot size is 64 bytes (ZMM) when AVX-512 is present, 32 bytes (YMM) otherwise.
        template <class VecT>
        void park_vec(VecT &reg, int slot_idx) {
            check_vec_slot(slot_idx);
            const ptrdiff_t off = vec_base_ + static_cast<ptrdiff_t>(slot_idx) * vec_slot_bytes_;
            do_store(rm_->cg_, reg, off);
            rm_->free(reg);
        }

        // Store vec reg at vector park slot without freeing it.
        template <class VecT>
        void park_vec(const VecT &reg, int slot_idx) {
            check_vec_slot(slot_idx);
            const ptrdiff_t off = vec_base_ + static_cast<ptrdiff_t>(slot_idx) * vec_slot_bytes_;
            do_store(rm_->cg_, reg, off);
        }

        // Allocate a new register of type VecT and load vector park slot slot_idx into it.
        // Caller is responsible for freeing the returned register.
        template <class VecT>
        VecT reload_vec(int slot_idx) {
            check_vec_slot(slot_idx);
            const ptrdiff_t off = vec_base_ + static_cast<ptrdiff_t>(slot_idx) * vec_slot_bytes_;
            VecT reg = rm_->alloc<VecT>();
            do_load(rm_->cg_, reg, off);
            return reg;
        }

        // Emit fixed-offset stores for each volatile register that is currently
        // live (allocated).  Must only be called if with_volatile_save() was
        // declared.  Does not move rsp.  The set saved here is exactly the set
        // that restore_volatiles() will reload.
        void save_volatiles() {
            if (!vol_save_declared_) XBYAK_THROW(ERR_RM_LAYOUT_SAVE_NOT_DECLARED)
            actually_saved_gp_indices_.clear();
            for (int i = 0; i < (int)volatile_gps_.size(); ++i) {
                if (!rm_->live_gp_.count(volatile_gps_[i])) continue;
                const ptrdiff_t off = vol_gp_base_ + static_cast<ptrdiff_t>(i) * 8;
                rm_->cg_->mov(rm_->cg_->qword[rm_->cg_->rsp + off], Reg64(volatile_gps_[i]));
                actually_saved_gp_indices_.push_back(i);
            }
            actually_saved_vec_indices_.clear();
            for (int i = 0; i < (int)volatile_vecs_.size(); ++i) {
                if (!rm_->live_vec_.count(volatile_vecs_[i])) continue;
                const ptrdiff_t off = vol_vec_base_ + static_cast<ptrdiff_t>(i) * vol_vec_slot_bytes_;
                if (vol_vec_slot_bytes_ == 64)
                    rm_->cg_->vmovdqu32(rm_->cg_->ptr[rm_->cg_->rsp + off], Zmm(volatile_vecs_[i]));
                else
                    rm_->cg_->vmovdqu(rm_->cg_->ptr[rm_->cg_->rsp + off], Ymm(volatile_vecs_[i]));
                actually_saved_vec_indices_.push_back(i);
            }
            vol_save_armed_ = true;
        }

        // Reload the registers saved by the preceding save_volatiles() call,
        // in reverse order.  Only the registers that were actually stored are
        // reloaded.  Must be called after save_volatiles().
        void restore_volatiles() {
            if (!vol_save_declared_) XBYAK_THROW(ERR_RM_LAYOUT_SAVE_NOT_DECLARED)
            if (!vol_save_armed_) XBYAK_THROW(ERR_RM_RESTORE_WITHOUT_SAVE)
            for (int i = (int)actually_saved_vec_indices_.size() - 1; i >= 0; --i) {
                const int slot_i = actually_saved_vec_indices_[i];
                const ptrdiff_t off = vol_vec_base_ + static_cast<ptrdiff_t>(slot_i) * vol_vec_slot_bytes_;
                if (vol_vec_slot_bytes_ == 64)
                    rm_->cg_->vmovdqu32(Zmm(volatile_vecs_[slot_i]), rm_->cg_->ptr[rm_->cg_->rsp + off]);
                else
                    rm_->cg_->vmovdqu(Ymm(volatile_vecs_[slot_i]), rm_->cg_->ptr[rm_->cg_->rsp + off]);
            }
            actually_saved_vec_indices_.clear();
            for (int i = (int)actually_saved_gp_indices_.size() - 1; i >= 0; --i) {
                const int slot_i = actually_saved_gp_indices_[i];
                const ptrdiff_t off = vol_gp_base_ + static_cast<ptrdiff_t>(slot_i) * 8;
                rm_->cg_->mov(Reg64(volatile_gps_[slot_i]), rm_->cg_->qword[rm_->cg_->rsp + off]);
            }
            actually_saved_gp_indices_.clear();
            vol_save_armed_ = false;
        }

        // Return a memory address [rsp + scratch_base + byte_offset] for use as a
        // memory operand in generated code.
        // byte_offset must be in [0, scratch_bytes).
        Xbyak::Address scratch_addr(ptrdiff_t byte_offset = 0) const {
            if (byte_offset < 0 || byte_offset >= scratch_bytes_)
                XBYAK_THROW_RET(ERR_RM_LAYOUT_SCRATCH_OOB,
                                rm_->cg_->qword[rm_->cg_->rsp])
            return rm_->cg_->ptr[rm_->cg_->rsp + scratch_base_ + byte_offset];
        }

        // Total bytes reserved by this layout.
        ptrdiff_t total_size() const { return total_; }

        // True if with_volatile_save() was declared on the builder.
        bool has_volatile_save() const { return vol_save_declared_; }

        // Returns the number of outgoing stack-overflow argument slots declared
        // with with_outgoing_args().  Zero if not declared.
        int outgoing_arg_count() const { return n_outgoing_args_; }

        // Returns an address operand for outgoing stack-overflow argument slot n.
        //
        //   SysV:  [rsp + n*8]        — the callee sees this as arg(6+n)
        //   Win64: [rsp + 32 + n*8]   — the callee sees this as arg(4+n),
        //                               above the 32-byte shadow space
        //
        // Write each overflow argument here before calling emit_call().
        // n must be in [0, with_outgoing_args count).
        Xbyak::Address outgoing_arg_addr(int n) const {
            if (n < 0 || n >= n_outgoing_args_)
                XBYAK_THROW_RET(ERR_RM_LAYOUT_SLOT_OOB,
                                rm_->cg_->qword[rm_->cg_->rsp])
            return rm_->cg_->ptr[rm_->cg_->rsp
                                 + outgoing_arg_base_
                                 + static_cast<ptrdiff_t>(n) * 8];
        }

        // Emit a call to func_ptr with automatic ABI correctness.
        //
        // Two behaviours depending on how the layout was built:
        //
        // — with_outgoing_args(n) declared:
        //     Emits only "mov rax, func; call rax" — no sub/add rsp.
        //     build() computed the frame total so that rsp is already 16-byte
        //     aligned at this call instruction.  Win64 shadow space is included
        //     in the frame total.  Write overflow arguments to
        //     outgoing_arg_addr(0..n-1) and load register arguments before
        //     calling.
        //
        // — with_outgoing_args() not declared:
        //     Delegates to RegPoolManager::emit_call(), which emits the standard
        //     "sub rsp, adj; mov rax, func; call rax; add rsp, adj" sequence.
        //     Use this form for calls whose arguments all fit in registers.
        //
        // rax will be clobbered in both cases (used to hold the function address).
        void emit_call(uint64_t func_ptr) {
            if (!rm_ || !rm_->cg_) XBYAK_THROW(ERR_RM_NO_CG)
            if (n_outgoing_args_ > 0) {
                // Bare call — rsp already aligned by build().
#ifndef NDEBUG
                if (rm_->live_gp_.count(0)) {
                    fprintf(stderr,
                            "CommittedLayout::emit_call: rax (index 0) is currently allocated — "
                            "its value will be destroyed. Park it before calling.\n");
                    assert(!rm_->live_gp_.count(0) &&
                           "CommittedLayout::emit_call: rax is live and will be clobbered");
                }
#endif
                rm_->cg_->mov(rm_->cg_->rax, func_ptr);
                rm_->cg_->call(rm_->cg_->rax);
            } else {
                // Standard call — delegate to manager for sub/add alignment.
                rm_->emit_call(func_ptr);
            }
        }

        template <typename FuncT>
        void emit_call(FuncT *func_ptr) {
            emit_call(reinterpret_cast<uint64_t>(func_ptr));
        }

    private:
        RegPoolManager *rm_;
        ptrdiff_t gp_base_;
        int       gp_count_;
        ptrdiff_t vec_base_;
        int       vec_count_;
        int       vec_slot_bytes_;
        ptrdiff_t scratch_base_;
        ptrdiff_t scratch_bytes_;
        ptrdiff_t total_;
        std::vector<int> volatile_gps_;
        ptrdiff_t vol_gp_base_;
        std::vector<int> volatile_vecs_;
        ptrdiff_t vol_vec_base_;
        int       vol_vec_slot_bytes_;
        bool      vol_save_declared_;
        bool      vol_save_armed_;
        ptrdiff_t outgoing_arg_base_;
        int       n_outgoing_args_;
        // Indices into volatile_gps_ / volatile_vecs_ that were actually stored
        // by the most recent save_volatiles() call.  Used by restore_volatiles()
        // to emit loads for exactly the same set.
        std::vector<int> actually_saved_gp_indices_;
        std::vector<int> actually_saved_vec_indices_;

        void check_gp_slot(int idx) const {
            if (idx < 0 || idx >= gp_count_)
                XBYAK_THROW(ERR_RM_LAYOUT_SLOT_OOB)
        }
        void check_vec_slot(int idx) const {
            if (idx < 0 || idx >= vec_count_)
                XBYAK_THROW(ERR_RM_LAYOUT_SLOT_OOB)
        }

        static void do_store(Xbyak::CodeGenerator *cg, const Reg64 &r, ptrdiff_t off) {
            cg->mov(cg->qword[cg->rsp + off], r);
        }
        static void do_store(Xbyak::CodeGenerator *cg, const Reg32 &r, ptrdiff_t off) {
            cg->mov(cg->dword[cg->rsp + off], r);
        }
        static void do_store(Xbyak::CodeGenerator *cg, const Reg16 &r, ptrdiff_t off) {
            cg->mov(cg->word[cg->rsp + off], r);
        }
        static void do_store(Xbyak::CodeGenerator *cg, const Xmm &r, ptrdiff_t off) {
            cg->vmovdqu(cg->ptr[cg->rsp + off], r);
        }
        static void do_store(Xbyak::CodeGenerator *cg, const Ymm &r, ptrdiff_t off) {
            cg->vmovdqu(cg->ptr[cg->rsp + off], r);
        }
        static void do_store(Xbyak::CodeGenerator *cg, const Zmm &r, ptrdiff_t off) {
            cg->vmovdqu32(cg->ptr[cg->rsp + off], r);
        }
        static void do_load(Xbyak::CodeGenerator *cg, Reg64 &r, ptrdiff_t off) {
            cg->mov(r, cg->qword[cg->rsp + off]);
        }
        static void do_load(Xbyak::CodeGenerator *cg, Reg32 &r, ptrdiff_t off) {
            cg->mov(r, cg->dword[cg->rsp + off]);
        }
        static void do_load(Xbyak::CodeGenerator *cg, Reg16 &r, ptrdiff_t off) {
            cg->mov(r, cg->word[cg->rsp + off]);
        }
        static void do_load(Xbyak::CodeGenerator *cg, Xmm &r, ptrdiff_t off) {
            cg->vmovdqu(r, cg->ptr[cg->rsp + off]);
        }
        static void do_load(Xbyak::CodeGenerator *cg, Ymm &r, ptrdiff_t off) {
            cg->vmovdqu(r, cg->ptr[cg->rsp + off]);
        }
        static void do_load(Xbyak::CodeGenerator *cg, Zmm &r, ptrdiff_t off) {
            cg->vmovdqu32(r, cg->ptr[cg->rsp + off]);
        }

        void do_destroy() noexcept {
            if (!rm_ || !rm_->cg_) return;
#if !defined(XBYAK_NO_EXCEPTION)
            try {
                rm_->cg_->add(rm_->cg_->rsp, static_cast<uint32_t>(total_));
                rm_->managed_push_count_ -= static_cast<size_t>(total_) / 8;
                rm_->allocated_stack_space_ -= total_;
                rm_->layout_active_ = false;
            } catch (...) {
#ifndef NDEBUG
                fprintf(stderr, "CommittedLayout::~CommittedLayout: exception swallowed\n");
#endif
            }
#else
            rm_->cg_->add(rm_->cg_->rsp, static_cast<uint32_t>(total_));
            rm_->managed_push_count_ -= static_cast<size_t>(total_) / 8;
            rm_->allocated_stack_space_ -= total_;
            rm_->layout_active_ = false;
#endif
            rm_ = NULL;
        }
    };

    // Create a StackLayout builder.  Chain declarations on the returned object
    // then call build() to commit the frame.
    //
    //   auto cl = make_stack_layout()
    //       .gp_parks(2)
    //       .scratch(32)
    //       .build();
    //
    // Throws Xbyak::Error if no CodeGenerator has been provided.
    StackLayout make_stack_layout() {
        return StackLayout(*this);
    }

    // build() is defined here so it can reference CommittedLayout's constructor.
    // Called by StackLayout::build() which delegates here after computing layout.
    CommittedLayout build_layout(int gp_count, int vec_count,
                                 ptrdiff_t scratch_bytes, bool with_vol,
                                 int outgoing_args = 0) {
        if (!cg_) XBYAK_THROW_RET(ERR_RM_NO_CG, CommittedLayout(*this,0,0,0,0,0,0,0,0,{},0,{},0,0,false))
        // Nested layouts are not allowed: only one CommittedLayout may be open
        // at a time.  Destroy the current layout before building a new one.
        if (layout_active_) XBYAK_THROW_RET(ERR_RM_LAYOUT_ALREADY_ACTIVE,
                CommittedLayout(*this,0,0,0,0,0,0,0,0,{},0,{},0,0,false))
        if (gp_count < 0 || vec_count < 0)
            XBYAK_THROW_RET(ERR_RM_LAYOUT_SLOT_OOB,
                            CommittedLayout(*this,0,0,0,0,0,0,0,0,{},0,{},0,0,false))
        if (scratch_bytes < 0)
            XBYAK_THROW_RET(ERR_RM_LAYOUT_SCRATCH_OOB,
                            CommittedLayout(*this,0,0,0,0,0,0,0,0,{},0,{},0,0,false))
        // Round scratch up to the nearest 8-byte boundary so callers can
        // request an arbitrary byte count without caring about alignment.
        scratch_bytes = (scratch_bytes + 7) & ~ptrdiff_t(7);
        const int vec_slot = has_avx512_ ? 64 : 32;

        // Slot layout (all offsets relative to the new rsp after sub):
        //
        //  When with_outgoing_args(n) is used (slots at lowest addresses):
        //    [0 .. 32)                        — Win64 shadow space (omitted on SysV)
        //    [shadow .. + outgoing_args*8)    — outgoing stack-arg slots
        //    [outg_end .. + gp_count*8)       — GP park slots
        //    ...                              — vec parks, volatile saves, scratch
        //
        //  Default (no with_outgoing_args):
        //    [0 .. gp_count*8)              — GP park slots
        //    [gp_base .. + vec_count*slot)  — vec park slots
        //    [vec_end .. + vol_gp*8)        — volatile GP save slots
        //    [vol_gp_end .. + vol_vec*slot) — volatile vec save slots
        //    [vol_end .. + scratch)         — scratch
        ptrdiff_t cursor = 0;

        // Outgoing stack-arg slots must sit at the lowest rsp offsets so the
        // callee finds them at the ABI-correct positions after the call
        // instruction pushes the 8-byte return address.
        ptrdiff_t outgoing_arg_base = 0;
        if (outgoing_args > 0) {
#ifdef _WIN32
            cursor += 32;               // shadow space at [rsp+0..31]
            outgoing_arg_base = cursor; // overflow args start at [rsp+32]
#else
            outgoing_arg_base = 0;      // overflow args start at [rsp+0]
#endif
            cursor += static_cast<ptrdiff_t>(outgoing_args) * 8;
        }

        const ptrdiff_t gp_base = cursor;
        cursor += static_cast<ptrdiff_t>(gp_count) * 8;

        const ptrdiff_t vec_base = cursor;
        cursor += static_cast<ptrdiff_t>(vec_count) * vec_slot;

        // Reserve slots for the full set of ABI-volatile registers so that
        // save_volatiles() / restore_volatiles() work regardless of which
        // registers are allocated at build() time vs at the call site.
        std::vector<int> vol_gps, vol_vecs;
        if (with_vol) {
            for (int idx : base_free_gp()) vol_gps.push_back(idx);
            if (has_apx_)
                for (int i = 16; i <= 31; ++i) vol_gps.push_back(i);
            if (has_vec_base_) {
                for (int idx : base_free_vec()) vol_vecs.push_back(idx);
                if (has_avx512_)
                    for (int i = 16; i <= 31; ++i) vol_vecs.push_back(i);
            }
        }

        const ptrdiff_t vol_gp_base = cursor;
        cursor += static_cast<ptrdiff_t>(vol_gps.size()) * 8;

        const ptrdiff_t vol_vec_base = cursor;
        cursor += static_cast<ptrdiff_t>(vol_vecs.size()) * vec_slot;

        const ptrdiff_t scratch_base = cursor;
        cursor += scratch_bytes;

        // Compute the frame total with the correct alignment for the intended
        // call style.
        //
        // Standard case (no outgoing args, use emit_call()):
        //   total is always a multiple of 16.  emit_call() adds an 8-byte pad
        //   at emission time when managed_push_count_ is even.
        //
        // Outgoing-args case (use emit_layout_call()):
        //   CommittedLayout::emit_call() is a bare "mov rax; call rax" with no sub/add.
        //   For rsp to be 16-aligned at the call instruction:
        //     rsp_at_call = rsp_entry − 8·P − total   (P = managed_push_count_)
        //     rsp_entry = 16k − 8  (C caller's call pushed 8-byte return addr)
        //   Solving: (8 + 8P + total) % 16 == 0
        //     P even  → total ≡  8 (mod 16)
        //     P odd   → total ≡  0 (mod 16)  ← same as normal rounding
        ptrdiff_t total;
        if (outgoing_args > 0) {
            const bool p_even = (managed_push_count_ % 2 == 0);
            if (p_even) {
                // Round cursor up to nearest value ≡ 8 (mod 16).
                const ptrdiff_t next_16 = (cursor + 15) & ~ptrdiff_t(15);
                total = next_16 - 8;
                if (total < cursor) total = next_16 + 8;
            } else {
                total = (cursor + 15) & ~ptrdiff_t(15);
            }
        } else {
            // Round total up to nearest 16 bytes for stack alignment.
            total = (cursor + 15) & ~ptrdiff_t(15);
        }
        if (total == 0)
            XBYAK_THROW_RET(ERR_RM_LAYOUT_SLOT_OOB,
                            CommittedLayout(*this,0,0,0,0,0,0,0,0,{},0,{},0,0,false))

        return CommittedLayout(*this, gp_base, gp_count, vec_base, vec_count,
                               vec_slot, scratch_base, scratch_bytes, total,
                               std::move(vol_gps), vol_gp_base,
                               std::move(vol_vecs), vol_vec_base, vec_slot,
                               with_vol, outgoing_arg_base, outgoing_args);
    }

    // helper methods to query APX support
    bool has_apx() const { return has_apx_; }
    int max_gp_registers() const { return max_gp_reg_idx_ + 1; }

    // helper methods to query AVX-512 support
    bool has_avx512() const { return has_avx512_; }
    // Returns true when opmask registers (k1-k7) are available.
    // Requires AVX-512F and OS-enabled OPMASK state (XCR0[5]).
    // May be true even when full ZMM (zmm16-31) support is absent.
    bool has_opmask() const { return has_opmask_; }
    int max_vec_registers() const { return max_vec_reg_idx_ + 1; }

    // helper methods to query AMX support
    bool has_amx() const { return has_amx_; }
    int max_tile_registers() const { return has_amx_ ? 8 : 0; }

    // Associates a CodeGenerator with this manager so that features that emit
    // machine instructions (e.g. spill/restore) have access to the code stream.
    // Pass NULL to detach any previously associated generator.
    // The caller is responsible for ensuring the pointer remains valid for the
    // lifetime of this manager.
    void set_code_generator(Xbyak::CodeGenerator *cg) { cg_ = cg; }

    // Returns true if a CodeGenerator has been associated with this manager,
    // either at construction or via set_code_generator().
    bool has_code_generator() const { return cg_ != NULL; }

    // Resets all allocation tracking to the post-construction baseline.
    //
    // This method updates internal bookkeeping only — it emits no machine code.
    // Any push, sub rsp, or add rsp instructions already written into the code
    // buffer are unaffected.  This means reset() must always be paired with
    // CodeGenerator::reset(), which discards the old code buffer:
    //
    //   CodeGenerator::reset();     // discard old machine code
    //   RegPoolManager::reset();    // clear allocation tracking
    //   // ... build new kernel ...
    //
    // Calling reset() without also calling CodeGenerator::reset() leaves the
    // previously emitted instructions in the buffer.  If that code were executed
    // with unmatched spills or open frames, rsp would be wrong at ret().
    //
    // Preserved: cg_, ISA capability flags (has_apx_, has_avx512_, has_amx_),
    // and max register indices.
    // Cleared: all in-use, reserved, and spilled registers; prologue history;
    // stack tracking.
    void reset() {
        ++generation_;
        live_gp_.clear();
        free_gp_regs = base_free_gp();
        preserved_gp = base_preserved_gp();
        if (has_apx_) {
            for (int i = 16; i <= 31; ++i)
                free_gp_regs.insert(i);
        }
        live_vec_.clear();
        if (has_vec_base_) {
            free_vec_regs = base_free_vec();
            preserved_vec = base_preserved_vec();
        } else {
            free_vec_regs.clear();
            preserved_vec.clear();
        }
        if (has_avx512_) {
            for (int i = 16; i <= 31; ++i)
                free_vec_regs.insert(i);
        }
        live_opmask_.clear();
        if (has_opmask_) {
            free_opmask_regs = base_free_opmask();
            preserved_opmask = base_preserved_opmask();
        } else {
            free_opmask_regs.clear();
            preserved_opmask.clear();
        }
        live_tile_.clear();
        free_tile_regs.clear();
        if (has_amx_) {
            for (int i = 0; i <= 7; ++i)
                free_tile_regs.insert(i);
        }
        reserved_gp.clear();
        reserved_vec.clear();
        reserved_opmask.clear();
        reserved_tile.clear();
        allocated_preserved_gp_.clear();
        allocated_preserved_vec_.clear();
        prologue_gp_cursor_ = 0;
        prologue_vec_cursor_ = 0;
        managed_push_count_ = 0;
        allocated_stack_space_ = 0;
        layout_active_ = false;
    }

    // Emits a push instruction for each callee-saved GP register promoted by
    // alloc() since the last call to emit_prologue().  Call this once near the
    // top of the JIT function body, before any callee-saved registers are written.
    //
    // Safe to call more than once: only registers promoted after the previous
    // call are pushed on each subsequent invocation.
    //
    // On Windows x64, also emits sub rsp + movdqu for any callee-saved XMM
    // registers (xmm6-xmm15) promoted since the last call.
    //
    // Pair with emit_epilogue() just before ret to emit the matching restore
    // sequence.  Throws Xbyak::Error if no CodeGenerator was provided.
    void emit_prologue() {
        if (!cg_) XBYAK_THROW(ERR_RM_NO_CG)
        const int n_new = (int)allocated_preserved_gp_.size() - prologue_gp_cursor_;
        for (int i = prologue_gp_cursor_; i < (int)allocated_preserved_gp_.size(); ++i)
            cg_->push(Reg64(allocated_preserved_gp_[i]));
        prologue_gp_cursor_ = (int)allocated_preserved_gp_.size();
        managed_push_count_ += n_new;
#ifdef _WIN32
        // Windows x64: xmm6-xmm15 are callee-saved and must be saved to the
        // stack with movdqu; there is no push instruction for XMM registers.
        const int new_vecs = (int)allocated_preserved_vec_.size() - prologue_vec_cursor_;
        if (new_vecs > 0) {
            cg_->sub(cg_->rsp, new_vecs * 16);
            // Account for the stack space emitted as push-equivalent 8-byte slots.
            managed_push_count_ += static_cast<size_t>(new_vecs) * (16 / 8);
            for (int i = 0; i < new_vecs; ++i)
                cg_->movdqu(cg_->ptr[cg_->rsp + i * 16],
                            Xmm(allocated_preserved_vec_[prologue_vec_cursor_ + i]));
            prologue_vec_cursor_ = (int)allocated_preserved_vec_.size();
        }
#endif
    }

    // Emits the restore sequence that matches emit_prologue():
    //   - on Windows x64: movdqu to restore callee-saved XMM registers,
    //     then add rsp to reclaim the space
    //   - on all platforms: pop for each callee-saved GP in reverse
    //     allocation order, matching the push sequence from emit_prologue()
    //
    // Call this once just before ret.
    // Throws Xbyak::Error if no CodeGenerator was provided.
    void emit_epilogue() {
        if (!cg_) XBYAK_THROW(ERR_RM_NO_CG)
#ifdef _WIN32
        if (!allocated_preserved_vec_.empty()) {
            const int n = (int)allocated_preserved_vec_.size();
            for (int i = 0; i < n; ++i)
                cg_->movdqu(Xmm(allocated_preserved_vec_[i]), cg_->ptr[cg_->rsp + i * 16]);
            cg_->add(cg_->rsp, n * 16);
            // Adjust managed_push_count_ to account for the reclaimed stack space
            managed_push_count_ -= static_cast<size_t>(n) * (16 / 8);
        }
#endif
        for (int i = (int)allocated_preserved_gp_.size() - 1; i >= 0; --i)
            cg_->pop(Reg64(allocated_preserved_gp_[i]));
        managed_push_count_ -= allocated_preserved_gp_.size();
    }

    // Emits an ABI-correct call to a runtime C function.
    // Handles 16-byte stack alignment and (on Windows x64) the 32-byte shadow
    // space requirement automatically.
    //
    // func_ptr — address of the target function.
    //
    // Return-value note: emit_call() uses rax internally (mov rax, func_ptr;
    //                call rax).  If rax is currently allocated, its value will
    //                be destroyed.  Park it with CommittedLayout::park() before
    //                the call and reload it afterwards.
    //
    // In debug builds (NDEBUG not defined): asserts that rax is not currently
    //                allocated and prints a diagnostic to stderr if it is.
    //
    // Throws Xbyak::Error if no CodeGenerator has been provided.
    void emit_call(uint64_t func_ptr) {
        if (!cg_) XBYAK_THROW(ERR_RM_NO_CG)
#ifndef NDEBUG
        if (live_gp_.count(0)) {
            fprintf(stderr,
                    "emit_call: rax (index 0) is currently allocated — its value will be "
                    "destroyed by the call. Park it with CommittedLayout::park() before calling.\n");
            assert(!live_gp_.count(0) && "emit_call: rax is live and will be clobbered");
        }
#endif
        const bool needs_pad = (managed_push_count_ % 2) == 0;
#ifdef _WIN32
        const int adj = 32 + (needs_pad ? 8 : 0);
#else
        const int adj = needs_pad ? 8 : 0;
#endif
        if (adj > 0) cg_->sub(cg_->rsp, adj);
        cg_->mov(cg_->rax, func_ptr);
        cg_->call(cg_->rax);
        if (adj > 0) cg_->add(cg_->rsp, adj);
    }

    // Convenience overload: accepts a typed function pointer.
    // The pointer is reinterpreted as a uint64_t address before emission.
    template <typename FuncT>
    void emit_call(FuncT *func_ptr) {
        emit_call(reinterpret_cast<uint64_t>(func_ptr));
    }

    // Returns the indices of callee-saved GP registers promoted by alloc(),
    // in allocation order.  These are the registers that emit_prologue() will
    // push and emit_epilogue() will pop.
    std::vector<int> get_allocated_preserved_gps() const {
        return allocated_preserved_gp_;
    }

    // Returns the indices of callee-saved vector registers promoted by alloc().
    // On Windows x64, xmm6-xmm15 are callee-saved; this list is always empty
    // on Linux and macOS where all vector registers are caller-saved.
    std::vector<int> get_allocated_preserved_vecs() const {
        return allocated_preserved_vec_;
    }

    // Returns the stack-pointer register object (rsp, index 4).
    // Useful as an argument to add_to_gp_pool() when repurposing rsp is intentional.
    inline Reg64 stack_ptr() const { return Reg64(4); }
    // Returns the base-pointer register object (rbp, index 5).
    // Useful as an argument to add_to_gp_pool() when repurposing rbp is intentional.
    inline Reg64 base_ptr() const { return Reg64(5); }
    // Returns the opmask k0 register object.
    // k0 means "unmasked" — when used as a write mask all elements are written.
    // k0 is intentionally excluded from the opmask allocation pool; this accessor
    // provides a named way to reference it without constructing Opmask(0) directly.
    inline Opmask k0() const { return Opmask(0); }

private:
    // helper method - converts members of set to vector
    static inline std::vector<int> make_index_vector(const std::set<int> &set) {
        std::vector<int> indices;
        indices.reserve(set.size());
        for (int idx : set)
            indices.emplace_back(idx);
        return indices;
    }

    //private helpers — reserve / unreserve for each family
    void reserve_reg_gp(int idx) {
        if (idx < 0 || idx > max_gp_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        if (live_gp_.count(idx))   XBYAK_THROW(ERR_RM_GP_IN_USE)
        if (reserved_gp.count(idx)) XBYAK_THROW(ERR_RM_REG_ALREADY_TRACKED)
        // Remove from whichever pool currently holds it.
        if (!free_gp_regs.erase(idx) && !preserved_gp.erase(idx))
            XBYAK_THROW(ERR_RM_GP_NOT_AVAILABLE)
        reserved_gp.insert(idx);
    }

    void unreserve_reg_gp(int idx) {
        if (idx < 0 || idx > max_gp_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        if (!reserved_gp.erase(idx))
            XBYAK_THROW(ERR_RM_GP_NOT_AVAILABLE)
        // Return to original pool based on ABI classification.
        // APX extended regs (r16-r31) are always volatile.
        if (idx <= max_gp_reg_idx_ && base_preserved_gp().count(idx))
            preserved_gp.insert(idx);
        else
            free_gp_regs.insert(idx);
    }

    void reserve_reg_vec(int idx) {
        if (idx < 0 || idx > max_vec_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        if (live_vec_.count(idx))   XBYAK_THROW(ERR_RM_VEC_IN_USE)
        if (reserved_vec.count(idx)) XBYAK_THROW(ERR_RM_REG_ALREADY_TRACKED)
        if (!free_vec_regs.erase(idx) && !preserved_vec.erase(idx))
            XBYAK_THROW(ERR_RM_VEC_NOT_AVAILABLE)
        reserved_vec.insert(idx);
    }

    void unreserve_reg_vec(int idx) {
        if (idx < 0 || idx > max_vec_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        if (!reserved_vec.erase(idx))
            XBYAK_THROW(ERR_RM_VEC_NOT_AVAILABLE)
        if (base_preserved_vec().count(idx))
            preserved_vec.insert(idx);
        else
            free_vec_regs.insert(idx);
    }

    void reserve_reg_opmask(int idx) {
        if (idx < 0 || idx > 7)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        if (live_opmask_.count(idx))   XBYAK_THROW(ERR_RM_OPMASK_IN_USE)
        if (reserved_opmask.count(idx)) XBYAK_THROW(ERR_RM_REG_ALREADY_TRACKED)
        if (!free_opmask_regs.erase(idx) && !preserved_opmask.erase(idx))
            XBYAK_THROW(ERR_RM_OPMASK_NOT_AVAILABLE)
        reserved_opmask.insert(idx);
    }

    void unreserve_reg_opmask(int idx) {
        if (idx < 0 || idx > 7)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        if (!reserved_opmask.erase(idx))
            XBYAK_THROW(ERR_RM_OPMASK_NOT_AVAILABLE)
        // All opmask registers are volatile; return to free pool.
        free_opmask_regs.insert(idx);
    }

    void reserve_reg_tile(int idx) {
        if (idx < 0 || idx > 7)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        if (live_tile_.count(idx))    XBYAK_THROW(ERR_RM_TILE_IN_USE)
        if (reserved_tile.count(idx))  XBYAK_THROW(ERR_RM_REG_ALREADY_TRACKED)
        if (!free_tile_regs.erase(idx))
            XBYAK_THROW(ERR_RM_TILE_NOT_AVAILABLE)
        reserved_tile.insert(idx);
    }

    void unreserve_reg_tile(int idx) {
        if (idx < 0 || idx > 7)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        if (!reserved_tile.erase(idx))
            XBYAK_THROW(ERR_RM_TILE_NOT_AVAILABLE)
        // All tile registers are volatile; return to free pool.
        free_tile_regs.insert(idx);
    }

    // helper method - checks reg in use before scoping
    template <class RegT>
    static void validate_scoped_reg(RegPoolManager *rm, RegT reg) {
        if (!rm->reg_in_use(reg))
            XBYAK_THROW(ERR_RM_SCOPED_REG_NOT_IN_USE)
    }

    // helper method - checks if a register index for a given family is currently in use
    bool reg_in_use_idx(int idx, RegFamily family) const {
        switch (family) {
            case RegFamily::GP:
                if (idx < 0 || idx > max_gp_reg_idx_)
                    XBYAK_THROW_RET(ERR_RM_REG_IDX_OUT_OF_RANGE, false)
                return live_gp_.find(idx) != live_gp_.end();
            case RegFamily::Vec:
                if (idx < 0 || idx > max_vec_reg_idx_)
                    XBYAK_THROW_RET(ERR_RM_REG_IDX_OUT_OF_RANGE, false)
                return live_vec_.find(idx) != live_vec_.end();
            case RegFamily::Opmask:
                if (idx < 0 || idx > 7)
                    XBYAK_THROW_RET(ERR_RM_REG_IDX_OUT_OF_RANGE, false)
                return live_opmask_.find(idx) != live_opmask_.end();
            case RegFamily::Tile:
                if (idx < 0 || idx > 7)
                    XBYAK_THROW_RET(ERR_RM_REG_IDX_OUT_OF_RANGE, false)
                return live_tile_.find(idx) != live_tile_.end();
            default: XBYAK_THROW_RET(ERR_INTERNAL, false)
        }
    }

    // helper method - finds next free register for a given family
    int next_gp_idx() const {
        if (!free_gp_regs.empty()) return *free_gp_regs.begin();
        if (!preserved_gp.empty()) return *preserved_gp.begin();
        XBYAK_THROW_RET(ERR_RM_NO_FREE_GP, 0)
    }
    int next_vec_idx() const {
        if (!free_vec_regs.empty()) return *free_vec_regs.begin();
        if (!preserved_vec.empty()) return *preserved_vec.begin();
        XBYAK_THROW_RET(ERR_RM_NO_FREE_VEC, 0)
    }
    int next_opmask_idx() const {
        if (!free_opmask_regs.empty()) return *free_opmask_regs.begin();
        if (!preserved_opmask.empty()) return *preserved_opmask.begin();
        XBYAK_THROW_RET(ERR_RM_NO_FREE_OPMASK, 0)
    }
    int next_tile_idx() const {
        if (!free_tile_regs.empty()) return *free_tile_regs.begin();
        XBYAK_THROW_RET(ERR_RM_NO_FREE_TILE, 0)
    }

    // tracking for in-use indices for a given register family
    void gp_reg(int idx) {
        if (reg_in_use_idx(idx, RegFamily::GP))
            XBYAK_THROW(ERR_RM_GP_IN_USE)
        auto it = free_gp_regs.find(idx);
        auto pres_it = preserved_gp.find(idx);
        if (it != free_gp_regs.end()) {
            live_gp_.insert(idx);
            free_gp_regs.erase(it);
        } else if (pres_it != preserved_gp.end()) {
            live_gp_.insert(idx);
            preserved_gp.erase(pres_it);
            allocated_preserved_gp_.push_back(idx);
        } else {
            XBYAK_THROW(ERR_RM_GP_NOT_AVAILABLE)
        }
    }
    void vec_reg(int idx) {
        if (reg_in_use_idx(idx, RegFamily::Vec))
            XBYAK_THROW(ERR_RM_VEC_IN_USE)
        auto it = free_vec_regs.find(idx);
        auto pres_it = preserved_vec.find(idx);
        if (it != free_vec_regs.end()) {
            live_vec_.insert(idx);
            free_vec_regs.erase(it);
        } else if (pres_it != preserved_vec.end()) {
            live_vec_.insert(idx);
            preserved_vec.erase(pres_it);
            allocated_preserved_vec_.push_back(idx);
        } else {
            XBYAK_THROW(ERR_RM_VEC_NOT_AVAILABLE)
        }
    }
    void opmask_reg(int idx) {
        if (reg_in_use_idx(idx, RegFamily::Opmask))
            XBYAK_THROW(ERR_RM_OPMASK_IN_USE)
        auto it = free_opmask_regs.find(idx);
        auto pres_it = preserved_opmask.find(idx);
        if (it != free_opmask_regs.end()) {
            live_opmask_.insert(idx);
            free_opmask_regs.erase(it);
        } else if (pres_it != preserved_opmask.end()) {
            live_opmask_.insert(idx);
            preserved_opmask.erase(pres_it);
        } else {
            XBYAK_THROW(ERR_RM_OPMASK_NOT_AVAILABLE)
        }
    }
    void tile_reg(int idx) {
        if (reg_in_use_idx(idx, RegFamily::Tile))
            XBYAK_THROW(ERR_RM_TILE_IN_USE)
        auto it = free_tile_regs.find(idx);
        if (it != free_tile_regs.end()) {
            live_tile_.insert(idx);
            free_tile_regs.erase(it);
        } else {
            XBYAK_THROW(ERR_RM_TILE_NOT_AVAILABLE)
        }
    }

    // member function - moves given index from in-use set to free set for given family
    void release_gp(int idx) {
        if (idx < 0 || idx > max_gp_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        auto it = live_gp_.find(idx);
        if (it == live_gp_.end())
            XBYAK_THROW(ERR_RM_GP_NOT_IN_USE)
        live_gp_.erase(it);
        if (base_preserved_gp().count(idx))
            preserved_gp.insert(idx);
        else
            free_gp_regs.insert(idx);
    }
    void release_vec(int idx) {
        if (idx < 0 || idx > max_vec_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        auto it = live_vec_.find(idx);
        if (it == live_vec_.end())
            XBYAK_THROW(ERR_RM_VEC_NOT_IN_USE)
        live_vec_.erase(it);
        if (base_preserved_vec().count(idx))
            preserved_vec.insert(idx);
        else
            free_vec_regs.insert(idx);
    }
    void release_opmask(int idx) {
        if (idx < 0 || idx > 7)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        auto it = live_opmask_.find(idx);
        if (it == live_opmask_.end())
            XBYAK_THROW(ERR_RM_OPMASK_NOT_IN_USE)
        live_opmask_.erase(it);
        free_opmask_regs.insert(idx);
    }
    void release_tile(int idx) {
        if (idx < 0 || idx > 7)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        auto it = live_tile_.find(idx);
        if (it == live_tile_.end())
            XBYAK_THROW(ERR_RM_TILE_NOT_IN_USE)
        live_tile_.erase(it);
        free_tile_regs.insert(idx);
    }

    // General Purpose registers (GP):
    // Indices: rax=0, rcx=1, rdx=2, rbx=3, rsp=4, rbp=5, rsi=6, rdi=7, r8-r15=8-15
    // Note: rsp (4) and rbp (5) are special and not typically allocated
#ifdef _WIN32
    // Windows x64 calling convention:
    // Volatile (caller-saved): rax, rcx, rdx, r8-r11 (7 registers)
    // Non-volatile (callee-saved): rbx, rbp, rdi, rsi, r12-r15 (8 registers)
    static const std::set<int> &base_free_gp() {
        static const std::set<int> s {0, 1, 2, 8, 9, 10, 11};
        return s;
    }
    static const std::set<int> &base_preserved_gp() {
        static const std::set<int> s {3, 6, 7, 12, 13, 14, 15};
        return s;
    }
#else
    // System V AMD64 ABI calling convention for Linux/macOS:
    // Call-clobbered (caller-saved): rax, rcx, rdx, rsi, rdi, r8-r11 (9 registers)
    // Call-preserved (callee-saved): rbx, r12-r15 (5 registers)
    static const std::set<int> &base_free_gp() {
        static const std::set<int> s {0, 1, 2, 6, 7, 8, 9, 10, 11};
        return s;
    }
    static const std::set<int> &base_preserved_gp() {
        static const std::set<int> s {3, 12, 13, 14, 15};
        return s;
    }
#endif

    std::set<int> live_gp_;
    std::set<int> free_gp_regs = base_free_gp();
    std::set<int> preserved_gp = base_preserved_gp();

    // Vector registers (XMM/YMM/ZMM):
    // - SSE/AVX/AVX2: 0-15 (xmm0-xmm15, ymm0-ymm15)
    // - AVX-512: 0-31 (xmm0-xmm31, ymm0-ymm31, zmm0-zmm31)
    // Note: Only include 0-15 by default; extended registers (16-31) are added in constructor if AVX-512 is detected
#ifdef _WIN32
    // Windows x64 calling convention:
    // Volatile (caller-saved): xmm0-xmm5 (6 registers)
    // Non-volatile (callee-saved): xmm6-xmm15 (10 registers)
    static const std::set<int> &base_free_vec() {
        static const std::set<int> s {0, 1, 2, 3, 4, 5};
        return s;
    }
    static const std::set<int> &base_preserved_vec() {
        static const std::set<int> s {6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        return s;
    }
#else
    // System V AMD64 ABI (Linux/macOS):
    // Call-clobbered: all vector registers are caller-saved
    static const std::set<int> &base_free_vec() {
        static const std::set<int> s {
                0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        return s;
    }
    static const std::set<int> &base_preserved_vec() {
        static const std::set<int> s {};
        return s;
    }
#endif

    std::set<int> live_vec_;
    std::set<int> free_vec_regs;              // populated in constructor after XCR0[1:2] check
    std::set<int> preserved_vec;              // populated in constructor after XCR0[1:2] check

    // Opmask registers (k0-k7) for AVX-512
    // k0 has special meaning (unmasked), typically k1-k7 are used
    // All opmask registers are call-clobbered
    static const std::set<int> &base_free_opmask() {
        static const std::set<int> s {1, 2, 3, 4, 5, 6, 7};
        return s;
    }
    // No preserved opmask registers
    static const std::set<int> &base_preserved_opmask() {
        static const std::set<int> s {};
        return s;
    }

    std::set<int> live_opmask_;
    std::set<int> free_opmask_regs; // populated in constructor if AVX-512 present
    std::set<int> preserved_opmask; // populated in constructor if AVX-512 present

    // Registers blocked from allocation via mark_unavailable().
    std::set<int> reserved_gp;
    std::set<int> reserved_vec;
    std::set<int> reserved_opmask;
    std::set<int> reserved_tile;

    // APX feature support
    bool has_apx_ = false;
    int max_gp_reg_idx_ = 15; // 15 without APX, 31 with APX

    // AVX-512 feature support
    bool has_avx512_ = false; // true when XCR0[5:7] all set (full ZMM support including zmm16-31)
    bool has_opmask_ = false; // true when XCR0[5] set (k registers available; subset of AVX-512)
    // 15 without (SSE/AVX/AVX2), 31 with AVX-512
    int max_vec_reg_idx_ = 15;
    // True when the OS has enabled XMM and YMM state saving (XCR0[1:2] == 3).
    // Determines whether base_free_vec() / base_preserved_vec() are applied.
    bool has_vec_base_ = false;

    // AMX tile registers (tmm0-tmm7): no preserved tiles, all caller-saved
    // Pool is empty by default; tmm0-tmm7 are added in constructor if AMX detected
    std::set<int> live_tile_;
    std::set<int> free_tile_regs;

    // AMX feature support
    bool has_amx_ = false;

    // Callee-saved registers promoted by alloc(), in allocation order.
    // emit_prologue() pushes these; emit_epilogue() pops them in reverse.
    std::vector<int> allocated_preserved_gp_;
    std::vector<int> allocated_preserved_vec_;  // non-empty only on Windows (xmm6-xmm15)

    // Cursors for idempotent emit_prologue(): count of entries already emitted
    // by previous emit_prologue() calls.
    int prologue_gp_cursor_;
    int prologue_vec_cursor_;

    // Running count of push-equivalent 8-byte stack slots emitted by the manager
    // since function entry (incremented by emit_prologue, decremented by
    // emit_epilogue).  Used by emit_call to compute 16-byte stack alignment.
    size_t managed_push_count_;

    // Total bytes currently reserved by live CommittedLayout objects.
    ptrdiff_t allocated_stack_space_;

    // Set by CommittedLayout construction, cleared by destroy()/destructor.
    // Guards against nested build() calls: only one CommittedLayout may be
    // open at a time on a given RegPoolManager.
    bool layout_active_ = false;

    // Optional CodeGenerator for instruction-emitting features (spill/restore, etc.).
    // Null when the manager is used for tracking only.
    Xbyak::CodeGenerator *cg_;

    // Incremented by reset() to invalidate any Scoped guards that outlive the
    // current kernel build.  A Scoped whose generation_ differs from the
    // manager's generation_ will skip the free() in its destructor, preventing
    // double-free or corrupt state after a reset.
    std::size_t generation_ = 0;
};

} // namespace Xbyak

#endif // XBYAK_REG_MANAGER_HPP
