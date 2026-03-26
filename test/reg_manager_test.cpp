/*******************************************************************************
 * Unit tests for xbyak/xbyak_reg_manager.hpp
 *
 * Uses the Cybozu test framework (cybozu/test.hpp) — same as all other tests
 * in this directory.
 *
 * Tests covered (matching the sample/test_xbyak_reg_manager.cpp):
 *   basicAllocation         – alloc / free round-trip for GP registers
 *   specificAllocation      – index-based alloc<T>(int) + duplicate-alloc exception
 *   namedRegisterAlloc      – named-register alloc(reg) overload across all families
 *   scopedRegisters         – RAII makeScoped auto-free on scope exit
 *   vectorRegisters         – alloc / free of Xmm / Ymm / Zmm
 *   opmaskRegisters         – alloc / free of Opmask (k1-k7, k0 is special)
 *   apxSupport              – max_gp_registers() reflects APX capability
 *   addToPool               – _stack_pointer / _base_pointer / add_to_gp_pool
 *   registerExhaustion      – allocating more regs than available throws
 *   mixedAllocation         – mix of GP / Vec / Opmask in one manager
 *   regInUse                – reg_in_use / gp_idx_in_use helpers
 *   gpRegisterAliasing      – Reg64/Reg32/Reg16 share a physical register index
 *   vectorRegisterAliasing  – Xmm/Ymm/Zmm share a physical register index
 *   registerContentsViaJIT  – write + read register values through JIT execution
 *   functionCallConvention  – ABI convention: parameter passing + non-volatile
 *                             register preservation across a JIT call
 *   realisticKernel         – code-generation using manager for register strategy
 *   dynamicSaveRestore      – dynamic push/pop using get_in_use_gps() +
 *                             gp_idx_in_use() to save/restore across a real call
 *   amxSupport              – has_amx() / max_tile_registers() reflect AMX capability
 *   amxTileRegisters        – alloc / free of Tmm (tmm0-tmm7)
 *   amxTileExhaustion       – allocating more tiles than available throws
 *   mixedAllocationWithAMX  – mix of GP / Vec / Opmask / AMX in one manager
 *   amxScopedRegisters      – RAII makeScoped auto-free for Tmm
 *   amxRegInUse             – tile_idx_in_use / reg_in_use helpers for Tmm
 *   inUseVolatilePreservedGPs – get_in_use_volatile/preserved_gps() correctness
 *   volatileGPCallerSave    – JIT caller-saves only volatile GPs around a call
 *   vecVolatilePreserved    – get_in_use_volatile/preserved_vecs() correctness
 *   opmaskVolatile          – get_in_use_volatile_opmasks(): all opmasks are volatile
 *   comprehensiveSaveRestore – multi-family volatile/preserved queries agree with totals
 *   amxVolatileTiles        – get_in_use_volatile_tiles(): all tiles are volatile
 *
 * Build:
 *   # via CMake (from xbyak/test/build/):
 *   cmake --build . --target reg_manager_test
 *   ctest -R reg_manager_test
 *
 *   # via Make (from xbyak/test/):
 *   make reg_manager_test
 *   ./reg_manager_test
 *******************************************************************************/

#include <xbyak/xbyak_reg_manager.hpp>  // brings in xbyak.h, xbyak_util.h
#include <cybozu/test.hpp>

#include <algorithm>
#include <vector>

using namespace Xbyak;

// =============================================================================
// JIT helper utilities (used by Tests 13-16)
// =============================================================================

static uint64_t call_jit(const void *code) {
    return reinterpret_cast<uint64_t (*)()>(code)();
}

// Generates and executes simple JIT snippets for register content verification.
class TestJit : public CodeGenerator {
public:
    TestJit() : CodeGenerator(4096) {}

    // Write a 64-bit immediate to a register, then return it in rax.
    void gen_gp_write_read(const Reg64 &reg, uint64_t value) {
        mov(reg, value);
        mov(rax, reg);
        ret();
    }

    // Write a 32-bit immediate to a Reg32; upper 32 bits of the 64-bit alias
    // must be zero-extended.
    void gen_gp_alias_test(const Reg32 &reg32, uint32_t value) {
        xor_(rax, rax);
        mov(reg32, value);
        mov(rax, Reg64(reg32.getIdx()));
        ret();
    }

    // Write a recognisable bit pattern into an XMM register and return the
    // low 64 bits in rax.
    void gen_vec_write_read(const Xmm &xmm_reg) {
        pcmpeqd(xmm_reg, xmm_reg);  // all ones
        psrld(xmm_reg, 31);          // each dword = 1
        pslld(xmm_reg, 6);           // each dword = 0x40
        paddd(xmm_reg, xmm_reg);     // 0x80
        psrld(xmm_reg, 1);           // 0x40
        paddb(xmm_reg, xmm_reg);     // 0x80 per byte
        movq(rax, xmm_reg);
        ret();
    }
};

// A real C function that clobbers all caller-saved registers and returns 210.
// Used in test 16 to verify that dynamic save/restore actually works.
extern "C" int call_function_that_clobbers_registers() {
    volatile int a = 10, b = 20, c = 30, d = 40, e = 50, f = 60;
    return a + b + c + d + e + f;  // 210
}

// =============================================================================
// Test – Basic allocation and deallocation
// =============================================================================
CYBOZU_TEST_AUTO(basicAllocation)
{
    RegPoolManager rm;

    auto r1 = rm.alloc<Reg64>();
    auto r2 = rm.alloc<Reg64>();
    auto r3 = rm.alloc<Reg32>();

    CYBOZU_TEST_EQUAL((int)rm.get_in_use_gps().size(), 3);

    rm.free(r1);
    rm.free(r2);
    rm.free(r3);

    CYBOZU_TEST_ASSERT(rm.get_in_use_gps().empty());
}

// =============================================================================
// Test – Specific register allocation
// =============================================================================
CYBOZU_TEST_AUTO(specificAllocation)
{
    RegPoolManager rm;

    auto r10 = rm.alloc<Reg64>(10);
    auto r11 = rm.alloc<Reg64>(11);

    CYBOZU_TEST_EQUAL(r10.getIdx(), 10);
    CYBOZU_TEST_EQUAL(r11.getIdx(), 11);

    // Allocating an already-in-use index must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg64>(10), Xbyak::Error);

    rm.free(r10);
    rm.free(r11);
}

