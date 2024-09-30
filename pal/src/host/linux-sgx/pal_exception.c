/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University
 *               2020 Intel Labs
 */

/*
 * This file contains APIs to set up signal handlers.
 */

#include <stddef.h> /* needed by <linux/signal.h> for size_t */
#include <linux/signal.h>

#include "api.h"
#include "asan.h"
#include "cpu.h"
#include "pal.h"
#include "pal_internal.h"
#include "pal_linux.h"

#define ADDR_IN_PAL(addr) ((void*)(addr) > TEXT_START && (void*)(addr) < TEXT_END)

/* Restore an sgx_cpu_context_t as generated by .Lhandle_exception. Execution will
 * continue as specified by the rip in the context. */
__attribute_no_sanitize_address
noreturn static void restore_sgx_context(sgx_cpu_context_t* uc, PAL_XREGS_STATE* xregs_state) {
    if (xregs_state == NULL)
        xregs_state = (PAL_XREGS_STATE*)g_xsave_reset_state;

#ifdef ASAN
    /* Unpoison the signal stack before leaving it */
    uintptr_t sig_stack_low = GET_ENCLAVE_TCB(sig_stack_low);
    uintptr_t sig_stack_high = GET_ENCLAVE_TCB(sig_stack_high);
    asan_unpoison_current_stack(sig_stack_low, sig_stack_high - sig_stack_low);
#endif

    _restore_sgx_context(uc, xregs_state);
}

noreturn static void restore_pal_context(sgx_cpu_context_t* uc, PAL_CONTEXT* ctx) {
    uc->rax    = ctx->rax;
    uc->rbx    = ctx->rbx;
    uc->rcx    = ctx->rcx;
    uc->rdx    = ctx->rdx;
    uc->rsp    = ctx->rsp;
    uc->rbp    = ctx->rbp;
    uc->rsi    = ctx->rsi;
    uc->rdi    = ctx->rdi;
    uc->r8     = ctx->r8;
    uc->r9     = ctx->r9;
    uc->r10    = ctx->r10;
    uc->r11    = ctx->r11;
    uc->r12    = ctx->r12;
    uc->r13    = ctx->r13;
    uc->r14    = ctx->r14;
    uc->r15    = ctx->r15;
    uc->rflags = ctx->efl;
    uc->rip    = ctx->rip;

    restore_sgx_context(uc, ctx->is_fpregs_used ? ctx->fpregs : NULL);
}

static void save_pal_context(PAL_CONTEXT* ctx, sgx_cpu_context_t* uc,
                             PAL_XREGS_STATE* xregs_state) {
    memset(ctx, 0, sizeof(*ctx));

    ctx->rax = uc->rax;
    ctx->rbx = uc->rbx;
    ctx->rcx = uc->rcx;
    ctx->rdx = uc->rdx;
    ctx->rsp = uc->rsp;
    ctx->rbp = uc->rbp;
    ctx->rsi = uc->rsi;
    ctx->rdi = uc->rdi;
    ctx->r8  = uc->r8;
    ctx->r9  = uc->r9;
    ctx->r10 = uc->r10;
    ctx->r11 = uc->r11;
    ctx->r12 = uc->r12;
    ctx->r13 = uc->r13;
    ctx->r14 = uc->r14;
    ctx->r15 = uc->r15;
    ctx->efl = uc->rflags;
    ctx->rip = uc->rip;
    union pal_csgsfs csgsfs = {
        .cs = 0x33, // __USER_CS(5) | 0(GDT) | 3(RPL)
        .fs = 0,
        .gs = 0,
        .ss = 0x2b, // __USER_DS(6) | 0(GDT) | 3(RPL)
    };
    ctx->csgsfsss = csgsfs.csgsfs;

    assert(xregs_state);
    ctx->fpregs = xregs_state;
    ctx->is_fpregs_used = 1;

    /* Emulate format for fp registers Linux sets up as signal frame.
     * https://elixir.bootlin.com/linux/v5.4.13/source/arch/x86/kernel/fpu/signal.c#L86
     * https://elixir.bootlin.com/linux/v5.4.13/source/arch/x86/kernel/fpu/signal.c#L459
     */
    PAL_FPX_SW_BYTES* fpx_sw = &xregs_state->fpstate.sw_reserved;
    fpx_sw->magic1        = PAL_FP_XSTATE_MAGIC1;
    fpx_sw->extended_size = g_xsave_size;
    fpx_sw->xfeatures     = g_xsave_features;
    memset(fpx_sw->padding, 0, sizeof(fpx_sw->padding));
    if (g_xsave_enabled) {
        fpx_sw->xstate_size = g_xsave_size + PAL_FP_XSTATE_MAGIC2_SIZE;
        *(__typeof__(PAL_FP_XSTATE_MAGIC2)*)((void*)xregs_state + g_xsave_size) =
            PAL_FP_XSTATE_MAGIC2;
    } else {
        fpx_sw->xstate_size = g_xsave_size;
    }
}

