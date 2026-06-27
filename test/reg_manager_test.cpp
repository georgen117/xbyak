/*******************************************************************************
 * Unit tests for xbyak/xbyak_reg_manager.hpp
 *
 * Uses the Cybozu test framework (cybozu/test.hpp) — same as all other tests
 * in this directory.
 *
 * Tests covered:
 *   basicAllocation         – alloc / free round-trip for GP registers
 *   specificAllocation      – index-based alloc<T>(int) + duplicate-alloc exception
 *   namedRegisterAlloc      – named-register alloc(reg) overload across all families
 *   scopedRegisters         – RAII makeScoped auto-free on scope exit
 *   allocScopedConvenience  – allocScoped<T>() / allocScoped<T>(idx) helpers
 *   scopedImplicitConversion – Scoped implicit conversion and forwarding accessors
 *   vectorRegisters         – alloc / free of Xmm / Ymm / Zmm
 *   opmaskRegisters         – alloc / free of Opmask (k1-k7, k0 is special)
 *   apxSupport              – max_gp_registers() reflects APX capability
 *   addToPool               – stack_ptr / base_ptr / add_to_gp_pool
 *   registerExhaustion      – allocating more regs than available throws
 *   mixedAllocation         – mix of GP / Vec / Opmask in one manager
 *   regInUseAllFamilies     – reg_live round-trip for every register family
 *   gpRegisterAliasing      – Reg64/Reg32/Reg16 share a physical register index
 *   vectorRegisterAliasing  – Xmm/Ymm/Zmm share a physical register index
 *   registerContentsViaJIT  – write + read register values through JIT execution
 *   functionCallConvention  – ABI convention: parameter passing + non-volatile
 *                             register preservation across a JIT call
 *   realisticKernel         – code-generation using manager for register strategy
 *   dynamicSaveRestore      – dynamic get_live_gps() usage for manual save/restore
 *   amxSupport              – has_amx() / max_tile_registers() reflect AMX capability
 *   amxTileRegisters        – alloc / free of Tmm (tmm0-tmm7)
 *   amxTileExhaustion       – allocating more tiles than available throws
 *   mixedAllocationWithAMX  – mix of GP / Vec / Opmask / AMX in one manager
 *   amxScopedRegisters      – RAII makeScoped auto-free for Tmm
 *   amxRegInUse             – reg_live helpers for Tmm
 *   inUseVolatilePreservedGPs – get_live_volatile/preserved_gps() correctness
 *   volatileGPCallerSave    – JIT caller-saves only volatile GPs around a call
 *   vecVolatilePreserved    – get_live_volatile/preserved_vecs() correctness
 *   comprehensiveSaveRestore – multi-family volatile/preserved queries agree with totals
 *   (mark_unavailable/allFree/setCodeGenerator/prologue/emitCall groups)
 *   stackLayout*            – StackFrameBuilder / StackFrame two-phase stack management
 *   managedAliasPatterns    – declare_alias overloads, has_stack_slot(), is_active()
 *   managedAliasPrime       – prime() allocates named/anonymous register; conflict throws
 *   managedAliasNoSlotActive – no-slot alias: lazy alloc(); throws GP_IN_USE on conflict
 *   managedAliasFreeAndReprime – free() then alloc() re-acquires the register
 *   managedAliasReset       – reset() clears pending_aliases_; register usable again
 *   managedAliasNoSlotNoop  – save/restore emit no code when has_stack_slot()==false
 *   managedAliasSaveRestoreJIT – save/restore slot round-trip via JIT execution
 *   managedAliasMixedWithParks – alias slot and GP park slot coexist in one layout
 *   managedAliasAnonymous   – declare_alias<Reg64>() anonymous slot round-trip
 *   aliasDeclareNoSlotBasic – declare_alias(reg, AliasMode::no_slot) lifecycle
 *   aliasDeclareNoSlotMutualExclusion – two no-slot aliases on same reg, alloc conflict
 *   aliasDeclareNoSlotSequential – sequential alloc/free/alloc across two aliases
 *   aliasDeclareNoSlotViaThreeArg – three-arg declare_alias smoke test
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

// CPU feature object — constructed once, shared across all tests.
static const Xbyak::util::Cpu g_cpu;

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
    RegPoolManager rm(g_cpu);

    auto r1 = rm.alloc<Reg64>();
    auto r2 = rm.alloc<Reg64>();
    auto r3 = rm.alloc<Reg32>();

    CYBOZU_TEST_EQUAL((int)rm.get_live_gps().size(), 3);

    rm.free(r1);
    rm.free(r2);
    rm.free(r3);

    CYBOZU_TEST_ASSERT(rm.get_live_gps().empty());
}

// =============================================================================
// Test – Specific register allocation
// =============================================================================
CYBOZU_TEST_AUTO(specificAllocation)
{
    RegPoolManager rm(g_cpu);

    auto r10 = rm.alloc<Reg64>(10);
    auto r11 = rm.alloc<Reg64>(11);

    CYBOZU_TEST_EQUAL(r10.getIdx(), 10);
    CYBOZU_TEST_EQUAL(r11.getIdx(), 11);

    // Allocating an already-in-use index must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg64>(10), Xbyak::RegManagerError);

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
            RegPoolManager rm(g_cpu);

            // GP Reg64 — names match assembly register notation exactly.
            auto reg_rdx = rm.alloc(rdx);   // rdx: caller-saved on both ABIs
            auto reg_r10 = rm.alloc(r10);   // r10: caller-saved
            CYBOZU_TEST_EQUAL(reg_rdx.getIdx(), rdx.getIdx());
            CYBOZU_TEST_EQUAL(reg_r10.getIdx(), r10.getIdx());

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

            // AMX Tile — tmm0 and tmm1 (skipped if AMX is not available).
            // tmm0-tmm7 are const Tmm members of CodeGenerator, so they work
            // identically to rax, xmm2, k1 etc.
            if (rm.has_amx()) {
                auto reg_tmm0 = rm.alloc(tmm0);
                auto reg_tmm1 = rm.alloc(tmm1);
                CYBOZU_TEST_EQUAL(reg_tmm0.getIdx(), tmm0.getIdx());
                CYBOZU_TEST_EQUAL(reg_tmm1.getIdx(), tmm1.getIdx());

                // Duplicate alloc by name must throw for tiles too.
                CYBOZU_TEST_EXCEPTION(rm.alloc(tmm0), Xbyak::RegManagerError);

                rm.free(reg_tmm0);
                rm.free(reg_tmm1);
                CYBOZU_TEST_ASSERT(rm.get_live_tiles().empty());
            }

            // Allocating an already-in-use register by name must throw — the
            // same error path as alloc<Reg64>(int idx) for a duplicate index.
            CYBOZU_TEST_EXCEPTION(rm.alloc(rdx),  Xbyak::RegManagerError);
            CYBOZU_TEST_EXCEPTION(rm.alloc(xmm2), Xbyak::RegManagerError);

            // Free GP and Vec registers.
            rm.free(reg_rdx);  rm.free(reg_r10);
            rm.free(reg_r8d);  rm.free(reg_r9w);
            rm.free(reg_xmm2); rm.free(reg_ymm3); rm.free(reg_zmm4);

            CYBOZU_TEST_ASSERT(rm.get_live_gps().empty());
            CYBOZU_TEST_ASSERT(rm.get_live_vecs().empty());

            // Opmask — k1 and k2 (skipped if AVX-512F is not available).
            if (rm.has_avx512()) {
                auto reg_k1 = rm.alloc(k1);
                auto reg_k2 = rm.alloc(k2);
                CYBOZU_TEST_EQUAL(reg_k1.getIdx(), k1.getIdx());
                CYBOZU_TEST_EQUAL(reg_k2.getIdx(), k2.getIdx());
                CYBOZU_TEST_EXCEPTION(rm.alloc(k1),   Xbyak::RegManagerError);
                rm.free(reg_k1);   rm.free(reg_k2);
                CYBOZU_TEST_ASSERT(rm.get_live_opmasks().empty());
            }
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
    RegPoolManager rm(g_cpu);

    {
        auto scoped1 = rm.makeScoped(rm.alloc<Reg64>());
        auto scoped2 = rm.makeScoped(rm.alloc<Reg64>());
        CYBOZU_TEST_EQUAL((int)rm.get_live_gps().size(), 2);
        // scoped1 and scoped2 are freed here by their destructors.
    }

    CYBOZU_TEST_ASSERT(rm.get_live_gps().empty());
}

// =============================================================================
// Test – allocScoped convenience helpers
// =============================================================================
CYBOZU_TEST_AUTO(allocScopedConvenience)
{
    RegPoolManager rm(g_cpu);

    // allocScoped<T>() — auto-free on scope exit, same as makeScoped(alloc<T>()).
    {
        auto r1 = rm.allocScoped<Reg64>();
        auto r2 = rm.allocScoped<Reg64>();
        CYBOZU_TEST_EQUAL((int)rm.get_live_gps().size(), 2);
    }
    CYBOZU_TEST_ASSERT(rm.get_live_gps().empty());

    // allocScoped<T>(idx) — specific register by index.
    {
        auto r10 = rm.allocScoped<Reg64>(10);
        CYBOZU_TEST_EQUAL((int)r10.get().getIdx(), 10);
        CYBOZU_TEST_ASSERT(rm.reg_live(r10.get()));
    }
    CYBOZU_TEST_ASSERT(rm.get_live_gps().empty());

    // Works across all tracked families.
    {
        auto xmm = rm.allocScoped<Xmm>();
        CYBOZU_TEST_EQUAL((int)rm.get_live_vecs().size(), 1);
        if (rm.has_avx512()) {
            auto k = rm.allocScoped<Opmask>();
            CYBOZU_TEST_EQUAL((int)rm.get_live_opmasks().size(), 1);
        }
    }
    CYBOZU_TEST_ASSERT(rm.get_live_vecs().empty());
    CYBOZU_TEST_ASSERT(rm.get_live_opmasks().empty());

    // Scoped guard is invalidated by reset() — no double-free or error.
    {
        auto r = rm.allocScoped<Reg64>();
        rm.reset();
        // destructor of r fires here; should silently no-op due to generation mismatch
    }
    CYBOZU_TEST_ASSERT(rm.get_live_gps().empty());
}

// =============================================================================
// Test – Scoped implicit conversion and forwarding accessors
// =============================================================================
CYBOZU_TEST_AUTO(scopedImplicitConversion)
{
    RegPoolManager rm(g_cpu);

    // getIdx() / getBit() forwarding — no .get() needed.
    {
        auto r = rm.allocScoped<Reg64>(8);  // r8
        CYBOZU_TEST_EQUAL(r.getIdx(), 8);
        CYBOZU_TEST_EQUAL(r.getBit(), 64);

        auto x = rm.allocScoped<Xmm>(2);   // xmm2
        CYBOZU_TEST_EQUAL(x.getIdx(), 2);
        CYBOZU_TEST_EQUAL(x.getBit(), 128);
    }
    CYBOZU_TEST_ASSERT(rm.get_live_gps().empty());
    CYBOZU_TEST_ASSERT(rm.get_live_vecs().empty());

    // Implicit conversion — Scoped<Reg64> passed to a function expecting const Reg64 &.
    {
        auto r = rm.allocScoped<Reg64>(9);  // r9
        // reg_live(const RegT&) accepts Scoped<Reg64> via the implicit conversion.
        CYBOZU_TEST_ASSERT(rm.reg_live<Reg64>(r));
    }

    // Implicit conversion in JIT emission — Scoped<Reg64> used as a CodeGenerator operand.
    {
        class ImplicitJit : public CodeGenerator {
        public:
            ImplicitJit() : CodeGenerator(4096) {}
            void gen(RegPoolManager &rm) {
                // allocScoped returns Scoped<Reg64>; the implicit conversion to const Reg64&
                // lets it pass directly to mov/ret without calling .get().
                auto dst = rm.allocScoped<Reg64>(0); // rax
                auto src = rm.allocScoped<Reg64>(8); // r8
                mov(src, uint32_t(0xABCDu));
                mov(dst, src);  // Scoped<Reg64> implicitly converts to const Reg64&
                ret();
            }
        };

        ImplicitJit jit;
        jit.gen(rm);
        CYBOZU_TEST_ASSERT(rm.get_live_gps().empty());
        auto result = reinterpret_cast<uint64_t(*)()>(jit.getCode())();
        CYBOZU_TEST_EQUAL(result, (uint64_t)0xABCDu);
    }
}

// =============================================================================
// Test – Vector register (Xmm / Ymm / Zmm) allocation
// =============================================================================
CYBOZU_TEST_AUTO(vectorRegisters)
{
    RegPoolManager rm(g_cpu);

    auto xmm1 = rm.alloc<Xmm>();
    auto xmm2 = rm.alloc<Xmm>();
    auto ymm3 = rm.alloc<Ymm>();
    auto ymm4 = rm.alloc<Ymm>();
    auto zmm5 = rm.alloc<Zmm>();

    CYBOZU_TEST_EQUAL((int)rm.get_live_vecs().size(), 5);

    rm.free(xmm1);
    rm.free(xmm2);
    rm.free(ymm3);
    rm.free(ymm4);
    rm.free(zmm5);

    CYBOZU_TEST_ASSERT(rm.get_live_vecs().empty());
}

// =============================================================================
// Test – Opmask register (k1-k7) allocation
// =============================================================================
CYBOZU_TEST_AUTO(opmaskRegisters)
{
    RegPoolManager rm(g_cpu);
    if (!rm.has_avx512()) return; // opmask requires AVX-512F

    auto k1 = rm.alloc<Opmask>();
    auto k2 = rm.alloc<Opmask>();

    CYBOZU_TEST_EQUAL((int)rm.get_live_opmasks().size(), 2);

    // k0 is reserved ("unmasked" sentinel) – allocated indices must be >= 1.
    CYBOZU_TEST_ASSERT(k1.getIdx() >= 1);
    CYBOZU_TEST_ASSERT(k2.getIdx() >= 1);
    CYBOZU_TEST_ASSERT(k1.getIdx() != k2.getIdx());

    rm.free(k1);
    rm.free(k2);

    CYBOZU_TEST_ASSERT(rm.get_live_opmasks().empty());
}

// =============================================================================
// Test – APX support detection
// =============================================================================
CYBOZU_TEST_AUTO(apxSupport)
{
    RegPoolManager rm(g_cpu);

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
    RegPoolManager rm(g_cpu);

    // stack_ptr() == rsp (index 4).
    auto rsp_reg = rm.stack_ptr();
    CYBOZU_TEST_EQUAL(rsp_reg.getIdx(), 4);

    // base_ptr() == rbp (index 5).
    auto rbp_reg = rm.base_ptr();
    CYBOZU_TEST_EQUAL(rbp_reg.getIdx(), 5);

    // add_to_gp_pool with an out-of-range index must throw.
    CYBOZU_TEST_EXCEPTION(rm.add_to_gp_pool(200), Xbyak::RegManagerError);

    // add_to_gp_pool for an index already in the free pool (rax=0, always
    // caller-saved) or the preserved pool (rbx=3, always callee-saved) must
    // throw on both Windows and Linux.
    CYBOZU_TEST_EXCEPTION(rm.add_to_gp_pool(0), Xbyak::RegManagerError);
    CYBOZU_TEST_EXCEPTION(rm.add_to_gp_pool(3), Xbyak::RegManagerError);
}

// =============================================================================
// Test – Exhausting all allocatable GP registers
// =============================================================================
CYBOZU_TEST_AUTO(registerExhaustion)
{
    RegPoolManager rm(g_cpu);
    std::vector<Reg64> allocated;

    // Allocate until the pool is dry.
    try {
        for (int i = 0; i < 50; ++i)
            allocated.push_back(rm.alloc<Reg64>());
    } catch (const Xbyak::RegManagerError &) {
        // Exhaustion exception is expected.
    }

    // rsp (index 4) and rbp (index 5) are never in the pool, so total
    // allocatable = max_gp_registers() - 2.
    CYBOZU_TEST_EQUAL((int)allocated.size(), rm.max_gp_registers() - 2);

    for (auto &r : allocated) rm.free(r);
    CYBOZU_TEST_ASSERT(rm.get_live_gps().empty());
}

// =============================================================================
// Test – Mixed register family allocation
// =============================================================================
CYBOZU_TEST_AUTO(mixedAllocation)
{
    RegPoolManager rm(g_cpu);

    auto r64 = rm.alloc<Reg64>();
    auto r32 = rm.alloc<Reg32>();
    auto r16 = rm.alloc<Reg16>();
    auto xmm = rm.alloc<Xmm>();
    auto ymm = rm.alloc<Ymm>();
    auto zmm = rm.alloc<Zmm>();

    CYBOZU_TEST_EQUAL((int)rm.get_live_gps().size(),  3);
    CYBOZU_TEST_EQUAL((int)rm.get_live_vecs().size(), 3);

    if (rm.has_avx512()) {
        auto k = rm.alloc<Opmask>();
        CYBOZU_TEST_EQUAL((int)rm.get_live_opmasks().size(), 1);
        rm.free(k);
        CYBOZU_TEST_ASSERT(rm.get_live_opmasks().empty());
    }

    rm.free(r64);  rm.free(r32);  rm.free(r16);
    rm.free(xmm);  rm.free(ymm);  rm.free(zmm);

    CYBOZU_TEST_ASSERT(rm.get_live_gps().empty());
    CYBOZU_TEST_ASSERT(rm.get_live_vecs().empty());
}

// =============================================================================
// Test – reg_live round-trip for every register family
// =============================================================================
CYBOZU_TEST_AUTO(regInUseAllFamilies)
{
    RegPoolManager rm(g_cpu);

    // GP (Reg64)
    auto gp = rm.alloc<Reg64>();
    CYBOZU_TEST_ASSERT(rm.reg_live(gp));
    rm.free(gp);
    CYBOZU_TEST_ASSERT(!rm.reg_live(gp));

    // GP aliases: Reg32 and Reg16 share the same physical index.
    auto gp32 = rm.alloc<Reg32>();
    CYBOZU_TEST_ASSERT(rm.reg_live(gp32));
    rm.free(gp32);
    CYBOZU_TEST_ASSERT(!rm.reg_live(gp32));

    // Vec (Xmm / Ymm / Zmm) — only exercised when the OS has enabled vector state
    if (rm.has_avx512() || !rm.get_free_vecs().empty()) {
        auto xmm = rm.alloc<Xmm>();
        CYBOZU_TEST_ASSERT(rm.reg_live(xmm));
        rm.free(xmm);
        CYBOZU_TEST_ASSERT(!rm.reg_live(xmm));

        auto ymm = rm.alloc<Ymm>();
        CYBOZU_TEST_ASSERT(rm.reg_live(ymm));
        rm.free(ymm);
        CYBOZU_TEST_ASSERT(!rm.reg_live(ymm));

        auto zmm = rm.alloc<Zmm>();
        CYBOZU_TEST_ASSERT(rm.reg_live(zmm));
        rm.free(zmm);
        CYBOZU_TEST_ASSERT(!rm.reg_live(zmm));
    }

    // Opmask (k1-k7)
    if (!rm.get_free_opmasks().empty()) {
        auto k = rm.alloc<Opmask>();
        CYBOZU_TEST_ASSERT(rm.reg_live(k));
        rm.free(k);
        CYBOZU_TEST_ASSERT(!rm.reg_live(k));
    }

    // AMX Tile (tmm0-tmm7) — only exercised when AMX is available
    if (rm.has_amx()) {
        auto tmm = rm.alloc<Tmm>();
        CYBOZU_TEST_ASSERT(rm.reg_live(tmm));
        rm.free(tmm);
        CYBOZU_TEST_ASSERT(!rm.reg_live(tmm));
    }
}

// =============================================================================
// Test – GP register aliasing (RAX / EAX / AX share index 0)
// =============================================================================
CYBOZU_TEST_AUTO(gpRegisterAliasing)
{
    RegPoolManager rm(g_cpu);

    // Allocate RAX (Reg64 index 0).
    auto rax_reg = rm.alloc<Reg64>(0);
    CYBOZU_TEST_EQUAL(rax_reg.getIdx(), 0);

    // EAX (Reg32(0)) shares the same physical register – must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg32>(0), Xbyak::RegManagerError);
    // AX (Reg16(0)) also shares it – must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg16>(0), Xbyak::RegManagerError);

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
    RegPoolManager rm(g_cpu);

    auto xmm0 = rm.alloc<Xmm>(0);
    CYBOZU_TEST_EQUAL(xmm0.getIdx(), 0);

    // YMM0 and ZMM0 share the same physical register – both must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Ymm>(0), Xbyak::RegManagerError);
    CYBOZU_TEST_EXCEPTION(rm.alloc<Zmm>(0), Xbyak::RegManagerError);

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
    RegPoolManager rm(g_cpu);

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
// Test – Function call convention: parameter passing, preserved register
//        survival, and volatile register save/restore.
// =============================================================================
CYBOZU_TEST_AUTO(functionCallConvention)
{
    class FunctionCallJit : public CodeGenerator {
    public:
        FunctionCallJit() : CodeGenerator(4096) {}

        // Callee: sum 4 integer parameters and return in rax.
        // Parameter registers differ between Win64 (rcx, rdx, r8, r9) and
        // SysV AMD64 (rdi, rsi, rdx, rcx).
        void gen_callee_function() {
#ifdef _WIN32
            mov(rax, rcx);
            add(rax, rdx);
            add(rax, r8);
            add(rax, r9);
#else
            mov(rax, rdi);
            add(rax, rsi);
            add(rax, rdx);
            add(rax, rcx);
#endif
            ret();
        }

        // Caller: store known values in preserved registers, call the callee
        // (JIT-to-JIT, no shadow space needed), add the preserved-register
        // values to the return value to demonstrate they survived the call.
        //
        // Win64 preserves rbx, rdi, rsi (3 registers → result = 10+100+200+300).
        // SysV AMD64 preserves rbx, r12 only (rdi/rsi are volatile → result = 10+100+200).
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
    };

    // Scenario 1: Parameter passing — (1+2+3+4) plus non-volatile register values.
    // Expected result differs by ABI because Win64 has more preserved registers.
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

    // Scenario 2: Volatile register save/restore using manager-allocated registers.
    // The manager allocates from the volatile pool on each ABI, so this works
    // without naming specific ABI registers.
    {
        class VolatileSaveRestoreJit : public CodeGenerator {
        public:
            VolatileSaveRestoreJit() : CodeGenerator(4096) {}

            void gen(RegPoolManager &rm) {
                auto r1 = rm.alloc<Reg64>();
                auto r2 = rm.alloc<Reg64>();
                auto r3 = rm.alloc<Reg64>();
                auto r4 = rm.alloc<Reg64>();
                mov(r1, 10);  mov(r2, 20);  mov(r3, 30);  mov(r4, 40);
                push(r1);  push(r2);  push(r3);  push(r4);
                xor_(r1, r1);  xor_(r2, r2);  xor_(r3, r3);  xor_(r4, r4);
                pop(r4);  pop(r3);  pop(r2);  pop(r1);
                mov(rax, r1);  add(rax, r2);  add(rax, r3);  add(rax, r4);
                rm.free(r1);  rm.free(r2);  rm.free(r3);  rm.free(r4);
                ret();
            }
        };

        RegPoolManager rm(g_cpu);
        VolatileSaveRestoreJit jit;
        jit.gen(rm);
        CYBOZU_TEST_EQUAL(call_jit(jit.getCode()), (uint64_t)(10 + 20 + 30 + 40));
    }

    // Scenario 3: Verify the register manager's preserved GP list matches the ABI.
    // rbx(3) and r12-r15(12-15) are callee-saved on all x86-64 ABIs.
    // Win64 additionally preserves rdi(7) and rsi(6); on SysV they are volatile.
    {
        RegPoolManager rm(g_cpu);
        const auto preserved = rm.get_preserved_gps();
        CYBOZU_TEST_ASSERT(
            std::find(preserved.begin(), preserved.end(), 3) != preserved.end());
        for (int i = 12; i <= 15; ++i)
            CYBOZU_TEST_ASSERT(
                std::find(preserved.begin(), preserved.end(), i) != preserved.end());
#ifdef _WIN32
        // Win64: rdi(7) and rsi(6) are callee-saved.
        CYBOZU_TEST_ASSERT(
            std::find(preserved.begin(), preserved.end(), 6) != preserved.end());
        CYBOZU_TEST_ASSERT(
            std::find(preserved.begin(), preserved.end(), 7) != preserved.end());
#else
        // SysV: rdi(7) and rsi(6) are caller-saved — must NOT appear in preserved pool.
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
        RegPoolManager rm(g_cpu);
        RealisticKernel jit;
        jit.gen_kernel_with_manager(rm);
        CYBOZU_TEST_EQUAL(call_jit(jit.getCode()), (uint64_t)(42 + 100));
    }
    {
        RegPoolManager rm(g_cpu);
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
        // Use get_live_gps() to generate push/pop code, then make a
        // real call that clobbers volatile registers, verify the sum.
        // Expected result: 100 + 200 + 300 + 400 + 210 (function return).
        void gen_caller_saves_all(RegPoolManager &rm) {
            // Attach this code generator so emit_call can emit the platform-correct
            // alignment adjustment and Win64 shadow space into this code stream.
            rm.set_code_generator(this);
            // Exclude rbx from the manager so it cannot appear in get_live_gps();
            // we manage it manually as a stash for the call return value.
            rm.mark_unavailable(rbx);
            push(rbx);

            auto r1 = rm.alloc<Reg64>();
            auto r2 = rm.alloc<Reg64>();
            auto r3 = rm.alloc<Reg64>();
            auto r4 = rm.alloc<Reg64>();
            mov(r1, 100);  mov(r2, 200);  mov(r3, 300);  mov(r4, 400);

            // Ask the manager which registers are live - generate save code.
            auto in_use = rm.get_live_gps();
            for (int idx : in_use) push(Reg64(idx));

            // extra_pushes = 1 (push rbx above) + in_use.size() (save loop above)
            // so emit_call computes the correct 16-byte alignment pad.
            rm.emit_call(&call_function_that_clobbers_registers, 1 + in_use.size());
            mov(rbx, rax);  // stash the return value (210) in rbx

            // Generate restore code in reverse order.
            for (auto it = in_use.rbegin(); it != in_use.rend(); ++it)
                pop(Reg64(*it));

            // Sum the four restored registers.
            mov(rax, Reg64(in_use[0]));
            for (size_t i = 1; i < in_use.size(); ++i)
                add(rax, Reg64(in_use[i]));
            add(rax, rbx);  // add the function's return value (210)

            pop(rbx);
            rm.free(r1);  rm.free(r2);  rm.free(r3);  rm.free(r4);
            rm.mark_available(rbx);
            ret();
        }

        // Use get_live_gps() to decide which registers to push/pop.
        // Expected result: 111 + 222 + 333 + 444 = 1110.
        void gen_loop_based_save_restore(RegPoolManager &rm) {
            std::vector<Reg64> regs;
            for (int i = 0; i < 4; ++i) {
                auto r = rm.alloc<Reg64>();
                regs.push_back(r);
                mov(r, (i + 1) * 111);
            }

            // Save every currently in-use GP register.
            std::vector<int> saved;
            for (int idx : rm.get_live_gps()) {
                if (true) {
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
        RegPoolManager rm(g_cpu);
        DynamicJit jit;
        jit.gen_caller_saves_all(rm);
        CYBOZU_TEST_EQUAL(call_jit(jit.getCode()),
                          (uint64_t)(100 + 200 + 300 + 400 + 210));
    }

    // Scenario 2: loop-based save/restore using reg_live().
    {
        RegPoolManager rm(g_cpu);
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
    RegPoolManager rm(g_cpu);

    if (rm.has_amx()) {
        // AMX present: 8 tile registers (tmm0-tmm7), free pool starts full.
        CYBOZU_TEST_EQUAL(rm.max_tile_registers(), 8);
        CYBOZU_TEST_EQUAL((int)rm.get_free_tiles().size(), 8);
        CYBOZU_TEST_ASSERT(rm.get_live_tiles().empty());
    } else {
        // No AMX: pool is empty and max is 0.
        CYBOZU_TEST_EQUAL(rm.max_tile_registers(), 0);
        CYBOZU_TEST_ASSERT(rm.get_free_tiles().empty());
        CYBOZU_TEST_ASSERT(rm.get_live_tiles().empty());
    }
}

// =============================================================================
// Test – AMX tile register allocation and deallocation
// =============================================================================
CYBOZU_TEST_AUTO(amxTileRegisters)
{
    RegPoolManager rm(g_cpu);

    if (!rm.has_amx()) {
        // Without AMX hardware alloc must throw immediately.
        CYBOZU_TEST_EXCEPTION(rm.alloc<Tmm>(), Xbyak::RegManagerError);
        return;
    }

    // Allocate a few tile registers.
    auto t0 = rm.alloc<Tmm>();
    auto t1 = rm.alloc<Tmm>();
    auto t2 = rm.alloc<Tmm>(2);  // allocate specific tile

    CYBOZU_TEST_EQUAL((int)rm.get_live_tiles().size(), 3);
    CYBOZU_TEST_EQUAL((int)rm.get_free_tiles().size(), 5);  // 8 - 3

    // Indices must be within the valid AMX range.
    CYBOZU_TEST_ASSERT(t0.getIdx() >= 0 && t0.getIdx() <= 7);
    CYBOZU_TEST_ASSERT(t1.getIdx() >= 0 && t1.getIdx() <= 7);
    CYBOZU_TEST_EQUAL(t2.getIdx(), 2);

    // Duplicate allocation of tile 2 must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Tmm>(2), Xbyak::RegManagerError);

    rm.free(t0);
    rm.free(t1);
    rm.free(t2);

    CYBOZU_TEST_ASSERT(rm.get_live_tiles().empty());
    CYBOZU_TEST_EQUAL((int)rm.get_free_tiles().size(), 8);

    // Freeing a tile that is not in use must throw.
    CYBOZU_TEST_EXCEPTION(rm.free(t0), Xbyak::RegManagerError);
}

// =============================================================================
// Test – AMX tile exhaustion
// =============================================================================
CYBOZU_TEST_AUTO(amxTileExhaustion)
{
    RegPoolManager rm(g_cpu);
    std::vector<Tmm> allocated;

    // Allocate until exhausted; expect an exception when the pool is empty.
    try {
        for (int i = 0; i < 10; ++i)
            allocated.push_back(rm.alloc<Tmm>());
    } catch (const Xbyak::RegManagerError &) {
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
    CYBOZU_TEST_ASSERT(rm.get_live_tiles().empty());
}

// =============================================================================
// Test – Mixed allocation including AMX tiles
// =============================================================================
CYBOZU_TEST_AUTO(mixedAllocationWithAMX)
{
    RegPoolManager rm(g_cpu);

    auto r64 = rm.alloc<Reg64>();
    auto xmm = rm.alloc<Xmm>();

    CYBOZU_TEST_EQUAL((int)rm.get_live_gps().size(),  1);
    CYBOZU_TEST_EQUAL((int)rm.get_live_vecs().size(), 1);

    if (rm.has_avx512()) {
        auto k = rm.alloc<Opmask>();
        CYBOZU_TEST_EQUAL((int)rm.get_live_opmasks().size(), 1);
        rm.free(k);
        CYBOZU_TEST_ASSERT(rm.get_live_opmasks().empty());
    }

    if (rm.has_amx()) {
        auto t0 = rm.alloc<Tmm>();
        auto t1 = rm.alloc<Tmm>();
        CYBOZU_TEST_EQUAL((int)rm.get_live_tiles().size(), 2);
        rm.free(t0);
        rm.free(t1);
        CYBOZU_TEST_ASSERT(rm.get_live_tiles().empty());
    } else {
        CYBOZU_TEST_ASSERT(rm.get_live_tiles().empty());
    }

    rm.free(r64);
    rm.free(xmm);

    CYBOZU_TEST_ASSERT(rm.get_live_gps().empty());
    CYBOZU_TEST_ASSERT(rm.get_live_vecs().empty());
}

// =============================================================================
// Test – AMX scoped registers (RAII)
// =============================================================================
CYBOZU_TEST_AUTO(amxScopedRegisters)
{
    RegPoolManager rm(g_cpu);

    if (!rm.has_amx()) {
        // Nothing to scope without AMX.
        CYBOZU_TEST_ASSERT(rm.get_live_tiles().empty());
        return;
    }

    {
        auto s0 = rm.makeScoped(rm.alloc<Tmm>());
        auto s1 = rm.makeScoped(rm.alloc<Tmm>());
        CYBOZU_TEST_EQUAL((int)rm.get_live_tiles().size(), 2);
        // s0 and s1 are freed here by their destructors.
    }

    CYBOZU_TEST_ASSERT(rm.get_live_tiles().empty());
    CYBOZU_TEST_EQUAL((int)rm.get_free_tiles().size(), 8);
}

// =============================================================================
// Test – reg_live helpers for Tmm
// =============================================================================
CYBOZU_TEST_AUTO(amxRegInUse)
{
    RegPoolManager rm(g_cpu);

    if (!rm.has_amx()) return;

    auto t3 = rm.alloc<Tmm>(3);

    CYBOZU_TEST_ASSERT(rm.reg_live(t3));
    CYBOZU_TEST_ASSERT(!rm.reg_live(Tmm(4)));

    rm.free(t3);

    CYBOZU_TEST_ASSERT(!rm.reg_live(t3));
}

// =============================================================================
// Test – In-use volatile / preserved GP register queries
// =============================================================================
CYBOZU_TEST_AUTO(inUseVolatilePreservedGPs)
{
    RegPoolManager rm(g_cpu);

    // Allocate three registers from the volatile (free) pool.
    auto v1 = rm.alloc<Reg64>();
    auto v2 = rm.alloc<Reg64>();
    auto v3 = rm.alloc<Reg64>();

    // Allocate up to two preserved registers (if any are available).
    auto preserved_list = rm.get_preserved_gps();
    std::vector<Reg64> preserved_regs;
    for (size_t i = 0; i < std::min(size_t(2), preserved_list.size()); ++i)
        preserved_regs.push_back(rm.alloc<Reg64>(preserved_list[i]));

    auto in_use_all      = rm.get_live_gps();
    auto in_use_volatile = rm.get_live_volatile_gps();
    auto in_use_preserved = rm.get_live_preserved_gps();

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
    CYBOZU_TEST_ASSERT(rm.get_live_volatile_gps().empty());
    CYBOZU_TEST_ASSERT(rm.get_live_preserved_gps().empty());
}

// =============================================================================
// Test – Volatile GP query drives optimal caller-save in JIT code
// =============================================================================
CYBOZU_TEST_AUTO(volatileGPCallerSave)
{
    // Generates a function that:
    //   1. Allocates a mix of volatile and preserved GP registers.
    //   2. Initialises them.
    //   3. Uses get_live_volatile_gps() to save ONLY volatile registers
    //      before a real function call (optimal — no unnecessary push/pop).
    //   4. Calls call_function_that_clobbers_registers() (returns 210).
    //   5. Restores only the volatile registers.
    //   6. Returns the sum of all allocated registers + 210.
    class OptimalCallerJit : public CodeGenerator {
    public:
        OptimalCallerJit() : CodeGenerator(8192) {}

        void gen(RegPoolManager &rm) {
            rm.set_code_generator(this);
            // Exclude rbx from the manager so it cannot appear in get_live_volatile_gps();
            // we manage it manually as a stash for the call return value.
            rm.mark_unavailable(rbx);
            push(rbx);

            auto r1 = rm.alloc<Reg64>();
            auto r2 = rm.alloc<Reg64>();
            auto r3 = rm.alloc<Reg64>();
            mov(r1, 100);  mov(r2, 200);  mov(r3, 300);

            // Optionally grab one preserved register.
            // (rbx is excluded via mark_unavailable above, so preserved_list will not contain it)
            auto preserved_list = rm.get_preserved_gps();
            Reg64 r4(0);
            bool have_preserved = false;
            if (!preserved_list.empty()) {
                r4 = rm.alloc<Reg64>(preserved_list[0]);
                have_preserved = true;
            }
            if (have_preserved) mov(r4, 400);

            // Save ONLY volatile in-use registers.
            auto volatile_regs = rm.get_live_volatile_gps();
            for (int idx : volatile_regs) push(Reg64(idx));

            // extra_pushes = 1 (push rbx above) + volatile_regs.size() (save loop above)
            // so emit_call computes the correct 16-byte alignment pad.
            rm.emit_call(&call_function_that_clobbers_registers, 1 + volatile_regs.size());
            mov(rbx, rax);  // stash return value (210)

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
            rm.mark_available(rbx);
            ret();
        }
    };

    RegPoolManager rm(g_cpu);
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
    RegPoolManager rm(g_cpu);

    // Allocate some vector registers from the free (volatile) pool.
    auto xr1 = rm.alloc<Xmm>();
    auto xr2 = rm.alloc<Xmm>();
    auto yr3 = rm.alloc<Ymm>();

    // Optionally allocate preserved vector registers (Windows only).
    auto preserved_vec_list = rm.get_preserved_vecs();
    std::vector<Xmm> preserved_vecs;
    for (size_t i = 0; i < std::min(size_t(2), preserved_vec_list.size()); ++i)
        preserved_vecs.push_back(rm.alloc<Xmm>(preserved_vec_list[i]));

    auto all_in_use       = rm.get_live_vecs();
    auto volatile_in_use  = rm.get_live_volatile_vecs();
    auto preserved_in_use = rm.get_live_preserved_vecs();

    // Volatile + preserved must equal total.
    CYBOZU_TEST_EQUAL(volatile_in_use.size() + preserved_in_use.size(),
                      all_in_use.size());

    // On platforms where all vector registers are volatile (e.g. SysV AMD64),
    // the preserved in-use list must be empty.
    if (rm.get_preserved_vecs().empty()) {
        CYBOZU_TEST_ASSERT(preserved_in_use.empty());
        CYBOZU_TEST_EQUAL(volatile_in_use.size(), all_in_use.size());
    }

    // Every preserved index must be in the preserved pool.
    for (int idx : preserved_in_use) {
        CYBOZU_TEST_ASSERT(std::find(preserved_vec_list.begin(),
                                     preserved_vec_list.end(), idx)
                           != preserved_vec_list.end());
    }

    rm.free(xr1);  rm.free(xr2);  rm.free(yr3);
    for (auto &v : preserved_vecs) rm.free(v);

    CYBOZU_TEST_ASSERT(rm.get_live_volatile_vecs().empty());
    CYBOZU_TEST_ASSERT(rm.get_live_preserved_vecs().empty());
}

// =============================================================================
// Test – Comprehensive multi-family volatile / preserved queries
// =============================================================================
CYBOZU_TEST_AUTO(comprehensiveSaveRestore)
{
    RegPoolManager rm(g_cpu);

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

    // Query GP and Vec families
    auto gp_volatile   = rm.get_live_volatile_gps();
    auto gp_preserved  = rm.get_live_preserved_gps();
    auto vec_volatile  = rm.get_live_volatile_vecs();
    auto vec_preserved = rm.get_live_preserved_vecs();

    // Within each family: volatile + preserved == total in-use.
    CYBOZU_TEST_EQUAL(gp_volatile.size()  + gp_preserved.size(),
                      rm.get_live_gps().size());
    CYBOZU_TEST_EQUAL(vec_volatile.size() + vec_preserved.size(),
                      rm.get_live_vecs().size());

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

    // Cleanup GP and Vec
    rm.free(r1);  rm.free(r2);
    if (have_gp_preserved) rm.free(r3);
    rm.free(xmm1);  rm.free(ymm2);
    if (have_vec_preserved) rm.free(xmm3);

    // Opmask registers (skipped if AVX-512F is not available).
    if (rm.has_avx512()) {
        auto k1 = rm.alloc<Opmask>();
        auto k2 = rm.alloc<Opmask>();
        rm.free(k1);  rm.free(k2);
    }
}

// =============================================================================
// Test – mark_unavailable / mark_available / is_reserved: GP registers
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailableGP)
{
    RegPoolManager rm(g_cpu);

    // Pick a known volatile GP register (rdi = 7 on SysV, or r8 = 8 on both ABIs).
    // r8 (idx=8) is caller-saved on both Windows and Linux, so it starts in free_gp_regs.
    const int idx = 8;  // r8

    // Initially not reserved and not in-use.
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Reg64>(idx));

    // After mark_unavailable, the register should be reserved and not allocatable.
    rm.mark_unavailable<Reg64>(idx);
    CYBOZU_TEST_ASSERT(rm.is_reserved<Reg64>(idx));

    // Reserved registers must not appear in the free GP pool.
    {
        auto free_gps = rm.get_free_gps();
        CYBOZU_TEST_ASSERT(std::find(free_gps.begin(), free_gps.end(), idx)
                           == free_gps.end());
    }

    // Attempting to alloc the reserved register by index must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg64>(idx), Xbyak::RegManagerError);

    // The next free alloc() must skip the reserved register.
    auto alloc1 = rm.alloc<Reg64>();
    CYBOZU_TEST_ASSERT(alloc1.getIdx() != idx);
    rm.free(alloc1);

    // Double-reserving must throw.
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Reg64>(idx), Xbyak::RegManagerError);

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
    CYBOZU_TEST_EXCEPTION(rm.mark_available<Reg64>(idx), Xbyak::RegManagerError);
}

// =============================================================================
// Test – mark_unavailable on a preserved (callee-saved) GP register
//            and verify it returns to the preserved pool on mark_available
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailablePreservedGP)
{
    RegPoolManager rm(g_cpu);

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
    RegPoolManager rm(g_cpu);

    // ---- Vec ----
    // xmm0 (idx=0) is caller-saved on both ABIs.
    const int vec_idx = 0;
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Xmm>(vec_idx));

    rm.mark_unavailable<Xmm>(vec_idx);
    CYBOZU_TEST_ASSERT(rm.is_reserved<Xmm>(vec_idx));

    // The reserved vector register must not appear in the free pool.
    {
        const auto free_vecs = rm.get_free_vecs();
        CYBOZU_TEST_ASSERT(std::find(free_vecs.begin(), free_vecs.end(), vec_idx)
                           == free_vecs.end());
    }

    // alloc by index must throw for reserved vec.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Xmm>(vec_idx), Xbyak::RegManagerError);

    rm.mark_available<Xmm>(vec_idx);
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Xmm>(vec_idx));
    {
        const auto free_vecs = rm.get_free_vecs();
        CYBOZU_TEST_ASSERT(std::find(free_vecs.begin(), free_vecs.end(), vec_idx)
                           != free_vecs.end());
    }

    // ---- Opmask (skipped if AVX-512F is not available) ----
    if (rm.has_avx512()) {
        // k1 (idx=1) is always in the free opmask pool.
        const int opmask_idx = 1;
        CYBOZU_TEST_ASSERT(!rm.is_reserved<Opmask>(opmask_idx));

        rm.mark_unavailable<Opmask>(opmask_idx);
        CYBOZU_TEST_ASSERT(rm.is_reserved<Opmask>(opmask_idx));

        // alloc by index must throw for reserved opmask.
        CYBOZU_TEST_EXCEPTION(rm.alloc<Opmask>(opmask_idx), Xbyak::RegManagerError);

        rm.mark_available<Opmask>(opmask_idx);
        CYBOZU_TEST_ASSERT(!rm.is_reserved<Opmask>(opmask_idx));
        {
            const auto free_masks = rm.get_free_opmasks();
            CYBOZU_TEST_ASSERT(std::find(free_masks.begin(), free_masks.end(), opmask_idx)
                               != free_masks.end());
        }
    }
}

// =============================================================================
// Test – mark_unavailable / mark_available / is_reserved for tile registers
// (only exercised when has_amx() is true; skipped silently otherwise)
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailableTile)
{
    RegPoolManager rm(g_cpu);
    if (!rm.has_amx()) return; // tile pool is empty without AMX hardware

    const int tile_idx = 0; // tmm0 is caller-saved
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Tmm>(tile_idx));

    rm.mark_unavailable<Tmm>(tile_idx);
    CYBOZU_TEST_ASSERT(rm.is_reserved<Tmm>(tile_idx));

    // Reserved tile must not appear in the free tile pool.
    {
        const auto free_tiles = rm.get_free_tiles();
        CYBOZU_TEST_ASSERT(std::find(free_tiles.begin(), free_tiles.end(), tile_idx)
                           == free_tiles.end());
    }

    // Allocating a reserved tile by index must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Tmm>(tile_idx), Xbyak::RegManagerError);

    // Double-reserve must throw.
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Tmm>(tile_idx), Xbyak::RegManagerError);

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

    // Reserving an in-use tile must throw.
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Tmm>(tile_idx), Xbyak::RegManagerError);

    rm.free(t);
}

// =============================================================================
// Test – named-register overloads (mark_unavailable(reg) / mark_available(reg))
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailableNamedReg)
{
    struct NamedTest : Xbyak::CodeGenerator {
        void run() {
            RegPoolManager rm(g_cpu);

            // mark_unavailable(rdi) — named overload
            rm.mark_unavailable(rdi);
            CYBOZU_TEST_ASSERT(rm.is_reserved(rdi));
            CYBOZU_TEST_EXCEPTION(rm.alloc(rdi), Xbyak::RegManagerError);

            rm.mark_available(rdi);
            CYBOZU_TEST_ASSERT(!rm.is_reserved(rdi));

            // After release, alloc by name must succeed.
            auto r = rm.alloc(rdi);
            CYBOZU_TEST_EQUAL(r.getIdx(), rdi.getIdx());
            rm.free(r);

            // Vec named overload
            rm.mark_unavailable(xmm1);
            CYBOZU_TEST_ASSERT(rm.is_reserved(xmm1));
            CYBOZU_TEST_EXCEPTION(rm.alloc(xmm1), Xbyak::RegManagerError);
            rm.mark_available(xmm1);
            CYBOZU_TEST_ASSERT(!rm.is_reserved(xmm1));

            // Opmask named overload (skipped if AVX-512F is not available).
            if (rm.has_avx512()) {
                rm.mark_unavailable(k2);
                CYBOZU_TEST_ASSERT(rm.is_reserved(k2));
                CYBOZU_TEST_EXCEPTION(rm.alloc(k2), Xbyak::RegManagerError);
                rm.mark_available(k2);
                CYBOZU_TEST_ASSERT(!rm.is_reserved(k2));
            }
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
    RegPoolManager rm(g_cpu);

    auto r = rm.alloc<Reg64>(9);  // r9 is caller-saved

    // Trying to reserve an already-allocated register must throw.
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Reg64>(9), Xbyak::RegManagerError);

    rm.free(r);
}

// =============================================================================
// Test – out-of-range index handling
// =============================================================================
CYBOZU_TEST_AUTO(markUnavailableOutOfRange)
{
    RegPoolManager rm(g_cpu);

    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Reg64>(200), Xbyak::RegManagerError);
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Xmm>(200), Xbyak::RegManagerError);
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Opmask>(8), Xbyak::RegManagerError);
    CYBOZU_TEST_EXCEPTION(rm.mark_unavailable<Tmm>(8),    Xbyak::RegManagerError);

    CYBOZU_TEST_EXCEPTION(rm.mark_available<Reg64>(200), Xbyak::RegManagerError);
    CYBOZU_TEST_EXCEPTION(rm.mark_available<Xmm>(200), Xbyak::RegManagerError);
    CYBOZU_TEST_EXCEPTION(rm.mark_available<Opmask>(8), Xbyak::RegManagerError);
    CYBOZU_TEST_EXCEPTION(rm.mark_available<Tmm>(8),    Xbyak::RegManagerError);
}

// =============================================================================
// Test – all_free() and assert_all_free() with GP registers
// =============================================================================
CYBOZU_TEST_AUTO(allFreeGP)
{
    RegPoolManager rm(g_cpu);

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
    RegPoolManager rm(g_cpu);

    auto r0 = rm.alloc<Reg64>();
    auto r1 = rm.alloc<Reg64>();
    auto v0 = rm.alloc<Xmm>();
    auto v1 = rm.alloc<Ymm>();

    CYBOZU_TEST_ASSERT(!rm.all_free());

    rm.free(r0);
    CYBOZU_TEST_ASSERT(!rm.all_free()); // r1, v0, v1 still in use

    rm.free(r1);
    rm.free(v0);
    rm.free(v1);

    // Opmask (skipped if AVX-512F is not available).
    if (rm.has_avx512()) {
        auto k0 = rm.alloc<Opmask>();
        CYBOZU_TEST_ASSERT(!rm.all_free());
        rm.free(k0);
    }

    CYBOZU_TEST_ASSERT(rm.all_free());
    rm.assert_all_free();
}

// =============================================================================
// Test – reserved registers (mark_unavailable) are invisible to all_free()
// =============================================================================
CYBOZU_TEST_AUTO(allFreeWithReserved)
{
    RegPoolManager rm(g_cpu);

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
    RegPoolManager rm(g_cpu);
    if (!rm.has_amx()) return;

    auto t = rm.alloc<Tmm>(0);
    CYBOZU_TEST_ASSERT(!rm.all_free());

    rm.free(t);
    CYBOZU_TEST_ASSERT(rm.all_free());
    rm.assert_all_free();
}

// =============================================================================
// Test – has_code_generator() is false when constructed without a pointer
// =============================================================================
CYBOZU_TEST_AUTO(setCodeGeneratorDefault)
{
    RegPoolManager rm(g_cpu);
    CYBOZU_TEST_ASSERT(!rm.has_code_generator());

    // Explicitly passing NULL behaves the same as the default constructor.
    RegPoolManager rm2(g_cpu);
    CYBOZU_TEST_ASSERT(!rm2.has_code_generator());
}

// =============================================================================
// Test – set_code_generator() attaches and detaches a CodeGenerator
// =============================================================================
CYBOZU_TEST_AUTO(setCodeGeneratorLateInject)
{
    Xbyak::CodeGenerator cg(4096);
    RegPoolManager rm(g_cpu);

    CYBOZU_TEST_ASSERT(!rm.has_code_generator());

    rm.set_code_generator(&cg);
    CYBOZU_TEST_ASSERT(rm.has_code_generator());

    // Passing NULL detaches the generator.
    rm.set_code_generator(NULL);
    CYBOZU_TEST_ASSERT(!rm.has_code_generator());

    // Re-attaching after detach works.
    rm.set_code_generator(&cg);
    CYBOZU_TEST_ASSERT(rm.has_code_generator());
}

// =============================================================================
// Test – composition pattern: RegPoolManager as a member initialised with 'this'
// =============================================================================
CYBOZU_TEST_AUTO(codeGeneratorComposition)
{
    // The recommended pattern for JIT kernels that do not want to inherit from
    // RegPoolManager directly.  The CodeGenerator base is fully constructed before
    // rm_(g_cpu, this) runs, so the pointer is valid when the manager stores it.
    struct MyKernel : public Xbyak::CodeGenerator {
        Xbyak::RegPoolManager rm_;
        MyKernel() : Xbyak::CodeGenerator(4096), rm_(g_cpu, this) {}
    };

    MyKernel k;
    CYBOZU_TEST_ASSERT(k.rm_.has_code_generator());

    // Register allocation still works normally after coupling.
    auto r1 = k.rm_.alloc<Reg64>();
    auto r2 = k.rm_.alloc<Reg64>();
    CYBOZU_TEST_EQUAL((int)k.rm_.get_live_gps().size(), 2);
    k.rm_.free(r1);
    k.rm_.free(r2);
    CYBOZU_TEST_ASSERT(k.rm_.all_free());
}

// =============================================================================
// Test – inheritance pattern: kernel inherits both CodeGenerator and RegPoolManager
// =============================================================================
CYBOZU_TEST_AUTO(codeGeneratorInheritance)
{
    // CodeGenerator must appear first in the base-class list so it is fully
    // constructed before RegPoolManager(g_cpu, this) runs.
    struct MyKernel : public Xbyak::CodeGenerator, public Xbyak::RegPoolManager {
        MyKernel()
            : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(g_cpu, this) {}

        void build() {
            // Register-manager methods are called without a prefix because they
            // are brought into scope by direct inheritance.
            CYBOZU_TEST_ASSERT(has_code_generator());
            auto r1 = alloc<Reg64>();
            auto r2 = alloc<Reg64>();
            CYBOZU_TEST_EQUAL((int)get_live_gps().size(), 2);
            RegPoolManager::free(r1);
            RegPoolManager::free(r2);
            ret();
        }
    };

    MyKernel k;
    k.build();
    CYBOZU_TEST_ASSERT(k.all_free());
}

// =============================================================================
// Test – get_allocated_preserved_gps() tracks promotions from preserved pool
// =============================================================================
CYBOZU_TEST_AUTO(prologueEpilogueTracking)
{
    RegPoolManager rm(g_cpu);

    // Nothing allocated yet.
    CYBOZU_TEST_ASSERT(rm.get_allocated_preserved_gps().empty());
    CYBOZU_TEST_ASSERT(rm.get_allocated_preserved_vecs().empty());

    // Allocate every volatile GP register (preserved pool untouched).
    std::vector<Reg64> v_regs;
    std::vector<int> free_idxs = rm.get_free_gps();
    for (int i = 0; i < (int)free_idxs.size(); ++i)
        v_regs.push_back(rm.alloc<Reg64>());
    CYBOZU_TEST_ASSERT(rm.get_allocated_preserved_gps().empty());

    // Promote the first callee-saved GP (rbx = index 3 on both ABIs).
    Reg64 r_pres1 = rm.alloc<Reg64>();
    CYBOZU_TEST_EQUAL((int)rm.get_allocated_preserved_gps().size(), 1);
    CYBOZU_TEST_EQUAL(rm.get_allocated_preserved_gps()[0], r_pres1.getIdx());

    // Promote a second callee-saved GP.
    Reg64 r_pres2 = rm.alloc<Reg64>();
    CYBOZU_TEST_EQUAL((int)rm.get_allocated_preserved_gps().size(), 2);
    CYBOZU_TEST_EQUAL(rm.get_allocated_preserved_gps()[1], r_pres2.getIdx());

    // free() does not remove entries from the tracking list — the registers
    // were promoted and must still be saved/restored at the ABI boundary.
    rm.free(r_pres1);
    rm.free(r_pres2);
    CYBOZU_TEST_EQUAL((int)rm.get_allocated_preserved_gps().size(), 2);

    for (auto &r : v_regs) rm.free(r);
}

// =============================================================================
// Test – emit_prologue() and emit_epilogue() throw when no CodeGenerator is set
// =============================================================================
CYBOZU_TEST_AUTO(prologueEpilogueThrowsNoCG)
{
    RegPoolManager rm(g_cpu); // no CodeGenerator attached

    // Promote a preserved register so there is something to emit.
    std::vector<Reg64> v_regs;
    std::vector<int> free_idxs = rm.get_free_gps();
    for (int i = 0; i < (int)free_idxs.size(); ++i)
        v_regs.push_back(rm.alloc<Reg64>());
    Reg64 r_pres = rm.alloc<Reg64>();

    CYBOZU_TEST_EXCEPTION(rm.emit_prologue(), Xbyak::RegManagerError);
    CYBOZU_TEST_EXCEPTION(rm.emit_epilogue(), Xbyak::RegManagerError);

    for (auto &r : v_regs) rm.free(r);
    rm.free(r_pres);
}

// =============================================================================
// Test – emit_prologue() is idempotent: only newly promoted registers are pushed
// =============================================================================
CYBOZU_TEST_AUTO(prologueEpilogueIdempotent)
{
    struct IdempotentKernel : public CodeGenerator, public RegPoolManager {
        IdempotentKernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}

        void build() {
            // First call with no preserved registers allocated — emits nothing.
            const size_t sz0 = getSize();
            emit_prologue();
            CYBOZU_TEST_EQUAL(sz0, getSize());

            // Exhaust the volatile GP pool.
            std::vector<Reg64> v_regs;
            std::vector<int> free_idxs = get_free_gps();
            for (int i = 0; i < (int)free_idxs.size(); ++i)
                v_regs.push_back(alloc<Reg64>());

            // Promote one callee-saved GP.
            Reg64 r_pres1 = alloc<Reg64>();

            // Second call — emits exactly one push.
            const size_t sz1 = getSize();
            emit_prologue();
            CYBOZU_TEST_ASSERT(getSize() > sz1);

            // Third call — nothing new, emits nothing.
            const size_t sz2 = getSize();
            emit_prologue();
            CYBOZU_TEST_EQUAL(sz2, getSize());

            // Promote a second callee-saved GP then call again — emits one more push.
            Reg64 r_pres2 = alloc<Reg64>();
            const size_t sz3 = getSize();
            emit_prologue();
            CYBOZU_TEST_ASSERT(getSize() > sz3);

            // Complete the function so the generated code is valid.
            xor_(rax, rax);
            for (auto &r : v_regs) RegPoolManager::free(r);
            RegPoolManager::free(r_pres1);
            RegPoolManager::free(r_pres2);
            emit_epilogue();
            ret();
        }
    };

    IdempotentKernel k;
    k.build();
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)0);
}

// =============================================================================
// Test – emit_prologue()/emit_epilogue() with only volatile registers
//        (no pushes or pops should be emitted)
// =============================================================================
CYBOZU_TEST_AUTO(prologueEpilogueVolatileOnly)
{
    struct VolatileKernel : public CodeGenerator, public RegPoolManager {
        VolatileKernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}

        void build() {
            emit_prologue(); // nothing to push — no preserved regs allocated

            auto r1 = alloc<Reg64>();
            mov(r1, 99);
            mov(rax, r1);

            RegPoolManager::free(r1);
            emit_epilogue(); // nothing to pop
            ret();
        }
    };

    VolatileKernel k;
    k.build();
    CYBOZU_TEST_ASSERT(k.get_allocated_preserved_gps().empty());
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)99);
}

// =============================================================================
// Test – emit_prologue()/emit_epilogue() end-to-end with a callee-saved GP
// =============================================================================
CYBOZU_TEST_AUTO(prologueEpilogueJIT)
{
    // Build a JIT kernel that exhausts the volatile GP pool, forces one
    // callee-saved GP to be allocated, and verifies the save/restore sequence
    // generated by emit_prologue()/emit_epilogue() produces correct results.
    struct PreservedKernel : public CodeGenerator, public RegPoolManager {
        PreservedKernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}

        void build() {
            // Exhaust all volatile GP registers.
            std::vector<Reg64> v_regs;
            std::vector<int> free_idxs = get_free_gps();
            for (int i = 0; i < (int)free_idxs.size(); ++i)
                v_regs.push_back(alloc<Reg64>());

            // This alloc promotes the first callee-saved GP (rbx on both ABIs).
            Reg64 r_pres = alloc<Reg64>();
            CYBOZU_TEST_EQUAL((int)get_allocated_preserved_gps().size(), 1);

            // Emit push for r_pres.
            emit_prologue();

            // Use r_pres and return its value via rax.
            mov(r_pres, 42);
            mov(rax, r_pres); // rax = 42 before pop

            for (auto &r : v_regs) RegPoolManager::free(r);
            RegPoolManager::free(r_pres);

            // Emit pop for r_pres (restores caller's original value).
            emit_epilogue();
            ret();
        }
    };

    PreservedKernel k;
    k.build();
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)42);
}

// =============================================================================
// Test – emit_call(): ABI-correct outgoing call (alignment / shadow space)
// =============================================================================
CYBOZU_TEST_AUTO(emitCall)
{
    // Without a CodeGenerator attached, emit_call() must throw.
    {
        RegPoolManager rm(g_cpu);
        CYBOZU_TEST_EXCEPTION(
            rm.emit_call(reinterpret_cast<uint64_t>(
                &call_function_that_clobbers_registers)),
            Xbyak::RegManagerError);
    }

    // emit_call() respects managed_push_count_ from emit_prologue(): when
    // emit_prologue pushes one callee-saved GP, managed_push_count_ becomes 1
    // (odd), so emit_call needs no alignment pad on SysV.
    {
        struct ManagedCallKernel : public CodeGenerator, public RegPoolManager {
            ManagedCallKernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}

            void build() {
                // Exhaust the volatile GP pool, promoting one preserved register.
                std::vector<Reg64> v;
                for (size_t i = 0, n = get_free_gps().size(); i < n; ++i)
                    v.push_back(alloc<Reg64>());
                Reg64 r_pres = alloc<Reg64>();  // promoted from preserved pool

                // emit_prologue pushes r_pres -> managed_push_count_ becomes 1.
                emit_prologue();

                // Release volatile registers before emit_call.  Their values are
                // not needed after the call; rax will hold the return value.
                for (auto &r : v) RegPoolManager::free(r);

                // total_pushes = 1 (odd) -> no alignment pad on SysV,
                // 32-byte shadow space only on Win64.
                emit_call(&call_function_that_clobbers_registers, 0);
                // rax = 210 on return.

                RegPoolManager::free(r_pres);
                emit_epilogue();
                ret();
            }
        };

        ManagedCallKernel k;
        k.build();
        CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)210);
    }

    // Near-call path with execution verification:
    // A tiny JIT-compiled callee is used instead of a C library function.
    // Both the callee and the AutoGrowKernel caller are allocated by
    // CodeGenerator (via the same OS allocator), so they occupy the same
    // mmap / VirtualAlloc region.  The rel32 displacement is always
    // within int32 range on any OS, making this test machine-independent.
    //
    // Note: any C function (in the test binary or in the C runtime) maybe
    // too far from the JIT buffer to use as a near-call target.
    // A JIT callee avoids that constraint entirely.
    //
    // The callee uses "mov eax, 210; ret" which is ABI-neutral: both
    // Win64 and SysV AMD64 return integer values in rax, and the stub
    // takes no arguments and touches no preserved registers.
    {
        // Callee: standalone JIT stub that returns 210.
        struct SimpleCallee : CodeGenerator {
            SimpleCallee() : CodeGenerator(256) {
                mov(eax, 210);
                ret();
            }
        };

        struct AutoGrowKernel : CodeGenerator, RegPoolManager {
            AutoGrowKernel()
                : CodeGenerator(4096, Xbyak::AutoGrow),
                  RegPoolManager(g_cpu, this) {}
            void build(uint64_t callee_addr) {
                emit_prologue();
                // No scratch argument -- auto-grow mode always takes the
                // near-call path (call rel32).  Does not consume any register.
                emit_call(callee_addr);
                emit_epilogue();
                ret();
                calcJmpAddress();  // patch call rel32 displacement
            }
        };

        SimpleCallee callee;
        AutoGrowKernel k;
        CYBOZU_TEST_NO_EXCEPTION(
            k.build(reinterpret_cast<uint64_t>(callee.getCode())));
        // AUTO_GROW buffers are mmap'd RW only (no exec).  Make executable
        // before calling into the generated code.
        k.setProtectModeRE();
        CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)210);
    }

    // NOTE: the far-call path (target > 2 GB from JIT buffer) is not tested
    // here.  Whether that condition holds depends on where the OS places the
    // JIT buffer relative to the C runtime, which cannot be relied on in a
    // portable test.  The path is covered by code inspection.
}
















// =============================================================================
// Test – reset() clears all allocation state and restores the free pool
// =============================================================================
CYBOZU_TEST_AUTO(resetClearsAllocation)
{
    RegPoolManager rm(g_cpu);

    // Alloc several registers, mark one unavailable, then reset.
    auto r0 = rm.alloc<Reg64>();
    auto r1 = rm.alloc<Reg64>();
    rm.mark_unavailable<Reg64>(3); // rbx
    CYBOZU_TEST_ASSERT(!rm.all_free());

    const std::vector<int> free_before = rm.get_free_gps();

    rm.reset();

    // All registers freed; pool matches a freshly constructed manager.
    CYBOZU_TEST_ASSERT(rm.all_free());

    RegPoolManager fresh(g_cpu);
    CYBOZU_TEST_EQUAL(rm.get_free_gps().size(),      fresh.get_free_gps().size());
    CYBOZU_TEST_EQUAL(rm.get_preserved_gps().size(), fresh.get_preserved_gps().size());
    CYBOZU_TEST_EQUAL(rm.get_free_vecs().size(),     fresh.get_free_vecs().size());
    CYBOZU_TEST_EQUAL(rm.get_preserved_vecs().size(),fresh.get_preserved_vecs().size());

    // Previously reserved register is now allocatable again.
    CYBOZU_TEST_ASSERT(!rm.is_reserved<Reg64>(3));
    CYBOZU_TEST_NO_EXCEPTION(auto rbx = rm.alloc<Reg64>(3); rm.free(rbx);)

    // Previously in-use indices are gone from in_use.
    CYBOZU_TEST_ASSERT(!rm.reg_live(r0));
    CYBOZU_TEST_ASSERT(!rm.reg_live(r1));
}


// =============================================================================
// Test – reset() clears prologue/epilogue history
// =============================================================================
CYBOZU_TEST_AUTO(resetClearsPrologueHistory)
{
    RegPoolManager rm(g_cpu);

    // Promote a callee-saved register.
    auto rbx = rm.alloc<Reg64>(3);
    CYBOZU_TEST_EQUAL(rm.get_allocated_preserved_gps().size(), (size_t)1);

    rm.reset();

    // History wiped; no preserved registers tracked.
    CYBOZU_TEST_ASSERT(rm.get_allocated_preserved_gps().empty());
    // reset() implicitly returns all registers to their pools.  Calling free()
    // on a register that was in-use before reset() is an error — it is no
    // longer in in_use after the reset.
    CYBOZU_TEST_EXCEPTION(rm.free(rbx), Xbyak::RegManagerError);
    // After reset, index 3 (rbx) is back in the preserved pool (not the free pool).
    const auto preserved = rm.get_preserved_gps();
    const auto free_gps  = rm.get_free_gps();
    CYBOZU_TEST_ASSERT(std::find(preserved.begin(), preserved.end(), 3) != preserved.end());
    CYBOZU_TEST_ASSERT(std::find(free_gps.begin(),  free_gps.end(),  3) == free_gps.end());
    CYBOZU_TEST_NO_EXCEPTION(auto r = rm.alloc<Reg64>(3); rm.free(r);)
}

// =============================================================================
// Test – reset() preserves the CodeGenerator pointer and ISA flags
// =============================================================================
CYBOZU_TEST_AUTO(resetPreservesConfig)
{
    struct ConfigKernel : public CodeGenerator, public RegPoolManager {
        ConfigKernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
    };
    ConfigKernel k;

    const bool apx_before  = k.has_apx();
    const bool avx_before  = k.has_avx512();
    const bool amx_before  = k.has_amx();
    const int  max_gp      = k.max_gp_registers();

    static_cast<RegPoolManager &>(k).reset();

    CYBOZU_TEST_ASSERT(k.has_code_generator()); // cg_ preserved
    CYBOZU_TEST_EQUAL(k.has_apx(),          apx_before);
    CYBOZU_TEST_EQUAL(k.has_avx512(),       avx_before);
    CYBOZU_TEST_EQUAL(k.has_amx(),          amx_before);
    CYBOZU_TEST_EQUAL(k.max_gp_registers(), max_gp);
}

// =============================================================================
// Test – re-emit pattern: two kernels emitted sequentially into the same object
// =============================================================================
CYBOZU_TEST_AUTO(resetReEmit)
{
    struct KernelFamily : public CodeGenerator, public RegPoolManager {
        KernelFamily() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}

        void emit_a() {
            CodeGenerator::reset();
            RegPoolManager::reset();
            // Kernel A: return 42
            auto r = alloc<Reg64>();
            mov(r, (uint64_t)42);
            mov(rax, r);
            free(r);
            ret();
        }

        void emit_b() {
            CodeGenerator::reset();
            RegPoolManager::reset();
            // Kernel B: return 99
            auto r = alloc<Reg64>();
            mov(r, (uint64_t)99);
            mov(rax, r);
            free(r);
            ret();
        }
    };

    KernelFamily kf;
    kf.emit_a();
    CYBOZU_TEST_EQUAL(call_jit(kf.getCode()), (uint64_t)42);

    kf.emit_b();
    CYBOZU_TEST_EQUAL(call_jit(kf.getCode()), (uint64_t)99);

    // Can emit A again after B.
    kf.emit_a();
    CYBOZU_TEST_EQUAL(call_jit(kf.getCode()), (uint64_t)42);
}



















// =============================================================================
// Test – StackFrameBuilder: make_stack_frame() throws without a CodeGenerator
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutNoCg)
{
    RegPoolManager rm(g_cpu);  // no CG
    CYBOZU_TEST_EXCEPTION(rm.make_stack_frame().build(), Xbyak::RegManagerError);
}

// =============================================================================
// Test – StackFrameBuilder: negative / misaligned builder arguments are rejected
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutNegativeArgs)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build_negative_gp()      { make_stack_frame().gp_parks(-1).build(); }
        void build_negative_vec()     { make_stack_frame().vec_parks(-1).build(); }
        void build_negative_scratch() { make_stack_frame().scratch(-8).build(); }
        void build_empty()            { make_stack_frame().build(); }
    };

    Kernel k;
    CYBOZU_TEST_EXCEPTION(k.build_negative_gp(),       Xbyak::RegManagerError);
    CYBOZU_TEST_EXCEPTION(k.build_negative_vec(),      Xbyak::RegManagerError);
    CYBOZU_TEST_EXCEPTION(k.build_negative_scratch(),  Xbyak::RegManagerError);
    CYBOZU_TEST_NO_EXCEPTION(k.build_empty());
}

// =============================================================================
// Test – StackFrameBuilder: gp_parks: park/reload round-trip via JIT execution
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutGpParkReload)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            // Allocate all three pointer arguments from the ABI.
            auto reg_a = alloc<Reg64>(0);  // rax — return val
            (void)reg_a; // silence unused warning
            auto reg_b = alloc<Reg64>(7);  // rdi — first arg (SysV)

            emit_prologue();

            auto sf = make_stack_frame()
                .gp_parks(2)
                .build();

            // Explicitly zero reg_b so the round-trip value is 0 on all
            // platforms (RDI is callee-saved on Windows and may not be 0).
            xor_(reg_b, reg_b);
            sf.park(reg_b, 0);

            // Now reload it into a fresh register and return it
            auto r = sf.reload<Reg64>(0);
            mov(rax, r);
            free(r);

            sf.destroy();
            emit_epilogue();
            ret();
        }
    };

    Kernel k;
    k.build();
    // The JIT function ignores all args; it parks rdi (=0 at call time via
    // call_jit), then reloads and returns it.  We just verify no crash and
    // the value round-trips.
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)0);
}

// =============================================================================
// Test – StackFrameBuilder: scratch_addr returns a valid address; write/read via JIT
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutScratch)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            emit_prologue();

            auto sf = make_stack_frame()
                .scratch(16)
                .build();

            auto tmp = alloc<Reg64>();

            // Store 0xDEAD into scratch slot 0 and read it back
            mov(tmp, 0xDEADUL);
            mov(sf.scratch_addr(0), tmp);    // [rsp + scratch_base + 0] = 0xDEAD
            mov(rax, sf.scratch_addr(0));    // rax = [rsp + scratch_base + 0]

            free(tmp);
            sf.destroy();
            emit_epilogue();
            ret();
        }
    };

    Kernel k;
    k.build();
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)0xDEAD);
}

// =============================================================================
// Test – StackFrameBuilder: scratch_addr offset out of bounds throws
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutScratchOob)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            emit_prologue();
            auto sf = make_stack_frame().scratch(8).build();
            CYBOZU_TEST_EXCEPTION(sf.scratch_addr(8), Xbyak::RegManagerError);   // == size → OOB
            CYBOZU_TEST_EXCEPTION(sf.scratch_addr(-1), Xbyak::RegManagerError);
            sf.destroy();
            emit_epilogue();
            ret();
        }
    };
    Kernel k;
    k.build();
}

// =============================================================================
// Test – StackFrameBuilder: slot index out of bounds throws
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutSlotOob)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            emit_prologue();
            auto sf = make_stack_frame().gp_parks(1).build();
            auto r = alloc<Reg64>();
            CYBOZU_TEST_EXCEPTION(sf.park(static_cast<const Reg64&>(r), 1), Xbyak::RegManagerError);  // OOB
            CYBOZU_TEST_EXCEPTION(sf.reload<Reg64>(1), Xbyak::RegManagerError);
            free(r);
            sf.destroy();
            emit_epilogue();
            ret();
        }
    };
    Kernel k;
    k.build();
}

// =============================================================================
// Test – StackFrameBuilder: save_volatiles throws if not declared
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutSaveNotDeclared)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            emit_prologue();
            auto sf = make_stack_frame().scratch(8).build();  // no with_volatile_save()
            CYBOZU_TEST_EXCEPTION(sf.save_volatiles(), Xbyak::RegManagerError);
            CYBOZU_TEST_EXCEPTION(sf.restore_volatiles(), Xbyak::RegManagerError);
            sf.destroy();
            emit_epilogue();
            ret();
        }
    };
    Kernel k;
    k.build();
}

// =============================================================================
// Test – StackFrameBuilder: restore_volatiles without preceding save throws
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutRestoreWithoutSave)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            emit_prologue();
            auto sf = make_stack_frame()
                .with_volatile_save()
                .build();
            CYBOZU_TEST_EXCEPTION(sf.restore_volatiles(), Xbyak::RegManagerError);
            sf.destroy();
            emit_epilogue();
            ret();
        }
    };
    Kernel k;
    k.build();
}

// =============================================================================
// Test – StackFrameBuilder: with_volatile_save() works for registers allocated after build()
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutVolatileSaveOrderIndependent)
{
    // Declaring with_volatile_save() reserves slots for all ABI-volatile registers
    // at frame sizing time.  save_volatiles() then saves whichever of those are
    // live when called — even registers that were allocated after build().
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            emit_prologue();
            // Declare volatile save BEFORE any volatile register is allocated.
            auto sf = make_stack_frame()
                .with_volatile_save()
                .build();   // no volatile regs live yet — slots pre-reserved for all

            // Allocate a volatile GP AFTER build().  Under the old (live-at-build)
            // design no slot would exist for this register.
            auto r = alloc<Reg64>(7);   // rdi — volatile on SysV and Windows

            // save_volatiles() must save rdi even though it was not live at build().
            CYBOZU_TEST_NO_EXCEPTION(sf.save_volatiles());
            CYBOZU_TEST_NO_EXCEPTION(emit_call(&call_function_that_clobbers_registers, 0));
            CYBOZU_TEST_NO_EXCEPTION(sf.restore_volatiles());

            free(r);
            sf.destroy();
            emit_epilogue();
            ret();
        }
    };
    Kernel k;
    CYBOZU_TEST_NO_EXCEPTION(k.build());
}

// =============================================================================
// Test – StackFrameBuilder: destroy() is idempotent (double-destroy is a no-op)
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutDoubleDestroy)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            emit_prologue();
            auto sf = make_stack_frame().scratch(8).build();
            sf.destroy();                   // first destroy — emits add rsp
            CYBOZU_TEST_NO_EXCEPTION(sf.destroy());  // second — no-op, no throw
            emit_epilogue();
            ret();
        }
    };
    Kernel k;
    k.build();
}

// =============================================================================
// Test – StackFrameBuilder: managed_push_count_ updated so emit_call stays aligned
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutEmitCallAlignment)
{
    // Build a kernel with an odd number of prologue pushes, then open a layout.
    // verify emit_call does not crash (alignment logic must account for the layout).
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            auto rbx_r = alloc<Reg64>(3);   // forces one callee-save push
            emit_prologue();                 // push rbx  → managed_push_count_ = 1

            auto sf = make_stack_frame().scratch(8).build();
            // scratch(8) → total = 16 → managed_push_count_ += 2  (total = 3 now, odd)
            // emit_call must add 8 bytes padding to align to 16.

            // Just verify it emits without throwing.
            CYBOZU_TEST_NO_EXCEPTION(emit_call(&call_function_that_clobbers_registers, 0));

            sf.destroy();
            free(rbx_r);
            emit_epilogue();
            ret();
        }
    };
    Kernel k;
    CYBOZU_TEST_NO_EXCEPTION(k.build());
}

// =============================================================================
// Test – StackFrameBuilder: total always a multiple of 16
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutTotalAlignment)
{
    struct Probe : CodeGenerator, RegPoolManager {
        ptrdiff_t recorded_total = 0;
        Probe() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build(int gp_n, int scratch_n) {
            emit_prologue();
            auto sf = make_stack_frame()
                .gp_parks(gp_n)
                .scratch(scratch_n)
                .build();
            recorded_total = sf.total_size();
            sf.destroy();
            emit_epilogue();
            ret();
        }
    };

    {
        Probe p;
        p.build(1, 0);  // 8 bytes of GP slots → rounded up to 16
        CYBOZU_TEST_EQUAL(p.recorded_total % 16, 0);
    }
    {
        Probe p;
        p.build(3, 8);  // 3*8 + 8 = 32 → already aligned
        CYBOZU_TEST_EQUAL(p.recorded_total % 16, 0);
    }
    {
        Probe p;
        p.build(0, 24);  // 24 bytes scratch → already aligned
        CYBOZU_TEST_EQUAL(p.recorded_total % 16, 0);
    }
}

// =============================================================================
// Test – StackFrameBuilder: assert_clean_stack() passes after destroy()
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutCleanStack)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            emit_prologue();
            auto sf = make_stack_frame().scratch(16).build();
            CYBOZU_TEST_ASSERT(!clean_stack());   // frame is open
            sf.destroy();
            CYBOZU_TEST_ASSERT(clean_stack());    // frame closed
            emit_epilogue();
            ret();
        }
    };
    Kernel k;
    k.build();
}

// =============================================================================
// Test – StackFrameBuilder: reset() clears layout_active_ flag
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutReset)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            emit_prologue();
            auto sf = make_stack_frame().scratch(8).build();
            sf.destroy();
            emit_epilogue();
            ret();
        }
    };
    Kernel k;
    k.build();
    k.CodeGenerator::reset();
    k.RegPoolManager::reset();
    // After reset, building a second kernel must not throw ERR_RM_LAYOUT_ALREADY_ACTIVE
    CYBOZU_TEST_NO_EXCEPTION(k.build());
}

// =============================================================================
// Test – StackFrameBuilder: park (non-freeing const overload) does not free register
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutParkConstNoFree)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            auto r = alloc<Reg64>(8);   // r8
            emit_prologue();
            auto sf = make_stack_frame().gp_parks(1).build();

            const Reg64 &cr = r;
            sf.park(cr, 0);             // const overload — should NOT free r

            CYBOZU_TEST_ASSERT(reg_live(r));   // r still allocated

            free(r);
            sf.destroy();
            emit_epilogue();
            ret();
        }
    };
    Kernel k;
    k.build();
}

// =============================================================================
// Test – StackFrameBuilder: end-to-end value round-trip using GP park/reload in JIT
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutEndToEnd)
{
    // JIT function: parks rdi (=42 passed by caller), reloads it, returns it.
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            // On SysV rdi (reg 7) is the first arg; on Windows x64 it is rcx (reg 1).
#ifdef _WIN32
            auto arg = alloc<Reg64>(1);   // rcx = first arg on Windows x64
#else
            auto arg = alloc<Reg64>(7);   // rdi = first arg on SysV
#endif
            emit_prologue();

            auto sf = make_stack_frame().gp_parks(1).build();
            sf.park(arg, 0);             // mov [rsp+base], rdi; free arg

            auto res = sf.reload<Reg64>(0);  // mov res, [rsp+base]; alloc res
            mov(rax, res);
            free(res);

            sf.destroy();
            emit_epilogue();
            ret();
        }
    };

    // Generate the kernel and call it via a typed function pointer.
    Kernel k;
    k.build();
    typedef uint64_t(*fn_t)(uint64_t);
    fn_t fn = (fn_t)k.getCode();
    CYBOZU_TEST_EQUAL(fn(42), (uint64_t)42);
    CYBOZU_TEST_EQUAL(fn(7),  (uint64_t)7);
}


// 4-arg sum: all args fit in registers on both ABIs.
// SysV:  a=rdi, b=rsi, c=rdx, d=rcx
// Win64: a=rcx, b=rdx, c=r8,  d=r9
extern "C" uint64_t spill_test_sum4(
        uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    volatile uint64_t va = a, vb = b, vc = c, vd = d;
    return va + vb + vc + vd;
}

// Indirect sum: a + b + sum(extra[0..count-1]).
// Pointer args let the caller pass "extra" values that live in the scratch
// area rather than requiring additional ABI stack slots.
// SysV:  a=rdi, b=rsi, extra=rdx, count=rcx
// Win64: a=rcx, b=rdx, extra=r8,  count=r9
extern "C" uint64_t spill_test_sum_indirect(
        uint64_t a, uint64_t b, const uint64_t *extra, uint64_t count) {
    uint64_t s = a + b;
    for (uint64_t i = 0; i < count; ++i) s += extra[i];
    return s;
}

// 8-arg sum: exceeds the GP register limit on both ABIs.
// SysV:  a-f in registers (rdi,rsi,rdx,rcx,r8,r9); g,h on stack ([rsp+0],[rsp+8])
// Win64: a-d in registers (rcx,rdx,r8,r9);  e-h on stack ([rsp+32..56], above shadow)
extern "C" uint64_t spill_test_sum8(
        uint64_t a, uint64_t b, uint64_t c, uint64_t d,
        uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    volatile uint64_t va=a, vb=b, vc=c, vd=d, ve=e, vf=f, vg=g, vh=h;
    return va + vb + vc + vd + ve + vf + vg + vh;
}

// =============================================================================
// Test – StackFrame as replacement for spill() / restore()
//
//  Three scenarios that together cover every use-case spill/restore addressed:
//
//  Scenario 1 – Register pressure relief:
//    The kernel parks an in-use register to free its hardware slot for other
//    work, then reloads the original value.  This is the direct replacement for
//    the old single-register spill(reg)/restore(reg) pattern.
//
//  Scenario 2 – Live-value preservation across a C call:
//    Two GP registers holding important values are parked before a call that
//    would otherwise clobber them (the old "spill all live regs, call, restore"
//    pattern).  park() frees the hardware registers for argument loading;
//    emit_call() handles alignment and shadow space; reload() brings the values
//    back.
//
//    Platform differences (Win64 vs SysV argument registers) are isolated to
//    the argument-loading block; all other code is platform-independent.
//
//  Scenario 3 – "Extra arguments beyond register count" via scratch + pointer:
//    The old compiler model: push extra values onto the stack before the call,
//    pop them after.  The StackFrame model: store the extra values in a
//    pre-declared scratch area (fixed offset, no rsp movement), then pass the
//    scratch address as a normal register argument.  The callee receives a
//    pointer and reads the extras from there.
//
//    This avoids the push/pop model entirely: rsp is stable while the layout
//    is open, so scratch offsets never drift and no manual alignment arithmetic
//    is required.
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutSpillEquivalent)
{
    // ------------------------------------------------------------------
    // Scenario 1 – park() / reload() as register pressure relief.
    //
    // Three live values (111, 222, 333).  Park 333 to free its hardware
    // register, occupy that freed slot with unrelated work, then reload.
    // Expected result: 111 + 222 + 333 = 666.
    // ------------------------------------------------------------------
    {
        struct Kernel : CodeGenerator, RegPoolManager {
            Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
            void build() {
                auto r0 = alloc<Reg64>(); // will hold 111
                auto r1 = alloc<Reg64>(); // will hold 222
                auto r2 = alloc<Reg64>(); // will hold 333 — to be parked/reloaded

                emit_prologue();

                // One GP park slot for r2.
                auto sf = make_stack_frame().gp_parks(1).build();

                mov(r0, 111); mov(r1, 222); mov(r2, 333);

                // park(r2, 0): emits  mov [rsp+slot], r2
                //              frees  r2's hardware register back to the pool.
                sf.park(r2, 0);

                // The freed slot is now available.  Use it for something else.
                auto r_tmp = alloc<Reg64>();
                mov(r_tmp, 0xDEADUL); // arbitrary work using the reclaimed register
                free(r_tmp);

                // reload<Reg64>(0): allocs a register, emits  mov reg, [rsp+slot]
                //                   r2 now refers to the reloaded register.
                r2 = sf.reload<Reg64>(0);

                mov(rax, r0); add(rax, r1); add(rax, r2);
                free(r0); free(r1); free(r2);

                sf.destroy();
                emit_epilogue();
                ret();
            }
        };

        Kernel k; k.build();
        CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)(111 + 222 + 333));
    }

    // ------------------------------------------------------------------
    // Scenario 2 – Preserve live values across a C call.
    //
    // Two live GP registers (100 and 200) must survive a call to
    // spill_test_sum4(10, 20, 30, 40) = 100 that clobbers all volatile regs.
    // After the call the parked values are reloaded and added to rax.
    //
    // Expected result: 10+20+30+40 + 100 + 200 = 400.
    // ------------------------------------------------------------------
    {
        struct Kernel : CodeGenerator, RegPoolManager {
            Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
            void build() {
                auto r_ka = alloc<Reg64>(); // preserved value 100
                auto r_kb = alloc<Reg64>(); // preserved value 200

                emit_prologue();

                // Two GP park slots: one for r_ka, one for r_kb.
                // No with_volatile_save() needed — we handle preservation manually
                // via explicit park/reload.
                auto sf = make_stack_frame().gp_parks(2).build();

                mov(r_ka, 100); mov(r_kb, 200);

                // Park both live values.  Their hardware registers are freed and
                // become available for loading call arguments.
                sf.park(r_ka, 0);
                sf.park(r_kb, 1);

                // Load call arguments into the now-free hardware registers.
                // Argument registers differ between Win64 and SysV.
#ifdef _WIN32
                mov(rcx, 10); mov(rdx, 20); mov(r8, 30); mov(r9, 40);
#else
                mov(rdi, 10); mov(rsi, 20); mov(rdx, 30); mov(rcx, 40);
#endif
                emit_call(&spill_test_sum4, 0); // rax = 10+20+30+40 = 100

                // Reload the preserved values.
                r_ka = sf.reload<Reg64>(0);
                r_kb = sf.reload<Reg64>(1);

                add(rax, r_ka); add(rax, r_kb);
                free(r_ka); free(r_kb);

                sf.destroy();
                emit_epilogue();
                ret();
            }
        };

        Kernel k; k.build();
        CYBOZU_TEST_EQUAL(call_jit(k.getCode()),
                          (uint64_t)(10 + 20 + 30 + 40 + 100 + 200));
    }

    // ------------------------------------------------------------------
    // Scenario 3 – Extra values beyond register count, via scratch + pointer.
    //
    // Old model (spill/restore):
    //   spill(r_x0);  spill(r_x1);   // push; push  ← moves rsp
    //   call func(a, b, [stack]);     // callee reads extras from [rsp+8]
    //   restore(r_x1); restore(r_x0); // pop; pop
    //
    // New model (StackFrame):
    //   mov [rsp+scratch+0], r_x0    // store into pre-declared scratch area
    //   mov [rsp+scratch+8], r_x1    // rsp never moves between build/destroy
    //   lea r_ptr, [rsp+scratch+0]   // form pointer to the scratch block
    //   call func(a, b, r_ptr, count) // callee receives pointer in a register
    //
    // spill_test_sum_indirect(100, 200, &scratch[0], 2)
    //   scratch[0] = 300, scratch[1] = 400
    //   return = 100 + 200 + 300 + 400 = 1000
    //
    // Arg registers per ABI (all 4 fit in registers — no ABI stack slots needed):
    //   SysV:  a=rdi, b=rsi, extra_ptr=rdx, count=rcx
    //   Win64: a=rcx, b=rdx, extra_ptr=r8,  count=r9
    //
    // Extra-pointer argument is moved into its destination register BEFORE any
    // other argument register is written, to avoid clobbering the pointer in
    // the case where alloc() assigned r_ptr to that same hardware register.
    // ------------------------------------------------------------------
    {
        struct Kernel : CodeGenerator, RegPoolManager {
            Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
            void build() {
                // Two "extra" register values that exceed the hypothetical
                // register-arg capacity of the callee.
                auto r_x0 = alloc<Reg64>(); // will hold 300
                auto r_x1 = alloc<Reg64>(); // will hold 400

                emit_prologue();

                // Reserve 2 × 8 bytes of scratch for the extra values.
                // This replaces the two push instructions of the old model.
                auto sf = make_stack_frame().scratch(2 * 8).build();

                mov(r_x0, 300); mov(r_x1, 400);

                // Store extras into scratch (no rsp movement).
                mov(sf.scratch_addr(0), r_x0); // scratch[0] = 300
                mov(sf.scratch_addr(8), r_x1); // scratch[1] = 400
                free(r_x0); free(r_x1);

                // Form a pointer to the scratch block.
                auto r_ptr = alloc<Reg64>();
                lea(r_ptr, sf.scratch_addr(0)); // r_ptr = &scratch[0]

                // Move the pointer into its ABI arg register first, before any
                // other argument is written.  This prevents clobbering r_ptr if
                // alloc() assigned it to one of the other arg registers.
#ifdef _WIN32
                mov(r8,  r_ptr); // extra_ptr (arg3)
                free(r_ptr);
                mov(rcx, 100);   // a (arg1)
                mov(rdx, 200);   // b (arg2)
                mov(r9,  2);     // count (arg4)
#else
                mov(rdx, r_ptr); // extra_ptr (arg3)
                free(r_ptr);
                mov(rdi, 100);   // a (arg1)
                mov(rsi, 200);   // b (arg2)
                mov(rcx, 2);     // count (arg4)
#endif
                // rax = 100 + 200 + 300 + 400 = 1000
                emit_call(&spill_test_sum_indirect, 0);

                sf.destroy();
                emit_epilogue();
                ret();
            }
        };

        Kernel k; k.build();
        CYBOZU_TEST_EQUAL(call_jit(k.getCode()),
                          (uint64_t)(100 + 200 + 300 + 400));
    }
}

// =============================================================================
// Test – with_outgoing_args() / StackFrame::emit_call() for stack-overflow arguments
//
// Demonstrates the correct way to call a function whose argument count exceeds
// the ABI register limit:
//   SysV:  6 GP register args (rdi,rsi,rdx,rcx,r8,r9); args 7+ go on the stack.
//   Win64: 4 GP register args (rcx,rdx,r8,r9);          args 5+ go on the stack.
//
// Why RegPoolManager::emit_call() cannot be used here:
//   emit_call() emits "sub rsp, adj; call; add rsp, adj" for alignment.  Any
//   stack args written to [rsp+X] before this sub would be at [rsp+X+adj] at
//   call time — the wrong offsets.
//
// Why with_outgoing_args() + StackFrame::emit_call() work:
//   with_outgoing_args(n) reserves the overflow-arg slots at [rsp+0] (SysV) or
//   [rsp+32] (Win64, above the shadow space that is also pre-reserved).
//   build() adjusts the frame total so that rsp is already 16-aligned at the
//   call instruction, so StackFrame::emit_call() can be a bare
//   "mov rax; call rax".
//
// Two sub-scenarios: P=0 (even pushes) and P=1 (odd push), covering both
// branches of the alignment logic in build_layout().
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutOutgoingStackArgs)
{
    // ------------------------------------------------------------------
    // Scenario A — no prologue push (P = 0, even).
    //   build() chooses total ≡ 8 (mod 16) to compensate for rsp being
    //   8-misaligned after an even number of pushes.
    //
    // Expected: spill_test_sum8(10,20,30,40,50,60,70,80) = 360.
    // ------------------------------------------------------------------
    {
        struct Kernel : CodeGenerator, RegPoolManager {
            Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
            void build() {
                // No preserved registers allocated — emit_prologue() is a no-op.
                // managed_push_count_ stays 0 (even) before build().
                emit_prologue();

                // Declare the overflow-arg slots.  The layout also pre-reserves
                // Win64 shadow space ([rsp+0..31]) automatically.
#ifdef _WIN32
                // Win64: args 5-8 overflow (e,f,g,h) → 4 slots
                auto sf = make_stack_frame().with_outgoing_args(4).build();
#else
                // SysV:  args 7-8 overflow (g,h)     → 2 slots
                auto sf = make_stack_frame().with_outgoing_args(2).build();
#endif
                auto r_tmp = alloc<Reg64>();

#ifdef _WIN32
                // outgoing_arg_addr(n) = [rsp + 32 + n*8]  (above shadow space).
                mov(r_tmp, 50); mov(sf.outgoing_arg_addr(0), r_tmp); // e
                mov(r_tmp, 60); mov(sf.outgoing_arg_addr(1), r_tmp); // f
                mov(r_tmp, 70); mov(sf.outgoing_arg_addr(2), r_tmp); // g
                mov(r_tmp, 80); mov(sf.outgoing_arg_addr(3), r_tmp); // h
                free(r_tmp);
                mov(rcx, 10); mov(rdx, 20); mov(r8, 30); mov(r9, 40);
#else
                // outgoing_arg_addr(n) = [rsp + n*8].
                mov(r_tmp, 70); mov(sf.outgoing_arg_addr(0), r_tmp); // g
                mov(r_tmp, 80); mov(sf.outgoing_arg_addr(1), r_tmp); // h
                free(r_tmp);
                mov(rdi, 10); mov(rsi, 20); mov(rdx, 30);
                mov(rcx, 40); mov(r8,  50); mov(r9,  60);
#endif
                // Bare call — no sub/add rsp.  rsp is already 16-aligned because
                // build() absorbed the required adjustment into the frame total.
                sf.emit_call(&spill_test_sum8);
                // rax = 10+20+30+40+50+60+70+80 = 360

                sf.destroy();
                emit_epilogue();
                ret();
            }
        };

        Kernel k;
        k.build();
        CYBOZU_TEST_EQUAL(call_jit(k.getCode()),
                          (uint64_t)(10+20+30+40+50+60+70+80));
    }

    // ------------------------------------------------------------------
    // Scenario B — one prologue push (P = 1, odd).
    //   build() uses standard ≡ 0 (mod 16) rounding — exercises the
    //   else-branch of the alignment logic in build_layout().
    //
    // Expected: spill_test_sum8(10,20,30,40,50,60,70,80) = 360.
    // ------------------------------------------------------------------
    {
        struct Kernel : CodeGenerator, RegPoolManager {
            Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
            void build() {
                // rbx (index 3) is callee-saved on all ABIs.  Allocating it
                // causes emit_prologue() to emit one push → managed_push_count_
                // becomes 1 (odd) before build().
                auto r_pres = alloc<Reg64>(3); // rbx
                emit_prologue();               // push rbx

#ifdef _WIN32
                auto sf = make_stack_frame().with_outgoing_args(4).build();
#else
                auto sf = make_stack_frame().with_outgoing_args(2).build();
#endif
                auto r_tmp = alloc<Reg64>();

#ifdef _WIN32
                mov(r_tmp, 50); mov(sf.outgoing_arg_addr(0), r_tmp); // e
                mov(r_tmp, 60); mov(sf.outgoing_arg_addr(1), r_tmp); // f
                mov(r_tmp, 70); mov(sf.outgoing_arg_addr(2), r_tmp); // g
                mov(r_tmp, 80); mov(sf.outgoing_arg_addr(3), r_tmp); // h
                free(r_tmp);
                mov(rcx, 10); mov(rdx, 20); mov(r8, 30); mov(r9, 40);
#else
                mov(r_tmp, 70); mov(sf.outgoing_arg_addr(0), r_tmp); // g
                mov(r_tmp, 80); mov(sf.outgoing_arg_addr(1), r_tmp); // h
                free(r_tmp);
                mov(rdi, 10); mov(rsi, 20); mov(rdx, 30);
                mov(rcx, 40); mov(r8,  50); mov(r9,  60);
#endif
                sf.emit_call(&spill_test_sum8);

                free(r_pres);
                sf.destroy();
                emit_epilogue(); // pop rbx
                ret();
            }
        };

        Kernel k;
        k.build();
        CYBOZU_TEST_EQUAL(call_jit(k.getCode()),
                          (uint64_t)(10+20+30+40+50+60+70+80));
    }

    // ------------------------------------------------------------------
    // Scenario C — outgoing_arg_addr() out-of-bounds throws.
    // ------------------------------------------------------------------
    {
        struct Kernel : CodeGenerator, RegPoolManager {
            Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
            void build() {
                emit_prologue();
                auto sf = make_stack_frame().with_outgoing_args(2).build();
                CYBOZU_TEST_EXCEPTION(sf.outgoing_arg_addr(2),  Xbyak::RegManagerError); // n == count
                CYBOZU_TEST_EXCEPTION(sf.outgoing_arg_addr(-1), Xbyak::RegManagerError);
                sf.destroy();
                emit_epilogue();
                ret();
            }
        };
        Kernel k;
        k.build();
    }
}

// =============================================================================
// Test – multiple calls in one with_outgoing_args layout, mixing call styles
//
// A single StackFrame built with with_outgoing_args() can serve both a
// register-only call and an overflow-arg call.  sf.emit_call() is used for
// both: the frame alignment guarantee from build() is correct for any call,
// not just ones that use the overflow slots.
//
// Sequence:
//   1. sf.emit_call(&call_function_that_clobbers_registers)
//        — all args in registers; overflow slots left untouched.
//        — returns 210, held in rbx (callee-saved) across the second call.
//   2. sf.emit_call(&spill_test_sum8(1,2,3,4,5,6,7,8))
//        — uses overflow slots for the args that spill past the register limit.
//        — returns 36.
//   3. add rax, rbx  → 246.
//
// Keeping the first result in a callee-saved register is the idiomatic approach:
// it survives any call automatically without needing a park slot.
// =============================================================================
CYBOZU_TEST_AUTO(stackLayoutMixedCalls)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            // rbx (index 3) is callee-saved on every x86-64 ABI.
            // emit_prologue() will push it; it survives sf.emit_call() calls.
            auto r_pres = alloc<Reg64>(3); // rbx
            emit_prologue();               // push rbx — managed_push_count_ = 1 (odd)

#ifdef _WIN32
            auto sf = make_stack_frame().with_outgoing_args(4).build();
#else
            auto sf = make_stack_frame().with_outgoing_args(2).build();
#endif

            // ---- Call 1: register-only ----
            // call_function_that_clobbers_registers() needs no stack args.
            // Overflow slots exist in the frame but are simply not written.
            sf.emit_call(&call_function_that_clobbers_registers); // rax = 210
            mov(r_pres, rax); // rbx = 210 — survives call 2 unchanged

            // ---- Call 2: overflow-arg call ----
            // spill_test_sum8(1,2,3,4,5,6,7,8) = 36.
            // Write stack-overflow args to outgoing_arg_addr(), populate
            // register args, then call.  Same sf.emit_call(), different path.
            auto r_tmp = alloc<Reg64>();
#ifdef _WIN32
            mov(r_tmp, 5); mov(sf.outgoing_arg_addr(0), r_tmp); // e
            mov(r_tmp, 6); mov(sf.outgoing_arg_addr(1), r_tmp); // f
            mov(r_tmp, 7); mov(sf.outgoing_arg_addr(2), r_tmp); // g
            mov(r_tmp, 8); mov(sf.outgoing_arg_addr(3), r_tmp); // h
            free(r_tmp);
            mov(rcx, 1); mov(rdx, 2); mov(r8, 3); mov(r9, 4);
#else
            mov(r_tmp, 7); mov(sf.outgoing_arg_addr(0), r_tmp); // g
            mov(r_tmp, 8); mov(sf.outgoing_arg_addr(1), r_tmp); // h
            free(r_tmp);
            mov(rdi, 1); mov(rsi, 2); mov(rdx, 3);
            mov(rcx, 4); mov(r8,  5); mov(r9,  6);
#endif
            sf.emit_call(&spill_test_sum8); // rax = 1+2+3+4+5+6+7+8 = 36

            add(rax, r_pres); // 36 + 210 = 246

            free(r_pres);
            sf.destroy();
            emit_epilogue(); // pop rbx
            ret();
        }
    };

    Kernel k;
    k.build();
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()),
                      (uint64_t)(210 + 1+2+3+4+5+6+7+8));
}

// =============================================================================
// ManagedAlias tests
// =============================================================================

// -----------------------------------------------------------------------------
// Test -- declare_alias overload properties: slot-backed vs no-slot
// -----------------------------------------------------------------------------
CYBOZU_TEST_AUTO(managedAliasPatterns)
{
    // Named alias on a specific register, always slot-backed; inactive until prime().
    {
        RegPoolManager rm(g_cpu);
        auto a = rm.declare_alias(Reg64(10));
        CYBOZU_TEST_ASSERT(a.has_stack_slot());
        CYBOZU_TEST_ASSERT(!a.is_active());
        // reg() must throw before prime().
        CYBOZU_TEST_EXCEPTION(a.reg(), Xbyak::RegManagerError);
        // r10 is not reserved -- declare_alias does not lock it from alloc().
        CYBOZU_TEST_ASSERT(!rm.is_reserved<Reg64>(10));
        // general alloc is free to return r10.
        auto r = rm.alloc<Reg64>(10);
        CYBOZU_TEST_EQUAL(r.getIdx(), 10);
        rm.free(r);
    }

    // Three-argument form: use_alt=true -> no-slot alias on alt register (lazy allocation).
    //                       use_alt=false -> slotted alias on primary register.
    // This is also how the APX-portable pattern works: declare_alias(rax, r22, has_apx()).
    {
        RegPoolManager rm(g_cpu);
        if (rm.has_apx()) {
            // On APX: r16 chosen, no stack slot, inactive until alloc().
            auto b = rm.declare_alias(Reg64(10), Reg64(16), true);
            CYBOZU_TEST_ASSERT(!b.has_stack_slot());
            CYBOZU_TEST_ASSERT(!b.is_active());  // lazy: not active until alloc()
            b.alloc();
            CYBOZU_TEST_ASSERT(b.is_active());
            CYBOZU_TEST_EQUAL(b.reg().getIdx(), 16);
            b.free();
        } else {
            // On non-APX: r16 is treated as no-slot regardless, but we use
            // use_alt=false to get the slotted primary path.
            auto b = rm.declare_alias(Reg64(10), Reg64(16), false);
            CYBOZU_TEST_ASSERT(b.has_stack_slot());
            CYBOZU_TEST_ASSERT(!b.is_active());
        }
    }

    // Conditional form: use_alt=true -> no-slot alias on alt register, inactive until alloc().
    {
        RegPoolManager rm(g_cpu);
        auto c = rm.declare_alias(Reg64(10), Reg64(11), true);
        CYBOZU_TEST_ASSERT(!c.has_stack_slot());
        CYBOZU_TEST_ASSERT(!c.is_active());  // lazy: not active until alloc()
        c.alloc();
        CYBOZU_TEST_ASSERT(c.is_active());
        CYBOZU_TEST_EQUAL(c.reg().getIdx(), 11);
        c.free();
    }

    // Conditional form: use_alt=false -> primary register, slot-backed, inactive.
    {
        RegPoolManager rm(g_cpu);
        auto c = rm.declare_alias(Reg64(10), Reg64(11), false);
        CYBOZU_TEST_ASSERT(c.has_stack_slot());
        CYBOZU_TEST_ASSERT(!c.is_active());
        c.alloc();
        CYBOZU_TEST_ASSERT(c.is_active());
        CYBOZU_TEST_EQUAL(c.reg().getIdx(), 10);
        c.free();
    }
}

// -----------------------------------------------------------------------------
// Test -- prime() allocates the register for a slot-backed alias
// -----------------------------------------------------------------------------
CYBOZU_TEST_AUTO(managedAliasPrime)
{
    // Named slot alias: prime() allocates the named register.
    {
        RegPoolManager rm(g_cpu);
        auto a = rm.declare_alias(Reg64(10));
        CYBOZU_TEST_ASSERT(!a.is_active());

        a.alloc();

        CYBOZU_TEST_ASSERT(a.is_active());
        CYBOZU_TEST_EQUAL(a.reg().getIdx(), 10);

        // r10 is now live; a second named alloc must throw.
        CYBOZU_TEST_EXCEPTION(rm.alloc<Reg64>(10), Xbyak::RegManagerError);

        a.free();
        CYBOZU_TEST_ASSERT(!a.is_active());

        // After release, r10 is back in the free pool.
        auto r = rm.alloc<Reg64>(10);
        CYBOZU_TEST_EQUAL(r.getIdx(), 10);
        rm.free(r);
    }

    // Named slot alias: prime() when register taken by another allocation throws.
    {
        RegPoolManager rm(g_cpu);
        auto a = rm.declare_alias(Reg64(10));
        auto r10 = rm.alloc<Reg64>(10);  // take r10 first
        CYBOZU_TEST_EXCEPTION(a.alloc(), Xbyak::RegManagerError);
        rm.free(r10);
    }

    // Anonymous slot alias: prime() picks any available GP.
    {
        RegPoolManager rm(g_cpu);
        auto a = rm.declare_alias<Reg64>();
        CYBOZU_TEST_ASSERT(!a.is_active());
        a.alloc();
        CYBOZU_TEST_ASSERT(a.is_active());
        // reg() returns a valid register (index in [0,15]).
        CYBOZU_TEST_ASSERT(a.reg().getIdx() < 16);
        a.free();
    }
}

// -----------------------------------------------------------------------------
// Test -- no-slot alias uses lazy allocation; alloc() throws on conflict
// -----------------------------------------------------------------------------
CYBOZU_TEST_AUTO(managedAliasNoSlotActive)
{
    RegPoolManager rm(g_cpu);
    auto a = rm.declare_alias(Reg64(10), Reg64(11), true);
    // No-slot aliases are inactive at declaration time.
    CYBOZU_TEST_ASSERT(!a.has_stack_slot());
    CYBOZU_TEST_ASSERT(!a.is_active());

    a.alloc();
    CYBOZU_TEST_ASSERT(a.is_active());
    CYBOZU_TEST_EQUAL(a.reg().getIdx(), 11);

    // alloc() when already active is a no-op.
    a.alloc();
    CYBOZU_TEST_ASSERT(a.is_active());
    CYBOZU_TEST_EQUAL(a.reg().getIdx(), 11);

    // r11 is now live; another alloc of r11 must throw.
    CYBOZU_TEST_EXCEPTION(rm.alloc<Reg64>(11), Xbyak::RegManagerError);

    a.free();
    CYBOZU_TEST_ASSERT(!a.is_active());
    // r11 back in pool.
    auto r = rm.alloc<Reg64>(11);
    CYBOZU_TEST_EQUAL(r.getIdx(), 11);
    rm.free(r);
}

// -----------------------------------------------------------------------------
// Test -- free() and re-prime() round-trip
// Note: This is not the expected usage pattern for ManagedAlias, but it is supported.
// -----------------------------------------------------------------------------
CYBOZU_TEST_AUTO(managedAliasFreeAndReprime)
{
    // Slot-backed: prime -> free -> prime again.
    {
        RegPoolManager rm(g_cpu);
        auto a = rm.declare_alias(Reg64(10));
        a.alloc();
        CYBOZU_TEST_ASSERT(a.is_active());

        a.free();
        CYBOZU_TEST_ASSERT(!a.is_active());
        // r10 is back in the free pool; other code can use it.
        auto r = rm.alloc<Reg64>(10);
        CYBOZU_TEST_EQUAL(r.getIdx(), 10);
        rm.free(r);

        // Re-prime.
        a.alloc();
        CYBOZU_TEST_ASSERT(a.is_active());
        CYBOZU_TEST_EQUAL(a.reg().getIdx(), 10);
        a.free();
    }

    // No-slot: alloc -> free -> alloc re-acquires the same register.
    {
        RegPoolManager rm(g_cpu);
        auto a = rm.declare_alias(Reg64(10), Reg64(11), true);
        CYBOZU_TEST_ASSERT(!a.is_active());  // inactive at declaration

        a.alloc();
        CYBOZU_TEST_ASSERT(a.is_active());

        a.free();
        CYBOZU_TEST_ASSERT(!a.is_active());

        a.alloc();
        CYBOZU_TEST_ASSERT(a.is_active());
        CYBOZU_TEST_EQUAL(a.reg().getIdx(), 11);
        a.free();
    }

    // free() on inactive alias is a no-op.
    {
        RegPoolManager rm(g_cpu);
        auto a = rm.declare_alias(Reg64(10));
        CYBOZU_TEST_ASSERT(!a.is_active());
        CYBOZU_TEST_NO_EXCEPTION(a.free();)
        CYBOZU_TEST_ASSERT(!a.is_active());
    }
}

// -----------------------------------------------------------------------------
// Test -- reset() clears pending_aliases_; register is usable again
// -----------------------------------------------------------------------------
CYBOZU_TEST_AUTO(managedAliasReset)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
    };

    Kernel k;
    // Declare a no-slot alias on r11; register is NOT live until alloc().
    auto a = k.declare_alias(Reg64(10), Reg64(11), true);
    CYBOZU_TEST_ASSERT(!a.has_stack_slot());
    CYBOZU_TEST_ASSERT(!a.is_active());
    // r11 is still free before alloc(); pool alloc must succeed.
    CYBOZU_TEST_NO_EXCEPTION(auto r = k.alloc<Reg64>(11); k.free(r);)
    // Now alloc the alias; r11 enters live_gp_.
    a.alloc();
    CYBOZU_TEST_ASSERT(a.is_active());
    // r11 is live; alloc(11) must throw.
    CYBOZU_TEST_EXCEPTION(k.alloc<Reg64>(11), Xbyak::RegManagerError);

    k.CodeGenerator::reset();
    k.RegPoolManager::reset();

    // After reset, r11 is free again.
    CYBOZU_TEST_NO_EXCEPTION(
        auto r11 = k.alloc<Reg64>(11);
        k.free(r11);
    )

    // pending_aliases_ cleared: build() succeeds without stale alias data.
    CYBOZU_TEST_NO_EXCEPTION(
        auto sf = k.make_stack_frame().build();
        sf.destroy();
    )
    (void)a;
}

// -----------------------------------------------------------------------------
// Test -- save/restore are no-ops when has_stack_slot() == false
// -----------------------------------------------------------------------------
CYBOZU_TEST_AUTO(managedAliasNoSlotNoop)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            auto a = declare_alias(Reg64(10), Reg64(11), true);
            CYBOZU_TEST_ASSERT(!a.has_stack_slot());
            CYBOZU_TEST_ASSERT(!a.is_active());  // inactive at declaration

            auto sf = make_stack_frame().build();

            // save/restore on an inactive no-slot alias emit no instructions.
            const size_t sz_before = getSize();
            a.save(sf);     // no-op: no slot assigned
            a.restore(sf);  // no-op: no slot assigned
            const size_t sz_after = getSize();

            CYBOZU_TEST_EQUAL(sz_before, sz_after);
            CYBOZU_TEST_ASSERT(!a.is_active());  // still inactive after no-ops

            a.alloc();
            CYBOZU_TEST_ASSERT(a.is_active());
            a.free();
            CYBOZU_TEST_ASSERT(!a.is_active());
            sf.destroy();
            ret();
        }
    };
    Kernel k;
    k.build();
}

// -----------------------------------------------------------------------------
// Test -- slot-backed save/restore round-trip via JIT execution
// -----------------------------------------------------------------------------
CYBOZU_TEST_AUTO(managedAliasSaveRestoreJIT)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            // r10 is volatile on SysV and Microsoft x64; no prologue/epilogue needed.
            auto a = declare_alias(Reg64(10));
            auto sf = make_stack_frame().build();

            a.alloc();                          // allocate r10
            mov(a.reg(), 0xABCD1234ULL);        // r10 = 0xABCD1234

            a.save(sf);                         // [rsp+<off>] = r10; free r10
            CYBOZU_TEST_ASSERT(!a.is_active());

            // r10 is free; use it temporarily with a different value.
            auto r_tmp = alloc<Reg64>(10);
            mov(r_tmp, 0xDEADBEEFULL);
            free(r_tmp);

            a.restore(sf);                      // allocate r10; load from slot
            CYBOZU_TEST_ASSERT(a.is_active());
            CYBOZU_TEST_EQUAL(a.reg().getIdx(), 10);

            mov(rax, a.reg());                  // rax = 0xABCD1234
            a.free();

            sf.destroy();
            ret();
        }
    };
    Kernel k;
    k.build();
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)0xABCD1234ULL);
}

// -----------------------------------------------------------------------------
// Test -- alias slot and GP park slot coexist in the same layout
// -----------------------------------------------------------------------------
CYBOZU_TEST_AUTO(managedAliasMixedWithParks)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            auto a = declare_alias(Reg64(10));
            auto sf = make_stack_frame().gp_parks(1).build();

            a.alloc();

            // Use r11 for the park slot.
            auto park_reg = alloc<Reg64>(11);

            mov(a.reg(),  0x1111111111111111ULL);
            mov(park_reg, 0x2222222222222222ULL);

            // Save both independently.
            a.save(sf);               // alias slot <- r10
            sf.park(park_reg, 0);     // gp park slot 0 <- r11; frees r11

            // Clobber r10 (currently free) and r11 with different values.
            auto r10_tmp = alloc<Reg64>(10);
            auto r11_tmp = alloc<Reg64>(11);
            mov(r10_tmp, 0ULL);
            mov(r11_tmp, 0ULL);
            free(r10_tmp);
            free(r11_tmp);

            // Restore both.
            a.restore(sf);                       // r10 = 0x1111...
            park_reg = sf.reload<Reg64>(0);      // r11 = 0x2222...

            // Return sum: must equal 0x1111... + 0x2222... = 0x3333...
            // Use park_reg as base so that if reload allocated rax, the
            // subsequent mov(rax, a.reg()) does not clobber it first.
            mov(rax, park_reg);
            add(rax, a.reg());

            a.free();
            free(park_reg);
            sf.destroy();
            ret();
        }
    };
    Kernel k;
    k.build();
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()),
                      (uint64_t)0x1111111111111111ULL + 0x2222222222222222ULL);
}

// -----------------------------------------------------------------------------
// Test -- anonymous alias: prime() picks any available GP
// -----------------------------------------------------------------------------
CYBOZU_TEST_AUTO(managedAliasAnonymous)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            auto a = declare_alias<Reg64>();
            auto sf = make_stack_frame().build();

            CYBOZU_TEST_ASSERT(!a.is_active());
            a.alloc();
            CYBOZU_TEST_ASSERT(a.is_active());

            // Write a known value, save to slot, clobber, restore, read back.
            const int idx = a.reg().getIdx();
            mov(a.reg(), 0xCAFEBABEULL);
            a.save(sf);

            // Use the same physical register with a different value.
            auto r_tmp = alloc<Reg64>(idx);
            mov(r_tmp, 0ULL);
            free(r_tmp);

            a.restore(sf);
            mov(rax, a.reg());
            a.free();

            sf.destroy();
            ret();
        }
    };
    Kernel k;
    k.build();
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)0xCAFEBABEULL);
}

// -----------------------------------------------------------------------------
// Test -- release() skips the store; slot value from last save() survives
// -----------------------------------------------------------------------------
CYBOZU_TEST_AUTO(managedAliasReleaseNoStore)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            auto a = declare_alias(Reg64(10));
            auto sf = make_stack_frame().build();

            a.alloc();
            mov(a.reg(), 0xBEEFCAFEULL);

            a.save(sf);                  // slot = 0xBEEFCAFE; r10 freed
            CYBOZU_TEST_ASSERT(!a.is_active());

            a.restore(sf);               // r10 = 0xBEEFCAFE
            CYBOZU_TEST_ASSERT(a.is_active());

            // Value not modified; release instead of save -- no store emitted.
            a.free();
            CYBOZU_TEST_ASSERT(!a.is_active());

            // Slot must still hold the value written by save().
            a.restore(sf);
            CYBOZU_TEST_ASSERT(a.is_active());
            CYBOZU_TEST_EQUAL(a.reg().getIdx(), 10);

            mov(rax, a.reg());           // rax = 0xBEEFCAFE
            a.free();

            sf.destroy();
            ret();
        }
    };
    Kernel k;
    k.build();
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)0xBEEFCAFEULL);
}

// ---------------------------------------------------------------------------
// AliasMode::no_slot explicit overload
// ---------------------------------------------------------------------------

// declare_alias(reg, AliasMode::no_slot): inactive at declaration, alloc() activates.
CYBOZU_TEST_AUTO(aliasDeclareNoSlotBasic)
{
    RegPoolManager rm(g_cpu);
    auto a = rm.declare_alias(Reg64(9), AliasMode::no_slot);
    CYBOZU_TEST_ASSERT(!a.has_stack_slot());
    CYBOZU_TEST_ASSERT(!a.is_active());

    a.alloc();
    CYBOZU_TEST_ASSERT(a.is_active());
    CYBOZU_TEST_EQUAL(a.reg().getIdx(), 9);

    a.free();
    CYBOZU_TEST_ASSERT(!a.is_active());

    // r9 is back in the pool after free().
    auto r = rm.alloc<Reg64>(9);
    CYBOZU_TEST_EQUAL(r.getIdx(), 9);
    rm.free(r);
}

// Two no-slot aliases targeting the same register: alloc() on the second
// throws GP_IN_USE while the first is active.
CYBOZU_TEST_AUTO(aliasDeclareNoSlotMutualExclusion)
{
    RegPoolManager rm(g_cpu);
    auto a = rm.declare_alias(Reg64(9), AliasMode::no_slot);
    auto b = rm.declare_alias(Reg64(9), AliasMode::no_slot);

    a.alloc();
    CYBOZU_TEST_ASSERT(a.is_active());

    // b cannot alloc while a holds r9.
    CYBOZU_TEST_EXCEPTION(b.alloc(), Xbyak::RegManagerError);

    a.free();
    // Now b can alloc.
    CYBOZU_TEST_NO_EXCEPTION(b.alloc();)
    CYBOZU_TEST_ASSERT(b.is_active());
    CYBOZU_TEST_EQUAL(b.reg().getIdx(), 9);
    b.free();
}

// Sequential use: alloc/free/alloc on alternate aliases sharing a register.
CYBOZU_TEST_AUTO(aliasDeclareNoSlotSequential)
{
    RegPoolManager rm(g_cpu);
    auto a = rm.declare_alias(Reg64(9), AliasMode::no_slot);
    auto b = rm.declare_alias(Reg64(9), AliasMode::no_slot);

    a.alloc();
    CYBOZU_TEST_EQUAL(a.reg().getIdx(), 9);
    a.free();

    b.alloc();
    CYBOZU_TEST_EQUAL(b.reg().getIdx(), 9);
    b.free();

    // Can cycle a again after b released r9.
    a.alloc();
    CYBOZU_TEST_EQUAL(a.reg().getIdx(), 9);
    a.free();
}

// declare_alias(primary, alt, use_alt) three-arg form with AliasMode semantics smoke test.
CYBOZU_TEST_AUTO(aliasDeclareNoSlotViaThreeArg)
{
    // use_alt=true path: no-slot alias on alt register.
    {
        RegPoolManager rm(g_cpu);
        auto a = rm.declare_alias(Reg64(8), Reg64(9), true);
        CYBOZU_TEST_ASSERT(!a.has_stack_slot());
        CYBOZU_TEST_ASSERT(!a.is_active());
        a.alloc();
        CYBOZU_TEST_EQUAL(a.reg().getIdx(), 9);
        a.free();
    }
    // use_alt=false path: slotted alias on primary register.
    {
        RegPoolManager rm(g_cpu);
        auto a = rm.declare_alias(Reg64(8), Reg64(9), false);
        CYBOZU_TEST_ASSERT(a.has_stack_slot());
        CYBOZU_TEST_ASSERT(!a.is_active());
        a.alloc();
        CYBOZU_TEST_EQUAL(a.reg().getIdx(), 8);
        a.free();
    }
}

// ---------------------------------------------------------------------------
// post-build declare_alias() guard
// ---------------------------------------------------------------------------

// All four declare_alias overloads must throw ALIAS_AFTER_BUILD when called
// after make_stack_frame().build() has been called on the same manager.
CYBOZU_TEST_AUTO(aliasDeclareAfterBuildNamed)
{
    struct DummyKernel : Xbyak::CodeGenerator, Xbyak::RegPoolManager {
        DummyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(g_cpu, this) {}
        void go() {
            auto sf = make_stack_frame().build();
            // declare_alias after build -- must throw
            CYBOZU_TEST_EXCEPTION(declare_alias(r10), Xbyak::RegManagerError);
        }
    };
    DummyKernel k;
    k.go();
}

CYBOZU_TEST_AUTO(aliasDeclareAfterBuildTwoArg)
{
    struct DummyKernel : Xbyak::CodeGenerator, Xbyak::RegPoolManager {
        DummyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(g_cpu, this) {}
        void go() {
            auto sf = make_stack_frame().build();
            CYBOZU_TEST_EXCEPTION(declare_alias(rax, r16, false), Xbyak::RegManagerError);
        }
    };
    DummyKernel k;
    k.go();
}

CYBOZU_TEST_AUTO(aliasDeclareAfterBuildThreeArg)
{
    struct DummyKernel : Xbyak::CodeGenerator, Xbyak::RegPoolManager {
        DummyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(g_cpu, this) {}
        void go() {
            auto sf = make_stack_frame().build();
            CYBOZU_TEST_EXCEPTION(declare_alias(rax, r16, false), Xbyak::RegManagerError);
        }
    };
    DummyKernel k;
    k.go();
}

CYBOZU_TEST_AUTO(aliasDeclareAfterBuildAnonymous)
{
    struct DummyKernel : Xbyak::CodeGenerator, Xbyak::RegPoolManager {
        DummyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(g_cpu, this) {}
        void go() {
            auto sf = make_stack_frame().build();
            CYBOZU_TEST_EXCEPTION(declare_alias<Xbyak::Reg64>(), Xbyak::RegManagerError);
        }
    };
    DummyKernel k;
    k.go();
}

// Declaring before build() is fine; the guard must not fire.
CYBOZU_TEST_AUTO(aliasDeclareBeforeBuildOk)
{
    struct DummyKernel : Xbyak::CodeGenerator, Xbyak::RegPoolManager {
        DummyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(g_cpu, this) {}
        void go() {
            CYBOZU_TEST_NO_EXCEPTION(declare_alias(r10));
            auto sf = make_stack_frame().build();
            sf.destroy();
        }
    };
    DummyKernel k;
    k.go();
}

// After reset(), build_done_ is cleared and declare_alias() works again.
CYBOZU_TEST_AUTO(aliasDeclareAfterBuildThenReset)
{
    struct DummyKernel : Xbyak::CodeGenerator, Xbyak::RegPoolManager {
        DummyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(g_cpu, this) {}
        void go() {
            auto sf = make_stack_frame().build();
            CYBOZU_TEST_EXCEPTION(declare_alias(r10), Xbyak::RegManagerError);
            sf.destroy();
            RegPoolManager::reset();
            // After reset, declare_alias must work again.
            CYBOZU_TEST_NO_EXCEPTION(declare_alias(r10));
        }
    };
    DummyKernel k;
    k.go();
}

// -----------------------------------------------------------------------------
// Tests -- ScopedAlias RAII wrapper
// -----------------------------------------------------------------------------

// Basic: scoped() activates the alias; register is freed when guard exits scope.
CYBOZU_TEST_AUTO(scopedAliasBasic)
{
    RegPoolManager rm(g_cpu);
    auto a = rm.declare_alias(Reg64(10));

    {
        auto g = a.scoped();
        CYBOZU_TEST_ASSERT(a.is_active());
        CYBOZU_TEST_EQUAL(a.reg().getIdx(), 10);
    }  // guard destroyed here

    // After the guard exits, the alias must be inactive and r10 back in pool.
    CYBOZU_TEST_ASSERT(!a.is_active());
    auto r = rm.alloc<Reg64>(10);
    CYBOZU_TEST_EQUAL(r.getIdx(), 10);
    rm.free(r);
}

// Explicit free(): disarms the guard; destructor must not double-free.
CYBOZU_TEST_AUTO(scopedAliasExplicitFree)
{
    RegPoolManager rm(g_cpu);
    auto a = rm.declare_alias(Reg64(10));

    {
        auto g = a.scoped();
        CYBOZU_TEST_ASSERT(a.is_active());

        g.free();  // explicit early release -- disarms destructor
        CYBOZU_TEST_ASSERT(!a.is_active());

        // r10 is now back in pool while guard is still in scope.
        auto r = rm.alloc<Reg64>(10);
        CYBOZU_TEST_EQUAL(r.getIdx(), 10);
        rm.free(r);
    }  // destructor runs; must not crash or double-free

    CYBOZU_TEST_ASSERT(!a.is_active());
}

// Move semantics: moved-from guard is disarmed; moved-to guard owns the release.
CYBOZU_TEST_AUTO(scopedAliasMove)
{
    RegPoolManager rm(g_cpu);
    auto a = rm.declare_alias(Reg64(10));

    RegPoolManager::ScopedAlias g2 = [&]() {
        auto g1 = a.scoped();
        CYBOZU_TEST_ASSERT(a.is_active());
        return g1;  // move-construct g2 from g1; g1 is disarmed
    }();
    // g1 is gone; alias must still be active (g2 owns it).
    CYBOZU_TEST_ASSERT(a.is_active());

    // g2 going out of scope releases the alias.
    g2.free();
    CYBOZU_TEST_ASSERT(!a.is_active());
}

// Works with AliasMode::no_slot aliases.
CYBOZU_TEST_AUTO(scopedAliasNoSlot)
{
    RegPoolManager rm(g_cpu);
    auto a = rm.declare_alias(Reg64(10), Reg64(11), true);
    CYBOZU_TEST_ASSERT(!a.is_active());

    {
        auto g = a.scoped();
        CYBOZU_TEST_ASSERT(a.is_active());
    }

    CYBOZU_TEST_ASSERT(!a.is_active());
    // r11 is free again; can allocate it explicitly.
    auto r = rm.alloc<Reg64>(11);
    CYBOZU_TEST_EQUAL(r.getIdx(), 11);
    rm.free(r);
}

// save() before guard exit: alias becomes inactive; destructor is a no-op.
CYBOZU_TEST_AUTO(scopedAliasSaveFirst)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            auto a = declare_alias(Reg64(10));
            auto sf = make_stack_frame().build();

            {
                auto g = a.scoped();
                CYBOZU_TEST_ASSERT(a.is_active());

                // save() spills to stack slot and releases the register.
                a.save(sf);
                CYBOZU_TEST_ASSERT(!a.is_active());

                // Disarm guard so its destructor does not try to free again.
                g.free();
            }  // destructor runs on already-inactive alias -- must be a no-op

            CYBOZU_TEST_ASSERT(!a.is_active());
            sf.destroy();
            ret();
        }
    };
    Kernel k;
    k.build();
}

// -----------------------------------------------------------------------------
// Tests -- ManagedVecAlias and ScopedVecAlias
// -----------------------------------------------------------------------------

// Basic lifecycle: declare, alloc, check idx, free.
CYBOZU_TEST_AUTO(managedVecAliasBasic)
{
    RegPoolManager rm(g_cpu);
    if (rm.get_free_vecs().empty() && !rm.has_avx512()) return;

    auto a = rm.declare_vec_alias(Zmm(5));
    CYBOZU_TEST_ASSERT(!a.is_active());
    CYBOZU_TEST_ASSERT(a.has_stack_slot());

    a.alloc();
    CYBOZU_TEST_ASSERT(a.is_active());
    CYBOZU_TEST_EQUAL(a.reg().getIdx(), 5);

    a.free();
    CYBOZU_TEST_ASSERT(!a.is_active());

    // zmm5 is back in pool.
    auto v = rm.alloc<Zmm>(5);
    CYBOZU_TEST_EQUAL(v.getIdx(), 5);
    rm.free(v);
}

// free() on inactive alias is idempotent (no crash).
CYBOZU_TEST_AUTO(managedVecAliasFreeIdempotent)
{
    RegPoolManager rm(g_cpu);
    if (rm.get_free_vecs().empty() && !rm.has_avx512()) return;

    auto a = rm.declare_vec_alias(Zmm(5));
    CYBOZU_TEST_ASSERT(!a.is_active());
    CYBOZU_TEST_NO_EXCEPTION(a.free();)
    CYBOZU_TEST_ASSERT(!a.is_active());
}

// alloc() when already active is a no-op (re-alloc idempotent).
CYBOZU_TEST_AUTO(managedVecAliasAllocIdempotent)
{
    RegPoolManager rm(g_cpu);
    if (rm.get_free_vecs().empty() && !rm.has_avx512()) return;

    auto a = rm.declare_vec_alias(Zmm(5));
    a.alloc();
    CYBOZU_TEST_ASSERT(a.is_active());
    CYBOZU_TEST_NO_EXCEPTION(a.alloc();)  // second alloc: no-op, no throw
    CYBOZU_TEST_ASSERT(a.is_active());
    CYBOZU_TEST_EQUAL(a.reg().getIdx(), 5);
    a.free();
}

// Declare after build() throws ALIAS_AFTER_BUILD.
CYBOZU_TEST_AUTO(managedVecAliasPostBuild)
{
    struct DummyKernel : Xbyak::CodeGenerator, Xbyak::RegPoolManager {
        DummyKernel() : Xbyak::CodeGenerator(4096), Xbyak::RegPoolManager(g_cpu, this) {}
        void go() {
            auto sf = make_stack_frame().build();
            CYBOZU_TEST_EXCEPTION(declare_vec_alias(zmm5), Xbyak::RegManagerError);
            sf.destroy();
            RegPoolManager::reset();
            CYBOZU_TEST_NO_EXCEPTION(declare_vec_alias(zmm5));
        }
    };
    DummyKernel k;
    k.go();
}

// Anonymous vec alias: alloc() picks any free vector register.
CYBOZU_TEST_AUTO(managedVecAliasAnonymous)
{
    RegPoolManager rm(g_cpu);
    if (rm.get_free_vecs().empty() && !rm.has_avx512()) return;

    auto a = rm.declare_vec_alias<Zmm>();
    CYBOZU_TEST_ASSERT(!a.is_active());
    CYBOZU_TEST_ASSERT(a.has_stack_slot());

    a.alloc();
    CYBOZU_TEST_ASSERT(a.is_active());
    CYBOZU_TEST_ASSERT(a.reg().getIdx() >= 0);
    a.free();
    CYBOZU_TEST_ASSERT(!a.is_active());
}

// reset() clears pending vec aliases; register is usable again.
CYBOZU_TEST_AUTO(managedVecAliasReset)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
    };
    Kernel k;
    if (k.get_free_vecs().empty() && !k.has_avx512()) return;

    auto a = k.declare_vec_alias(Zmm(5));
    CYBOZU_TEST_ASSERT(!a.is_active());
    CYBOZU_TEST_ASSERT(a.has_stack_slot());

    k.RegPoolManager::reset();

    // After reset, zmm5 is back in pool and declare is allowed again.
    CYBOZU_TEST_NO_EXCEPTION(k.declare_vec_alias(Zmm(5)));
}

// GP and vec aliases coexist in the same layout -- GP part is JIT-executed.
CYBOZU_TEST_AUTO(managedVecAliasMixedWithGP)
{
    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            // Declare both before build.
            auto gp_alias  = declare_alias(Reg64(10));
            auto vec_alias = declare_vec_alias(Zmm(5));
            auto sf        = make_stack_frame().build();

            gp_alias.alloc();
            mov(gp_alias.reg(), 0xABCD1234ULL);
            gp_alias.save(sf);           // spill GP to its alias slot

            if (has_avx512() || !get_free_vecs().empty()) {
                vec_alias.alloc();
                vec_alias.free();        // release without store (no-op path)
            }

            gp_alias.restore(sf);
            mov(rax, gp_alias.reg());
            gp_alias.free();
            sf.destroy();
            ret();
        }
    };
    Kernel k;
    k.build();
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)0xABCD1234ULL);
}

// JIT execution: write a 32-bit value to a vec alias, save, clobber, restore,
// read back.  Guarded on AVX availability.
CYBOZU_TEST_AUTO(managedVecAliasSaveRestoreJIT)
{
    if (!g_cpu.has(Xbyak::util::Cpu::tAVX)) return;

    struct Kernel : CodeGenerator, RegPoolManager {
        Kernel() : CodeGenerator(4096), RegPoolManager(g_cpu, this) {}
        void build() {
            auto a  = declare_vec_alias(Zmm(5));
            auto sf = make_stack_frame().build();

            a.alloc();

            // Write 0xABCD1234 into the low 32 bits of the vec register.
            mov(eax, 0xABCD1234U);
            vmovd(Xmm(a.reg().getIdx()), eax);

            a.save(sf);  // spill to vec alias slot
            CYBOZU_TEST_ASSERT(!a.is_active());

            // Clobber: zero the register so the slot is the only copy.
            if (has_avx512()) {
                vpxord(Zmm(5), Zmm(5), Zmm(5));
            } else {
                vpxor(Ymm(5), Ymm(5), Ymm(5));
            }

            a.restore(sf);
            CYBOZU_TEST_ASSERT(a.is_active());
            CYBOZU_TEST_EQUAL(a.reg().getIdx(), 5);

            // Read low 32 bits back into rax (32-bit write zero-extends to rax).
            vmovd(eax, Xmm(a.reg().getIdx()));

            a.free();
            sf.destroy();
            ret();
        }
    };
    Kernel k;
    k.build();
    CYBOZU_TEST_EQUAL(call_jit(k.getCode()), (uint64_t)0xABCD1234ULL);
}

// ScopedVecAlias: basic RAII -- freed on scope exit.
CYBOZU_TEST_AUTO(scopedVecAliasBasic)
{
    RegPoolManager rm(g_cpu);
    if (rm.get_free_vecs().empty() && !rm.has_avx512()) return;

    auto a = rm.declare_vec_alias(Zmm(5));

    {
        auto g = a.scoped();
        CYBOZU_TEST_ASSERT(a.is_active());
        CYBOZU_TEST_EQUAL(a.reg().getIdx(), 5);
    }

    CYBOZU_TEST_ASSERT(!a.is_active());
    auto v = rm.alloc<Zmm>(5);
    CYBOZU_TEST_EQUAL(v.getIdx(), 5);
    rm.free(v);
}

// ScopedVecAlias: explicit free() disarms the destructor.
CYBOZU_TEST_AUTO(scopedVecAliasExplicitFree)
{
    RegPoolManager rm(g_cpu);
    if (rm.get_free_vecs().empty() && !rm.has_avx512()) return;

    auto a = rm.declare_vec_alias(Zmm(5));

    {
        auto g = a.scoped();
        CYBOZU_TEST_ASSERT(a.is_active());

        g.free();  // explicit early release -- disarms destructor
        CYBOZU_TEST_ASSERT(!a.is_active());

        // zmm5 back in pool while guard still in scope.
        auto v = rm.alloc<Zmm>(5);
        CYBOZU_TEST_EQUAL(v.getIdx(), 5);
        rm.free(v);
    }  // destructor runs; must not double-free

    CYBOZU_TEST_ASSERT(!a.is_active());
}

// ScopedVecAlias: move semantics -- moved-from is disarmed.
CYBOZU_TEST_AUTO(scopedVecAliasMove)
{
    RegPoolManager rm(g_cpu);
    if (rm.get_free_vecs().empty() && !rm.has_avx512()) return;

    auto a = rm.declare_vec_alias(Zmm(5));

    RegPoolManager::ScopedVecAlias g2 = [&]() {
        auto g1 = a.scoped();
        CYBOZU_TEST_ASSERT(a.is_active());
        return g1;  // move-construct g2; g1 is disarmed
    }();
    CYBOZU_TEST_ASSERT(a.is_active());  // g2 still owns the alias

    g2.free();
    CYBOZU_TEST_ASSERT(!a.is_active());
}