// =============================================================================
// Test – Named-register alloc(reg) overload
// =============================================================================
CYBOZU_TEST_AUTO(namedRegisterAlloc)
{
    // Named register constants (rax, rdx, r10, xmm2, zmm4, k1, etc.) are
    // member variables of CodeGenerator — they are available by name inside any
    // CodeGenerator subclass without any explicit index.  This inner struct
    // mirrors the real usage pattern, where a JIT kernel inherits CodeGenerator
    // and writes rm.alloc(rdx) instead of rm.alloc<Reg64>(2).
    struct NamedAllocTest : Xbyak::CodeGenerator {
        void run() {
            RegPoolManager rm;

            // GP Reg64 — names match assembly register notation exactly.
            auto reg_rdx = rm.alloc(rdx);   // rdx: caller-saved on both ABIs
            auto reg_r10 = rm.alloc(r10);   // r10: caller-saved
            CYBOZU_TEST_EQUAL(reg_rdx.getIdx(), rdx.getIdx());
            CYBOZU_TEST_EQUAL(reg_r10.getIdx(), r10.getIdx());
            CYBOZU_TEST_ASSERT(rm.gp_idx_in_use(rdx.getIdx()));
            CYBOZU_TEST_ASSERT(rm.gp_idx_in_use(r10.getIdx()));

            // GP Reg32 and Reg16 — r8d and r9w are distinct physical registers.
            auto reg_r8d = rm.alloc(r8d);
            auto reg_r9w = rm.alloc(r9w);
            CYBOZU_TEST_EQUAL(reg_r8d.getIdx(), r8d.getIdx());
            CYBOZU_TEST_EQUAL(reg_r9w.getIdx(), r9w.getIdx());

            // Vec — Xmm, Ymm and Zmm each deduce a different RegT.
            auto reg_xmm2 = rm.alloc(xmm2);
            auto reg_ymm3 = rm.alloc(ymm3);
            auto reg_zmm4 = rm.alloc(zmm4);
            CYBOZU_TEST_EQUAL(reg_xmm2.getIdx(), xmm2.getIdx());
            CYBOZU_TEST_EQUAL(reg_ymm3.getIdx(), ymm3.getIdx());
            CYBOZU_TEST_EQUAL(reg_zmm4.getIdx(), zmm4.getIdx());
            CYBOZU_TEST_ASSERT(rm.vec_idx_in_use(xmm2.getIdx()));
            CYBOZU_TEST_ASSERT(rm.vec_idx_in_use(ymm3.getIdx()));
            CYBOZU_TEST_ASSERT(rm.vec_idx_in_use(zmm4.getIdx()));

            // Opmask — k1 and k2.
            auto reg_k1 = rm.alloc(k1);
            auto reg_k2 = rm.alloc(k2);
            CYBOZU_TEST_EQUAL(reg_k1.getIdx(), k1.getIdx());
            CYBOZU_TEST_EQUAL(reg_k2.getIdx(), k2.getIdx());
            CYBOZU_TEST_ASSERT(rm.opmask_idx_in_use(k1.getIdx()));
            CYBOZU_TEST_ASSERT(rm.opmask_idx_in_use(k2.getIdx()));

            // AMX Tile — tmm0 and tmm1 (skipped if AMX is not available).
            // tmm0-tmm7 are const Tmm members of CodeGenerator, so they work
            // identically to rax, xmm2, k1 etc.
            if (rm.has_amx()) {
                auto reg_tmm0 = rm.alloc(tmm0);
                auto reg_tmm1 = rm.alloc(tmm1);
                CYBOZU_TEST_EQUAL(reg_tmm0.getIdx(), tmm0.getIdx());
                CYBOZU_TEST_EQUAL(reg_tmm1.getIdx(), tmm1.getIdx());
                CYBOZU_TEST_ASSERT(rm.tile_idx_in_use(tmm0.getIdx()));
                CYBOZU_TEST_ASSERT(rm.tile_idx_in_use(tmm1.getIdx()));

                // Duplicate alloc by name must throw for tiles too.
                CYBOZU_TEST_EXCEPTION(rm.alloc(tmm0), Xbyak::Error);

                rm.free(reg_tmm0);
                rm.free(reg_tmm1);
                CYBOZU_TEST_ASSERT(rm.get_in_use_tiles().empty());
            }

            // Allocating an already-in-use register by name must throw — the
            // same error path as alloc<Reg64>(int idx) for a duplicate index.
            CYBOZU_TEST_EXCEPTION(rm.alloc(rdx),  Xbyak::Error);
            CYBOZU_TEST_EXCEPTION(rm.alloc(xmm2), Xbyak::Error);
            CYBOZU_TEST_EXCEPTION(rm.alloc(k1),   Xbyak::Error);

            // Free all and confirm every pool is clean.
            rm.free(reg_rdx);  rm.free(reg_r10);
            rm.free(reg_r8d);  rm.free(reg_r9w);
            rm.free(reg_xmm2); rm.free(reg_ymm3); rm.free(reg_zmm4);
            rm.free(reg_k1);   rm.free(reg_k2);

            CYBOZU_TEST_ASSERT(rm.get_in_use_gps().empty());
            CYBOZU_TEST_ASSERT(rm.get_in_use_vecs().empty());
            CYBOZU_TEST_ASSERT(rm.get_in_use_opmasks().empty());
        }
    };
    NamedAllocTest t;
    t.run();
}

// =============================================================================
// Test – RAII scoped registers
// =============================================================================
CYBOZU_TEST_AUTO(scopedRegisters)
{
    RegPoolManager rm;

    {
        auto scoped1 = rm.makeScoped(rm.alloc<Reg64>());
        auto scoped2 = rm.makeScoped(rm.alloc<Reg64>());
        CYBOZU_TEST_EQUAL((int)rm.get_in_use_gps().size(), 2);
        // scoped1 and scoped2 are freed here by their destructors.
    }

    CYBOZU_TEST_ASSERT(rm.get_in_use_gps().empty());
}

// =============================================================================
// Test – Vector register (Xmm / Ymm / Zmm) allocation
// =============================================================================
CYBOZU_TEST_AUTO(vectorRegisters)
{
    RegPoolManager rm;

    auto xmm1 = rm.alloc<Xmm>();
    auto xmm2 = rm.alloc<Xmm>();
    auto ymm3 = rm.alloc<Ymm>();
    auto ymm4 = rm.alloc<Ymm>();
    auto zmm5 = rm.alloc<Zmm>();

    CYBOZU_TEST_EQUAL((int)rm.get_in_use_vecs().size(), 5);

    rm.free(xmm1);
    rm.free(xmm2);
    rm.free(ymm3);
    rm.free(ymm4);
    rm.free(zmm5);

    CYBOZU_TEST_ASSERT(rm.get_in_use_vecs().empty());
}

// =============================================================================
// Test – Opmask register (k1-k7) allocation
// =============================================================================
CYBOZU_TEST_AUTO(opmaskRegisters)
{
    RegPoolManager rm;

    auto k1 = rm.alloc<Opmask>();
    auto k2 = rm.alloc<Opmask>();

    CYBOZU_TEST_EQUAL((int)rm.get_in_use_opmasks().size(), 2);

    // k0 is reserved ("unmasked" sentinel) – allocated indices must be >= 1.
    CYBOZU_TEST_ASSERT(k1.getIdx() >= 1);
    CYBOZU_TEST_ASSERT(k2.getIdx() >= 1);
    CYBOZU_TEST_ASSERT(k1.getIdx() != k2.getIdx());

    rm.free(k1);
    rm.free(k2);

    CYBOZU_TEST_ASSERT(rm.get_in_use_opmasks().empty());
}

// =============================================================================
// Test – APX support detection
// =============================================================================
CYBOZU_TEST_AUTO(apxSupport)
{
    RegPoolManager rm;

    // Without APX: 16 GP registers (r0-r15).
    // With APX:    32 GP registers (r0-r31).
    int maxGP = rm.max_gp_registers();
    if (rm.has_apx()) {
        CYBOZU_TEST_EQUAL(maxGP, 32);
    } else {
        CYBOZU_TEST_EQUAL(maxGP, 16);
    }
}

// =============================================================================
// Test – Special register accessors and add_to_gp_pool
// =============================================================================
CYBOZU_TEST_AUTO(addToPool)
{
    RegPoolManager rm;

    // _stack_pointer() == rsp (index 4).
    auto rsp_reg = rm._stack_pointer();
    CYBOZU_TEST_EQUAL(rsp_reg.getIdx(), 4);

    // _base_pointer() == rbp (index 5).
    auto rbp_reg = rm._base_pointer();
    CYBOZU_TEST_EQUAL(rbp_reg.getIdx(), 5);

    // add_to_gp_pool with an out-of-range index must throw.
    CYBOZU_TEST_EXCEPTION(rm.add_to_gp_pool(200), Xbyak::Error);

    // add_to_gp_pool for an index already in the free pool (rax=0, always
    // caller-saved) or the preserved pool (rbx=3, always callee-saved) must
    // throw on both Windows and Linux.
    CYBOZU_TEST_EXCEPTION(rm.add_to_gp_pool(0), Xbyak::Error);
    CYBOZU_TEST_EXCEPTION(rm.add_to_gp_pool(3), Xbyak::Error);
}

// =============================================================================
// Test – Exhausting all allocatable GP registers
// =============================================================================
CYBOZU_TEST_AUTO(registerExhaustion)
{
    RegPoolManager rm;
    std::vector<Reg64> allocated;

    // Allocate until the pool is dry.
    try {
        for (int i = 0; i < 50; ++i)
            allocated.push_back(rm.alloc<Reg64>());
    } catch (const Xbyak::Error &) {
        // Exhaustion exception is expected.
    }

    // rsp (index 4) and rbp (index 5) are never in the pool, so total
    // allocatable = max_gp_registers() - 2.
    CYBOZU_TEST_EQUAL((int)allocated.size(), rm.max_gp_registers() - 2);

    for (auto &r : allocated) rm.free(r);
    CYBOZU_TEST_ASSERT(rm.get_in_use_gps().empty());
}