static void emulate_rdtsc_and_print_warning(sgx_cpu_context_t* uc) {
    if (FIRST_TIME()) {
        /* if we end up emulating RDTSC/RDTSCP instruction, we cannot use invariant TSC */
        extern uint64_t g_tsc_hz;
        g_tsc_hz = 0;
        log_warning("all RDTSC/RDTSCP instructions are emulated (imprecisely) via gettime() "
                    "syscall.");
    }

    uint64_t usec;
    int res = _PalSystemTimeQuery(&usec);
    if (res < 0) {
        log_error("_PalSystemTimeQuery() failed in unrecoverable context, exiting.");
        _PalProcessExit(1);
    }
    /* FIXME: Ideally, we would like to scale microseconds back to RDTSC clock cycles */
    uc->rdx = (uint32_t)(usec >> 32);
    uc->rax = (uint32_t)usec;
}

static void emulate_iret_and_print_warning(sgx_cpu_context_t* uc) {
#ifndef __x86_64__
    #error "The iret emulation is unsupported on other platforms."
#endif

    if (FIRST_TIME()) {
        log_warning("Emulating a raw iret instruction. This degrades performance.");
    }

    uc->rip = *(uint64_t*)(intptr_t)uc->rsp;
    uc->rsp += 8;

    /* Assume that cs register doesn't change. */
#ifdef DEBUG
    uint64_t cs = *(uint64_t*)(intptr_t)uc->rsp;
    uint64_t cur_cs = 0;
    __asm__ volatile (
        "movq %%cs, %0\n"
        : "=r"(cur_cs)
    );
    assert(cs == cur_cs);
#endif
    uc->rsp += 8;

    uc->rflags = *(uint64_t*)(intptr_t)uc->rsp;
    uc->rsp += 8;

    uint64_t tmprsp = *(uint64_t*)(intptr_t)uc->rsp;
    uc->rsp += 8;

    /* Assume that ss register doesn't change. */
#ifdef DEBUG
    uint64_t ss = *(uint64_t*)(intptr_t)uc->rsp;
    uint64_t cur_ss = 0;
    __asm__ volatile (
        "movq %%ss, %0\n"
        : "=r"(cur_ss)
    );
    assert(ss == cur_ss);
#endif
    uc->rsp += 8;

    uc->rsp = tmprsp;
}

/* return value: true if #UD was handled and execution can be continued without propagating #UD;
 *               false if #UD was not handled and exception needs to be raised up to LibOS/app */
