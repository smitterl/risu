/******************************************************************************
 * Copyright (c) 2013 Linaro Limited
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *     Claudio Fontana (Linaro) - initial implementation
 *     based on Peter Maydell's risu_arm.c
 *****************************************************************************/

#include <stdio.h>
#include <ucontext.h>
#include <string.h>
#include <signal.h> /* for FPSIMD_MAGIC */
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>
#include <assert.h>
#include <errno.h>
#include <sys/prctl.h>

#include "risu.h"
#include "risu_reginfo_aarch64.h"

/* Should we test SVE register state */
static int test_sve;
static const struct option extra_opts[] = {
    {"test-sve", required_argument, NULL, FIRST_ARCH_OPT },
    {0, 0, 0, 0}
};

const struct option * const arch_long_opts = &extra_opts[0];
const char * const arch_extra_help
    = "  --test-sve=<vq>        Compare SVE registers with VQ\n";

void process_arch_opt(int opt, const char *arg)
{
    assert(opt == FIRST_ARCH_OPT);
    test_sve = strtol(arg, 0, 10);

    if (test_sve <= 0 || test_sve > SVE_VQ_MAX) {
        fprintf(stderr, "Invalid value for VQ (1-%d)\n", SVE_VQ_MAX);
        exit(EXIT_FAILURE);
    }
}

void arch_init(void)
{
    long want, got;

    if (test_sve) {
        want = sve_vl_from_vq(test_sve);
        got = prctl(PR_SVE_SET_VL, want);
        if (want != got) {
            if (got >= 0) {
                fprintf(stderr, "Unsupported VQ for SVE (%d != %d)\n",
                        test_sve, (int)sve_vq_from_vl(got));
            } else if (errno == EINVAL) {
                fprintf(stderr, "System does not support SVE\n");
            } else {
                perror("prctl PR_SVE_SET_VL");
            }
            exit(EXIT_FAILURE);
        }
    }
}

int reginfo_size(struct reginfo *ri)
{
    int size = offsetof(struct reginfo, extra);

    if (ri->sve_vl) {
        int vq = sve_vq_from_vl(ri->sve_vl);
        size += RISU_SVE_REGS_SIZE(vq);
    } else {
        size += RISU_SIMD_REGS_SIZE;
    }
    return size;
}

/* reginfo_init: initialize with a ucontext */
void reginfo_init(struct reginfo *ri, ucontext_t *uc, void *siaddr)
{
    int i;
    struct _aarch64_ctx *ctx, *extra = NULL;
    struct fpsimd_context *fp = NULL;
    struct sve_context *sve = NULL;

    /* necessary to be able to compare with memcmp later */
    memset(ri, 0, sizeof(*ri));

    for (i = 0; i < 31; i++) {
        ri->regs[i] = uc->uc_mcontext.regs[i];
    }

    ri->sp = 0xdeadbeefdeadbeef;
    ri->pc = uc->uc_mcontext.pc - image_start_address;
    ri->flags = uc->uc_mcontext.pstate & 0xf0000000;    /* get only flags */

    ri->fault_address = uc->uc_mcontext.fault_address;
    ri->faulting_insn = *((uint32_t *) uc->uc_mcontext.pc);

    ctx = (struct _aarch64_ctx *) &uc->uc_mcontext.__reserved[0];
    while (ctx) {
        switch (ctx->magic) {
        case FPSIMD_MAGIC:
            fp = (void *)ctx;
            break;
        case SVE_MAGIC:
            sve = (void *)ctx;
            break;
        case EXTRA_MAGIC:
            extra = (void *)((struct extra_context *)(ctx))->datap;
            break;
        case 0:
            /* End of list.  */
            ctx = extra;
            extra = NULL;
            continue;
        default:
            /* Unknown record -- skip it.  */
            break;
        }
        ctx = (void *)ctx + ctx->size;
    }

    if (!fp || fp->head.size != sizeof(*fp)) {
        fprintf(stderr, "risu_reginfo_aarch64: failed to get FP/SIMD state\n");
        return;
    }
    ri->fpsr = fp->fpsr;
    ri->fpcr = fp->fpcr;

    if (test_sve) {
        int vq = test_sve;

        if (sve == NULL) {
            fprintf(stderr, "risu_reginfo_aarch64: failed to get SVE state\n");
            return;
        }

        if (sve->vl != sve_vl_from_vq(vq)) {
            fprintf(stderr, "risu_reginfo_aarch64: "
                    "unexpected SVE state: %d != %d\n",
                    sve->vl, sve_vl_from_vq(vq));
            return;
        }

        if (sve->head.size <= SVE_SIG_CONTEXT_SIZE(0)) {
            /* Only AdvSIMD state is present. */
        } else if (sve->head.size < SVE_SIG_CONTEXT_SIZE(vq)) {
            fprintf(stderr, "risu_reginfo_aarch64: "
                    "failed to get complete SVE state\n");
            return;
        } else {
            ri->sve_vl = sve->vl;
            memcpy(reginfo_zreg(ri, vq, 0),
                   (char *)sve + SVE_SIG_REGS_OFFSET,
                   SVE_SIG_REGS_SIZE(vq));
            return;
        }
    }

    memcpy(reginfo_vreg(ri, 0), fp->vregs, RISU_SIMD_REGS_SIZE);
}