// =============================================================================
// Test – Mixed register family allocation
// =============================================================================
CYBOZU_TEST_AUTO(mixedAllocation)
{
    RegPoolManager rm;

    auto r64 = rm.alloc<Reg64>();
    auto r32 = rm.alloc<Reg32>();
    auto r16 = rm.alloc<Reg16>();
    auto xmm = rm.alloc<Xmm>();
    auto ymm = rm.alloc<Ymm>();
    auto zmm = rm.alloc<Zmm>();
    auto k   = rm.alloc<Opmask>();

    CYBOZU_TEST_EQUAL((int)rm.get_in_use_gps().size(),     3);
    CYBOZU_TEST_EQUAL((int)rm.get_in_use_vecs().size(),    3);
    CYBOZU_TEST_EQUAL((int)rm.get_in_use_opmasks().size(), 1);

    rm.free(r64);  rm.free(r32);  rm.free(r16);
    rm.free(xmm);  rm.free(ymm);  rm.free(zmm);
    rm.free(k);

    CYBOZU_TEST_ASSERT(rm.get_in_use_gps().empty());
    CYBOZU_TEST_ASSERT(rm.get_in_use_vecs().empty());
    CYBOZU_TEST_ASSERT(rm.get_in_use_opmasks().empty());
}

// =============================================================================
// Test – reg_in_use / gp_idx_in_use helpers
// =============================================================================
CYBOZU_TEST_AUTO(regInUse)
{
    RegPoolManager rm;
    auto r1 = rm.alloc<Reg64>();

    CYBOZU_TEST_ASSERT(rm.reg_in_use(r1));
    CYBOZU_TEST_ASSERT(rm.gp_idx_in_use(r1.getIdx()));

    rm.free(r1);

    CYBOZU_TEST_ASSERT(!rm.reg_in_use(r1));
    CYBOZU_TEST_ASSERT(!rm.gp_idx_in_use(r1.getIdx()));
}

// =============================================================================
// Test – GP register aliasing (RAX / EAX / AX share index 0)
// =============================================================================
CYBOZU_TEST_AUTO(gpRegisterAliasing)
{
    RegPoolManager rm;

    // Allocate RAX (Reg64 index 0).
    auto rax_reg = rm.alloc<Reg64>(0);
    CYBOZU_TEST_EQUAL(rax_reg.getIdx(), 0);
    CYBOZU_TEST_ASSERT(rm.gp_idx_in_use(0));

    // EAX (Reg32(0)) shares the same physical register – must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg32>(0), Xbyak::Error);
    // AX (Reg16(0)) also shares it – must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg16>(0), Xbyak::Error);

    rm.free(rax_reg);

    // After freeing RAX, allocating EAX must succeed.
    CYBOZU_TEST_NO_EXCEPTION(
        auto eax = rm.alloc<Reg32>(0);
        CYBOZU_TEST_EQUAL(eax.getIdx(), 0);
        rm.free(eax);
    );
}

// =============================================================================
// Test – Vector register aliasing (XMM / YMM / ZMM share index)
// =============================================================================
CYBOZU_TEST_AUTO(vectorRegisterAliasing)
{
    RegPoolManager rm;

    auto xmm0 = rm.alloc<Xmm>(0);
    CYBOZU_TEST_EQUAL(xmm0.getIdx(), 0);
    CYBOZU_TEST_ASSERT(rm.vec_idx_in_use(0));

    // YMM0 and ZMM0 share the same physical register – both must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Ymm>(0), Xbyak::Error);
    CYBOZU_TEST_EXCEPTION(rm.alloc<Zmm>(0), Xbyak::Error);

    rm.free(xmm0);

    // After freeing XMM0, allocating YMM0 must succeed.
    CYBOZU_TEST_NO_EXCEPTION(
        auto ymm0 = rm.alloc<Ymm>(0);
        CYBOZU_TEST_EQUAL(ymm0.getIdx(), 0);
        rm.free(ymm0);
    );
}

// =============================================================================
// Test – Write and read register contents via JIT execution
// =============================================================================
CYBOZU_TEST_AUTO(registerContentsViaJIT)
{
    RegPoolManager rm;

    // 64-bit GP register write/read.
    {
        TestJit jit;
        auto reg = rm.alloc<Reg64>(8);  // r8
        const uint64_t testVal = 0x123456789ABCDEF0ULL;
        jit.gen_gp_write_read(reg, testVal);
        CYBOZU_TEST_EQUAL(call_jit(jit.getCode()), testVal);
        rm.free(reg);
    }

    // Writing to Reg32 must zero-extend the upper 32 bits of the 64-bit alias.
    {
        TestJit jit;
        auto reg32 = rm.alloc<Reg32>(9);  // r9d
        const uint32_t testVal = 0xDEADBEEFu;
        jit.gen_gp_alias_test(reg32, testVal);
        uint64_t result = call_jit(jit.getCode());
        CYBOZU_TEST_EQUAL(result, (uint64_t)testVal);
        CYBOZU_TEST_EQUAL((uint32_t)(result >> 32), 0u);
        rm.free(reg32);
    }

    // Vector register: the computed bit pattern must be non-zero.
    {
        TestJit jit;
        auto xmm_reg = rm.alloc<Xmm>(5);  // xmm5
        jit.gen_vec_write_read(xmm_reg);
        CYBOZU_TEST_ASSERT(call_jit(jit.getCode()) != 0u);
        rm.free(xmm_reg);
    }
}

// =============================================================================
// Test – Function call convention: parameter passing and non-volatile
//           register preservation across a JIT-to-JIT call.
// =============================================================================
CYBOZU_TEST_AUTO(functionCallConvention)
{
    class FunctionCallJit : public CodeGenerator {
    public:
        FunctionCallJit() : CodeGenerator(4096) {}

        // Callee: sum 4 integer parameters and return in rax.
        void gen_callee_function() {
#ifdef _WIN32
            // Windows x64: rcx, rdx, r8, r9
            mov(rax, rcx);
            add(rax, rdx);
            add(rax, r8);
            add(rax, r9);
#else
            // System V AMD64: rdi, rsi, rdx, rcx
            mov(rax, rdi);
            add(rax, rsi);
            add(rax, rdx);
            add(rax, rcx);
#endif
            ret();
        }

        // Caller: store known values in preserved registers, call the callee,
        // add the preserved-register values to the return value to demonstrate
        // they survived the call.
        void gen_caller_with_preserved_regs(uint64_t callee_addr) {
#ifdef _WIN32
            push(rbx);  push(rsi);  push(rdi);
            mov(rbx, 100);  mov(rsi, 200);  mov(rdi, 300);
            mov(rcx, 1);  mov(rdx, 2);  mov(r8, 3);  mov(r9, 4);
            mov(rax, callee_addr);  call(rax);
            add(rax, rbx);  add(rax, rsi);  add(rax, rdi);
            pop(rdi);  pop(rsi);  pop(rbx);
#else
            push(rbx);  push(r12);
            mov(rbx, 100);  mov(r12, 200);
            mov(rdi, 1);  mov(rsi, 2);  mov(rdx, 3);  mov(rcx, 4);
            mov(rax, callee_addr);  call(rax);
            add(rax, rbx);  add(rax, r12);
            pop(r12);  pop(rbx);
#endif
            ret();
        }

        // Save volatile registers, zero them out (simulating a clobbering
        // call), restore them, then return their sum.
        void gen_volatile_reg_save_restore() {
#ifdef _WIN32
            mov(rcx, 10);  mov(rdx, 20);  mov(r8, 30);  mov(r9, 40);
            push(rcx);  push(rdx);  push(r8);  push(r9);
            xor_(rcx, rcx);  xor_(rdx, rdx);  xor_(r8, r8);  xor_(r9, r9);
            pop(r9);  pop(r8);  pop(rdx);  pop(rcx);
            mov(rax, rcx);  add(rax, rdx);  add(rax, r8);  add(rax, r9);
#else
            mov(rdi, 10);  mov(rsi, 20);  mov(rdx, 30);  mov(rcx, 40);
            push(rdi);  push(rsi);  push(rdx);  push(rcx);
            xor_(rdi, rdi);  xor_(rsi, rsi);  xor_(rdx, rdx);  xor_(rcx, rcx);
            pop(rcx);  pop(rdx);  pop(rsi);  pop(rdi);
            mov(rax, rdi);  add(rax, rsi);  add(rax, rdx);  add(rax, rcx);
#endif
            ret();
        }
    };

    // Parameter passing: (1+2+3+4) plus non-volatile register values.
    {
        FunctionCallJit callee;  callee.gen_callee_function();
        FunctionCallJit caller;
        caller.gen_caller_with_preserved_regs(
            reinterpret_cast<uint64_t>(callee.getCode()));
        uint64_t result = call_jit(caller.getCode());
#ifdef _WIN32
        CYBOZU_TEST_EQUAL(result, (uint64_t)(10 + 100 + 200 + 300));
#else
        CYBOZU_TEST_EQUAL(result, (uint64_t)(10 + 100 + 200));
#endif
    }

    // Volatile register save/restore: sum must survive the clobber.
    {
        FunctionCallJit jit;
        jit.gen_volatile_reg_save_restore();
        CYBOZU_TEST_EQUAL(call_jit(jit.getCode()), (uint64_t)(10 + 20 + 30 + 40));
    }

    // Verify the register manager's preserved GP list matches the ABI.
    {
        RegPoolManager rm;
        const auto preserved = rm.get_preserved_gps();
#ifdef _WIN32
        // Windows: rbx(3), rdi(6), rsi(7), r12-r15(12-15)
        CYBOZU_TEST_ASSERT(
            std::find(preserved.begin(), preserved.end(), 3) != preserved.end());
        CYBOZU_TEST_ASSERT(
            std::find(preserved.begin(), preserved.end(), 6) != preserved.end());
        CYBOZU_TEST_ASSERT(
            std::find(preserved.begin(), preserved.end(), 7) != preserved.end());
#else
        // System V: rbx(3), r12-r15(12-15)
        CYBOZU_TEST_ASSERT(
            std::find(preserved.begin(), preserved.end(), 3) != preserved.end());
        CYBOZU_TEST_ASSERT(
            std::find(preserved.begin(), preserved.end(), 12) != preserved.end());
        // rdi(7) and rsi(6) are volatile on System V – must NOT be preserved.
        CYBOZU_TEST_ASSERT(
            std::find(preserved.begin(), preserved.end(), 6) == preserved.end());
        CYBOZU_TEST_ASSERT(
            std::find(preserved.begin(), preserved.end(), 7) == preserved.end());
#endif
    }
}

