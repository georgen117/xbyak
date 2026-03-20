/*******************************************************************************
 * Unit tests for xbyak/xbyak_reg_manager.hpp
 *
 * Uses the Cybozu test framework (cybozu/test.hpp) — same as all other tests
 * in this directory.
 *
 * Tests covered (matching the sample/test_xbyak_reg_manager.cpp):
 *   basicAllocation         – alloc / free round-trip for GP registers
 *   specificAllocation      – named-register alloc + duplicate-alloc exception
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
// Test 1 – Basic allocation and deallocation
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
// Test 2 – Specific register allocation
// =============================================================================
CYBOZU_TEST_AUTO(specificAllocation)
{
    RegPoolManager rm;

    auto r10 = rm.alloc<Reg64>(10);
    auto r11 = rm.alloc<Reg64>(11);

    CYBOZU_TEST_EQUAL(r10.getIdx(), 10);
    CYBOZU_TEST_EQUAL(r11.getIdx(), 11);

    // Allocating an already-in-use index must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg64>(10), std::runtime_error);

    rm.free(r10);
    rm.free(r11);
}

// =============================================================================
// Test 3 – RAII scoped registers
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
// Test 4 – Vector register (Xmm / Ymm / Zmm) allocation
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
// Test 5 – Opmask register (k1-k7) allocation
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
// Test 6 – APX support detection
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
// Test 7 – Special register accessors and add_to_gp_pool
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
    CYBOZU_TEST_EXCEPTION(rm.add_to_gp_pool(200), std::runtime_error);

    // add_to_gp_pool for an index already in the free pool (rax=0, always
    // caller-saved) or the preserved pool (rbx=3, always callee-saved) must
    // throw on both Windows and Linux.
    CYBOZU_TEST_EXCEPTION(rm.add_to_gp_pool(0), std::runtime_error);
    CYBOZU_TEST_EXCEPTION(rm.add_to_gp_pool(3), std::runtime_error);
}

// =============================================================================
// Test 8 – Exhausting all allocatable GP registers
// =============================================================================
CYBOZU_TEST_AUTO(registerExhaustion)
{
    RegPoolManager rm;
    std::vector<Reg64> allocated;

    // Allocate until the pool is dry.
    try {
        for (int i = 0; i < 50; ++i)
            allocated.push_back(rm.alloc<Reg64>());
    } catch (const std::runtime_error &) {
        // Exhaustion exception is expected.
    }

    // rsp (index 4) and rbp (index 5) are never in the pool, so total
    // allocatable = max_gp_registers() - 2.
    CYBOZU_TEST_EQUAL((int)allocated.size(), rm.max_gp_registers() - 2);

    for (auto &r : allocated) rm.free(r);
    CYBOZU_TEST_ASSERT(rm.get_in_use_gps().empty());
}

// =============================================================================
// Test 9 – Mixed register family allocation
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
// Test 10 – reg_in_use / gp_idx_in_use helpers
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
// Test 11 – GP register aliasing (RAX / EAX / AX share index 0)
// =============================================================================
CYBOZU_TEST_AUTO(gpRegisterAliasing)
{
    RegPoolManager rm;

    // Allocate RAX (Reg64 index 0).
    auto rax_reg = rm.alloc<Reg64>(0);
    CYBOZU_TEST_EQUAL(rax_reg.getIdx(), 0);
    CYBOZU_TEST_ASSERT(rm.gp_idx_in_use(0));

    // EAX (Reg32(0)) shares the same physical register – must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg32>(0), std::runtime_error);
    // AX (Reg16(0)) also shares it – must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg16>(0), std::runtime_error);

    rm.free(rax_reg);

    // After freeing RAX, allocating EAX must succeed.
    CYBOZU_TEST_NO_EXCEPTION(
        auto eax = rm.alloc<Reg32>(0);
        CYBOZU_TEST_EQUAL(eax.getIdx(), 0);
        rm.free(eax);
    );
}

// =============================================================================
// Test 12 – Vector register aliasing (XMM / YMM / ZMM share index)
// =============================================================================
CYBOZU_TEST_AUTO(vectorRegisterAliasing)
{
    RegPoolManager rm;

    auto xmm0 = rm.alloc<Xmm>(0);
    CYBOZU_TEST_EQUAL(xmm0.getIdx(), 0);
    CYBOZU_TEST_ASSERT(rm.vec_idx_in_use(0));

    // YMM0 and ZMM0 share the same physical register – both must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Ymm>(0), std::runtime_error);
    CYBOZU_TEST_EXCEPTION(rm.alloc<Zmm>(0), std::runtime_error);

    rm.free(xmm0);

    // After freeing XMM0, allocating YMM0 must succeed.
    CYBOZU_TEST_NO_EXCEPTION(
        auto ymm0 = rm.alloc<Ymm>(0);
        CYBOZU_TEST_EQUAL(ymm0.getIdx(), 0);
        rm.free(ymm0);
    );
}

// =============================================================================
// Test 13 – Write and read register contents via JIT execution
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
// Test 14 – Function call convention: parameter passing and non-volatile
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
// Test 15 – Realistic JIT kernel using the register manager for strategy
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
// Test 16 – Dynamic save/restore across a real function call
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

            // Call a real function that clobbers caller-saved registers.
            mov(rax,
                reinterpret_cast<uint64_t>(&call_function_that_clobbers_registers));
            call(rax);
            mov(rbx, rax);  // stash the return value (210) in rbx

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
// Test 17 – AMX support detection
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
// Test 18 – AMX tile register allocation and deallocation
// =============================================================================
CYBOZU_TEST_AUTO(amxTileRegisters)
{
    RegPoolManager rm;

    if (!rm.has_amx()) {
        // Without AMX hardware alloc must throw immediately.
        CYBOZU_TEST_EXCEPTION(rm.alloc<Tmm>(), std::runtime_error);
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
    CYBOZU_TEST_EXCEPTION(rm.alloc<Tmm>(2), std::runtime_error);

    rm.free(t0);
    rm.free(t1);
    rm.free(t2);

    CYBOZU_TEST_ASSERT(rm.get_in_use_tiles().empty());
    CYBOZU_TEST_EQUAL((int)rm.get_free_tiles().size(), 8);

    // Freeing a tile that is not in use must throw.
    CYBOZU_TEST_EXCEPTION(rm.free(t0), std::runtime_error);
}

// =============================================================================
// Test 19 – AMX tile exhaustion
// =============================================================================
CYBOZU_TEST_AUTO(amxTileExhaustion)
{
    RegPoolManager rm;
    std::vector<Tmm> allocated;

    // Allocate until exhausted; expect an exception when the pool is empty.
    try {
        for (int i = 0; i < 10; ++i)
            allocated.push_back(rm.alloc<Tmm>());
    } catch (const std::runtime_error &) {
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
// Test 20 – Mixed allocation including AMX tiles
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
// Test 21 – AMX scoped registers (RAII)
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
// Test 22 – AMX reg_in_use and tile_idx_in_use helpers
// =============================================================================
CYBOZU_TEST_AUTO(amxRegInUse)
{
    RegPoolManager rm;

    if (!rm.has_amx()) {
        // tile_idx_in_use on valid indices must return false.
        CYBOZU_TEST_ASSERT(!rm.tile_idx_in_use(0));
        CYBOZU_TEST_ASSERT(!rm.tile_idx_in_use(7));
        // Out-of-range must throw regardless of AMX availability.
        CYBOZU_TEST_EXCEPTION(rm.tile_idx_in_use(8), std::runtime_error);
        return;
    }

    auto t3 = rm.alloc<Tmm>(3);

    CYBOZU_TEST_ASSERT(rm.reg_in_use(t3));
    CYBOZU_TEST_ASSERT(rm.tile_idx_in_use(3));
    CYBOZU_TEST_ASSERT(!rm.tile_idx_in_use(4));

    // Out-of-range index must throw.
    CYBOZU_TEST_EXCEPTION(rm.tile_idx_in_use(8), std::runtime_error);

    rm.free(t3);

    CYBOZU_TEST_ASSERT(!rm.reg_in_use(t3));
    CYBOZU_TEST_ASSERT(!rm.tile_idx_in_use(3));
}
