// M1 executable-memory probe: emit, execute, rewrite, synchronise and execute
// again, across branches, yields, threads and whole-pool reuse.
//
// Log format: "RESULT <check> <PASS|FAIL|INFO> key=value ...". INFO lines are
// observations that are not pass/fail criteria (e.g. stale-code visibility).
#include "probe/jitprobe.h"
#include "runtime/armemit.h"
#include "runtime/jitmem.h"

#include <inttypes.h>
#include <stdint.h>

typedef uint32_t (*fn0_t)(void);
typedef uint32_t (*fn2_t)(uint32_t, uint32_t);

// Prevents the compiler from moving code-buffer stores past a call into it.
#define CODE_BARRIER() __asm__ volatile("" ::: "memory")

static jitprobe_result *g_res;

static void check(const char *name, int ok, const char *fmt_extra, uint32_t got, uint32_t want)
{
    plat_log("RESULT %s %s got=0x%08" PRIx32 " want=0x%08" PRIx32 "%s%s\n", name, ok ? "PASS" : "FAIL",
             got, want, fmt_extra ? " " : "", fmt_extra ? fmt_extra : "");
    if (ok)
        g_res->passed++;
    else
        g_res->failed++;
}

static uint32_t *wptr(const jit_region *r, size_t off) { return (uint32_t *)(r->rw + off); }
static fn0_t fn0_at(const jit_region *r, size_t off) { return (fn0_t)(uintptr_t)(r->rx + off); }

static uint32_t emit_and_sync_const(const jit_region *r, size_t off, uint32_t value)
{
    arm_emit_ret_const(wptr(r, off), value);
    CODE_BARRIER();
    return (uint32_t)jit_sync(r, off, ARM_RET_CONST_WORDS * 4);
}

// --- thread check -------------------------------------------------------------

typedef struct thread_ctx {
    const jit_region *r;
    size_t off;
    uint32_t rewrite_value; // 0: only call
    uint32_t result;
} thread_ctx;

static void thread_entry(void *arg)
{
    thread_ctx *t = arg;
    if (t->rewrite_value)
        emit_and_sync_const(t->r, t->off, t->rewrite_value);
    t->result = fn0_at(t->r, t->off)();
}

// --- checks -------------------------------------------------------------------