// =============================================================================
// Test – Realistic JIT kernel using the register manager for strategy
// =============================================================================
CYBOZU_TEST_AUTO(realisticKernel)
{
    class RealisticKernel : public CodeGenerator {
    public:
        RealisticKernel() : CodeGenerator(4096) {}

        // Allocate registers via manager: two volatile temporaries and one
        // preserved "state" register.  Result: rax = 42 + 100.
        void gen_kernel_with_manager(RegPoolManager &rm) {
            auto temp1 = rm.alloc<Reg64>();
            auto temp2 = rm.alloc<Reg64>();
            auto preserved_list = rm.get_preserved_gps();
            const bool use_preserved = !preserved_list.empty();
            auto state_reg = use_preserved
                ? rm.alloc<Reg64>(preserved_list[0])
                : rm.alloc<Reg64>();

            // Callee-saved registers must be preserved across the JIT call
            // boundary back into C++.
            if (use_preserved) push(state_reg);

            mov(state_reg, 100);
            mov(temp1, 200);
            mov(temp2, 300);
            add(temp1, temp2);  // temp1 = 500
            mov(rax, 42);
            add(rax, state_reg);  // 42 + 100

            if (use_preserved) pop(state_reg);
            rm.free(temp1);
            rm.free(temp2);
            rm.free(state_reg);
            ret();
        }

        // Multi-phase: compute a value in volatile regs, save it in a
        // preserved register before a "call", add 100.  Result: 100 + 60.
        void gen_multi_phase_kernel(RegPoolManager &rm) {
            auto v1 = rm.alloc<Reg64>();
            auto v2 = rm.alloc<Reg64>();
            auto v3 = rm.alloc<Reg64>();
            mov(v1, 10);  mov(v2, 20);  mov(v3, 30);
            add(v1, v2);  add(v1, v3);  // v1 = 60

            auto preserved = rm.get_preserved_gps();
            if (!preserved.empty()) {
                auto safe = rm.alloc<Reg64>(preserved[0]);
                push(safe);        // save callee-saved register before use
                mov(safe, v1);
                rm.free(v1);  rm.free(v2);  rm.free(v3);
                // Simulate post-call: preserved register still holds 60.
                mov(rax, 100);
                add(rax, safe);  // 100 + 60
                pop(safe);         // restore callee-saved register
                rm.free(safe);
            } else {
                // Fall back: use stack scratch space only (no preserved regs).
                push(v1);
                rm.free(v1);  rm.free(v2);  rm.free(v3);
                mov(rax, 100);
                pop(rcx);      // rcx is volatile — safe to use here
                add(rax, rcx); // 100 + 60
            }
            ret();
        }
    };

    {
        RegPoolManager rm;
        RealisticKernel jit;
        jit.gen_kernel_with_manager(rm);
        CYBOZU_TEST_EQUAL(call_jit(jit.getCode()), (uint64_t)(42 + 100));
    }
    {
        RegPoolManager rm;
        RealisticKernel jit;
        jit.gen_multi_phase_kernel(rm);
        CYBOZU_TEST_EQUAL(call_jit(jit.getCode()), (uint64_t)(100 + 60));
    }
}

// =============================================================================
// Test – Dynamic save/restore across a real function call
// =============================================================================
CYBOZU_TEST_AUTO(dynamicSaveRestore)
{
    class DynamicJit : public CodeGenerator {
    public:
        DynamicJit() : CodeGenerator(8192) {}

        // Allocate four registers, initialise them.
        // Use get_in_use_gps() to generate push/pop code, then make a
        // real call that clobbers volatile registers, verify the sum.
        // Expected result: 100 + 200 + 300 + 400 + 210 (function return).
        void gen_caller_saves_all(RegPoolManager &rm) {
            // Use rbx as a temporary to hold the function return value.
            // rbx is callee-saved, so we must save/restore it ourselves.
            push(rbx);

            auto r1 = rm.alloc<Reg64>();
            auto r2 = rm.alloc<Reg64>();
            auto r3 = rm.alloc<Reg64>();
            auto r4 = rm.alloc<Reg64>();
            mov(r1, 100);  mov(r2, 200);  mov(r3, 300);  mov(r4, 400);

            // Ask the manager which registers are live — generate save code.
            auto in_use = rm.get_in_use_gps();
            for (int idx : in_use) push(Reg64(idx));

#ifdef _WIN32
            // Win64 ABI: allocate 32-byte shadow space; also re-align the stack
            // to 16 bytes at the call instruction.  On entry rsp%16==8 (return
            // address pushed by caller).  After push(rbx) + in_use.size() pushes
            // the adjustment needed is:
            //   total_pushes_from_entry = 1 + in_use.size()
            //   bytes_pushed = total_pushes_from_entry * 8
            //   rsp_mod16 = (8 + bytes_pushed) % 16   (8 = initial offset)
            //   If rsp_mod16 == 8: already aligned before call, just add 32 shadow.
            //   If rsp_mod16 == 0: need 8-byte pad + 32 shadow = 40 bytes.
            {
                size_t total_pushes = 1 + in_use.size(); // rbx + in_use
                bool needs_pad = (total_pushes % 2) == 0;
                int adj = needs_pad ? 40 : 32;
                sub(rsp, adj);
#endif
            // Call a real function that clobbers caller-saved registers.
            mov(rax,
                reinterpret_cast<uint64_t>(&call_function_that_clobbers_registers));
            call(rax);
            mov(rbx, rax);  // stash the return value (210) in rbx
#ifdef _WIN32
                add(rsp, adj);
            }
#endif

            // Generate restore code in reverse order.
            for (auto it = in_use.rbegin(); it != in_use.rend(); ++it)
                pop(Reg64(*it));

            // Sum the four restored registers.
            mov(rax, Reg64(in_use[0]));
            for (size_t i = 1; i < in_use.size(); ++i)
                add(rax, Reg64(in_use[i]));
            add(rax, rbx);  // add the function's return value (210)

            pop(rbx);  // restore original rbx
            rm.free(r1);  rm.free(r2);  rm.free(r3);  rm.free(r4);
            ret();
        }

        // Use gp_idx_in_use() in a loop to decide which registers to push/pop.
        // Expected result: 111 + 222 + 333 + 444 = 1110.
        void gen_loop_based_save_restore(RegPoolManager &rm) {
            std::vector<Reg64> regs;
            for (int i = 0; i < 4; ++i) {
                auto r = rm.alloc<Reg64>();
                regs.push_back(r);
                mov(r, (i + 1) * 111);
            }

            // Loop over all possible GP indices and save every in-use one.
            std::vector<int> saved;
            for (int idx = 0; idx < 16; ++idx) {
                if (rm.gp_idx_in_use(idx)) {
                    push(Reg64(idx));
                    saved.push_back(idx);
                }
            }

            // Clobber the allocated registers.
            for (auto &r : regs) xor_(r, r);

            // Restore in reverse order.
            for (auto it = saved.rbegin(); it != saved.rend(); ++it)
                pop(Reg64(*it));

            // Return the sum of the restored values.
            mov(rax, regs[0]);
            for (size_t i = 1; i < regs.size(); ++i) add(rax, regs[i]);

            for (auto &r : regs) rm.free(r);
            ret();
        }
    };

    // Scenario 1: caller saves all live registers around a real function call.
    {
        RegPoolManager rm;
        DynamicJit jit;
        jit.gen_caller_saves_all(rm);
        CYBOZU_TEST_EQUAL(call_jit(jit.getCode()),
                          (uint64_t)(100 + 200 + 300 + 400 + 210));
    }

    // Scenario 2: loop-based save/restore using gp_idx_in_use().
    {
        RegPoolManager rm;
        DynamicJit jit;
        jit.gen_loop_based_save_restore(rm);
        CYBOZU_TEST_EQUAL(call_jit(jit.getCode()),
                          (uint64_t)(111 + 222 + 333 + 444));
    }
}