/* reginfo_is_eq: compare the reginfo structs, returns nonzero if equal */
int reginfo_is_eq(struct reginfo *r1, struct reginfo *r2)
{
    return memcmp(r1, r2, reginfo_size(r1)) == 0;
}

static int sve_zreg_is_eq(int vq, const void *z1, const void *z2)
{
    return memcmp(z1, z2, vq * 16) == 0;
}

static int sve_preg_is_eq(int vq, const void *p1, const void *p2)
{
    return memcmp(p1, p2, vq * 2) == 0;
}

static void sve_dump_preg(FILE *f, int vq, const uint16_t *p)
{
    int q;
    for (q = vq - 1; q >= 0; q--) {
        fprintf(f, "%04x", p[q]);
    }
}

static void sve_dump_preg_diff(FILE *f, int vq, const uint16_t *p1,
                               const uint16_t *p2)
{
    sve_dump_preg(f, vq, p1);
    fprintf(f, " vs ");
    sve_dump_preg(f, vq, p2);
    fprintf(f, "\n");
}

static void sve_dump_zreg_diff(FILE *f, int vq, const uint64_t *za,
                               const uint64_t *zb)
{
    const char *pad = "";
    int q;

    for (q = 0; q < vq; ++q) {
        uint64_t za0 = za[2 * q], za1 = za[2 * q + 1];
        uint64_t zb0 = zb[2 * q], zb1 = zb[2 * q + 1];

        if (za0 != zb0 || za1 != zb1) {
            fprintf(f, "%sq%-2d: %016" PRIx64 "%016" PRIx64
                    " vs %016" PRIx64 "%016" PRIx64"\n",
                    pad, q, za1, za0, zb1, zb0);
            pad = "      ";
        }
    }
}

/* reginfo_dump: print state to a stream, returns nonzero on success */
int reginfo_dump(struct reginfo *ri, FILE * f)
{
    int i;
    fprintf(f, "  faulting insn %08x\n", ri->faulting_insn);

    for (i = 0; i < 31; i++) {
        fprintf(f, "  X%-2d    : %016" PRIx64 "\n", i, ri->regs[i]);
    }

    fprintf(f, "  sp     : %016" PRIx64 "\n", ri->sp);
    fprintf(f, "  pc     : %016" PRIx64 "\n", ri->pc);
    fprintf(f, "  flags  : %08x\n", ri->flags);
    fprintf(f, "  fpsr   : %08x\n", ri->fpsr);
    fprintf(f, "  fpcr   : %08x\n", ri->fpcr);

    if (ri->sve_vl) {
        int vq = sve_vq_from_vl(ri->sve_vl);
        int q;

        fprintf(f, "  vl     : %d\n", ri->sve_vl);

        for (i = 0; i < SVE_NUM_ZREGS; i++) {
            uint64_t *z = reginfo_zreg(ri, vq, i);

            fprintf(f, "  Z%-2d q%-2d: %016" PRIx64 "%016" PRIx64 "\n",
                    i, 0, z[1], z[0]);
            for (q = 1; q < vq; ++q) {
                fprintf(f, "      q%-2d: %016" PRIx64 "%016" PRIx64 "\n",
                        q, z[q * 2 + 1], z[q * 2]);
            }
        }

        for (i = 0; i < SVE_NUM_PREGS + 1; i++) {
            uint16_t *p = reginfo_preg(ri, vq, i);

            if (i == SVE_NUM_PREGS) {
                fprintf(f, "  FFR    : ");
            } else {
                fprintf(f, "  P%-2d    : ", i);
            }
            sve_dump_preg(f, vq, p);
            fprintf(f, "\n");
        }
        return !ferror(f);
    }

    for (i = 0; i < 32; i++) {
        uint64_t *v = reginfo_vreg(ri, i);
        fprintf(f, "  V%-2d    : %016" PRIx64 "%016" PRIx64 "\n",
                i, v[1], v[0]);
    }

    return !ferror(f);
}

