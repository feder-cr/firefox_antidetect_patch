/* Shared definitions for the CFF rasterizer ports (the C files in reimpl/cff/, notes/cff.md).
 *
 * Every port is written as a plain C function `<name>_impl` and exported as `cff_<name>`.
 * In oracle mode the export is a register-preserving thunk: DWrite is built with
 * whole-program optimisation and its callers keep live values in volatile registers across
 * calls to small leaves (inv_d_bcz_fixed_tfm keeps R8-R11 across fixmul, notes/cff.md
 * "Register hazards"), so the thunk saves RCX, RDX, R8-R11 and XMM1-XMM5 around the call and
 * returns RAX (and XMM0) from the port. In standalone mode (-DIDWR_STANDALONE) every caller is
 * a port, and the export is the C function itself under its export name.
 *
 * Calls from one port to another original go through CFF_ORIG(type, rva), which is
 * idwr_orig (truetype/orig.c): DWrite.dll base + RVA in oracle mode, so whichever version is
 * swapped in at that RVA runs. */
#ifndef CFF_PORT_H
#define CFF_PORT_H
#include <stdint.h>
#include <string.h>
#include "../truetype/fp_exact.h" /* no fused multiply-add without -ffp-contract=off either */

void *idwr_orig(uint32_t rva);
#define CFF_ORIG(type, rva) ((type)idwr_orig(rva))

#ifdef IDWR_STANDALONE
#define CFF_EXPORT(name, impl) _Static_assert(sizeof(&name) == sizeof(&impl), #name)
#elif defined(_WIN32)
#define CFF_EXPORT(name, impl)                                                         \
    __asm__(".text\n"                                                                  \
            ".globl " #name "\n"                                                       \
            ".def " #name "; .scl 2; .type 32; .endef\n"                               \
            ".p2align 4\n" #name ":\n"                                                 \
            ".seh_proc " #name "\n"                                                    \
            "pushq %rcx\n.seh_pushreg %rcx\n"                                          \
            "pushq %rdx\n.seh_pushreg %rdx\n"                                          \
            "pushq %r8\n.seh_pushreg %r8\n"                                            \
            "pushq %r9\n.seh_pushreg %r9\n"                                            \
            "pushq %r10\n.seh_pushreg %r10\n"                                          \
            "pushq %r11\n.seh_pushreg %r11\n"                                          \
            "subq $0x88, %rsp\n.seh_stackalloc 0x88\n"                                 \
            "movaps %xmm1, 0x30(%rsp)\n.seh_savexmm %xmm1, 0x30\n"                     \
            "movaps %xmm2, 0x40(%rsp)\n.seh_savexmm %xmm2, 0x40\n"                     \
            "movaps %xmm3, 0x50(%rsp)\n.seh_savexmm %xmm3, 0x50\n"                     \
            "movaps %xmm4, 0x60(%rsp)\n.seh_savexmm %xmm4, 0x60\n"                     \
            "movaps %xmm5, 0x70(%rsp)\n.seh_savexmm %xmm5, 0x70\n"                     \
            ".seh_endprologue\n"                                                       \
            "callq " #impl "\n"                                                        \
            "movaps 0x30(%rsp), %xmm1\n"                                               \
            "movaps 0x40(%rsp), %xmm2\n"                                               \
            "movaps 0x50(%rsp), %xmm3\n"                                               \
            "movaps 0x60(%rsp), %xmm4\n"                                               \
            "movaps 0x70(%rsp), %xmm5\n"                                               \
            "addq $0x88, %rsp\n"                                                       \
            "popq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdx\npopq %rcx\n"         \
            "retq\n"                                                                   \
            ".seh_endproc\n"                                                           \
            ".section .drectve,\"yn\"\n"                                               \
            ".ascii \" -export:" #name "\"\n"                                          \
            ".text\n")
#else
#error "oracle mode (hooking DWrite.dll) is Windows x64 only; build with -DIDWR_STANDALONE"
#endif

/* CFF_EXPORT for five to eight arguments: the same register-preserving thunk, which also
 * copies four stack arguments (entry rsp + 0x28 .. + 0x40) into the callee's argument area.
 * Plain CFF_EXPORT moves rsp, so a port behind it would read its stack arguments from the
 * wrong place. Copying a slot the caller did not fill only reads the caller's frame. */
#ifdef IDWR_STANDALONE
#define CFF_EXPORT8(name, impl) CFF_EXPORT(name, impl)
#else
#define CFF_EXPORT8(name, impl)                                                        \
    __asm__(".text\n"                                                                  \
            ".globl " #name "\n"                                                       \
            ".def " #name "; .scl 2; .type 32; .endef\n"                               \
            ".p2align 4\n" #name ":\n"                                                 \
            ".seh_proc " #name "\n"                                                    \
            "pushq %rcx\n.seh_pushreg %rcx\n"                                          \
            "pushq %rdx\n.seh_pushreg %rdx\n"                                          \
            "pushq %r8\n.seh_pushreg %r8\n"                                            \
            "pushq %r9\n.seh_pushreg %r9\n"                                            \
            "pushq %r10\n.seh_pushreg %r10\n"                                          \
            "pushq %r11\n.seh_pushreg %r11\n"                                          \
            "subq $0x98, %rsp\n.seh_stackalloc 0x98\n"                                 \
            "movaps %xmm1, 0x40(%rsp)\n.seh_savexmm %xmm1, 0x40\n"                     \
            "movaps %xmm2, 0x50(%rsp)\n.seh_savexmm %xmm2, 0x50\n"                     \
            "movaps %xmm3, 0x60(%rsp)\n.seh_savexmm %xmm3, 0x60\n"                     \
            "movaps %xmm4, 0x70(%rsp)\n.seh_savexmm %xmm4, 0x70\n"                     \
            "movaps %xmm5, 0x80(%rsp)\n.seh_savexmm %xmm5, 0x80\n"                     \
            ".seh_endprologue\n"                                                       \
            "movq 0xf0(%rsp), %rax\nmovq %rax, 0x20(%rsp)\n"                           \
            "movq 0xf8(%rsp), %rax\nmovq %rax, 0x28(%rsp)\n"                           \
            "movq 0x100(%rsp), %rax\nmovq %rax, 0x30(%rsp)\n"                          \
            "movq 0x108(%rsp), %rax\nmovq %rax, 0x38(%rsp)\n"                          \
            "callq " #impl "\n"                                                        \
            "movaps 0x40(%rsp), %xmm1\n"                                               \
            "movaps 0x50(%rsp), %xmm2\n"                                               \
            "movaps 0x60(%rsp), %xmm3\n"                                               \
            "movaps 0x70(%rsp), %xmm4\n"                                               \
            "movaps 0x80(%rsp), %xmm5\n"                                               \
            "addq $0x98, %rsp\n"                                                       \
            "popq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdx\npopq %rcx\n"         \
            "retq\n"                                                                   \
            ".seh_endproc\n"                                                           \
            ".section .drectve,\"yn\"\n"                                               \
            ".ascii \" -export:" #name "\"\n"                                          \
            ".text\n")
#endif

#if defined(IDWR_STATIC)
#define CFF_API
#elif defined(IDWR_STANDALONE) && !defined(_WIN32)
#define CFF_API __attribute__((visibility("default")))
#elif defined(IDWR_STANDALONE)
#define CFF_API __declspec(dllexport)
#else
#define CFF_API
#endif

#endif