// =============================================================================
// Test – AMX support detection
// =============================================================================
CYBOZU_TEST_AUTO(amxSupport)
{
    RegPoolManager rm;

    if (rm.has_amx()) {
        // AMX present: 8 tile registers (tmm0-tmm7), free pool starts full.
        CYBOZU_TEST_EQUAL(rm.max_tile_registers(), 8);
        CYBOZU_TEST_EQUAL((int)rm.get_free_tiles().size(), 8);
        CYBOZU_TEST_ASSERT(rm.get_in_use_tiles().empty());
    } else {
        // No AMX: pool is empty and max is 0.
        CYBOZU_TEST_EQUAL(rm.max_tile_registers(), 0);
        CYBOZU_TEST_ASSERT(rm.get_free_tiles().empty());
        CYBOZU_TEST_ASSERT(rm.get_in_use_tiles().empty());
    }
}

// =============================================================================
// Test – AMX tile register allocation and deallocation
// =============================================================================
CYBOZU_TEST_AUTO(amxTileRegisters)
{
    RegPoolManager rm;

    if (!rm.has_amx()) {
        // Without AMX hardware alloc must throw immediately.
        CYBOZU_TEST_EXCEPTION(rm.alloc<Tmm>(), Xbyak::Error);
        return;
    }

    // Allocate a few tile registers.
    auto t0 = rm.alloc<Tmm>();
    auto t1 = rm.alloc<Tmm>();
    auto t2 = rm.alloc<Tmm>(2);  // allocate specific tile

    CYBOZU_TEST_EQUAL((int)rm.get_in_use_tiles().size(), 3);
    CYBOZU_TEST_EQUAL((int)rm.get_free_tiles().size(), 5);  // 8 - 3

    // Indices must be within the valid AMX range.
    CYBOZU_TEST_ASSERT(t0.getIdx() >= 0 && t0.getIdx() <= 7);
    CYBOZU_TEST_ASSERT(t1.getIdx() >= 0 && t1.getIdx() <= 7);
    CYBOZU_TEST_EQUAL(t2.getIdx(), 2);

    // Duplicate allocation of tile 2 must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Tmm>(2), Xbyak::Error);

    rm.free(t0);
    rm.free(t1);
    rm.free(t2);

    CYBOZU_TEST_ASSERT(rm.get_in_use_tiles().empty());
    CYBOZU_TEST_EQUAL((int)rm.get_free_tiles().size(), 8);

    // Freeing a tile that is not in use must throw.
    CYBOZU_TEST_EXCEPTION(rm.free(t0), Xbyak::Error);
}

// =============================================================================
// Test – AMX tile exhaustion
// =============================================================================
CYBOZU_TEST_AUTO(amxTileExhaustion)
{
    RegPoolManager rm;
    std::vector<Tmm> allocated;

    // Allocate until exhausted; expect an exception when the pool is empty.
    try {
        for (int i = 0; i < 10; ++i)
            allocated.push_back(rm.alloc<Tmm>());
    } catch (const Xbyak::Error &) {
        // Exhaustion exception is expected.
    }

    if (rm.has_amx()) {
        // All 8 tile registers should have been allocated before exhaustion.
        CYBOZU_TEST_EQUAL((int)allocated.size(), 8);
    } else {
        // No AMX: first alloc throws, nothing allocated.
        CYBOZU_TEST_EQUAL((int)allocated.size(), 0);
    }

    for (auto &t : allocated) rm.free(t);
    CYBOZU_TEST_ASSERT(rm.get_in_use_tiles().empty());
}

// =============================================================================
// Test – Mixed allocation including AMX tiles
// =============================================================================
CYBOZU_TEST_AUTO(mixedAllocationWithAMX)
{
    RegPoolManager rm;

    auto r64 = rm.alloc<Reg64>();
    auto xmm = rm.alloc<Xmm>();
    auto k   = rm.alloc<Opmask>();

    CYBOZU_TEST_EQUAL((int)rm.get_in_use_gps().size(),     1);
    CYBOZU_TEST_EQUAL((int)rm.get_in_use_vecs().size(),    1);
    CYBOZU_TEST_EQUAL((int)rm.get_in_use_opmasks().size(), 1);

    if (rm.has_amx()) {
        auto t0 = rm.alloc<Tmm>();
        auto t1 = rm.alloc<Tmm>();
        CYBOZU_TEST_EQUAL((int)rm.get_in_use_tiles().size(), 2);
        rm.free(t0);
        rm.free(t1);
        CYBOZU_TEST_ASSERT(rm.get_in_use_tiles().empty());
    } else {
        CYBOZU_TEST_ASSERT(rm.get_in_use_tiles().empty());
    }

    rm.free(r64);
    rm.free(xmm);
    rm.free(k);

    CYBOZU_TEST_ASSERT(rm.get_in_use_gps().empty());
    CYBOZU_TEST_ASSERT(rm.get_in_use_vecs().empty());
    CYBOZU_TEST_ASSERT(rm.get_in_use_opmasks().empty());
}

// =============================================================================
// Test – AMX scoped registers (RAII)
// =============================================================================
CYBOZU_TEST_AUTO(amxScopedRegisters)
{
    RegPoolManager rm;

    if (!rm.has_amx()) {
        // Nothing to scope without AMX.
        CYBOZU_TEST_ASSERT(rm.get_in_use_tiles().empty());
        return;
    }

    {
        auto s0 = rm.makeScoped(rm.alloc<Tmm>());
        auto s1 = rm.makeScoped(rm.alloc<Tmm>());
        CYBOZU_TEST_EQUAL((int)rm.get_in_use_tiles().size(), 2);
        // s0 and s1 are freed here by their destructors.
    }

    CYBOZU_TEST_ASSERT(rm.get_in_use_tiles().empty());
    CYBOZU_TEST_EQUAL((int)rm.get_free_tiles().size(), 8);
}

// =============================================================================
// Test – AMX reg_in_use and tile_idx_in_use helpers
// =============================================================================
CYBOZU_TEST_AUTO(amxRegInUse)
{
    RegPoolManager rm;

    if (!rm.has_amx()) {
        // tile_idx_in_use on valid indices must return false.
        CYBOZU_TEST_ASSERT(!rm.tile_idx_in_use(0));
        CYBOZU_TEST_ASSERT(!rm.tile_idx_in_use(7));
        // Out-of-range must throw regardless of AMX availability.
        CYBOZU_TEST_EXCEPTION(rm.tile_idx_in_use(8), Xbyak::Error);
        return;
    }

    auto t3 = rm.alloc<Tmm>(3);

    CYBOZU_TEST_ASSERT(rm.reg_in_use(t3));
    CYBOZU_TEST_ASSERT(rm.tile_idx_in_use(3));
    CYBOZU_TEST_ASSERT(!rm.tile_idx_in_use(4));

    // Out-of-range index must throw.
    CYBOZU_TEST_EXCEPTION(rm.tile_idx_in_use(8), Xbyak::Error);

    rm.free(t3);

    CYBOZU_TEST_ASSERT(!rm.reg_in_use(t3));
    CYBOZU_TEST_ASSERT(!rm.tile_idx_in_use(3));
}