static bool handle_ud(sgx_cpu_context_t* uc, int* out_event_num) {
    /* most unhandled #UD faults are translated and sent to LibOS/app as "Illegal instruction"
     * exceptions; however some #UDs (e.g. triggered due to IN/OUT/INS/OUTS) must be translated as
     * "Memory fault" exceptions */
    *out_event_num = PAL_EVENT_ILLEGAL;

    uint8_t* instr = (uint8_t*)uc->rip;
    if (instr[0] == 0x0f && instr[1] == 0xa2) {
        /* cpuid */
        unsigned int values[4];
        if (!_PalCpuIdRetrieve(uc->rax & 0xffffffff, uc->rcx & 0xffffffff, values)) {
            uc->rip += 2;
            uc->rax = values[0];
            uc->rbx = values[1];
            uc->rcx = values[2];
            uc->rdx = values[3];
            return true;
        }
    } else if (instr[0] == 0x0f && instr[1] == 0x31) {
        /* rdtsc */
        emulate_rdtsc_and_print_warning(uc);
        uc->rip += 2;
        return true;
    } else if (instr[0] == 0x0f && instr[1] == 0x01 && instr[2] == 0xf9) {
        /* rdtscp */
        emulate_rdtsc_and_print_warning(uc);
        uc->rip += 3;
        uc->rcx = 0; /* dummy IA32_TSC_AUX; Linux encodes it as (numa_id << 12) | cpu_id */
        return true;
    } else if (0x48 <= instr[0] && instr[0] <= 0x4F && instr[1] == 0xcf) {
        /*
         * The IRETQ (interrupt return, 64-bit operand size) is prefixed with REX.W (bit 3).
         * From Intel manual:
         * REX prefixes are a set of 16 opcodes that span one row of the opcode map and occupy
         * entries 40H to 4FH.
         */
        emulate_iret_and_print_warning(uc);
        return true;
    } else if (instr[0] == 0xf3 && (instr[1] & ~1) == 0x48 && instr[2] == 0x0f &&
               instr[3] == 0xae && instr[4] >> 6 == 0b11 && ((instr[4] >> 3) & 0b111) < 4) {
        /* A disabled {RD,WR}{FS,GS}BASE instruction generated a #UD */
        log_error(
            "{RD,WR}{FS,GS}BASE instructions are not permitted on this platform. Please check the "
            "instructions under \"Building with SGX support\" from Gramine documentation.");
        return false;
    } else if (instr[0] == 0x0f && instr[1] == 0x05) {
        /* syscall: LibOS may know how to handle this */
        if (FIRST_TIME()) {
            log_always("Emulating a raw syscall instruction. This degrades performance, consider"
                       " patching your application to use Gramine syscall API.");
        }
        log_trace("Emulating raw syscall instruction with number %lu at address %p",
                  uc->rax, (void*)uc->rip);
        return false;
    } else if (is_in_out(instr) && !has_lock_prefix(instr)) {
        /*
         * Executing I/O instructions (e.g., IN/OUT/INS/OUTS) inside an SGX enclave generates a #UD
         * fault. Without the below corner-case handling, PAL would propagate this fault to LibOS as
         * an "Illegal instruction" Gramine exception. However, I/O instructions result in a #GP
         * fault outside SGX (which corresponds to "Memory fault" Gramine exception) if I/O is not
         * permitted (which is true in userspace apps). Let PAL emulate these instructions as if
         * they ended up in a memory fault.
         *
         * Note that I/O instructions with a LOCK prefix always result in a #UD fault, so they are
         * special-cased here.
         */
        if (FIRST_TIME()) {
            log_warning("Emulating In/OUT/INS/OUTS instruction as a SIGSEGV signal to app.");
        }
        *out_event_num = PAL_EVENT_MEMFAULT;
        return false;
    }

    char buf[LOCATION_BUF_SIZE];
    pal_describe_location(uc->rip, buf, sizeof(buf));
    log_warning("Unknown or illegal instruction executed at %s", buf);
    return false;
}