void jitprobe_run(const jitprobe_config *cfg, jitprobe_result *out)
{
    out->passed = out->failed = out->info = 0;
    g_res = out;

    jit_region r;
    int32_t rc = jit_alloc(&r, cfg->pool_size);
    plat_log("RESULT jit.alloc %s method=\"%s\" size=%zu rc=0x%08" PRIx32 " rw=%p rx=%p\n",
             rc == 0 ? "PASS" : "FAIL", jit_method(), cfg->pool_size, (uint32_t)rc,
             rc == 0 ? (void *)r.rw : 0, rc == 0 ? (void *)r.rx : 0);
    if (rc != 0) {
        out->failed++;
        return;
    }
    out->passed++;

    // 1. First execution of generated code.
    rc = emit_and_sync_const(&r, 0, 0x12345678u);
    {
        uint32_t got = fn0_at(&r, 0)();
        check("jit.exec_first", rc == 0 && got == 0x12345678u, 0, got, 0x12345678u);
    }

    // 2. Rewrite the same bytes after they have executed.
    rc = emit_and_sync_const(&r, 0, 0x0BADF00Du);
    {
        uint32_t got = fn0_at(&r, 0)();
        check("jit.rewrite_same_addr", rc == 0 && got == 0x0BADF00Du, 0, got, 0x0BADF00Du);
    }

    // 3. Stale visibility: rewrite with data-cache clean only. Either value is
    // legal; this records whether instruction invalidation is required on this
    // target (Azahar keeps its own translation cache). Then synchronise fully.
    arm_emit_ret_const(wptr(&r, 0), 0x5EEDBEEFu);
    CODE_BARRIER();
    jit_sync_dcache_only(&r, 0, ARM_RET_CONST_WORDS * 4);
    {
        uint32_t got = fn0_at(&r, 0)();
        plat_log("RESULT jit.stale_without_icache_inval INFO observed=%s got=0x%08" PRIx32 "\n",
                 got == 0x5EEDBEEFu ? "new" : (got == 0x0BADF00Du ? "old" : "other"), got);
        out->info++;
        jit_sync(&r, 0, ARM_RET_CONST_WORDS * 4);
        got = fn0_at(&r, 0)();
        check("jit.rewrite_after_stale", got == 0x5EEDBEEFu, 0, got, 0x5EEDBEEFu);
    }

    // 4. Arguments and the AAPCS return path.
    arm_emit_add_args(wptr(&r, 64));
    CODE_BARRIER();
    jit_sync(&r, 64, 8);
    {
        uint32_t got = ((fn2_t)(uintptr_t)(r.rx + 64))(40, 2);
        check("jit.args", got == 42, 0, got, 42);
    }

    // 5. Branch into another block, then invalidate only the target block.
    {
        const size_t a = 128, b = 256;
        uint32_t insn = arm_b((uintptr_t)(r.rx + a), (uintptr_t)(r.rx + b));
        *wptr(&r, a) = insn;
        arm_emit_ret_const(wptr(&r, b), 0xC0DE0001u);
        CODE_BARRIER();
        jit_sync(&r, a, 4);
        jit_sync(&r, b, ARM_RET_CONST_WORDS * 4);
        uint32_t got = fn0_at(&r, a)();
        check("jit.branch_first", insn != 0 && got == 0xC0DE0001u, 0, got, 0xC0DE0001u);

        emit_and_sync_const(&r, b, 0xC0DE0002u);
        got = fn0_at(&r, a)();
        check("jit.branch_target_rewrite", got == 0xC0DE0002u, "synced=target_only", got, 0xC0DE0002u);
    }

    // 6. Execution and rewriting across scheduler yields.
    {
        const size_t off = 384;
        emit_and_sync_const(&r, off, 0x1111u);
        uint32_t got = fn0_at(&r, off)();
        for (unsigned i = 0; i < cfg->yield_count; i++)
            plat_yield_ms(1);
        emit_and_sync_const(&r, off, 0x2222u);
        for (unsigned i = 0; i < cfg->yield_count; i++)
            plat_yield_ms(1);
        uint32_t got2 = fn0_at(&r, off)();
        check("jit.across_yields", got == 0x1111u && got2 == 0x2222u, 0, got2, 0x2222u);
    }

    // 7. Cross-thread: code written here runs there, and vice versa.
    {
        thread_ctx t = {&r, 512, 0, 0};
        emit_and_sync_const(&r, 512, 0x3333u);
        int trc = plat_run_on_thread(thread_entry, &t);
        check("jit.thread_exec", trc == 0 && t.result == 0x3333u, 0, t.result, 0x3333u);

        t.rewrite_value = 0x4444u;
        trc = plat_run_on_thread(thread_entry, &t);
        uint32_t got = fn0_at(&r, 512)();
        check("jit.thread_rewrite_main_exec", trc == 0 && got == 0x4444u, 0, got, 0x4444u);
    }

    // 8. Whole-pool reuse: fill every slot, then repeatedly rewrite all slots
    // with per-slot synchronisation and verify every result.
    {
        const size_t slot = 16, base = 1024;
        const size_t nslots = (r.size - base) / slot;
        unsigned mismatches = 0;
        uint64_t sync_ticks = 0, syncs = 0;

        for (size_t i = 0; i < nslots; i++)
            arm_emit_ret_const(wptr(&r, base + i * slot), (uint32_t)(0xA0000000u ^ i));
        CODE_BARRIER();
        uint64_t t0 = plat_ticks();
        jit_sync(&r, base, nslots * slot);
        uint64_t whole_pool_ticks = plat_ticks() - t0;
        for (size_t i = 0; i < nslots; i++)
            if (fn0_at(&r, base + i * slot)() != (uint32_t)(0xA0000000u ^ i))
                mismatches++;

        for (unsigned round = 1; round <= cfg->stress_rounds; round++) {
            for (size_t i = 0; i < nslots; i++) {
                arm_emit_ret_const(wptr(&r, base + i * slot), (uint32_t)(round << 24 ^ i));
                CODE_BARRIER();
                t0 = plat_ticks();
                jit_sync(&r, base + i * slot, ARM_RET_CONST_WORDS * 4);
                sync_ticks += plat_ticks() - t0;
                syncs++;
            }
            for (size_t i = 0; i < nslots; i++)
                if (fn0_at(&r, base + i * slot)() != (uint32_t)(round << 24 ^ i))
                    mismatches++;
        }
        plat_log("RESULT jit.pool_reuse %s slots=%zu rounds=%u mismatches=%u\n", mismatches ? "FAIL" : "PASS",
                 nslots, cfg->stress_rounds, mismatches);
        if (mismatches)
            out->failed++;
        else
            out->passed++;

        uint64_t tps = plat_ticks_per_sec();
        plat_log("RESULT jit.sync_cost INFO small_sync_avg_ns=%" PRIu64 " whole_pool_sync_us=%" PRIu64
                 " pool_bytes=%zu\n",
                 syncs ? sync_ticks * 1000000000ull / tps / syncs : 0, whole_pool_ticks * 1000000ull / tps,
                 nslots * slot);
        out->info++;
    }

    jit_free(&r);
    plat_log("RESULT jit.summary %s passed=%u failed=%u info=%u\n", out->failed ? "FAIL" : "PASS", out->passed,
             out->failed, out->info);
}