// =============================================================================
// Test – In-use volatile / preserved GP register queries
// =============================================================================
CYBOZU_TEST_AUTO(inUseVolatilePreservedGPs)
{
    RegPoolManager rm;

    // Allocate three registers from the volatile (free) pool.
    auto v1 = rm.alloc<Reg64>();
    auto v2 = rm.alloc<Reg64>();
    auto v3 = rm.alloc<Reg64>();

    // Allocate up to two preserved registers (if any are available).
    auto preserved_list = rm.get_preserved_gps();
    std::vector<Reg64> preserved_regs;
    for (size_t i = 0; i < std::min(size_t(2), preserved_list.size()); ++i)
        preserved_regs.push_back(rm.alloc<Reg64>(preserved_list[i]));

    auto in_use_all      = rm.get_in_use_gps();
    auto in_use_volatile = rm.get_in_use_volatile_gps();
    auto in_use_preserved = rm.get_in_use_preserved_gps();

    // Volatile + preserved must equal total in-use.
    CYBOZU_TEST_EQUAL(in_use_volatile.size() + in_use_preserved.size(),
                      in_use_all.size());

    // Every volatile index must be in the base free pool.
    auto base_free = rm.get_free_gps();
    // (base_free is dynamic; the three newly allocated registers were taken from
    //  it so we check via get_preserved_gps non-membership instead.)
    for (int idx : in_use_volatile) {
        // A volatile register must NOT be in the preserved set.
        CYBOZU_TEST_ASSERT(std::find(preserved_list.begin(),
                                     preserved_list.end(), idx)
                           == preserved_list.end());
    }
    // Every preserved index must be in the preserved pool.
    for (int idx : in_use_preserved) {
        CYBOZU_TEST_ASSERT(std::find(preserved_list.begin(),
                                     preserved_list.end(), idx)
                           != preserved_list.end());
    }

    rm.free(v1);  rm.free(v2);  rm.free(v3);
    for (auto &r : preserved_regs) rm.free(r);

    // After full cleanup both queries must be empty.
    CYBOZU_TEST_ASSERT(rm.get_in_use_volatile_gps().empty());
    CYBOZU_TEST_ASSERT(rm.get_in_use_preserved_gps().empty());
}

// =============================================================================
// Test – Volatile GP query drives optimal caller-save in JIT code
// =============================================================================
CYBOZU_TEST_AUTO(volatileGPCallerSave)
{
    // Generates a function that:
    //   1. Allocates a mix of volatile and preserved GP registers.
    //   2. Initialises them.
    //   3. Uses get_in_use_volatile_gps() to save ONLY volatile registers
    //      before a real function call (optimal — no unnecessary push/pop).
    //   4. Calls call_function_that_clobbers_registers() (returns 210).
    //   5. Restores only the volatile registers.
    //   6. Returns the sum of all allocated registers + 210.
    class OptimalCallerJit : public CodeGenerator {
    public:
        OptimalCallerJit() : CodeGenerator(8192) {}

        void gen(RegPoolManager &rm) {
            push(rbx);  // rbx is callee-saved; we use it to stash the call result

            auto r1 = rm.alloc<Reg64>();
            auto r2 = rm.alloc<Reg64>();
            auto r3 = rm.alloc<Reg64>();
            mov(r1, 100);  mov(r2, 200);  mov(r3, 300);

            // Optionally grab one preserved register.
            auto preserved_list = rm.get_preserved_gps();
            Reg64 r4(0);
            bool have_preserved = false;
            if (!preserved_list.empty()) {
                // skip rbx (idx 3) since we already pushed it manually
                for (int idx : preserved_list) {
                    if (idx != rbx.getIdx()) {
                        r4 = rm.alloc<Reg64>(idx);
                        have_preserved = true;
                        break;
                    }
                }
            }
            if (have_preserved) mov(r4, 400);

            // Save ONLY volatile in-use registers.
            auto volatile_regs = rm.get_in_use_volatile_gps();
            for (int idx : volatile_regs) push(Reg64(idx));

#ifdef _WIN32
            // Win64 ABI: 32-byte shadow space + 16-byte alignment.
            // On entry rsp%16==8. After push(rbx) + volatile_regs.size() pushes:
            //   total_pushes_from_entry = 1 + volatile_regs.size()
            //   needs_pad if (total_pushes_from_entry % 2) == 0
            {
                size_t total_pushes = 1 + volatile_regs.size();
                bool needs_pad = (total_pushes % 2) == 0;
                int adj = needs_pad ? 40 : 32;
                sub(rsp, adj);
#endif
            mov(rax, reinterpret_cast<uint64_t>(
                         &call_function_that_clobbers_registers));
            call(rax);
            mov(rbx, rax);  // stash return value (210)
#ifdef _WIN32
                add(rsp, adj);
            }
#endif

            // Restore only volatile registers (in reverse order).
            for (auto it = volatile_regs.rbegin(); it != volatile_regs.rend(); ++it)
                pop(Reg64(*it));

            // Sum all allocated registers + the call's return value.
            mov(rax, r1);
            add(rax, r2);
            add(rax, r3);
            if (have_preserved) add(rax, r4);
            add(rax, rbx);

            rm.free(r1);  rm.free(r2);  rm.free(r3);
            if (have_preserved) rm.free(r4);
            pop(rbx);
            ret();
        }
    };

    RegPoolManager rm;
    OptimalCallerJit jit;
    jit.gen(rm);

    // The sum depends on whether a preserved register was grabbed, but we
    // always know the function-return contribution is 210.
    uint64_t result = call_jit(jit.getCode());
    // Minimum: 100 + 200 + 300 + 210 = 810 (no preserved reg allocated)
    // With preserved: 100 + 200 + 300 + 400 + 210 = 1210
    CYBOZU_TEST_ASSERT(result == 810 || result == 1210);
}

// =============================================================================
// Test – In-use volatile / preserved vector register queries
// =============================================================================
CYBOZU_TEST_AUTO(vecVolatilePreserved)
{
    RegPoolManager rm;

    // Allocate some vector registers from the free (volatile) pool.
    auto xr1 = rm.alloc<Xmm>();
    auto xr2 = rm.alloc<Xmm>();
    auto yr3 = rm.alloc<Ymm>();

    // Optionally allocate preserved vector registers (Windows only).
    auto preserved_vec_list = rm.get_preserved_vecs();
    std::vector<Xmm> preserved_vecs;
    for (size_t i = 0; i < std::min(size_t(2), preserved_vec_list.size()); ++i)
        preserved_vecs.push_back(rm.alloc<Xmm>(preserved_vec_list[i]));

    auto all_in_use       = rm.get_in_use_vecs();
    auto volatile_in_use  = rm.get_in_use_volatile_vecs();
    auto preserved_in_use = rm.get_in_use_preserved_vecs();

    // Volatile + preserved must equal total.
    CYBOZU_TEST_EQUAL(volatile_in_use.size() + preserved_in_use.size(),
                      all_in_use.size());

    // On Linux/macOS all vector registers are caller-saved: no preserved.
#ifndef _WIN32
    CYBOZU_TEST_ASSERT(preserved_in_use.empty());
    CYBOZU_TEST_EQUAL(volatile_in_use.size(), all_in_use.size());
#endif

    // Every preserved index must be in the preserved pool.
    for (int idx : preserved_in_use) {
        CYBOZU_TEST_ASSERT(std::find(preserved_vec_list.begin(),
                                     preserved_vec_list.end(), idx)
                           != preserved_vec_list.end());
    }

    rm.free(xr1);  rm.free(xr2);  rm.free(yr3);
    for (auto &v : preserved_vecs) rm.free(v);

    CYBOZU_TEST_ASSERT(rm.get_in_use_volatile_vecs().empty());
    CYBOZU_TEST_ASSERT(rm.get_in_use_preserved_vecs().empty());
}

// =============================================================================
// Test – Opmask volatile query (all opmasks are caller-saved)
// =============================================================================
CYBOZU_TEST_AUTO(opmaskVolatile)
{
    RegPoolManager rm;

    auto k1 = rm.alloc<Opmask>();
    auto k2 = rm.alloc<Opmask>();
    auto k3 = rm.alloc<Opmask>();

    auto all_in_use      = rm.get_in_use_opmasks();
    auto volatile_in_use = rm.get_in_use_volatile_opmasks();

    // All opmasks are volatile on both Windows and Linux.
    CYBOZU_TEST_EQUAL(all_in_use.size(), volatile_in_use.size());
    CYBOZU_TEST_ASSERT(all_in_use == volatile_in_use);

    rm.free(k1);  rm.free(k2);  rm.free(k3);

    CYBOZU_TEST_ASSERT(rm.get_in_use_volatile_opmasks().empty());
}