/* perform exception handling inside the enclave */
void _PalExceptionHandler(uint32_t trusted_exit_info_,
                          uint32_t untrusted_external_event, sgx_cpu_context_t* uc,
                          PAL_XREGS_STATE* xregs_state, sgx_arch_exinfo_t* exinfo) {
    assert(IS_ALIGNED_PTR(xregs_state, PAL_XSTATE_ALIGN));

    sgx_arch_exit_info_t trusted_exit_info;
    static_assert(sizeof(trusted_exit_info) == sizeof(trusted_exit_info_), "invalid size");
    memcpy(&trusted_exit_info, &trusted_exit_info_, sizeof(trusted_exit_info));

    /*
     * Intel SGX hardware exposes information on a HW exception in the EXITINFO struct.
     * Host OS + Gramine's untrusted part of PAL deliver a SW signal. The SW signal can be a
     * reaction to HW exception (synchronous signal) or a reaction to software events (asynchronous
     * signal). For security, it is important to cross-check HW exception state vs SW signal state.
     *
     * The below table shows the cross checks. "yes" means allowed combination, "no" means
     * prohibited combination (Gramine terminates). "yes*" means a special case of #PF, see comments
     * below on #PF handling.
     *
     *   +-----------------------------+-----+-----+-----+-----+------------------+------------+
     *   | HW exceptions (trusted) ->  |     | #DE |     |     |                  |            |
     *   | --------------------------- |     | #MF |     | #GP | others           |   none     |
     *   |  SW signals (untrusted) |   | #UD | #XM | #PF | #AC | (#BR,#DB,#BP,#CP)| (valid=0)  |
     *   |                         v   |     |     |     |     |                  |            |
     * --+-----------------------------+-----+-----+-----+-----+------------------+------------+
     * s |                             |     |     |     |     |                  |            |
     * y | PAL_EVENT_ILLEGAL           | yes | no  | no  | no  |                  |            |
     * n |                             |     |     |     |     |                  |            |
     * c +-----------------------------+-----+-----+-----+-----+        no        |    no      |
     * h |                             |     |     |     |     |   (exceptions    | (malicious |
     * r | PAL_EVENT_ARITHMETIC_ERROR  | no  | yes | no  | no  |    unsupported   |  host      |
     * o |                             |     |     |     |     |    by Gramine)   |  injected  |
     * n +-----------------------------+-----+-----+-----+-----+                  |  SW signal)|
     * o |                             |     |     |     |     |                  |            |
     * u | PAL_EVENT_MEMFAULT          | no  | no  |yes* | yes |                  |            |
     * s |                             |     |     |     |     |                  |            |
     * --+-----------------------------+-----+-----+-----+-----+------------------+------------+
     *   |                             |                                          |            |
     * a | PAL_EVENT_QUIT              |                                          |    yes     |
     * s |                             |          no, except #PF case*            |            |
     * y +-----------------------------+  (malicious host ignored HW exception)   +------------+
     * n |                             |                                          |            |
     * c | PAL_EVENT_INTERRUPTED       |                                          |    yes     |
     *   |                             |                                          |            |
     * --+-----------------------------+------------------------------------------+------------+
     */

    bool is_synthetic_gp = false; /* IN/OUT/INS/OUTS instructions morph #UD into a synthetic #GP */

    uint32_t event_num = 0; /* illegal event */

    if (!trusted_exit_info.valid) {
        /* corresponds to last column in the table above */
        if (untrusted_external_event != PAL_EVENT_QUIT
                && untrusted_external_event != PAL_EVENT_INTERRUPTED) {
            if (untrusted_external_event == PAL_EVENT_MEMFAULT
                    && g_pal_linuxsgx_state.memfaults_without_exinfo_allowed) {
                /*
                 * NOTE: Old CPUs may have SGX without the EXINFO feature, thus they do not
                 * report/reflect #PF and #GP exceptions in the trusted EXITINFO struct. In some
                 * situations (debugging using older CPUs) we don't want to terminate immediately.
                 * Instead we propagate this reported-by-host and possibly malicious exception to
                 * the app, with MADDR (faulting addr) and ERRCD (error code) set to zeros.
                 *
                 * This is enabled via an (insecure) manifest option and will be removed in the near
                 * future.
                 */
                memset(&trusted_exit_info, 0, sizeof(trusted_exit_info));
            } else {
                log_error("Host injected malicious signal %u", untrusted_external_event);
                _PalProcessExit(1);
            }
        }
        event_num = untrusted_external_event;
    } else {
        /* corresponds to all but last columns in the table above */
        const char* exception_name = NULL;
        switch (trusted_exit_info.vector) {
            case SGX_EXCEPTION_VECTOR_UD:
                if (untrusted_external_event != PAL_EVENT_ILLEGAL) {
                    log_error("Host reported mismatching signal (expected %u, got %u)",
                              PAL_EVENT_ILLEGAL, untrusted_external_event);
                    _PalProcessExit(1);
                }
                int event_num_from_handle_ud;
                if (handle_ud(uc, &event_num_from_handle_ud)) {
                    restore_sgx_context(uc, xregs_state);
                    /* UNREACHABLE */
                }
                assert(event_num_from_handle_ud == PAL_EVENT_ILLEGAL
                        || event_num_from_handle_ud == PAL_EVENT_MEMFAULT);
                if (event_num_from_handle_ud == PAL_EVENT_MEMFAULT) {
                    /* it's a #UD on IN/OUT/INS/OUTS instructions, morphed into a #GP in handle_ud()
                     * logic: adjust exception info sent to LibOS to mimic a #GP (see code below) */
                    is_synthetic_gp = true;
                }
                event_num = event_num_from_handle_ud;
                break;
            case SGX_EXCEPTION_VECTOR_DE:
            case SGX_EXCEPTION_VECTOR_MF:
            case SGX_EXCEPTION_VECTOR_XM:
                if (untrusted_external_event != PAL_EVENT_ARITHMETIC_ERROR) {
                    log_error("Host reported mismatching signal (expected %u, got %u)",
                              PAL_EVENT_ARITHMETIC_ERROR, untrusted_external_event);
                    _PalProcessExit(1);
                }
                event_num = PAL_EVENT_ARITHMETIC_ERROR;
                break;
            case SGX_EXCEPTION_VECTOR_PF:
                if (untrusted_external_event == PAL_EVENT_QUIT
                        || untrusted_external_event == PAL_EVENT_INTERRUPTED) {
                    /*
                     * The host delivered an asynchronous signal, so the reported-by-SGX #PF must be
                     * benign (resolved completely by the host kernel), otherwise the host would
                     * deliver PAL_EVENT_MEMFAULT (to signify a #PF which should be acted upon by
                     * Gramine).
                     *
                     * The SGX hardware always reports such benign #PFs though they can be
                     * considered spurious and should be ignored. So the event must be a
                     * host-induced external event, so in the following we handle this external
                     * event and ignore the #PF info.
                     *
                     * Note that the host could modify a real memory fault (a valid #PF) to e.g. a
                     * PAL_EVENT_INTERRUPTED signal. Then we end up in this special case and the app
                     * will not handle a real memory fault but a dummy PAL_EVENT_INTERRUPTED. This
                     * will lead to the app getting stuck on #PF. Since this is a DoS, and Intel SGX
                     * and Gramine don't care about DoSes, this special case is benign.
                     */
                    memset(&trusted_exit_info, 0, sizeof(trusted_exit_info));
                    event_num = untrusted_external_event;
                    break;
                }
                /* fallthrough */
            case SGX_EXCEPTION_VECTOR_GP:
            case SGX_EXCEPTION_VECTOR_AC:
                if (untrusted_external_event != PAL_EVENT_MEMFAULT) {
                    log_error("Host reported mismatching signal (expected %u, got %u)",
                              PAL_EVENT_MEMFAULT, untrusted_external_event);
                    _PalProcessExit(1);
                }
                event_num = PAL_EVENT_MEMFAULT;
                break;
            case SGX_EXCEPTION_VECTOR_BR:
                exception_name = exception_name ? : "#BR";
                /* fallthrough */
            case SGX_EXCEPTION_VECTOR_DB:
                exception_name = exception_name ? : "#DB";
                /* fallthrough */
            case SGX_EXCEPTION_VECTOR_BP:
                exception_name = exception_name ? : "#BP";
                /* fallthrough */
            case SGX_EXCEPTION_VECTOR_CP:
                exception_name = exception_name ? : "#CP";
                /* fallthrough */
            default:
                log_error("Handling %s exceptions is currently unsupported by Gramine",
                          exception_name ? : "[unknown]");
                _PalProcessExit(1);
                /* UNREACHABLE */
        }
    }

    if (event_num == 0 || event_num >= PAL_EVENT_NUM_BOUND) {
        log_error("Illegal exception reported: %d", event_num);
        _PalProcessExit(1);
    }

    /* in PAL, and event isn't asynchronous (i.e., synchronous exception) */
    if (ADDR_IN_PAL(uc->rip) && event_num != PAL_EVENT_QUIT && event_num != PAL_EVENT_INTERRUPTED) {
        char buf[LOCATION_BUF_SIZE];
        pal_describe_location(uc->rip, buf, sizeof(buf));

        const char* event_name = pal_event_name(event_num);
        log_error("Unexpected %s occurred inside PAL (%s)", event_name, buf);

        if (trusted_exit_info.valid) {
            /* EXITINFO field: vector = exception number, exit_type = 0x3 for HW / 0x6 for SW */
            log_debug("(SGX HW reported AEX vector 0x%x with exit_type = 0x%x)",
                      trusted_exit_info.vector, trusted_exit_info.exit_type);
        } else {
            log_debug("(untrusted PAL sent PAL event 0x%x)", untrusted_external_event);
        }

        _PalProcessExit(1);
    }

    PAL_CONTEXT ctx = { 0 };
    save_pal_context(&ctx, uc, xregs_state);

    bool has_hw_fault_address = false;

    if (trusted_exit_info.valid) {
        ctx.trapno = trusted_exit_info.vector;
        /* Only these two exceptions save information in EXINFO. */
        if (!is_synthetic_gp && (trusted_exit_info.vector == SGX_EXCEPTION_VECTOR_GP
                || trusted_exit_info.vector == SGX_EXCEPTION_VECTOR_PF)) {
            ctx.err = exinfo->error_code_val; /* bits: Present, Write/Read, User/Kernel, etc. */
            ctx.cr2 = exinfo->maddr;          /* NOTE: on #GP, maddr = 0 */
            has_hw_fault_address = true;
        }
    }

    uintptr_t addr = 0;
    switch (event_num) {
        case PAL_EVENT_ILLEGAL:
            addr = uc->rip;
            break;
        case PAL_EVENT_MEMFAULT:
            if (!has_hw_fault_address && !is_synthetic_gp
                    && !g_pal_linuxsgx_state.memfaults_without_exinfo_allowed) {
                log_error("Tried to handle a memory fault with no faulting address reported by "
                          "SGX. Please consider enabling 'sgx.use_exinfo' in the manifest.");
                _PalProcessExit(1);
            }
            addr = ctx.cr2;
            break;
        default:
            break;
    }

    pal_event_handler_t upcall = _PalGetExceptionHandler(event_num);
    if (upcall) {
        (*upcall)(ADDR_IN_PAL(uc->rip), addr, &ctx);
    }

    restore_pal_context(uc, &ctx);
}
