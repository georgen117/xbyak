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
#ifndef CPU_X64_XBYAK_REG_MANAGER_HPP
#define CPU_X64_XBYAK_REG_MANAGER_HPP

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>
#include <type_traits>

#define XBYAK64
#define XBYAK_NO_OP_NAMES
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
    //       emitted by spill(), restore(), make_stack_frame(), emit_prologue(), and
    //       emit_epilogue().  Pass NULL (default) when only register tracking is
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

        // Detect AVX-512 for extended vector registers (zmm16-zmm31)
        if (cpu.has(Xbyak::util::Cpu::tAVX512F)) {
            has_avx512_ = ((xcr0 >> 7) & 1) == 1; // Check if XCR0[7] is set for AVX-512 support
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

    // Returns true if no registers are currently on the spill stack.
    // Use this to inspect state programmatically.  For a hard stop in debug
    // builds, use assert_spill_stack_empty() instead.
    bool spill_stack_empty() const {
        return spill_stack_gp_.empty();
    }

    // Checks that every spill()ed register has been matched by a restore().
    //
    // Call this at the end of JIT kernel construction alongside assert_all_free()
    // to confirm there are no dangling spills.  An unrestored spill leaves a
    // stale value on the hardware stack, making the return address unreachable.
    //
    // In debug builds (NDEBUG not defined): prints the indices of unrestored
    // spilled registers to stderr and triggers an assertion.
    //
    // In release builds (NDEBUG defined): compiles to nothing.
    void assert_spill_stack_empty() const {
#ifndef NDEBUG
        if (!spill_stack_gp_.empty()) {
            fprintf(stderr, "assert_spill_stack_empty: unrestored spills:");
            for (size_t i = 0; i < spill_stack_gp_.size(); ++i)
                fprintf(stderr, " %d", spill_stack_gp_[i]);
            fprintf(stderr, "\n");
        }
        assert(spill_stack_gp_.empty() &&
               "assert_spill_stack_empty: spill() without matching restore()");
#endif
    }

    // Returns true if both the spill stack is empty and no StackFrame is open.
    // Use this to inspect state programmatically.  For a hard stop in debug
    // builds, use assert_clean_stack() instead.
    bool clean_stack() const {
        return spill_stack_gp_.empty() && allocated_stack_space_ == 0;
    }

    // Checks that the hardware stack is fully balanced: no unrestored spills
    // and no open StackFrames.
    //
    // Call this just before ret() to confirm every spill() has a restore() and
    // every make_stack_frame() result has been destroyed.  An open frame or
    // dangling spill corrupts rsp, making the return address unreachable.
    //
    // In debug builds (NDEBUG not defined): prints diagnostic information to
    // stderr and triggers an assertion.
    //
    // In release builds (NDEBUG defined): compiles to nothing.
    void assert_clean_stack() const {
#ifndef NDEBUG
        assert_spill_stack_empty();
        if (allocated_stack_space_ != 0)
            fprintf(stderr,
                    "assert_clean_stack: StackFrame not destroyed (%td bytes still allocated)\n",
                    allocated_stack_space_);
        assert(clean_stack() &&
               "assert_clean_stack: unbalanced stack — open StackFrame or unrestored spill");
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

    // member function - add a register to the free pool of general registers
    void add_to_gp_pool(const Reg64 &reg) { add_to_gp_pool(reg.getIdx()); }
    void add_to_gp_pool(int idx) {
        if (idx < 0 || idx > max_gp_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        const bool in_free = free_gp_regs.count(idx) != 0;
        const bool in_preserved = preserved_gp.count(idx) != 0;
        const bool in_use = live_gp_.count(idx) != 0;
        if (in_free || in_preserved || in_use)
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
            : rm_(&rm), reg_(r) {
            validate_scoped_reg(rm_, reg_);
        }

        // if object is owner of scoped reg and goes out of scope, deallocate
        ~Scoped() noexcept {
            if (!rm_) return;
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
        Scoped(Scoped &&other) noexcept : rm_(other.rm_), reg_(other.reg_) {
            other.rm_ = NULL; // set previous owner to no longer own
        }

        // expose underlying register for implicit use in JIT helpers
        operator const Reg &() const noexcept { return reg_; }
        const Reg &get() const noexcept { return reg_; }

    private:
        RegPoolManager *rm_
                = NULL; // pointer to allocator, initialised as NULL
        Reg reg_ {};
    };

    // helper factory - calls Scoped ctor
    template <class Reg>
    inline Scoped<Reg> makeScoped(Reg r) & {
        return Scoped<Reg>(*this, r);
    }

    // RAII helper owning a stack frame allocated via make_stack_frame().
    // Construction emits: sub rsp, size
    // Destruction emits:  add rsp, size  (unless destroy() was called first)
    // Move-only; do not copy.  All nested frames must be closed in LIFO order.
    class StackFrame {
    public:
        StackFrame(RegPoolManager &rm, ptrdiff_t size)
                : rm_(&rm), size_(size) {
            if (!rm_->cg_) XBYAK_THROW(ERR_RM_NO_CG)
            if (size_ <= 0 || (size_ % 8) != 0) XBYAK_THROW(ERR_RM_STACK_FRAME_SIZE_INVALID)
            rm_->cg_->sub(rm_->cg_->rsp, static_cast<uint32_t>(size_));
            rm_->managed_push_count_ += static_cast<size_t>(size_) / 8;
            rm_->allocated_stack_space_ += size_;
        }

        ~StackFrame() noexcept {
            if (!rm_ || !rm_->cg_) return;
#if !defined(XBYAK_NO_EXCEPTION)
            try {
                rm_->cg_->add(rm_->cg_->rsp, static_cast<uint32_t>(size_));
                rm_->managed_push_count_ -= static_cast<size_t>(size_) / 8;
                rm_->allocated_stack_space_ -= size_;
            } catch (...) {
#ifndef NDEBUG
                fprintf(stderr, "RegPoolManager::StackFrame::~StackFrame: exception swallowed\n");
#endif
            }
#else
            rm_->cg_->add(rm_->cg_->rsp, static_cast<uint32_t>(size_));
            rm_->managed_push_count_ -= static_cast<size_t>(size_) / 8;
            rm_->allocated_stack_space_ -= size_;
#endif
        }

        StackFrame(const StackFrame &) = delete;
        StackFrame &operator=(const StackFrame &) = delete;

        StackFrame(StackFrame &&other) noexcept : rm_(other.rm_), size_(other.size_) {
            other.rm_ = NULL;
        }

        // Explicitly closes the frame: emits add rsp, size immediately and
        // makes the destructor a no-op.  Use this instead of a scope block when
        // the frame lifetime does not align neatly with C++ scope boundaries,
        // e.g. when ret() must follow immediately:
        //
        //   auto frame = make_stack_frame(N);
        //   ...
        //   frame.destroy();  // add rsp, N emitted here
        //   ret();            // correct: rsp fully restored
        //
        // Calling destroy() more than once on the same frame is a no-op.
        void destroy() {
            if (!rm_) return;
            if (rm_->cg_) {
                rm_->cg_->add(rm_->cg_->rsp, static_cast<uint32_t>(size_));
                rm_->managed_push_count_ -= static_cast<size_t>(size_) / 8;
                rm_->allocated_stack_space_ -= size_;
            }
            rm_ = NULL; // disarms destructor
        }

        // Store reg at [rsp + offset] and free it from the allocator.
        // Use this to park a register value on the stack while reclaiming the
        // hardware register for other uses.
        template <class RegT>
        void put_on_stack(RegT &reg, ptrdiff_t offset) {
            check_offset(offset);
            do_store(rm_->cg_, reg, offset);
            rm_->free(reg);
        }

        // Store reg at [rsp + offset] without freeing it.
        template <class RegT>
        void put_on_stack(const RegT &reg, ptrdiff_t offset) {
            check_offset(offset);
            do_store(rm_->cg_, reg, offset);
        }

        // Allocate a register of type RegT, load [rsp + offset] into it, and
        // return it.  Caller is responsible for freeing the returned register.
        template <class RegT>
        RegT read_from_stack(ptrdiff_t offset) {
            check_offset(offset);
            RegT reg = rm_->alloc<RegT>();
            do_load(rm_->cg_, reg, offset);
            return reg;
        }

        ptrdiff_t size() const { return size_; }

    private:
        RegPoolManager *rm_;
        ptrdiff_t       size_;

        void check_offset(ptrdiff_t offset) const {
            if (offset < 0 || offset >= size_)
                XBYAK_THROW(ERR_RM_STACK_FRAME_OFFSET_OOB)
        }

        // GP store / load — one overload per concrete register type (C++11).
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
    };

    // Allocates a stack frame of size bytes.  Emits sub rsp, size immediately
    // and add rsp, size when the frame is closed.
    // size must be a positive multiple of 8; multiples of 16 ensure the stack
    // remains 16-byte aligned after the allocation.
    //
    // Close the frame explicitly with destroy() before ret() — preferred:
    //   auto frame = make_stack_frame(N);
    //   ...
    //   frame.destroy();  // add rsp, N emitted here
    //   ret();
    //
    // Alternatively, use a block scope to let the destructor close it:
    //   { auto frame = make_stack_frame(N); ...; }  // add rsp, N on scope exit
    //   ret();
    //
    // Throws Xbyak::Error if no CodeGenerator has been provided.
    StackFrame make_stack_frame(ptrdiff_t size) {
        return StackFrame(*this, size);
    }

    // -------------------------------------------------------------------------
    // StackLayout / CommittedLayout — two-phase unified stack management
    //
    // Guarantees rsp moves exactly once (at build()) and never again until
    // destroy().  All slot offsets are fixed at build() time, so park,
    // reload, and scratch operations remain valid regardless of what else
    // happens between build() and destroy().
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
                  scratch_bytes_(0), with_volatile_save_(false) {}

        // Reserve n GP-sized (8-byte) park slots.
        StackLayout &gp_parks(int n) { gp_count_ = n; return *this; }

        // Reserve n vector park slots.
        // Each slot is 64 bytes when AVX-512 is present, 32 bytes otherwise.
        StackLayout &vec_parks(int n) { vec_count_ = n; return *this; }

        // Reserve a raw scratch area of 'bytes' bytes (must be a positive multiple of 8).
        StackLayout &scratch(ptrdiff_t bytes) { scratch_bytes_ = bytes; return *this; }

        // Reserve fixed-offset slots for all ABI-volatile GP and vector registers.
        // save_volatiles() will store only those that are live at call time;
        // restore_volatiles() reloads exactly the same set.  Because slots are
        // sized for every possible volatile register, the set of live registers
        // at save_volatiles() time does not need to match the set at build() time.
        StackLayout &with_volatile_save() { with_volatile_save_ = true; return *this; }

        // Commit: compute the total frame size, emit sub rsp, <total>, and
        // return a CommittedLayout owning the frame.
        // Throws Xbyak::Error if no CodeGenerator has been provided, or if a
        // CommittedLayout is already active on this manager.
        CommittedLayout build() {
            return rm_->build_layout(gp_count_, vec_count_,
                                     scratch_bytes_, with_volatile_save_);
        }

    private:
        RegPoolManager *rm_;
        int    gp_count_;
        int    vec_count_;
        ptrdiff_t scratch_bytes_;
        bool   with_volatile_save_;
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
                        bool           vol_save_declared)
                : rm_(&rm), gp_base_(gp_base), gp_count_(gp_count),
                  vec_base_(vec_base), vec_count_(vec_count),
                  vec_slot_bytes_(vec_slot_bytes),
                  scratch_base_(scratch_base), scratch_bytes_(scratch_bytes),
                  total_(total),
                  volatile_gps_(volatile_gps), vol_gp_base_(vol_gp_base),
                  volatile_vecs_(volatile_vecs), vol_vec_base_(vol_vec_base),
                  vol_vec_slot_bytes_(vol_vec_slot_bytes),
                  vol_save_declared_(vol_save_declared),
                  vol_save_armed_(false) {
            if (!rm_->cg_) XBYAK_THROW(ERR_RM_NO_CG)
            rm_->cg_->sub(rm_->cg_->rsp, static_cast<uint32_t>(total_));
            rm_->managed_push_count_ += static_cast<size_t>(total_) / 8;
            rm_->allocated_stack_space_ += total_;
#ifndef NDEBUG
            rm_->layout_active_ = true;
#endif
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
#ifndef NDEBUG
                rm_->layout_active_ = false;
#endif
            } catch (...) {
#ifndef NDEBUG
                fprintf(stderr, "CommittedLayout::~CommittedLayout: exception swallowed\n");
#endif
            }
#else
            rm_->cg_->add(rm_->cg_->rsp, static_cast<uint32_t>(total_));
            rm_->managed_push_count_ -= static_cast<size_t>(total_) / 8;
            rm_->allocated_stack_space_ -= total_;
#ifndef NDEBUG
            rm_->layout_active_ = false;
#endif
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
                                 ptrdiff_t scratch_bytes, bool with_vol) {
        if (!cg_) XBYAK_THROW_RET(ERR_RM_NO_CG, CommittedLayout(*this,0,0,0,0,0,0,0,0,{},0,{},0,0,false))
#ifndef NDEBUG
        if (layout_active_) XBYAK_THROW_RET(ERR_RM_LAYOUT_ALREADY_ACTIVE,
                CommittedLayout(*this,0,0,0,0,0,0,0,0,{},0,{},0,0,false))
#endif
        const int vec_slot = has_avx512_ ? 64 : 32;

        // Slot layout (all offsets relative to the new rsp after sub):
        //  [0 .. gp_count*8)              — GP park slots
        //  [gp_base .. + vec_count*slot)  — vec park slots
        //  [vec_end .. + vol_gp*8)        — volatile GP save slots
        //  [vol_gp_end .. + vol_vec*slot) — volatile vec save slots
        //  [vol_end .. + scratch)         — scratch
        ptrdiff_t cursor = 0;

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

        // Round total up to nearest 16 bytes for stack alignment.
        ptrdiff_t total = (cursor + 15) & ~ptrdiff_t(15);
        if (total == 0) total = 16; // always emit something for simpler state

        return CommittedLayout(*this, gp_base, gp_count, vec_base, vec_count,
                               vec_slot, scratch_base, scratch_bytes, total,
                               std::move(vol_gps), vol_gp_base,
                               std::move(vol_vecs), vol_vec_base, vec_slot,
                               with_vol);
    }

    // helper methods to query APX support
    bool has_apx() const { return has_apx_; }
    int max_gp_registers() const { return max_gp_reg_idx_ + 1; }

    // helper methods to query AVX-512 support
    bool has_avx512() const { return has_avx512_; }
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
        free_opmask_regs = base_free_opmask();
        preserved_opmask = base_preserved_opmask();
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
        spill_stack_gp_.clear();
        saved_volatile_gp_.clear();
        saved_volatile_vec_.clear();
        saved_volatile_vec_bytes_ = 0;
        saved_volatiles_armed_ = false;
        allocated_preserved_gp_.clear();
        allocated_preserved_vec_.clear();
        prologue_gp_cursor_ = 0;
        prologue_vec_cursor_ = 0;
        managed_push_count_ = 0;
        allocated_stack_space_ = 0;
#ifndef NDEBUG
        layout_active_ = false;
#endif
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
    }

    // Emits an ABI-correct call to a runtime C function.
    // Handles 16-byte stack alignment and (on Windows x64) the 32-byte shadow
    // space requirement automatically.
    //
    // func_ptr     — address of the target function.
    // extra_pushes — escape hatch for push instructions emitted directly via
    //                CodeGenerator that the manager has not tracked (e.g. a raw
    //                push(rbx) to stash a return value).  Default 0.
    //                Normal usage should route all pushes through spill(),
    //                emit_prologue(), or save_volatiles(), in which case
    //                extra_pushes is always 0.
    //
    // Return-value note: emit_call() uses rax internally (mov rax, func_ptr;
    //                call rax).  If rax is currently allocated, its value will
    //                be destroyed.  Use save_volatiles() before the call and
    //                stash the return value (mov preserved_reg, rax) before
    //                calling restore_volatiles().
    //
    // In debug builds (NDEBUG not defined): asserts that rax is not currently
    //                allocated and prints a diagnostic to stderr if it is.
    //
    // Throws Xbyak::Error if no CodeGenerator has been provided.
    void emit_call(uint64_t func_ptr, size_t extra_pushes = 0) {
        if (!cg_) XBYAK_THROW(ERR_RM_NO_CG)
#ifndef NDEBUG
        if (live_gp_.count(0)) {
            fprintf(stderr,
                    "emit_call: rax (index 0) is currently allocated — its value will be "
                    "destroyed by the call. Save it with save_volatiles() before calling.\n");
            assert(!live_gp_.count(0) && "emit_call: rax is live and will be clobbered");
        }
#endif
        const size_t total_pushes = managed_push_count_ + extra_pushes;
        const bool needs_pad = (total_pushes % 2) == 0;
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
    void emit_call(FuncT *func_ptr, size_t extra_pushes = 0) {
        emit_call(reinterpret_cast<uint64_t>(func_ptr), extra_pushes);
    }

    // Pushes reg onto the hardware stack, marks it not-in-use, and returns its
    // index to the free pool so alloc() can reuse it as a scratch register.
    // Pair each spill() with a matching restore() once the scratch is freed.
    //
    // Throws Xbyak::Error if reg is not currently allocated, or if no
    // CodeGenerator has been provided.
    void spill(const Reg64 &reg) {
        if (!cg_) XBYAK_THROW(ERR_RM_NO_CG)
        const int idx = reg.getIdx();
        if (!live_gp_.count(idx)) XBYAK_THROW(ERR_RM_SPILL_NOT_IN_USE)
        cg_->push(Reg64(idx));
        ++managed_push_count_;
        live_gp_.erase(idx);
        free_gp_regs.insert(idx);
        spill_stack_gp_.push_back(idx);
    }

    // Restores the most-recently-spilled GP register (LIFO).
    // Emits pop, removes it from the free pool, and re-adds it to in-use.
    // Returns the restored register.
    //
    // Throws Xbyak::Error if nothing is spilled, or if no CodeGenerator
    // has been provided.
    Reg64 restore() {
        if (!cg_) XBYAK_THROW_RET(ERR_RM_NO_CG, Reg64(0))
        if (spill_stack_gp_.empty()) XBYAK_THROW_RET(ERR_RM_SPILL_NOT_IN_USE, Reg64(0))
        const int idx = spill_stack_gp_.back();
        spill_stack_gp_.pop_back();
        cg_->pop(Reg64(idx));
        --managed_push_count_;
        free_gp_regs.erase(idx);
        live_gp_.insert(idx);
        return Reg64(idx);
    }

    // Restores a specific spilled register; reg must be the most-recently-spilled
    // (top of the spill stack).  Throws if reg is not at the top, the stack is
    // empty, or no CodeGenerator has been provided.
    void restore(const Reg64 &reg) {
        if (!cg_) XBYAK_THROW(ERR_RM_NO_CG)
        if (spill_stack_gp_.empty() || spill_stack_gp_.back() != reg.getIdx())
            XBYAK_THROW(ERR_RM_SPILL_NOT_IN_USE)
        const int idx = spill_stack_gp_.back();
        spill_stack_gp_.pop_back();
        cg_->pop(Reg64(idx));
        --managed_push_count_;
        free_gp_regs.erase(idx);
        live_gp_.insert(idx);
    }

    // Restores multiple spilled registers in reverse spill order.
    // Pass the registers in the order they were spilled; the vector is
    // iterated in reverse so the pops match the push sequence.
    void restore(const std::vector<Reg64> &spilled_regs) {
        for (int i = (int)spilled_regs.size() - 1; i >= 0; --i)
            restore(spilled_regs[i]);
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

    // Pushes every currently live volatile GP register onto the hardware stack,
    // in index order, and records them so restore_gp_volatiles() can reverse the
    // sequence.  managed_push_count_ is incremented accordingly, keeping
    // emit_call() alignment correct with no extra bookkeeping on your part.
    //
    // Pair each call with exactly one restore_gp_volatiles().  Nested calls are
    // not supported.
    //
    // Throws Xbyak::Error if no CodeGenerator has been provided.
    void save_gp_volatiles() {
        if (!cg_) XBYAK_THROW(ERR_RM_NO_CG)
        saved_volatile_gp_.clear();
        const std::vector<int> vols = get_live_volatile_gps();
        for (int idx : vols) {
            cg_->push(Reg64(idx));
            ++managed_push_count_;
            saved_volatile_gp_.push_back(idx);
        }
    }

    // Pops the registers saved by the most recent save_gp_volatiles() in reverse
    // order, restoring their values.  managed_push_count_ is decremented to
    // match.
    //
    // Throws Xbyak::Error if called without a preceding save_gp_volatiles(),
    // or if no CodeGenerator has been provided.
    void restore_gp_volatiles() {
        if (!cg_) XBYAK_THROW(ERR_RM_NO_CG)
        if (saved_volatile_gp_.empty()) XBYAK_THROW(ERR_RM_RESTORE_WITHOUT_SAVE)
        for (int i = (int)saved_volatile_gp_.size() - 1; i >= 0; --i) {
            cg_->pop(Reg64(saved_volatile_gp_[i]));
            --managed_push_count_;
        }
        saved_volatile_gp_.clear();
    }

    // Saves every currently live volatile vector register to the stack using
    // vmovdqu/vmovdqu32 and records them so restore_vec_volatiles() can reverse
    // the sequence.  managed_push_count_ is updated accordingly, keeping
    // emit_call() alignment correct with no extra bookkeeping on your part.
    //
    // On Windows (x64): only xmm0–xmm5 are volatile; xmm6–xmm15 are
    // callee-saved and handled by emit_prologue()/emit_epilogue().
    // On Linux/macOS: all vector registers are volatile.
    //
    // When AVX-512 is available, each register is saved at ZMM width (64
    // bytes), preserving the full 512-bit state.  Otherwise each register is
    // saved at YMM width (32 bytes), preserving both the XMM and upper-XMM
    // (YMM) state.
    //
    // Pair each call with exactly one restore_vec_volatiles().  Nested calls
    // are not supported.
    //
    // Throws Xbyak::Error if no CodeGenerator has been provided.
    void save_vec_volatiles() {
        if (!cg_) XBYAK_THROW(ERR_RM_NO_CG)
        saved_volatile_vec_.clear();
        const std::vector<int> vols = get_live_volatile_vecs();
        if (vols.empty()) return;
        const int bytes_per = has_avx512_ ? 64 : 32;
        const int total     = (int)vols.size() * bytes_per;
        cg_->sub(cg_->rsp, total);
        managed_push_count_ += total / 8;
        for (int i = 0; i < (int)vols.size(); ++i) {
            if (has_avx512_)
                cg_->vmovdqu32(cg_->ptr[cg_->rsp + i * bytes_per], Zmm(vols[i]));
            else
                cg_->vmovdqu(cg_->ptr[cg_->rsp + i * bytes_per], Ymm(vols[i]));
        }
        saved_volatile_vec_ = vols;
        saved_volatile_vec_bytes_ = total;
    }

    // Restores the vector registers saved by the most recent save_vec_volatiles()
    // in the same slot order and reclaims the stack space.  managed_push_count_
    // is decremented to match.
    //
    // Throws Xbyak::Error if called without a preceding save_vec_volatiles(),
    // or if no CodeGenerator has been provided.
    void restore_vec_volatiles() {
        if (!cg_) XBYAK_THROW(ERR_RM_NO_CG)
        if (saved_volatile_vec_.empty()) XBYAK_THROW(ERR_RM_RESTORE_WITHOUT_SAVE)
        const int bytes_per = has_avx512_ ? 64 : 32;
        for (int i = 0; i < (int)saved_volatile_vec_.size(); ++i) {
            if (has_avx512_)
                cg_->vmovdqu32(Zmm(saved_volatile_vec_[i]), cg_->ptr[cg_->rsp + i * bytes_per]);
            else
                cg_->vmovdqu(Ymm(saved_volatile_vec_[i]), cg_->ptr[cg_->rsp + i * bytes_per]);
        }
        cg_->add(cg_->rsp, saved_volatile_vec_bytes_);
        managed_push_count_ -= saved_volatile_vec_bytes_ / 8;
        saved_volatile_vec_.clear();
        saved_volatile_vec_bytes_ = 0;
    }

    // Saves all currently live volatile GP and vector registers in one call.
    // Calls save_gp_volatiles() followed by save_vec_volatiles() and arms the
    // paired restore.  Pair with restore_volatiles().
    //
    // Unlike the individual save_gp_volatiles() / save_vec_volatiles(), the
    // matching restore_volatiles() does not throw when nothing was saved (i.e.
    // all volatile registers happen to be free at the call site).
    //
    // Throws Xbyak::Error if no CodeGenerator has been provided.
    void save_volatiles() {
        save_gp_volatiles();
        save_vec_volatiles();
        saved_volatiles_armed_ = true;
    }

    // Restores all registers saved by the most recent save_volatiles().
    // Restores vector registers first (they were pushed via sub rsp last),
    // then GP registers, maintaining correct LIFO stack order.
    //
    // Safe to call when save_volatiles() saved nothing.  Throws Xbyak::Error
    // if called without a preceding save_volatiles(), or if no CodeGenerator
    // has been provided.
    void restore_volatiles() {
        if (!cg_) XBYAK_THROW(ERR_RM_NO_CG)
        if (!saved_volatiles_armed_) XBYAK_THROW(ERR_RM_RESTORE_WITHOUT_SAVE)
        saved_volatiles_armed_ = false;
        if (!saved_volatile_vec_.empty()) restore_vec_volatiles();
        if (!saved_volatile_gp_.empty())  restore_gp_volatiles();
    }

    // helper methods to return special registers as per x86-64 calling convention (System V AMD64 ABI)
    // Stack pointer: rsp
    inline Reg64 _stack_pointer() { return Reg64(4); }
    // Base pointer: rbp
    inline Reg64 _base_pointer() { return Reg64(5); }
    // Opmask k0: special mask register that means "unmasked" (no masking)
    // When k0 is used as a write mask, all elements are written (effectively no masking)
    inline Opmask _opmask_k0() { return Opmask(0); }

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

    // member function - moves given index from in-use set to free set for given family
    void release_gp(int idx) {
        if (idx < 0 || idx > max_gp_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        auto it = live_gp_.find(idx);
        if (it == live_gp_.end())
            XBYAK_THROW(ERR_RM_GP_NOT_IN_USE)
        live_gp_.erase(it);
        free_gp_regs.insert(idx);
    }
    void release_vec(int idx) {
        if (idx < 0 || idx > max_vec_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        auto it = live_vec_.find(idx);
        if (it == live_vec_.end())
            XBYAK_THROW(ERR_RM_VEC_NOT_IN_USE)
        live_vec_.erase(it);
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
    std::set<int> free_opmask_regs = base_free_opmask();
    std::set<int> preserved_opmask = base_preserved_opmask();

    // Registers blocked from allocation via mark_unavailable().
    std::set<int> reserved_gp;
    std::set<int> reserved_vec;
    std::set<int> reserved_opmask;
    std::set<int> reserved_tile;

    // APX feature support
    bool has_apx_ = false;
    int max_gp_reg_idx_ = 15; // 15 without APX, 31 with APX

    // AVX-512 feature support
    bool has_avx512_ = false;
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
    // since function entry (incremented by emit_prologue and spill, decremented
    // by restore).  Used by emit_call to compute 16-byte stack alignment.
    size_t managed_push_count_;

    // Total bytes currently reserved by live StackFrame objects.
    ptrdiff_t allocated_stack_space_;

    // LIFO stack of GP register indices currently pushed onto the hardware stack
    // by spill().  restore() pops entries from the back in reverse order.
    std::vector<int> spill_stack_gp_;

    // GP register indices pushed by the most recent save_gp_volatiles(), in push
    // order.  restore_gp_volatiles() pops them in reverse.
    std::vector<int> saved_volatile_gp_;

    // Vector register indices stored by the most recent save_vec_volatiles(),
    // in save order.  restore_vec_volatiles() reloads from the same slots.
    std::vector<int> saved_volatile_vec_;
    int saved_volatile_vec_bytes_ = 0;

    // Armed by save_volatiles() (combined); cleared by restore_volatiles() (combined).
    // Distinguishes a correct restore_volatiles() call from one with no preceding save.
    bool saved_volatiles_armed_ = false;

#ifndef NDEBUG
    // Set by CommittedLayout construction, cleared by destroy()/destructor.
    // Allows detection of nested build() calls in debug builds.
    bool layout_active_ = false;
#endif

    // Optional CodeGenerator for instruction-emitting features (spill/restore, etc.).
    // Null when the manager is used for tracking only.
    Xbyak::CodeGenerator *cg_;
};

} // namespace Xbyak

#endif // CPU_X64_XBYAK_REG_MANAGER_HPP