// =============================================================================
// Test – Comprehensive multi-family volatile / preserved queries
// =============================================================================
CYBOZU_TEST_AUTO(comprehensiveSaveRestore)
{
    RegPoolManager rm;

    // GP registers
    auto r1 = rm.alloc<Reg64>();
    auto r2 = rm.alloc<Reg64>();
    auto gp_preserved_list = rm.get_preserved_gps();
    Reg64 r3(0);
    bool have_gp_preserved = false;
    for (int idx : gp_preserved_list) {
        // avoid rsp (4) and rbp (5)
        if (idx != 4 && idx != 5) {
            r3 = rm.alloc<Reg64>(idx);
            have_gp_preserved = true;
            break;
        }
    }

    // Vector registers
    auto xmm1 = rm.alloc<Xmm>();
    auto ymm2  = rm.alloc<Ymm>();
    auto vec_preserved_list = rm.get_preserved_vecs();
    Xmm xmm3(0);
    bool have_vec_preserved = false;
    if (!vec_preserved_list.empty()) {
        xmm3 = rm.alloc<Xmm>(vec_preserved_list[0]);
        have_vec_preserved = true;
    }

    // Opmask registers
    auto k1 = rm.alloc<Opmask>();
    auto k2 = rm.alloc<Opmask>();

    // Query all families
    auto gp_volatile   = rm.get_in_use_volatile_gps();
    auto gp_preserved  = rm.get_in_use_preserved_gps();
    auto vec_volatile  = rm.get_in_use_volatile_vecs();
    auto vec_preserved = rm.get_in_use_preserved_vecs();
    auto om_volatile   = rm.get_in_use_volatile_opmasks();

    // Within each family: volatile + preserved == total in-use.
    CYBOZU_TEST_EQUAL(gp_volatile.size()  + gp_preserved.size(),
                      rm.get_in_use_gps().size());
    CYBOZU_TEST_EQUAL(vec_volatile.size() + vec_preserved.size(),
                      rm.get_in_use_vecs().size());
    CYBOZU_TEST_EQUAL(om_volatile.size(), rm.get_in_use_opmasks().size());

    // If a preserved GP was allocated it must appear in gp_preserved.
    if (have_gp_preserved) {
        CYBOZU_TEST_ASSERT(std::find(gp_preserved.begin(), gp_preserved.end(),
                                     r3.getIdx())
                           != gp_preserved.end());
    }
    // If a preserved vec was allocated it must appear in vec_preserved.
    if (have_vec_preserved) {
        CYBOZU_TEST_ASSERT(std::find(vec_preserved.begin(), vec_preserved.end(),
                                     xmm3.getIdx())
                           != vec_preserved.end());
    }

    // Cleanup
    rm.free(r1);  rm.free(r2);
    if (have_gp_preserved) rm.free(r3);
    rm.free(xmm1);  rm.free(ymm2);
    if (have_vec_preserved) rm.free(xmm3);
    rm.free(k1);  rm.free(k2);
}

// =============================================================================
// Test – AMX volatile tile query (all tiles are caller-saved)
// =============================================================================
CYBOZU_TEST_AUTO(amxVolatileTiles)
{
    RegPoolManager rm;

    if (!rm.has_amx()) {
        // Without AMX the in-use set is always empty.
        CYBOZU_TEST_ASSERT(rm.get_in_use_volatile_tiles().empty());
        return;
    }

    auto t1 = rm.alloc<Tmm>();
    auto t2 = rm.alloc<Tmm>();
    auto t3 = rm.alloc<Tmm>();

    auto all_in_use      = rm.get_in_use_tiles();
    auto volatile_in_use = rm.get_in_use_volatile_tiles();

    // All AMX tile registers are caller-saved on both Windows and Linux.
    CYBOZU_TEST_EQUAL(all_in_use.size(), volatile_in_use.size());
    CYBOZU_TEST_ASSERT(all_in_use == volatile_in_use);
    CYBOZU_TEST_EQUAL(volatile_in_use.size(), size_t(3));

    // Freeing one tile must be reflected immediately.
    rm.free(t3);
    CYBOZU_TEST_EQUAL(rm.get_in_use_volatile_tiles().size(), size_t(2));

    rm.free(t1);
    rm.free(t2);
    CYBOZU_TEST_ASSERT(rm.get_in_use_volatile_tiles().empty());
}

// =============================================================================
// Test – mark_unavailable / mark_available / is_reserved: GP registers
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailableGP)
{
    RegPoolManager rm;

    // Pick a known volatile GP register (rdi = 7 on SysV, or r8 = 8 on both ABIs).
    // r8 (idx=8) is caller-saved on both Windows and Linux, so it starts in free_gp_regs.
    const int idx = 8;  // r8

    // Initially not reserved and not in-use.
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Reg64>(idx));
    CYBOZU_TEST_ASSERT(!rm.gp_idx_in_use(idx));

    // After mark_unavailable, the register should be reserved and not allocatable.
    rm.mark_unavailable<Reg64>(idx);
    CYBOZU_TEST_ASSERT(rm.is_reserved<Reg64>(idx));
    CYBOZU_TEST_ASSERT(!rm.gp_idx_in_use(idx));

    // Reserved registers must not appear in the free GP pool.
    {
        auto free_gps = rm.get_free_gps();
        CYBOZU_TEST_ASSERT(std::find(free_gps.begin(), free_gps.end(), idx)
                           == free_gps.end());
    }

    // Attempting to alloc the reserved register by index must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg64>(idx), Xbyak::Error);

    // The next free alloc() must skip the reserved register.
    auto alloc1 = rm.alloc<Reg64>();
    CYBOZU_TEST_ASSERT(alloc1.getIdx() != idx);
    rm.free(alloc1);

    // Double-reserving must throw.
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Reg64>(idx), Xbyak::Error);

    // mark_available releases it back to its pool.
    rm.mark_available<Reg64>(idx);
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Reg64>(idx));
    {
        auto free_gps = rm.get_free_gps();
        CYBOZU_TEST_ASSERT(std::find(free_gps.begin(), free_gps.end(), idx)
                           != free_gps.end());
    }

    // Register is now allocatable again.
    auto r = rm.alloc<Reg64>(idx);
    CYBOZU_TEST_EQUAL(r.getIdx(), idx);
    rm.free(r);

    // mark_available on a non-reserved register must throw.
    CYBOZU_TEST_EXCEPTION(rm.mark_available<Reg64>(idx), Xbyak::Error);
}

// =============================================================================
// Test – mark_unavailable on a preserved (callee-saved) GP register
//            and verify it returns to the preserved pool on mark_available
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailablePreservedGP)
{
    RegPoolManager rm;

    // rbx = 3 is callee-saved on both ABIs.
    const int idx = 3;  // rbx

    CYBOZU_TEST_ASSERT(!rm.is_reserved<Reg64>(idx));
    {
        const auto pres = rm.get_preserved_gps();
        CYBOZU_TEST_ASSERT(std::find(pres.begin(), pres.end(), idx) != pres.end());
    }

    rm.mark_unavailable<Reg64>(idx);
    CYBOZU_TEST_ASSERT(rm.is_reserved<Reg64>(idx));

    // Must have left the preserved pool too.
    {
        const auto pres = rm.get_preserved_gps();
        CYBOZU_TEST_ASSERT(std::find(pres.begin(), pres.end(), idx) == pres.end());
    }

    rm.mark_available<Reg64>(idx);
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Reg64>(idx));

    // Must have returned to the preserved pool (not free).
    {
        const auto pres = rm.get_preserved_gps();
        const auto free = rm.get_free_gps();
        CYBOZU_TEST_ASSERT(std::find(pres.begin(), pres.end(), idx) != pres.end());
        CYBOZU_TEST_ASSERT(std::find(free.begin(), free.end(), idx) == free.end());
    }
}