/* reginfo_dump_mismatch: print mismatch details to a stream, ret nonzero=ok */
int reginfo_dump_mismatch(struct reginfo *m, struct reginfo *a, FILE * f)
{
    int i;
    fprintf(f, "mismatch detail (master : apprentice):\n");
    if (m->faulting_insn != a->faulting_insn) {
        fprintf(f, "  faulting insn mismatch %08x vs %08x\n",
                m->faulting_insn, a->faulting_insn);
    }
    for (i = 0; i < 31; i++) {
        if (m->regs[i] != a->regs[i]) {
            fprintf(f, "  X%-2d    : %016" PRIx64 " vs %016" PRIx64 "\n",
                    i, m->regs[i], a->regs[i]);
        }
    }

    if (m->sp != a->sp) {
        fprintf(f, "  sp     : %016" PRIx64 " vs %016" PRIx64 "\n",
                m->sp, a->sp);
    }

    if (m->pc != a->pc) {
        fprintf(f, "  pc     : %016" PRIx64 " vs %016" PRIx64 "\n",
                m->pc, a->pc);
    }

    if (m->flags != a->flags) {
        fprintf(f, "  flags  : %08x vs %08x\n", m->flags, a->flags);
    }

    if (m->fpsr != a->fpsr) {
        fprintf(f, "  fpsr   : %08x vs %08x\n", m->fpsr, a->fpsr);
    }

    if (m->fpcr != a->fpcr) {
        fprintf(f, "  fpcr   : %08x vs %08x\n", m->fpcr, a->fpcr);
    }

    if (m->sve_vl != a->sve_vl) {
        fprintf(f, "  vl    : %d vs %d\n", m->sve_vl, a->sve_vl);
    }

    if (m->sve_vl) {
        int vq = sve_vq_from_vl(m->sve_vl);

        for (i = 0; i < SVE_NUM_ZREGS; i++) {
            uint64_t *zm = reginfo_zreg(m, vq, i);
            uint64_t *za = reginfo_zreg(a, vq, i);

            if (!sve_zreg_is_eq(vq, zm, za)) {
                fprintf(f, "  Z%-2d ", i);
                sve_dump_zreg_diff(f, vq, zm, za);
            }
        }
        for (i = 0; i < SVE_NUM_PREGS + 1; i++) {
            uint16_t *pm = reginfo_preg(m, vq, i);
            uint16_t *pa = reginfo_preg(a, vq, i);

            if (!sve_preg_is_eq(vq, pm, pa)) {
                if (i == SVE_NUM_PREGS) {
                    fprintf(f, "  FFR   : ");
                } else {
                    fprintf(f, "  P%-2d    : ", i);
                }
                sve_dump_preg_diff(f, vq, pm, pa);
            }
        }
        return !ferror(f);
    }

    for (i = 0; i < 32; i++) {
        uint64_t *mv = reginfo_vreg(m, i);
        uint64_t *av = reginfo_vreg(a, i);

        if (mv[0] != av[0] || mv[1] != av[1]) {
            fprintf(f, "  V%-2d    : "
                    "%016" PRIx64 "%016" PRIx64 " vs "
                    "%016" PRIx64 "%016" PRIx64 "\n",
                    i, mv[1], mv[0], av[1], av[0]);
        }
    }

    return !ferror(f);
}
