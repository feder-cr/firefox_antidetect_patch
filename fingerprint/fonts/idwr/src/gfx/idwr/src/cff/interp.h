/* Shared by the interp group's files (reimpl/cff/interp.c, interp_procs.c). */
#ifndef CFF_INTERP_H
#define CFF_INTERP_H
#include "cff_port.h"

/* CFF_EXPORT_N: CFF_EXPORT for a port with n (<= 6) arguments on the stack. CFF_EXPORT's thunk
 * moves the stack, so a port with more than four arguments would read its fifth and later
 * arguments from the wrong place; this thunk copies them into its own outgoing argument area. */
#ifdef IDWR_STANDALONE
#define CFF_EXPORT_N(name, impl, n) CFF_EXPORT(name, impl)
#elif defined(_WIN32)
#define CFF_STK_COPY(name, n, k)                                                               \
    ".if " #n " > " #k "\n"                                                                    \
    "movq " #name "_F+0x58+8*" #k "(%rsp), %rax\n"                                             \
    "movq %rax, 0x20+8*" #k "(%rsp)\n"                                                         \
    ".endif\n"
#define CFF_EXPORT_N(name, impl, n)                                                            \
    __asm__(".text\n"                                                                          \
            ".globl " #name "\n"                                                               \
            ".def " #name "; .scl 2; .type 32; .endef\n"                                       \
            ".set " #name "_X, ((0x20+8*" #n "+15)&~15)\n"                                     \
            ".set " #name "_F, " #name "_X+0x58\n"                                             \
            ".p2align 4\n" #name ":\n"                                                         \
            ".seh_proc " #name "\n"                                                            \
            "pushq %rcx\n.seh_pushreg %rcx\n"                                                  \
            "pushq %rdx\n.seh_pushreg %rdx\n"                                                  \
            "pushq %r8\n.seh_pushreg %r8\n"                                                    \
            "pushq %r9\n.seh_pushreg %r9\n"                                                    \
            "pushq %r10\n.seh_pushreg %r10\n"                                                  \
            "pushq %r11\n.seh_pushreg %r11\n"                                                  \
            "subq $" #name "_F, %rsp\n.seh_stackalloc " #name "_F\n"                           \
            "movaps %xmm1, " #name "_X+0x00(%rsp)\n.seh_savexmm %xmm1, " #name "_X+0x00\n"     \
            "movaps %xmm2, " #name "_X+0x10(%rsp)\n.seh_savexmm %xmm2, " #name "_X+0x10\n"     \
            "movaps %xmm3, " #name "_X+0x20(%rsp)\n.seh_savexmm %xmm3, " #name "_X+0x20\n"     \
            "movaps %xmm4, " #name "_X+0x30(%rsp)\n.seh_savexmm %xmm4, " #name "_X+0x30\n"     \
            "movaps %xmm5, " #name "_X+0x40(%rsp)\n.seh_savexmm %xmm5, " #name "_X+0x40\n"     \
            ".seh_endprologue\n"                                                               \
            CFF_STK_COPY(name, n, 0) CFF_STK_COPY(name, n, 1) CFF_STK_COPY(name, n, 2)         \
            CFF_STK_COPY(name, n, 3) CFF_STK_COPY(name, n, 4) CFF_STK_COPY(name, n, 5)         \
            "callq " #impl "\n"                                                                \
            "movaps " #name "_X+0x00(%rsp), %xmm1\n"                                           \
            "movaps " #name "_X+0x10(%rsp), %xmm2\n"                                           \
            "movaps " #name "_X+0x20(%rsp), %xmm3\n"                                           \
            "movaps " #name "_X+0x30(%rsp), %xmm4\n"                                           \
            "movaps " #name "_X+0x40(%rsp), %xmm5\n"                                           \
            "addq $" #name "_F, %rsp\n"                                                        \
            "popq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdx\npopq %rcx\n"                 \
            "retq\n"                                                                           \
            ".seh_endproc\n"                                                                   \
            ".section .drectve,\"yn\"\n"                                                       \
            ".ascii \" -export:" #name "\"\n"                                                  \
            ".text\n")
#endif

#endif