// =============================================================================
// Test – mark_unavailable / mark_available: Vec and Opmask families
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailableVecOpmask)
{
    RegPoolManager rm;

    // ---- Vec ----
    // xmm0 (idx=0) is caller-saved on both ABIs.
    const int vec_idx = 0;
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Xmm>(vec_idx));

    rm.mark_unavailable<Xmm>(vec_idx);
    CYBOZU_TEST_ASSERT(rm.is_reserved<Xmm>(vec_idx));
    CYBOZU_TEST_ASSERT(!rm.vec_idx_in_use(vec_idx));

    // The reserved vector register must not appear in the free pool.
    {
        const auto free_vecs = rm.get_free_vecs();
        CYBOZU_TEST_ASSERT(std::find(free_vecs.begin(), free_vecs.end(), vec_idx)
                           == free_vecs.end());
    }

    // alloc by index must throw for reserved vec.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Xmm>(vec_idx), Xbyak::Error);

    rm.mark_available<Xmm>(vec_idx);
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Xmm>(vec_idx));
    {
        const auto free_vecs = rm.get_free_vecs();
        CYBOZU_TEST_ASSERT(std::find(free_vecs.begin(), free_vecs.end(), vec_idx)
                           != free_vecs.end());
    }

    // ---- Opmask ----
    // k1 (idx=1) is always in the free opmask pool.
    const int opmask_idx = 1;
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Opmask>(opmask_idx));

    rm.mark_unavailable<Opmask>(opmask_idx);
    CYBOZU_TEST_ASSERT(rm.is_reserved<Opmask>(opmask_idx));
    CYBOZU_TEST_ASSERT(!rm.opmask_idx_in_use(opmask_idx));

    // alloc by index must throw for reserved opmask.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Opmask>(opmask_idx), Xbyak::Error);

    rm.mark_available<Opmask>(opmask_idx);
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Opmask>(opmask_idx));
    {
        const auto free_masks = rm.get_free_opmasks();
        CYBOZU_TEST_ASSERT(std::find(free_masks.begin(), free_masks.end(), opmask_idx)
                           != free_masks.end());
    }
}

// =============================================================================
// Test – mark_unavailable / mark_available / is_reserved for tile registers
// (only exercised when has_amx() is true; skipped silently otherwise)
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailableTile)
{
    RegPoolManager rm;
    if (!rm.has_amx()) return; // tile pool is empty without AMX hardware

    const int tile_idx = 0; // tmm0 is caller-saved
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Tmm>(tile_idx));

    rm.mark_unavailable<Tmm>(tile_idx);
    CYBOZU_TEST_ASSERT(rm.is_reserved<Tmm>(tile_idx));
    CYBOZU_TEST_ASSERT(!rm.tile_idx_in_use(tile_idx));

    // Reserved tile must not appear in the free tile pool.
    {
        const auto free_tiles = rm.get_free_tiles();
        CYBOZU_TEST_ASSERT(std::find(free_tiles.begin(), free_tiles.end(), tile_idx)
                           == free_tiles.end());
    }

    // Allocating a reserved tile by index must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Tmm>(tile_idx), Xbyak::Error);

    // Double-reserve must throw.
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Tmm>(tile_idx), Xbyak::Error);

    rm.mark_available<Tmm>(tile_idx);
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Tmm>(tile_idx));

    // After release the register must be back in the free pool.
    {
        const auto free_tiles = rm.get_free_tiles();
        CYBOZU_TEST_ASSERT(std::find(free_tiles.begin(), free_tiles.end(), tile_idx)
                           != free_tiles.end());
    }

    // Alloc must succeed after mark_available.
    auto t = rm.alloc<Tmm>(tile_idx);
    CYBOZU_TEST_EQUAL(t.getIdx(), tile_idx);
    CYBOZU_TEST_ASSERT(rm.tile_idx_in_use(tile_idx));

    // Reserving an in-use tile must throw.
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Tmm>(tile_idx), Xbyak::Error);

    rm.free(t);
}

// =============================================================================
// Test – named-register overloads (mark_unavailable(reg) / mark_available(reg))
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailableNamedReg)
{
    struct NamedTest : Xbyak::CodeGenerator {
        void run() {
            RegPoolManager rm;

            // mark_unavailable(rdi) — named overload
            rm.mark_unavailable(rdi);
            CYBOZU_TEST_ASSERT(rm.is_reserved(rdi));
            CYBOZU_TEST_EXCEPTION(rm.alloc(rdi), Xbyak::Error);

            rm.mark_available(rdi);
            CYBOZU_TEST_ASSERT(!rm.is_reserved(rdi));

            // After release, alloc by name must succeed.
            auto r = rm.alloc(rdi);
            CYBOZU_TEST_EQUAL(r.getIdx(), rdi.getIdx());
            rm.free(r);

            // Vec named overload
            rm.mark_unavailable(xmm1);
            CYBOZU_TEST_ASSERT(rm.is_reserved(xmm1));
            CYBOZU_TEST_EXCEPTION(rm.alloc(xmm1), Xbyak::Error);
            rm.mark_available(xmm1);
            CYBOZU_TEST_ASSERT(!rm.is_reserved(xmm1));

            // Opmask named overload
            rm.mark_unavailable(k2);
            CYBOZU_TEST_ASSERT(rm.is_reserved(k2));
            CYBOZU_TEST_EXCEPTION(rm.alloc(k2), Xbyak::Error);
            rm.mark_available(k2);
            CYBOZU_TEST_ASSERT(!rm.is_reserved(k2));
        }
    };
    NamedTest t;
    t.run();
}

// =============================================================================
// Test – reserving an already-in-use register must throw
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailableInUseThrows)
{
    RegPoolManager rm;

    auto r = rm.alloc<Reg64>(9);  // r9 is caller-saved
    CYBOZU_TEST_ASSERT(rm.gp_idx_in_use(9));

    // Trying to reserve an already-allocated register must throw.
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Reg64>(9), Xbyak::Error);

    rm.free(r);
}

// =============================================================================
// Test – out-of-range index handling
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailableOutOfRange)
{
    RegPoolManager rm;

    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Reg64>(200), Xbyak::Error);
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Xmm>(200), Xbyak::Error);
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Opmask>(8), Xbyak::Error);
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Tmm>(8),    Xbyak::Error);

    CYBOZU_TEST_EXCEPTION(rm.mark_available<Reg64>(200), Xbyak::Error);
    CYBOZU_TEST_EXCEPTION(rm.mark_available<Xmm>(200), Xbyak::Error);
    CYBOZU_TEST_EXCEPTION(rm.mark_available<Opmask>(8), Xbyak::Error);
    CYBOZU_TEST_EXCEPTION(rm.mark_available<Tmm>(8),    Xbyak::Error);
}

// =============================================================================
// Test – all_free() and assert_all_free() with GP registers
// =============================================================================
CYBOZU_TEST_AUTO(allFreeGP)
{
    RegPoolManager rm;

    // Fresh manager: nothing allocated.
    CYBOZU_TEST_ASSERT(rm.all_free());
    rm.assert_all_free();

    // Allocated register: not all free.
    auto r = rm.alloc<Reg64>();
    CYBOZU_TEST_ASSERT(!rm.all_free());

    // After release: all free again.
    rm.free(r);
    CYBOZU_TEST_ASSERT(rm.all_free());
    rm.assert_all_free();
}

// =============================================================================
// Test – all_free() and assert_all_free() across GP, Vec, and Opmask families
// =============================================================================
CYBOZU_TEST_AUTO(allFreeMultiFamily)
{
    RegPoolManager rm;

    auto r0 = rm.alloc<Reg64>();
    auto r1 = rm.alloc<Reg64>();
    auto v0 = rm.alloc<Xmm>();
    auto v1 = rm.alloc<Ymm>();
    auto k0 = rm.alloc<Opmask>();

    CYBOZU_TEST_ASSERT(!rm.all_free());

    rm.free(r0);
    CYBOZU_TEST_ASSERT(!rm.all_free()); // four still in use

    rm.free(r1);
    rm.free(v0);
    rm.free(v1);
    rm.free(k0);

    CYBOZU_TEST_ASSERT(rm.all_free());
    rm.assert_all_free();
}

// =============================================================================
// Test – reserved registers (mark_unavailable) are invisible to all_free()
// =============================================================================
CYBOZU_TEST_AUTO(allFreeWithReserved)
{
    RegPoolManager rm;

    // Reserved registers are not in-use, so all_free() must still return true.
    rm.mark_unavailable<Reg64>(8);
    rm.mark_unavailable<Xmm>(0);
    CYBOZU_TEST_ASSERT(rm.all_free());
    rm.assert_all_free();

    rm.mark_available<Reg64>(8);
    rm.mark_available<Xmm>(0);
    CYBOZU_TEST_ASSERT(rm.all_free());
}

// =============================================================================
// Test – all_free() with tile registers when AMX is available
// =============================================================================
CYBOZU_TEST_AUTO(allFreeTile)
{
    RegPoolManager rm;
    if (!rm.has_amx()) return;

    auto t = rm.alloc<Tmm>(0);
    CYBOZU_TEST_ASSERT(!rm.all_free());

    rm.free(t);
    CYBOZU_TEST_ASSERT(rm.all_free());
    rm.assert_all_free();
}
