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
    // Constructor - detects APX and AVX-512 support and sets up register pools accordingly
    RegPoolManager() {
        Xbyak::util::Cpu cpu;
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
    std::vector<int> get_in_use_gps() const {
        return make_index_vector(in_use_gp);
    }
    std::vector<int> get_preserved_gps() const {
        return make_index_vector(preserved_gp);
    }
    std::vector<int> get_used_gps() const { return make_index_vector(used_gp); }

    // Returns only in-use registers that are volatile (caller-saved)
    // These MUST be saved by the caller before making a function call
    std::vector<int> get_in_use_volatile_gps() const {
        std::vector<int> result;
        const auto& base_free = base_free_gp();
        for (int idx : in_use_gp) {
            if (base_free.count(idx) > 0) {
                result.push_back(idx);
            }
        }
        return result;
    }

    // Returns only in-use registers that are preserved (callee-saved)
    // These will be saved by the callee if it uses them
    std::vector<int> get_in_use_preserved_gps() const {
        std::vector<int> result;
        const auto& base_preserved = base_preserved_gp();
        for (int idx : in_use_gp) {
            if (base_preserved.count(idx) > 0) {
                result.push_back(idx);
            }
        }
        return result;
    }

    std::vector<int> get_free_vecs() const {
        return make_index_vector(free_vec_regs);
    }
    std::vector<int> get_in_use_vecs() const {
        return make_index_vector(in_use_vec);
    }
    std::vector<int> get_preserved_vecs() const {
        return make_index_vector(preserved_vec);
    }
    std::vector<int> get_used_vecs() const {
        return make_index_vector(used_vec);
    }

    // Returns only in-use vector registers that are volatile (caller-saved)
    // These MUST be saved by the caller before making a function call
    // IMPORTANT for cross-platform code: On Windows, xmm6-xmm15 are preserved (callee-saved),
    // but on Linux/macOS ALL vector registers are volatile (caller-saved).
    // Use this method to write portable code that avoids unnecessary saves on Windows.
    std::vector<int> get_in_use_volatile_vecs() const {
        std::vector<int> result;
        const auto& base_free = base_free_vec();
        for (int idx : in_use_vec) {
            if (base_free.count(idx) > 0) {
                result.push_back(idx);
            }
        }
        return result;
    }

    // Returns only in-use vector registers that are preserved (callee-saved)
    // These will be saved by the callee if it uses them
    std::vector<int> get_in_use_preserved_vecs() const {
        std::vector<int> result;
        const auto& base_preserved = base_preserved_vec();
        for (int idx : in_use_vec) {
            if (base_preserved.count(idx) > 0) {
                result.push_back(idx);
            }
        }
        return result;
    }

    std::vector<int> get_free_opmasks() const {
        return make_index_vector(free_opmask_regs);
    }
    std::vector<int> get_in_use_opmasks() const {
        return make_index_vector(in_use_opmask);
    }
    std::vector<int> get_preserved_opmasks() const {
        return make_index_vector(preserved_opmask);
    }
    std::vector<int> get_used_opmasks() const {
        return make_index_vector(used_opmask);
    }

    // Returns in-use opmask registers (all opmasks are volatile/caller-saved)
    // These MUST be saved by the caller before making a function call
    // Note: All opmask registers are caller-saved on both Windows and Linux
    std::vector<int> get_in_use_volatile_opmasks() const {
        return make_index_vector(in_use_opmask);
    }

    std::vector<int> get_free_tiles() const {
        return make_index_vector(free_tile_regs);
    }
    std::vector<int> get_in_use_tiles() const {
        return make_index_vector(in_use_tile);
    }
    std::vector<int> get_used_tiles() const {
        return make_index_vector(used_tile);
    }

    // Returns in-use AMX tile registers (all tiles are volatile/caller-saved)
    // These MUST be saved by the caller before making a function call
    // Note: All AMX tile registers are caller-saved on both Windows and Linux
    std::vector<int> get_in_use_volatile_tiles() const {
        return make_index_vector(in_use_tile);
    }

    // Prevents a register from being returned by alloc() without marking it as in-use.
    // Useful for protecting registers that must stay off-limits during code generation,
    // such as ABI argument registers or registers dedicated to a runtime helper.
    //
    // The register must not be currently allocated. Reserving an already-in-use register,
    // or calling mark_unavailable() twice on the same register, throws Xbyak::Error.
    // Reserved registers are not visible to get_in_use_gps() / get_in_use_vecs() etc.
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
        const bool in_use = in_use_gp.count(idx) != 0;
        if (in_free || in_preserved || in_use)
            XBYAK_THROW(ERR_RM_REG_ALREADY_TRACKED)
        free_gp_regs.insert(idx);
    }

    // helper function - returns true if a register object is currently in the used set of registers
    template <class RegT>
    bool reg_in_use(const RegT &reg) const {
        return reg_in_use_idx(reg.getIdx(), reg_family<RegT>::value);
    }

    // helper functions - returns true if an index in a given family is in use
    bool gp_idx_in_use(int reg_idx) const {
        return reg_in_use_idx(reg_idx, RegFamily::GP);
    }
    bool vec_idx_in_use(int reg_idx) const {
        return reg_in_use_idx(reg_idx, RegFamily::Vec);
    }
    bool opmask_idx_in_use(int reg_idx) const {
        return reg_in_use_idx(reg_idx, RegFamily::Opmask);
    }
    bool tile_idx_in_use(int reg_idx) const {
        return reg_in_use_idx(reg_idx, RegFamily::Tile);
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
            // pointer to allocator, allocate scoped reg at construction & track, unowned = nullptr
            : rm_(&rm), reg_(r) {
            validate_scoped_reg(rm_, reg_);
        }

        // if object is owner of scoped reg and goes out of scope, deallocate
        ~Scoped() {
            if (!rm_) return;
            rm_->free(reg_);
        }

        // disable copy - scoped regs are move only to avoid ownership/double free issues as per RAII
        Scoped(const Scoped &) = delete;
        Scoped &operator=(const Scoped &) = delete;

        // move constructor - used when scoped regs initialised from rvalue (incl. std::move)
        Scoped(Scoped &&other) noexcept : rm_(other.rm_), reg_(other.reg_) {
            other.rm_ = nullptr; // set previous owner to no longer own
        }

        // expose underlying register for implicit use in JIT helpers
        operator const Reg &() const noexcept { return reg_; }
        const Reg &get() const noexcept { return reg_; }

    private:
        RegPoolManager *rm_
                = nullptr; // pointer to allocator, initialised as nullptr
        Reg reg_ {};
    };

    // helper factory - calls Scoped ctor
    template <class Reg>
    inline Scoped<Reg> makeScoped(Reg r) & {
        return Scoped<Reg>(*this, r);
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

    // helper methods to return special registers as per x86-64 calling convention (System V AMD64 ABI)
    // Stack pointer: rsp
    inline Reg64 _stack_pointer() {
        used_gp.insert(4); // rsp is index 4
        return Reg64(4);
    }
    // Base pointer: rbp
    inline Reg64 _base_pointer() {
        used_gp.insert(5); // rbp is index 5
        return Reg64(5);
    }
    // Opmask k0: special mask register that means "unmasked" (no masking)
    // When k0 is used as a write mask, all elements are written (effectively no masking)
    inline Opmask _opmask_k0() {
        used_opmask.insert(0); // k0 is index 0
        return Opmask(0);
    }

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
        if (in_use_gp.count(idx))   XBYAK_THROW(ERR_RM_GP_IN_USE)
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
        if (in_use_vec.count(idx))   XBYAK_THROW(ERR_RM_VEC_IN_USE)
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
        if (in_use_opmask.count(idx))   XBYAK_THROW(ERR_RM_OPMASK_IN_USE)
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
        if (in_use_tile.count(idx))    XBYAK_THROW(ERR_RM_TILE_IN_USE)
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
                return in_use_gp.find(idx) != in_use_gp.end();
            case RegFamily::Vec:
                if (idx < 0 || idx > max_vec_reg_idx_)
                    XBYAK_THROW_RET(ERR_RM_REG_IDX_OUT_OF_RANGE, false)
                return in_use_vec.find(idx) != in_use_vec.end();
            case RegFamily::Opmask:
                if (idx < 0 || idx > 7)
                    XBYAK_THROW_RET(ERR_RM_REG_IDX_OUT_OF_RANGE, false)
                return in_use_opmask.find(idx) != in_use_opmask.end();
            case RegFamily::Tile:
                if (idx < 0 || idx > 7)
                    XBYAK_THROW_RET(ERR_RM_REG_IDX_OUT_OF_RANGE, false)
                return in_use_tile.find(idx) != in_use_tile.end();
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
            in_use_gp.insert(idx);
            free_gp_regs.erase(it);
            used_gp.insert(idx);
        } else if (pres_it != preserved_gp.end()) {
            in_use_gp.insert(idx);
            preserved_gp.erase(pres_it);
            used_gp.insert(idx);
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
            in_use_vec.insert(idx);
            free_vec_regs.erase(it);
            used_vec.insert(idx);
        } else if (pres_it != preserved_vec.end()) {
            in_use_vec.insert(idx);
            preserved_vec.erase(pres_it);
            used_vec.insert(idx);
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
            in_use_opmask.insert(idx);
            free_opmask_regs.erase(it);
            used_opmask.insert(idx);
        } else if (pres_it != preserved_opmask.end()) {
            in_use_opmask.insert(idx);
            preserved_opmask.erase(pres_it);
            used_opmask.insert(idx);
        } else {
            XBYAK_THROW(ERR_RM_OPMASK_NOT_AVAILABLE)
        }
    }

    // member function - moves given index from in-use set to free set for given family
    void release_gp(int idx) {
        if (idx < 0 || idx > max_gp_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        auto it = in_use_gp.find(idx);
        if (it == in_use_gp.end())
            XBYAK_THROW(ERR_RM_GP_NOT_IN_USE)
        in_use_gp.erase(it);
        free_gp_regs.insert(idx);
    }
    void release_vec(int idx) {
        if (idx < 0 || idx > max_vec_reg_idx_)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        auto it = in_use_vec.find(idx);
        if (it == in_use_vec.end())
            XBYAK_THROW(ERR_RM_VEC_NOT_IN_USE)
        in_use_vec.erase(it);
        free_vec_regs.insert(idx);
    }
    void release_opmask(int idx) {
        if (idx < 0 || idx > 7)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        auto it = in_use_opmask.find(idx);
        if (it == in_use_opmask.end())
            XBYAK_THROW(ERR_RM_OPMASK_NOT_IN_USE)
        in_use_opmask.erase(it);
        free_opmask_regs.insert(idx);
    }
    void tile_reg(int idx) {
        if (reg_in_use_idx(idx, RegFamily::Tile))
            XBYAK_THROW(ERR_RM_TILE_IN_USE)
        auto it = free_tile_regs.find(idx);
        if (it != free_tile_regs.end()) {
            in_use_tile.insert(idx);
            free_tile_regs.erase(it);
            used_tile.insert(idx);
        } else {
            XBYAK_THROW(ERR_RM_TILE_NOT_AVAILABLE)
        }
    }
    void release_tile(int idx) {
        if (idx < 0 || idx > 7)
            XBYAK_THROW(ERR_RM_REG_IDX_OUT_OF_RANGE)
        auto it = in_use_tile.find(idx);
        if (it == in_use_tile.end())
            XBYAK_THROW(ERR_RM_TILE_NOT_IN_USE)
        in_use_tile.erase(it);
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

    std::set<int> used_gp;
    std::set<int> in_use_gp;
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

    std::set<int> used_vec;
    std::set<int> in_use_vec;
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

    std::set<int> used_opmask;
    std::set<int> in_use_opmask;
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

    // AMX tile registers (tmm0-tmm7): no preserved tiles, all caller-saved
    // Pool is empty by default; tmm0-tmm7 are added in constructor if AMX detected
    std::set<int> used_tile;
    std::set<int> in_use_tile;
    std::set<int> free_tile_regs;

    // AMX feature support
    bool has_amx_ = false;
};

} // namespace Xbyak

#endif // CPU_X64_XBYAK_REG_MANAGER_HPP
