/* CFF/CFF2 parser and blending of the CFF rasterizer (notes/cff.md, group 3 "parse"):
 *
 *   AdobeCFFConvertNDV        0x8dd34   AdobeCFFParse             0xb6708
 *   AdobeCFFFreeBlend         0x8de3c   AdobeCFFDecodeIndex       0xb6d90
 *   AdobeCFFFreeVstore        0x8de70   AdobeDecodeFracNumber     0xb6f70
 *   AdobeCFFSetupValues1      0xb46bc   AdobeFloatToFixed         0xb6fa8
 *   AdobeCFFSetDictDefaults   0xb4ff8   AdobeCFFMakeBlend         0xb6fe4
 *   AdobeCFFSetDefaults       0xb50fc   AdobeCFFGetOne            0xd0bdc
 *   AdobeCFFParseDict         0xb5208   AdobeCFFResetBlend        0xe7fc4
 *   AdobeDecodeNumber         0xb66d8   AdobeCFFDoCharStringBlend 0xed7c0
 *   AdobeCFFDecodeVstore      0x10a30c  AdobeCFFBuildBlendVector  0x10a848
 *   AdobeCFFDoDictBlend       0x10b1b8  AdobeCFFCheckBlendVector  0x10c428
 *   AdobeCFFIsUsedBlend       0x10cb90
 *
 * Ported from the assembly of DWrite.dll 10.0.26100.9711. Each port is exported as
 * cff_<name> (CFF_EXPORT thunk), every call to another function of the engine, inside this
 * group too, goes through CFF_ORIG so each port swaps alone.
 *
 * Hidden arguments. DWrite is built with whole-program optimisation, and some calls pass
 * arguments in registers the callee's C signature does not show:
 *   - AdobeCFFParse calls AdobeCFFDecodeVstore(inst, vstore) with R8/R9 still holding the font
 *     data bounds from the AdobeCFFDecodeIndex call just before (DecodeIndex never writes
 *     R8/R9). DecodeVstore really takes (inst, p, lo, hi); the port passes them explicitly, and
 *     the thunk of the DecodeIndex port restores R8/R9 for the original Parse.
 *   - AdobeCFFParseDict calls AdobeDecodeNumber and AdobeDecodeFracNumber in runs with RCX
 *     (inst) loaded once, and keeps RDX (the operand index), R8-R11 live across them.
 *   - AdobeCFFDecodeIndex, AdobeCFFParse and AdobeCFFDecodeVstore call AdobeCFFGetOne with RDX
 *     incremented in place and R8-R11 live.
 * The thunks save RCX, RDX, R8-R11, so the original callers see what they expect.
 *
 * Structures (offsets into CFFFontInst, see notes/cff.md):
 *   +0x08 font data, +0x10 size, +0x14 CFF2, +0x18 u16 CFF2 Top DICT length, +0x20 vstore
 *   data (CFF2 op 24), +0x28 the decoded vstore, +0x30/+0x38 two blend records (Private DICT
 *   and charstrings), +0x40/+0x48 the normalized design vector (count, 16.16 coordinates),
 *   +0x50 vsindex; +0x58/+0x60/+0x68 the DICT operand stack (int values, float values, "is
 *   real" flags), +0x70 operand count, +0x74 capacity (48, 513 for CFF2); +0x78 the charstring
 *   operand stack, +0x80 its capacity, +0x88 its "already blended" flags.
 *   vstore (0x20 bytes): +0x00 u32 ItemVariationData count, +0x08 records (16 bytes:
 *   u32 regionIndexCount, +8 u32 *regionIndices), +0x10 u32 axisCount, +0x14 u32 regionCount,
 *   +0x18 region pointers (each axisCount x {start, peak, end} in 16.16).
 *   blend (0x30 bytes): +0x00 built, +0x04 used, +0x08 inst, +0x10 vsindex, +0x14 NDV count,
 *   +0x18 NDV copy, +0x20 scalar count (regions + 1), +0x28 scalars (scalars[0] = 1.0).
 *
 * Memory: _o_malloc/_o_realloc/_o_free and _o_pow are DWrite's CRT imports in oracle mode (the
 * blocks are freed by the original AdobeFreeFontInst), the C library in standalone mode.
 *
 * Negative controls (-DCFF_CONTROL_<NAME>, reimpl/cff_check.py --control <name>): one per
 * function, each named after it in lower case without the Adobe/AdobeCFF prefix, e.g.
 * --control parsedict; see the #ifdef CFF_CONTROL_* below. */
#include "cff_port.h"

#ifdef IDWR_STANDALONE
#include <math.h>
#include <stdlib.h>
#define cff_AdobeCFFConvertNDV_impl cff_AdobeCFFConvertNDV
#define cff_AdobeCFFFreeBlend_impl cff_AdobeCFFFreeBlend
#define cff_AdobeCFFFreeVstore_impl cff_AdobeCFFFreeVstore
#define cff_AdobeCFFSetupValues1_impl cff_AdobeCFFSetupValues1
#define cff_AdobeCFFSetDictDefaults_impl cff_AdobeCFFSetDictDefaults
#define cff_AdobeCFFSetDefaults_impl cff_AdobeCFFSetDefaults
#define cff_AdobeCFFParseDict_impl cff_AdobeCFFParseDict
#define cff_AdobeDecodeNumber_impl cff_AdobeDecodeNumber
#define cff_AdobeCFFParse_impl cff_AdobeCFFParse
#define cff_AdobeCFFDecodeIndex_impl cff_AdobeCFFDecodeIndex
#define cff_AdobeDecodeFracNumber_impl cff_AdobeDecodeFracNumber
#define cff_AdobeFloatToFixed_impl cff_AdobeFloatToFixed
#define cff_AdobeCFFMakeBlend_impl cff_AdobeCFFMakeBlend
#define cff_AdobeCFFGetOne_impl cff_AdobeCFFGetOne
#define cff_AdobeCFFResetBlend_impl cff_AdobeCFFResetBlend
#define cff_AdobeCFFDoCharStringBlend_impl cff_AdobeCFFDoCharStringBlend
#define cff_AdobeCFFDecodeVstore_impl cff_AdobeCFFDecodeVstore
#define cff_AdobeCFFBuildBlendVector_impl cff_AdobeCFFBuildBlendVector
#define cff_AdobeCFFDoDictBlend_impl cff_AdobeCFFDoDictBlend
#define cff_AdobeCFFCheckBlendVector_impl cff_AdobeCFFCheckBlendVector
#define cff_AdobeCFFIsUsedBlend_impl cff_AdobeCFFIsUsedBlend
#endif

/* CFF_EXPORT for functions with more than four arguments (AdobeCFFParseDict: 6,
 * AdobeCFFDecodeIndex: 9). The thunk of cff_port.h moves RSP by 0xb8, so the port would read
 * its stack arguments from the thunk's frame; this variant also copies the caller's six stack
 * argument slots ([entry RSP + 0x28 ...]) into its own outgoing area. Reading slots the caller
 * did not fill only reads its frame. */
#if !defined(IDWR_STANDALONE) && defined(_WIN32)
#define CFF_EXPORT_STACKARGS(name, impl)                                               \
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
            "subq $0xa8, %rsp\n.seh_stackalloc 0xa8\n"                                 \
            "movaps %xmm1, 0x50(%rsp)\n.seh_savexmm %xmm1, 0x50\n"                     \
            "movaps %xmm2, 0x60(%rsp)\n.seh_savexmm %xmm2, 0x60\n"                     \
            "movaps %xmm3, 0x70(%rsp)\n.seh_savexmm %xmm3, 0x70\n"                     \
            "movaps %xmm4, 0x80(%rsp)\n.seh_savexmm %xmm4, 0x80\n"                     \
            "movaps %xmm5, 0x90(%rsp)\n.seh_savexmm %xmm5, 0x90\n"                     \
            ".seh_endprologue\n"                                                       \
            "movq 0x100(%rsp), %rax\nmovq %rax, 0x20(%rsp)\n"                          \
            "movq 0x108(%rsp), %rax\nmovq %rax, 0x28(%rsp)\n"                          \
            "movq 0x110(%rsp), %rax\nmovq %rax, 0x30(%rsp)\n"                          \
            "movq 0x118(%rsp), %rax\nmovq %rax, 0x38(%rsp)\n"                          \
            "movq 0x120(%rsp), %rax\nmovq %rax, 0x40(%rsp)\n"                          \
            "movq 0x128(%rsp), %rax\nmovq %rax, 0x48(%rsp)\n"                          \
            "callq " #impl "\n"                                                        \
            "movaps 0x50(%rsp), %xmm1\n"                                               \
            "movaps 0x60(%rsp), %xmm2\n"                                               \
            "movaps 0x70(%rsp), %xmm3\n"                                               \
            "movaps 0x80(%rsp), %xmm4\n"                                               \
            "movaps 0x90(%rsp), %xmm5\n"                                               \
            "addq $0xa8, %rsp\n"                                                       \
            "popq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdx\npopq %rcx\n"         \
            "retq\n"                                                                   \
            ".seh_endproc\n"                                                           \
            ".section .drectve,\"yn\"\n"                                               \
            ".ascii \" -export:" #name "\"\n"                                          \
            ".text\n")
#else
#define CFF_EXPORT_STACKARGS(name, impl) CFF_EXPORT(name, impl)
#endif

/* ------------------------------------------------------------------------------------------
 * field access, RVAs, memory */

#define U8(b, o) (*(uint8_t *)((uint8_t *)(b) + (o)))
#define U16(b, o) (*(uint16_t *)((uint8_t *)(b) + (o)))
#define U32(b, o) (*(uint32_t *)((uint8_t *)(b) + (o)))
#define I32(b, o) (*(int32_t *)((uint8_t *)(b) + (o)))
#define U64(b, o) (*(uint64_t *)((uint8_t *)(b) + (o)))
#define PTR(b, o) (*(uint8_t **)((uint8_t *)(b) + (o)))

typedef uintptr_t uptr; /* positions in the font data: bounds checks on integers, not pointers */
#define RD(q) (*(const uint8_t *)(q))
#define CP(q) ((const uint8_t *)(q))
/* int32 arithmetic with the wraparound of the original */
#define ADD(a, b) ((int32_t)((uint32_t)(a) + (uint32_t)(b)))
#define SUB(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)))

enum {
    RVA_ConvertNDV = 0x8dd34, RVA_FreeBlend = 0x8de3c, RVA_FreeVstore = 0x8de70,
    RVA_SetupValues1 = 0xb46bc, RVA_SetDictDefaults = 0xb4ff8, RVA_SetDefaults = 0xb50fc,
    RVA_ParseDict = 0xb5208, RVA_DecodeNumber = 0xb66d8, RVA_Parse = 0xb6708,
    RVA_DecodeIndex = 0xb6d90, RVA_DecodeFracNumber = 0xb6f70, RVA_FloatToFixed = 0xb6fa8,
    RVA_MakeBlend = 0xb6fe4, RVA_GetOne = 0xd0bdc, RVA_ResetBlend = 0xe7fc4,
    RVA_DoCharStringBlend = 0xed7c0, RVA_DecodeVstore = 0x10a30c, RVA_BuildBlendVector = 0x10a848,
    RVA_DoDictBlend = 0x10b1b8, RVA_CheckBlendVector = 0x10c428, RVA_IsUsedBlend = 0x10cb90,
    /* outside the group */
    RVA_fixdiv = 0xb316c, RVA_fixmul = 0xb3340, RVA_fixratio = 0xb3544, RVA_DoSetUpIfNeeded = 0x8cd28,
    RVA_my_free = 0x8de18,
};

typedef int32_t (*fn_ii)(int32_t, int32_t);
typedef int32_t (*fn_getone)(uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *);
typedef int32_t (*fn_decnum)(uint8_t *, uint32_t);
typedef int32_t (*fn_f2fix)(float);
typedef int32_t (*fn_decidx)(uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *, uint32_t *, uint16_t *,
                             const uint8_t **, const uint8_t **, const uint8_t **);
typedef int32_t (*fn_pdict)(uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *, int32_t);
typedef int32_t (*fn_vstore)(uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *);
typedef uint8_t *(*fn_mkblend)(uint8_t *);
typedef int32_t (*fn_blendvec)(uint8_t *, uint32_t, uint32_t, const int32_t *);
typedef int32_t (*fn_dodict)(uint8_t *, uint32_t);
typedef int32_t (*fn_isused)(uint8_t *);
typedef int32_t (*fn_inst)(uint8_t *);
typedef void (*fn_vinst)(uint8_t *);
typedef void (*fn_dictdef)(uint8_t *, uint32_t);
typedef int32_t (*fn_setup)(uint8_t *, uint16_t, void *, void *, uint16_t, uint32_t);

#define fixmul(a, b) CFF_ORIG(fn_ii, RVA_fixmul)(a, b)
#define fixdiv(a, b) CFF_ORIG(fn_ii, RVA_fixdiv)(a, b)
#define fixratio(a, b) CFF_ORIG(fn_ii, RVA_fixratio)(a, b)
#define GetOne(o, p, lo, hi) CFF_ORIG(fn_getone, RVA_GetOne)(o, CP(p), CP(lo), CP(hi))
#define DecodeNumber(inst, i) CFF_ORIG(fn_decnum, RVA_DecodeNumber)(inst, i)
#define DecodeFracNumber(inst, i) CFF_ORIG(fn_decnum, RVA_DecodeFracNumber)(inst, i)
#define FloatToFixed(f) CFF_ORIG(fn_f2fix, RVA_FloatToFixed)(f)

#ifdef IDWR_STANDALONE
static void *p_malloc(size_t n) { return malloc(n); }
static void *p_realloc(void *p, size_t n) { return realloc(p, n); }
static void p_free(void *p) { if (p) free(p); }
/* AdobeCFFParseDict only computes 10^e for an integer e (the exponent of a real). DWrite's
 * CRT (ucrtbase pow) is not correctly rounded for e = 23 and e = 126, and other libms may
 * differ elsewhere, so the standalone build takes ucrtbase's results from a table:
 * pow(10, e) for e in [-323, 308], 0 below, +inf above. */
static const uint64_t pow10_ucrt[632] = {
    0x0000000000000002, 0x0000000000000014, 0x00000000000000ca, 0x00000000000007e8,
    0x0000000000004f10, 0x00000000000316a2, 0x00000000001ee257, 0x000000000134d761,
    0x000000000c1069cd, 0x0000000078a42205, 0x00000004b6695433, 0x0000002f201d49fb,
    0x000001d74124e3d1, 0x000012688b70e62b, 0x0000b8157268fdaf, 0x000730d67819e8d2,
    0x0031fa182c40c60d, 0x0066789e3750f791, 0x009c16c5c5253575, 0x00d18e3b9b374169,
    0x0105f1ca820511c3, 0x013b6e3d22865634, 0x017124e63593f5e1, 0x01a56e1fc2f8f359,
    0x01dac9a7b3b7302f, 0x0210be08d0527e1d, 0x0244ed8b04671da5, 0x027a28edc580e50e,
    0x02b059949b708f29, 0x02e46ff9c24cb2f3, 0x03198bf832dfdfb0, 0x034feef63f97d79c,
    0x0383f559e7bee6c1, 0x03b8f2b061aea072, 0x03ef2f5c7a1a488e, 0x04237d99cc506d59,
    0x04585d003f6488af, 0x048e74404f3daadb, 0x04c308a831868ac9, 0x04f7cad23de82d7b,
    0x052dbd86cd6238d9, 0x05629674405d6388, 0x05973c115074bc6a, 0x05cd0b15a491eb84,
    0x060226ed86db3333, 0x0636b0a8e891ffff, 0x066c5cd322b67fff, 0x06a1ba03f5b21000,
    0x06d62884f31e93ff, 0x070bb2a62fe638ff, 0x07414fa7ddefe3a0, 0x0775a391d56bdc87,
    0x07ab0c764ac6d3a9, 0x07e0e7c9eebc444a, 0x081521bc6a6b555c, 0x084a6a2b85062ab3,
    0x0880825b3323dab0, 0x08b4a2f1ffecd15c, 0x08e9cbae7fe805b3, 0x09201f4d0ff10390,
    0x0954272053ed4474, 0x098930e868e89591, 0x09bf7d228322baf5, 0x09f3ae3591f5b4d9,
    0x0a2899c2f6732210, 0x0a5ec033b40fea93, 0x0a9338205089f29c, 0x0ac8062864ac6f43,
    0x0afe07b27dd78b14, 0x0b32c4cf8ea6b6ec, 0x0b677603725064a8, 0x0b9d53844ee47dd1,
    0x0bd25432b14ecea3, 0x0c06e93f5da2824c, 0x0c3ca38f350b22df, 0x0c71e6398126f5cb,
    0x0ca65fc7e170b33e, 0x0cdbf7b9d9cce00d, 0x0d117ad428200c08, 0x0d45d98932280f0a,
    0x0d7b4feb7eb212cd, 0x0db111f32f2f4bc0, 0x0de5566ffafb1eb0, 0x0e1aac0bf9b9e65c,
    0x0e50ab877c142ffa, 0x0e84d6695b193bf8, 0x0eba0c03b1df8af6, 0x0ef047824f2bb6da,
    0x0f245962e2f6a490, 0x0f596fbb9bb44db4, 0x0f8fcbaa82a16121, 0x0fc3df4a91a4dcb5,
    0x0ff8d71d360e13e2, 0x102f0ce4839198db, 0x1063680ed23aff89, 0x1098421286c9bf6b,
    0x10ce5297287c2f45, 0x1102f39e794d9d8b, 0x1137b08617a104ee, 0x116d9ca79d89462a,
    0x11a281e8c275cbda, 0x11d72262f3133ed1, 0x120ceafbafd80e85, 0x124212dd4de70913,
    0x12769794a160cb58, 0x12ac3d79c9b8fe2e, 0x12e1a66c1e139edd, 0x1316100725988694,
    0x134b9408eefea839, 0x13813c85955f2923, 0x13b58ba6fab6f36c, 0x13eaee90b964b047,
    0x1420d51a73deee2d, 0x14550a6110d6a9b8, 0x148a4cf9550c5426, 0x14c0701bd527b498,
    0x14f48c22ca71a1bd, 0x1529af2b7d0e0a2d, 0x15600d7b2e28c65c, 0x159410d9f9b2f7f3,
    0x15c91510781fb5f0, 0x15ff5a549627a36c, 0x16339874ddd8c623, 0x16687e92154ef7ac,
    0x169e9e369aa2b597, 0x16d322e220a5b17e, 0x1707eb9aa8cf1dde, 0x173de6815302e556,
    0x1772b010d3e1cf56, 0x17a75c1508da432b, 0x17dd331a4b10d3f6, 0x18123ff06eea847a,
    0x1846cfec8aa52598, 0x187c83e7ad4e6efe, 0x18b1d270cc51055f, 0x18e6470cff6546b6,
    0x191bd8d03f3e9864, 0x1951678227871f3e, 0x1985c162b168e70e, 0x19bb31bb5dc320d2,
    0x19f0ff151a99f483, 0x1a253eda614071a4, 0x1a5a8e90f9908e0d, 0x1a90991a9bfa58c8,
    0x1ac4bf6142f8eefa, 0x1af9ef3993b72ab8, 0x1b303583fc527ab3, 0x1b6442e4fb671960,
    0x1b99539e3a40dfb8, 0x1bcfa885c8d117a6, 0x1c03c9539d82aec8, 0x1c38bba884e35a7a,
    0x1c6eea92a61c3118, 0x1ca3529ba7d19eaf, 0x1cd8274291c6065b, 0x1d0e3113363787f2,
    0x1d42deac01e2b4f7, 0x1d779657025b6235, 0x1dad7becc2f23ac2, 0x1de26d73f9d764b9,
    0x1e1708d0f84d3de7, 0x1e4ccb0536608d61, 0x1e81fee341fc585d, 0x1eb67e9c127b6e74,
    0x1eec1e43171a4a11, 0x1f2192e9ee706e4b, 0x1f55f7a46a0c89dd, 0x1f8b758d848fac55,
    0x1fc1297872d9cbb5, 0x1ff573d68f903ea2, 0x202ad0cc33744e4b, 0x2060c27fa028b0ef,
    0x2094f31f8832dd2a, 0x20ca2fe76a3f9475, 0x21005df0a267bcc9, 0x2134756ccb01abfb,
    0x216992c7fdc216fa, 0x219ff779fd329cb9, 0x21d3faac3e3fa1f3, 0x2208f9574dcf8a70,
    0x223f37ad21436d0c, 0x227382cc34ca2428, 0x22a8637f41fcad32, 0x22de7c5f127bd87e,
    0x23130dbb6b8d674f, 0x2347d12a4670c123, 0x237dc574d80cf16b, 0x23b29b69070816e3,
    0x23e7424348ca1c9c, 0x241d12d41afca3c3, 0x24522bc490dde65a, 0x2486b6b5b5155ff0,
    0x24bc6463225ab7ec, 0x24f1bebdf578b2f4, 0x25262e6d72d6dfb0, 0x255bba08cf8c979d,
    0x2591544581b7dec2, 0x25c5a956e225d672, 0x25fb13ac9aaf4c0f, 0x2630ec4be0ad8f89,
    0x2665275ed8d8f36c, 0x269a71368f0f3047, 0x26d086c219697e2c, 0x2704a8729fc3ddb7,
    0x2739d28f47b4d525, 0x277023998cd10537, 0x27a42c7ff0054685, 0x27d9379fec069826,
    0x280f8587e7083e30, 0x2843b374f06526de, 0x2878a0522c7e7095, 0x28aec866b79e0cba,
    0x28e33d4032c2c7f5, 0x29180c903f7379f2, 0x294e0fb44f50586e, 0x2982c9d0b1923745,
    0x29b77c44ddf6c516, 0x29ed5b561574765b, 0x2a225915cd68c9f9, 0x2a56ef5b40c2fc77,
    0x2a8cab3210f3bb95, 0x2ac1eaff4a98553d, 0x2af665bf1d3e6a8d, 0x2b2bff2ee48e0530,
    0x2b617f7d4ed8c33e, 0x2b95df5ca28ef40d, 0x2bcb5733cb32b111, 0x2c0116805effaeaa,
    0x2c355c2076bf9a55, 0x2c6ab328946f80ea, 0x2ca0aff95cc5b092, 0x2cd4dbf7b3f71cb7,
    0x2d0a12f5a0f4e3e5, 0x2d404bd984990e6f, 0x2d745ecfe5bf520b, 0x2da97683df2f268d,
    0x2ddfd424d6faf031, 0x2e13e497065cd61f, 0x2e48ddbcc7f40ba6, 0x2e7f152bf9f10e90,
    0x2eb36d3b7c36a91a, 0x2ee8488a5b445360, 0x2f1e5aacf2156838, 0x2f52f8ac174d6123,
    0x2f87b6d71d20b96c, 0x2fbda48ce468e7c7, 0x2ff286d80ec190dc, 0x3027288e1271f513,
    0x305cf2b1970e7258, 0x309217aefe690777, 0x30c69d9abe034955, 0x30fc45016d841baa,
    0x3131ab20e472914a, 0x316615e91d8f359d, 0x319b9b6364f30304, 0x31d1411e1f17e1e3,
    0x32059165a6ddda5b, 0x323af5bf109550f2, 0x3270d9976a5d5297, 0x32a50ffd44f4a73d,
    0x32da53fc9631d10d, 0x3310747ddddf22a8, 0x3344919d5556eb52, 0x3379b604aaaca626,
    0x33b011c2eaabe7d8, 0x33e41633a556e1ce, 0x34191bc08eac9a41, 0x344f62b0b257c0d2,
    0x34839dae6f76d883, 0x34b8851a0b548ea4, 0x34eea6608e29b24d, 0x352327fc58da0f70,
    0x3557f1fb6f10934c, 0x358dee7a4ad4b81f, 0x35c2b50c6ec4f313, 0x35f7624f8a762fd8,
    0x362d3ae36d13bbce, 0x366244ce242c5561, 0x3696d601ad376ab9, 0x36cc8b8218854567,
    0x3701d7314f534b61, 0x37364cfda3281e39, 0x376be03d0bf225c7, 0x37a16c262777579c,
    0x37d5c72fb1552d83, 0x380b38fb9daa78e4, 0x3841039d428a8b8f, 0x38754484932d2e72,
    0x38aa95a5b7f87a0f, 0x38e09d8792fb4c49, 0x3914c4e977ba1f5c, 0x3949f623d5a8a733,
    0x398039d665896880, 0x39b4484bfeebc2a0, 0x39e95a5efea6b347, 0x3a1fb0f6be506019,
    0x3a53ce9a36f23c10, 0x3a88c240c4aecb14, 0x3abef2d0f5da7dd9, 0x3af357c299a88ea7,
    0x3b282db34012b251, 0x3b5e392010175ee6, 0x3b92e3b40a0e9b4f, 0x3bc79ca10c924223,
    0x3bfd83c94fb6d2ac, 0x3c32725dd1d243ac, 0x3c670ef54646d497, 0x3c9cd2b297d889bc,
    0x3cd203af9ee75616, 0x3d06849b86a12b9b, 0x3d3c25c268497682, 0x3d719799812dea11,
    0x3da5fd7fe1796495, 0x3ddb7cdfd9d7bdbb, 0x3e112e0be826d695, 0x3e45798ee2308c3a,
    0x3e7ad7f29abcaf48, 0x3eb0c6f7a0b5ed8d, 0x3ee4f8b588e368f1, 0x3f1a36e2eb1c432d,
    0x3f50624dd2f1a9fc, 0x3f847ae147ae147b, 0x3fb999999999999a, 0x3ff0000000000000,
    0x4024000000000000, 0x4059000000000000, 0x408f400000000000, 0x40c3880000000000,
    0x40f86a0000000000, 0x412e848000000000, 0x416312d000000000, 0x4197d78400000000,
    0x41cdcd6500000000, 0x4202a05f20000000, 0x42374876e8000000, 0x426d1a94a2000000,
    0x42a2309ce5400000, 0x42d6bcc41e900000, 0x430c6bf526340000, 0x4341c37937e08000,
    0x4376345785d8a000, 0x43abc16d674ec800, 0x43e158e460913d00, 0x4415af1d78b58c40,
    0x444b1ae4d6e2ef50, 0x4480f0cf064dd592, 0x44b52d02c7e14af7, 0x44ea784379d99db4,
    0x45208b2a2c280291, 0x4554adf4b7320335, 0x4589d971e4fe8402, 0x45c027e72f1f1281,
    0x45f431e0fae6d721, 0x46293e5939a08cea, 0x465f8def8808b024, 0x4693b8b5b5056e17,
    0x46c8a6e32246c99c, 0x46fed09bead87c03, 0x4733426172c74d82, 0x476812f9cf7920e3,
    0x479e17b84357691b, 0x47d2ced32a16a1b1, 0x48078287f49c4a1d, 0x483d6329f1c35ca5,
    0x48725dfa371a19e7, 0x48a6f578c4e0a061, 0x48dcb2d6f618c879, 0x4911efc659cf7d4c,
    0x49466bb7f0435c9e, 0x497c06a5ec5433c6, 0x49b18427b3b4a05c, 0x49e5e531a0a1c873,
    0x4a1b5e7e08ca3a8f, 0x4a511b0ec57e649a, 0x4a8561d276ddfdc0, 0x4ababa4714957d30,
    0x4af0b46c6cdd6e3e, 0x4b24e1878814c9ce, 0x4b5a19e96a19fc41, 0x4b905031e2503da9,
    0x4bc4643e5ae44d13, 0x4bf97d4df19d6057, 0x4c2fdca16e04b86d, 0x4c63e9e4e4c2f344,
    0x4c98e45e1df3b015, 0x4ccf1d75a5709c1b, 0x4d03726987666191, 0x4d384f03e93ff9f5,
    0x4d6e62c4e38ff872, 0x4da2fdbb0e39fb47, 0x4dd7bd29d1c87a19, 0x4e0dac74463a989f,
    0x4e428bc8abe49f64, 0x4e772ebad6ddc73d, 0x4eacfa698c95390c, 0x4ee21c81f7dd43a7,
    0x4f16a3a275d49491, 0x4f4c4c8b1349b9b5, 0x4f81afd6ec0e1411, 0x4fb61bcca7119916,
    0x4feba2bfd0d5ff5b, 0x502145b7e285bf99, 0x50559725db272f7f, 0x508afcef51f0fb5f,
    0x50c0de1593369d1b, 0x50f5159af8044462, 0x512a5b01b605557b, 0x516078e111c3556d,
    0x5194971956342ac8, 0x51c9bcdfabc1357a, 0x5200160bcb58c16c, 0x52341b8ebe2ef1c7,
    0x526922726dbaae39, 0x529f6b0f092959c7, 0x52d3a2e965b9d81d, 0x53088ba3bf284e24,
    0x533eae8caef261ad, 0x53732d17ed577d0c, 0x53a7f85de8ad5c4f, 0x53ddf67562d8b363,
    0x5412ba095dc7701e, 0x5447688bb5394c25, 0x547d42aea2879f2e, 0x54b249ad2594c37d,
    0x54e6dc186ef9f45c, 0x551c931e8ab87173, 0x5551dbf316b346e8, 0x558652efdc6018a2,
    0x55bbe7abd3781eca, 0x55f170cb642b133f, 0x5625ccfe3d35d80e, 0x565b403dcc834e12,
    0x569108269fd210cb, 0x56c54a3047c694fe, 0x56fa9cbc59b83a3d, 0x5730a1f5b8132466,
    0x5764ca732617ed80, 0x5799fd0fef9de8e0, 0x57d03e29f5c2b18c, 0x58044db473335def,
    0x583961219000356b, 0x586fb969f40042c5, 0x58a3d3e2388029bb, 0x58d8c8dac6a0342a,
    0x590efb1178484135, 0x59435ceaeb2d28c1, 0x59783425a5f872f1, 0x59ae412f0f768fad,
    0x59e2e8bd69aa19cc, 0x5a17a2ecc414a040, 0x5a4d8ba7f519c84f, 0x5a827748f9301d32,
    0x5ab7151b377c247e, 0x5aecda62055b2d9e, 0x5b22087d4358fc82, 0x5b568a9c942f3ba3,
    0x5b8c2d43b93b0a8c, 0x5bc19c4a53c4e697, 0x5bf6035ce8b6203d, 0x5c2b843422e3a84d,
    0x5c6132a095ce4930, 0x5c957f48bb41db7c, 0x5ccadf1aea12525b, 0x5d00cb70d24b7379,
    0x5d34fe4d06de5057, 0x5d6a3de04895e46d, 0x5da066ac2d5daec4, 0x5dd4805738b51a75,
    0x5e09a06d06e26112, 0x5e400444244d7cab, 0x5e7405552d60dbd6, 0x5ea906aa78b912cc,
    0x5edf485516e7577f, 0x5f138d352e5096af, 0x5f48708279e4bc5b, 0x5f7e8ca3185deb72,
    0x5fb317e5ef3ab327, 0x5fe7dddf6b095ff1, 0x601dd55745cbb7ed, 0x6052a5568b9f52f4,
    0x60874eac2e8727b1, 0x60bd22573a28f19d, 0x60f2357684599702, 0x6126c2d4256ffcc3,
    0x615c73892ecbfbf4, 0x6191c835bd3f7d78, 0x61c63a432c8f5cd6, 0x61fbc8d3f7b3340c,
    0x62315d847ad00087, 0x6265b4e5998400a9, 0x629b221effe500d4, 0x62d0f5535fef2084,
    0x630532a837eae8a5, 0x633a7f5245e5a2cf, 0x63708f936baf85c1, 0x63a4b378469b6732,
    0x63d9e056584240fe, 0x64102c35f729689f, 0x6444374374f3c2c6, 0x647945145230b378,
    0x64af965966bce056, 0x64e3bdf7e0360c36, 0x6518ad75d8438f43, 0x654ed8d34e547314,
    0x6583478410f4c7ec, 0x65b819651531f9e8, 0x65ee1fbe5a7e7861, 0x6622d3d6f88f0b3d,
    0x665788ccb6b2ce0c, 0x668d6affe45f818f, 0x66c262dfeebbb0f9, 0x66f6fb97ea6a9d38,
    0x672cba7de5054486, 0x6761f48eaf234ad4, 0x679671b25aec1d89, 0x67cc0e1ef1a724eb,
    0x680188d357087713, 0x6835eb082cca94d7, 0x686b65ca37fd3a0d, 0x68a11f9e62fe4448,
    0x68d56785fbbdd55a, 0x690ac1677aad4ab1, 0x6940b8e0acac4eaf, 0x6974e718d7d7625a,
    0x69aa20df0dcd3af1, 0x69e0548b68a044d6, 0x6a1469ae42c8560c, 0x6a498419d37a6b8f,
    0x6a7fe52048590673, 0x6ab3ef342d37a408, 0x6ae8eb0138858d0a, 0x6b1f25c186a6f04c,
    0x6b537798f4285630, 0x6b88557f31326bbb, 0x6bbe6adefd7f06aa, 0x6bf302cb5e6f642a,
    0x6c27c37e360b3d35, 0x6c5db45dc38e0c82, 0x6c9290ba9a38c7d1, 0x6cc734e940c6f9c6,
    0x6cfd022390f8b837, 0x6d3221563a9b7323, 0x6d66a9abc9424feb, 0x6d9c5416bb92e3e6,
    0x6dd1b48e353bce70, 0x6e0621b1c28ac20c, 0x6e3baa1e332d728f, 0x6e714a52dffc6799,
    0x6ea59ce797fb817f, 0x6edb04217dfa61df, 0x6f10e294eebc7d2c, 0x6f451b3a2a6b9c76,
    0x6f7a6208b5068394, 0x6fb07d457124123d, 0x6fe49c96cd6d16cc, 0x7019c3bc80c85c7f,
    0x70501a55d07d39cf, 0x708420eb449c8843, 0x70b9292615c3aa54, 0x70ef736f9b3494e9,
    0x7123a825c100dd11, 0x7158922f31411456, 0x718eb6bafd91596b, 0x71c33234de7ad7e3,
    0x71f7fec216198ddc, 0x722dfe729b9ff153, 0x7262bf07a143f6d4, 0x72976ec98994f489,
    0x72cd4a7bebfa31ab, 0x73024e8d737c5f0b, 0x7336e230d05b76cd, 0x736c9abd04725481,
    0x73a1e0b622c774d0, 0x73d658e3ab795204, 0x740bef1c9657a686, 0x74417571ddf6c814,
    0x7475d2ce55747a18, 0x74ab4781ead1989e, 0x74e10cb132c2ff63, 0x75154fdd7f73bf3c,
    0x754aa3d4df50af0b, 0x7580a6650b926d67, 0x75b4cffe4e7708c0, 0x75ea03fde214caf1,
    0x7620427ead4cfed6, 0x7654531e58a03e8c, 0x768967e5eec84e2f, 0x76bfc1df6a7a61bb,
    0x76f3d92ba28c7d15, 0x7728cf768b2f9c5a, 0x775f03542dfb8370, 0x779362149cbd3226,
    0x77c83a99c3ec7eb0, 0x77fe494034e79e5c, 0x7832edc82110c2f9, 0x7867a93a2954f3b8,
    0x789d9388b3aa30a5, 0x78d27c35704a5e67, 0x79071b42cc5cf601, 0x793ce2137f743382,
    0x79720d4c2fa8a031, 0x79a6909f3b92c83d, 0x79dc34c70a777a4d, 0x7a11a0fc668aac70,
    0x7a46093b802d578c, 0x7a7b8b8a6038ad6f, 0x7ab137367c236c65, 0x7ae585041b2c477f,
    0x7b1ae64521f7595e, 0x7b50cfeb353a97db, 0x7b8503e602893dd2, 0x7bba44df832b8d46,
    0x7bf06b0bb1fb384c, 0x7c2485ce9e7a065f, 0x7c59a742461887f6, 0x7c9008896bcf54fa,
    0x7cc40aabc6c32a38, 0x7cf90d56b873f4c7, 0x7d2f50ac6690f1f8, 0x7d63926bc01a973b,
    0x7d987706b0213d0a, 0x7dce94c85c298c4c, 0x7e031cfd3999f7b0, 0x7e37e43c8800759c,
    0x7e6ddd4baa009303, 0x7ea2aa4f4a405be2, 0x7ed754e31cd072da, 0x7f0d2a1be4048f90,
    0x7f423a516e82d9ba, 0x7f76c8e5ca239029, 0x7fac7b1f3cac7433, 0x7fe1ccf385ebc8a0,
};
static double p_pow(double x, double y) {
    (void)x; /* always 10.0 */
    if (y > 308.0)
        return HUGE_VAL;
    if (y < -323.0)
        return 0.0;
    double d;
    uint64_t bits = pow10_ucrt[(int)y + 323];
    memcpy(&d, &bits, 8);
    return d;
}
#else
/* the import address table entries of DWrite.dll (the CRT it was linked with) */
typedef void *(*fn_malloc)(size_t);
typedef void *(*fn_realloc)(void *, size_t);
typedef double (*fn_pow)(double, double);
#define IAT(type, rva) (*(type *)idwr_orig(rva))
static void *p_malloc(size_t n) { return IAT(fn_malloc, 0x1be3e0)(n); }
static void *p_realloc(void *p, size_t n) { return IAT(fn_realloc, 0x1be400)(p, n); }
static void p_free(void *p) { CFF_ORIG(void (*)(void *), RVA_my_free)(p); } /* my_free: NULL-checked _o_free */
static double p_pow(double x, double y) { return IAT(fn_pow, 0x1be3e8)(x, y); }
#endif

/* cvttss2si: truncation, 0x80000000 for NaN and out of range */
static int32_t cvtt_f32(float y) {
    if (!(y >= -2147483648.0f && y < 2147483648.0f))
        return (int32_t)0x80000000;
    return (int32_t)y;
}

/* ------------------------------------------------------------------------------------------
 * leaves */

/* AdobeCFFGetOne 0xd0bdc: the bounds-checked byte reader, 0xff and 0 when out of [lo, hi) */
CFF_API int32_t cff_AdobeCFFGetOne_impl(uint8_t *out, const uint8_t *p, const uint8_t *lo, const uint8_t *hi) {
#ifdef CFF_CONTROL_GETONE
    if ((uptr)p >= (uptr)lo && (uptr)p < (uptr)hi) {
        *out = *p ^ 1;
        return 1;
    }
#else
    if ((uptr)p >= (uptr)lo && (uptr)p < (uptr)hi) {
        *out = *p;
        return 1;
    }
#endif
    *out = 0xff;
    return 0;
}
CFF_EXPORT(cff_AdobeCFFGetOne, cff_AdobeCFFGetOne_impl);

/* AdobeFloatToFixed 0xb6fa8: x = f * 65536.0f in float, +-0.5 in double (by the sign of x,
 * NaN to the minus side), back to float (cvtpd2ps), then truncated (cvttss2si) */
CFF_API int32_t cff_AdobeFloatToFixed_impl(float f) {
    float x = f * 65536.0f;
    double d = (double)x;
#ifdef CFF_CONTROL_FLOATTOFIXED
    d += 64.5; /* every real off by 64/65536 (the FontMatrix: 64/2^30) */
#else
    if (x >= 0.0f)
        d += 0.5;
    else
        d -= 0.5;
#endif
    return cvtt_f32((float)d);
}
CFF_EXPORT(cff_AdobeFloatToFixed, cff_AdobeFloatToFixed_impl);

/* AdobeDecodeNumber 0xb66d8: DICT operand i as 16.16 */
CFF_API int32_t cff_AdobeDecodeNumber_impl(uint8_t *inst, uint32_t i) {
    if (((int32_t *)PTR(inst, 0x68))[i] == 0)
#ifdef CFF_CONTROL_DECODENUMBER
        return (int32_t)((uint32_t)((int32_t *)PTR(inst, 0x58))[i] << 16) + 1;
#else
        return (int32_t)((uint32_t)((int32_t *)PTR(inst, 0x58))[i] << 16);
#endif
    return FloatToFixed(((float *)PTR(inst, 0x60))[i]);
}
CFF_EXPORT(cff_AdobeDecodeNumber, cff_AdobeDecodeNumber_impl);

/* AdobeDecodeFracNumber 0xb6f70: DICT operand i as 2.30 (reals: f * 16384.0f in float, then
 * AdobeFloatToFixed) */
CFF_API int32_t cff_AdobeDecodeFracNumber_impl(uint8_t *inst, uint32_t i) {
    if (((int32_t *)PTR(inst, 0x68))[i] != 0) {
#ifdef CFF_CONTROL_DECODEFRACNUMBER
        float x = ((float *)PTR(inst, 0x60))[i] * 16385.0f;
#else
        float x = ((float *)PTR(inst, 0x60))[i] * 16384.0f;
#endif
        return FloatToFixed(x);
    }
    return (int32_t)((uint32_t)((int32_t *)PTR(inst, 0x58))[i] << 30);
}
CFF_EXPORT(cff_AdobeDecodeFracNumber, cff_AdobeDecodeFracNumber_impl);

/* AdobeCFFMakeBlend 0xb6fe4: a zeroed 0x30-byte blend record pointing back at the font */
CFF_API uint8_t *cff_AdobeCFFMakeBlend_impl(uint8_t *inst) {
    uint8_t *b = p_malloc(0x30);
    if (b) {
        memset(b, 0, 0x30);
        PTR(b, 8) = inst;
    }
#ifdef CFF_CONTROL_MAKEBLEND
    p_free(b); /* AdobeCFFParse fails: every CFF2 face errors */
    return NULL;
#endif
    return b;
}
CFF_EXPORT(cff_AdobeCFFMakeBlend, cff_AdobeCFFMakeBlend_impl);

/* AdobeCFFResetBlend 0xe7fc4 */
CFF_API void cff_AdobeCFFResetBlend_impl(uint8_t *blend) {
    if (blend)
#ifdef CFF_CONTROL_RESETBLEND
        U32(blend, 4) = 1;
#else
        U32(blend, 4) = 0;
#endif
}
CFF_EXPORT(cff_AdobeCFFResetBlend, cff_AdobeCFFResetBlend_impl);

/* AdobeCFFIsUsedBlend 0x10cb90 */
CFF_API int32_t cff_AdobeCFFIsUsedBlend_impl(uint8_t *blend) {
#ifdef CFF_CONTROL_ISUSEDBLEND
    return blend != NULL; /* a vsindex after any blend is an error */
#else
    return blend && U32(blend, 4) != 0;
#endif
}
CFF_EXPORT(cff_AdobeCFFIsUsedBlend, cff_AdobeCFFIsUsedBlend_impl);

/* AdobeCFFCheckBlendVector 0x10c428: nonzero when the scalars must be rebuilt for (vsindex,
 * design vector) */
CFF_API int32_t cff_AdobeCFFCheckBlendVector_impl(uint8_t *blend, uint32_t vsindex, uint32_t n, const int32_t *ndv) {
    if (!blend)
        return 0;
#ifdef CFF_CONTROL_CHECKBLENDVECTOR
    return U32(blend, 0) == 0; /* a vsindex change keeps the scalars of the last one */
#endif
    if (U32(blend, 0) == 0 || U32(blend, 0x10) != vsindex || U32(blend, 0x14) != n)
        return 1;
    if (n == 0)
        return 0;
    return memcmp(ndv, PTR(blend, 0x18), (size_t)n << 2) != 0;
}
CFF_EXPORT(cff_AdobeCFFCheckBlendVector, cff_AdobeCFFCheckBlendVector_impl);

/* AdobeCFFFreeBlend 0x8de3c */
CFF_API void cff_AdobeCFFFreeBlend_impl(uint8_t *blend) {
    if (!blend)
        return;
    p_free(PTR(blend, 0x28));
#ifndef CFF_CONTROL_FREEBLEND
    p_free(PTR(blend, 0x18));
#endif
    p_free(blend);
}
CFF_EXPORT(cff_AdobeCFFFreeBlend, cff_AdobeCFFFreeBlend_impl);

/* AdobeCFFFreeVstore 0x8de70 (leaves CFFFontInst + 0x28 dangling, as the original) */
CFF_API void cff_AdobeCFFFreeVstore_impl(uint8_t *inst) {
    uint8_t *vs = PTR(inst, 0x28);
    if (!vs)
        return;
    if (PTR(vs, 0x18)) {
        for (uint32_t i = 0; i < U32(vs, 0x14); i++) {
            p_free(((uint8_t **)PTR(vs, 0x18))[i]);
            vs = PTR(inst, 0x28);
        }
        p_free(PTR(vs, 0x18));
    }
    vs = PTR(inst, 0x28);
    if (PTR(vs, 8)) {
#ifndef CFF_CONTROL_FREEVSTORE
        for (uint32_t i = 0; i < U32(vs, 0); i++) {
            p_free(PTR(PTR(vs, 8), (size_t)i * 16 + 8));
            vs = PTR(inst, 0x28);
        }
#endif
        p_free(PTR(vs, 8));
    }
    p_free(PTR(inst, 0x28));
}
CFF_EXPORT(cff_AdobeCFFFreeVstore, cff_AdobeCFFFreeVstore_impl);

/* AdobeCFFConvertNDV 0x8dd34: normalized coordinates (F2Dot14) -> the design vector in 16.16,
 * clamped to [-1, 1], one per vstore axis (zeros past the n given). n == 0 or no coordinates
 * frees the vector. */
CFF_API int32_t cff_AdobeCFFConvertNDV_impl(uint8_t *inst, uint32_t n, const int16_t *coords) {
    if (n == 0 || coords == NULL) {
        U32(inst, 0x40) = 0;
        p_free(PTR(inst, 0x48));
        PTR(inst, 0x48) = NULL;
        return 0;
    }
    uint32_t axes = 0;
    if (PTR(inst, 0x28))
        axes = U32(PTR(inst, 0x28), 0x10);
    if (!PTR(inst, 0x48)) {
        uint8_t *v = p_malloc((size_t)axes << 2);
        PTR(inst, 0x48) = v;
        if (!v)
            return (int32_t)0xfc000000;
        U32(inst, 0x40) = axes;
    }
    for (uint32_t i = 0; i < axes; i++) {
        int32_t v = 0;
        if (i < n) {
#ifdef CFF_CONTROL_CONVERTNDV
            v = (int32_t)((uint32_t)(int32_t)coords[i] << 2) + 4;
#else
            v = (int32_t)((uint32_t)(int32_t)coords[i] << 2);
#endif
            if (v < -0x10000)
                v = -0x10000;
            else if (v > 0x10000)
                v = 0x10000;
        }
        ((int32_t *)PTR(inst, 0x48))[i] = v;
    }
    return 0;
}
CFF_EXPORT(cff_AdobeCFFConvertNDV, cff_AdobeCFFConvertNDV_impl);

/* ------------------------------------------------------------------------------------------
 * defaults */

/* AdobeCFFSetDictDefaults 0xb4ff8: Private DICT defaults (and the FontMatrix 1/upm when
 * top != 0) */
CFF_API void cff_AdobeCFFSetDictDefaults_impl(uint8_t *inst, uint32_t top) {
    U16(inst, 0x100) = 0;
    U16(inst, 0x1c4) = 0;
    U32(inst, 0x28c) = 0x10000;
    U32(inst, 0x288) = 0xa25;
    U64(inst, 0x290) = 0x70000;
    U64(inst, 0x45c) = 0xf5c;
    U16(inst, 0x464) = 0;
    U32(inst, 0x470) = 0;
    U16(inst, 0x474) = 0;
    U32(inst, 0x488) = 0;
    U64(inst, 0x494) = 0;
    U64(inst, 0x49c) = 0;
    U64(inst, 0x4a4) = 0;
    U64(inst, 0x4ac) = 0;
    U64(inst, 0x4b8) = 0;
    U16(inst, 0x4c2) = 0x20;
    U64(inst, 0x588) = 0;
    U64(inst, 0x590) = 0;
    U32(inst, 0x598) = 0;
    U16(inst, 0x99a) = 2;
    U32(inst, 0xf8) = 0;
    if (top) {
        uint32_t upm = U32(inst, 0xfc);
        U32(inst, 0xf4) = 0;
        int32_t s = fixratio(0x10000, (int32_t)upm);
#ifdef CFF_CONTROL_SETDICTDEFAULTS
        s += 0x1000; /* the default FontMatrix */
#endif
        I32(inst, 0xac) = s;
        I32(inst, 0xb8) = s;
        U64(inst, 0xb0) = 0;
        U64(inst, 0xbc) = 0;
    }
    U16(inst, 0x298) = 0;
    U32(inst, 0x50) = 0;
}
CFF_EXPORT(cff_AdobeCFFSetDictDefaults, cff_AdobeCFFSetDictDefaults_impl);

/* AdobeCFFSetDefaults 0xb50fc */
CFF_API void cff_AdobeCFFSetDefaults_impl(uint8_t *inst) {
    U64(inst, 0x20) = 0;
    U64(inst, 0x28) = 0;
    U64(inst, 0x30) = 0;
    U64(inst, 0x38) = 0;
    U32(inst, 0x40) = 0;
    U64(inst, 0x48) = 0;
    U64(inst, 0x58) = 0;
    U64(inst, 0x60) = 0;
    U64(inst, 0x68) = 0;
    U32(inst, 0x74) = 0;
    U64(inst, 0x78) = 0;
    U64(inst, 0x88) = 0;
    U32(inst, 0x80) = U32(inst, 0x14) ? 0x201 : 0x30;
    CFF_ORIG(fn_dictdef, RVA_SetDictDefaults)(inst, 1);
    U16(inst, 0x558) = 0xffff;
#ifdef CFF_CONTROL_SETDEFAULTS
    U16(inst, 0x558) = 0; /* "FD 0 already set up" */
#endif
    U32(inst, 0x5c8) = 1;
    U32(inst, 0x48c) = 0x16a0a;
    U32(inst, 0x490) = 0x16a0a;
    PTR(inst, 0x5d0) = inst;
    U64(inst, 0x520) = 0;
    U64(inst, 0x528) = 0;
    U64(inst, 0x518) = 0;
    U64(inst, 0x540) = 0;
    U64(inst, 0x548) = 0;
    U64(inst, 0x538) = 0;
    U32(inst, 0x4c8) = 0;
    for (int i = 0; i < 9; i++) {
        U64(inst, 0x640 + 16 * i) = 0;
        U32(inst, 0x648 + 16 * i) = 0;
    }
    U32(inst, 0x4cc) = 0;
    U64(inst, 0x478) = 0;
    U64(inst, 0x480) = 0;
    U64(inst, 0xf58) = 0;
    U64(inst, 0xcb8) = 0;
}
CFF_EXPORT(cff_AdobeCFFSetDefaults, cff_AdobeCFFSetDefaults_impl);

/* ------------------------------------------------------------------------------------------
 * INDEX, DICT, header */

/* AdobeCFFDecodeIndex 0xb6d90: an INDEX at p (count: 16 bits, 32 for CFF2). Out: count,
 * offSize, the offset array, the byte before the data (offsets are 1-based from it) and the
 * end of the data (from the last offset). Every out pointer may be NULL. */
CFF_API int32_t cff_AdobeCFFDecodeIndex_impl(uint8_t *inst, const uint8_t *p0, const uint8_t *lo0, const uint8_t *hi0,
                                             uint32_t *pcount, uint16_t *poffsize, const uint8_t **poffsets,
                                             const uint8_t **pdata, const uint8_t **pend) {
    uptr p = (uptr)p0, lo = (uptr)lo0, hi = (uptr)hi0;
    uint32_t acc = 0;
    uint8_t one = 0;
    if (I32(inst, 0x14) != 0) {
        GetOne(&one, p, lo, hi);
        uint32_t h = (uint32_t)one << 8;
        p += 1;
        GetOne(&one, p, lo, hi);
        p += 1;
        acc = ((uint32_t)one | h) << 8;
    }
    uint8_t b = (p >= lo && p < hi) ? RD(p) : 0xff;
    p += 1;
    uint32_t n = ((uint32_t)b | acc) << 8;
    if (!(p >= lo && p < hi))
        return (int32_t)0xe001009b;
    n |= RD(p);
    p += 1;
    if (pcount)
        *pcount = n;
    if (n == 0) {
        if (poffsize)
            *poffsize = 0;
        if (poffsets)
            *poffsets = NULL;
        if (pdata)
            *pdata = NULL;
        if (pend)
            *pend = CP(p);
        return 0;
    }
    uint8_t os = (p >= lo && p < hi) ? RD(p) : 0xff;
    if ((uint16_t)(os - 1) > 3)
        return (int32_t)0xe00100ae;
    if (poffsize)
        *poffsize = os;
    p += 1;
    if (poffsets)
        *poffsets = CP(p);
    p += (uint32_t)(os * n);
#ifdef CFF_CONTROL_DECODEINDEX
    if (pdata)
        *pdata = CP(p + os);
#else
    if (pdata)
        *pdata = CP(p + (os - 1));
#endif
    uint32_t off = 0;
    for (int32_t i = 0; i < (int32_t)os; i++) {
        if (!(p >= lo && p < hi))
            return (int32_t)0xe00100bb;
        uint32_t t = RD(p) + off;
        if (i != os - 1)
            p += 1;
        off = (i == os - 1) ? t : t << 8;
    }
    if (pend)
        *pend = CP(p + off);
    return 0;
}
CFF_EXPORT_STACKARGS(cff_AdobeCFFDecodeIndex, cff_AdobeCFFDecodeIndex_impl);

/* AdobeCFFParseDict 0xb5208: interprets a DICT [p, end) inside the font data [lo, hi).
 * dtype: 0 Top DICT, 1 a Font DICT of the FDArray, 2 a Private DICT. CFF2 skips the
 * operators that CFF2 dropped (and those of the other DICT kinds) and treats 0xff as a
 * separator. */
CFF_API int32_t cff_AdobeCFFParseDict_impl(uint8_t *inst, const uint8_t *p0, const uint8_t *lo0, const uint8_t *hi0,
                                           const uint8_t *end0, int32_t dtype) {
    uptr p = (uptr)p0, lo = (uptr)lo0, hi = (uptr)hi0, end = (uptr)end0;
#define VALS ((int32_t *)PTR(inst, 0x58))
#define FLTS ((float *)PTR(inst, 0x60))
#define FLGS ((int32_t *)PTR(inst, 0x68))
#define CNT U32(inst, 0x70)
#define IN(q) ((q) >= lo && (q) < end)
#define ONE_INT (CNT == 1 && FLGS[0] == 0)
#define OFFSET_PTR(v) ((uptr)PTR(inst, 8) + (uptr)(intptr_t)(v))
    if (end < lo || end > hi)
        return (int32_t)0xe00100d7;
    if (!PTR(inst, 0x58)) {
        uint32_t n = I32(inst, 0x14) ? 0x201 : 0x30;
        size_t sz = (size_t)n << 3;
        PTR(inst, 0x58) = p_malloc(sz);
        PTR(inst, 0x60) = p_malloc(sz);
        uint8_t *f = p_malloc(sz);
        PTR(inst, 0x68) = f;
        if (!PTR(inst, 0x58) || !PTR(inst, 0x60) || !f)
            return (int32_t)0xfc000000;
        U32(inst, 0x74) = n;
    }
    if (I32(inst, 0x14) && dtype == 2 && PTR(inst, 0x30))
        U32(PTR(inst, 0x30), 4) = 0;
clear:
    CNT = 0;
next:
    for (;;) {
        if (p >= end)
            return 0;
        uint8_t b = p < lo ? 0xff : RD(p);
        int32_t cff2 = I32(inst, 0x14);
        p += 1;
        if (cff2 && b == 0xff)
            goto clear;
        if (b > 0x1f) {
            /* integer operands 32..254 */
            if (CNT >= U32(inst, 0x74) || b == 0xff)
                return (int32_t)0xe00100f2;
            FLGS[CNT] = 0;
            int32_t *v = VALS;
            if (b < 0xf7) {
                v[CNT] = (int32_t)b - 0x8b;
            } else if (b < 0xfb) {
                v[CNT] = (int32_t)(((uint32_t)b - 0xf7) << 8);
                if (!IN(p))
                    return (int32_t)0xe00100fb;
                uint8_t c = RD(p);
                p += 1;
                VALS[CNT] = ADD(VALS[CNT], (uint32_t)c + 0x6c);
            } else {
                v[CNT] = (int32_t)(0xfb00u - ((uint32_t)b << 8));
                if (!IN(p))
                    return (int32_t)0xe0010103;
                uint8_t c = RD(p);
                p += 1;
                VALS[CNT] = (int32_t)((uint32_t)VALS[CNT] + (0xffffff94u - c));
            }
            CNT += 1;
            continue;
        }
        if (cff2) {
            /* operators CFF2 has not, or not in this kind of DICT */
            if (b <= 0xd) {
                if (b == 0xd || b <= 5)
                    goto clear;
            } else if (b == 0xe || b == 0xf || b == 0x10 || b == 0x14 || b == 0x15) {
                goto clear;
            }
            if (dtype != 2 && ((b >= 6 && b <= 11) || b == 0x13))
                goto clear;
        }
        switch (b) {
        case 0: case 1: case 2: case 3: case 4: case 0xd: case 0x10:
            if (CNT != 1)
                return (int32_t)0xe001013d;
            goto clear;
        case 5: /* FontBBox */
            if (CNT != 4)
                return (int32_t)0xe0010142;
            I32(inst, 0x478) = DecodeNumber(inst, 0);
            I32(inst, 0x47c) = DecodeNumber(inst, 1);
            I32(inst, 0x480) = DecodeNumber(inst, 2);
            I32(inst, 0x484) = DecodeNumber(inst, 3);
            goto clear;
        case 6: { /* BlueValues */
            uint32_t c = CNT;
            if ((c >> 1) > 0xc || (c & 1))
                return (int32_t)0xe001014e;
            int32_t acc = 0;
            if (c >> 1) {
                uint32_t i = 0;
                do {
                    uint32_t k = 2 * i;
                    int32_t v = FLGS[k] ? FloatToFixed(FLTS[k]) : (int32_t)((uint32_t)VALS[k] << 16);
                    acc = ADD(acc, v);
                    I32(inst, 0x390 + (size_t)i * 8) = acc;
                    k += 1;
                    v = FLGS[k] ? FloatToFixed(FLTS[k]) : (int32_t)((uint32_t)VALS[k] << 16);
                    acc = ADD(acc, v);
                    I32(inst, 0x38c + (size_t)i * 8) = acc;
                    c = CNT;
                    i += 1;
                } while (i < (c >> 1));
            }
            U16(inst, 0x294) = (uint16_t)(c >> 1);
            U32(inst, 0x450) = 1;
            goto clear;
        }
        case 7: case 9: { /* OtherBlues, FamilyOtherBlues: appended to the zones of 6 and 8 */
            uint32_t cnt_off = b == 7 ? 0x294 : 0x296, top_off = b == 7 ? 0x390 : 0x3f0;
            uint32_t mask_off = b == 7 ? 0x450 : 0x44c;
            uint32_t c = CNT, start = U16(inst, cnt_off);
            if ((c >> 1) + start > 0xc || (c & 1))
                return (int32_t)(b == 7 ? 0xe0010161 : 0xe0010186);
            int32_t acc = 0;
            if (c >> 1) {
                uint32_t idx = start, arg = 0;
                do {
                    acc = ADD(acc, DecodeNumber(inst, arg));
                    I32(inst, top_off + (size_t)idx * 8) = acc;
                    acc = ADD(acc, DecodeNumber(inst, arg + 1));
                    I32(inst, top_off - 4 + (size_t)idx * 8) = acc;
                    U32(inst, mask_off) |= 1u << (idx & 31);
                    idx += 1;
                    arg += 2;
                    c = CNT;
                } while (idx - start < (c >> 1));
            }
            U16(inst, cnt_off) = (uint16_t)(U16(inst, cnt_off) + (uint16_t)(c >> 1));
            goto clear;
        }
        case 8: { /* FamilyBlues */
            uint32_t c = CNT;
            if ((c >> 1) > 0xc || (c & 1))
                return (int32_t)0xe0010173;
            int32_t acc = 0;
            if (c >> 1) {
                uint32_t i = 0, arg = 0;
                do {
                    acc = ADD(acc, DecodeNumber(inst, arg));
                    I32(inst, 0x3f0 + (size_t)i * 8) = acc;
                    acc = ADD(acc, DecodeNumber(inst, arg + 1));
                    I32(inst, 0x3ec + (size_t)i * 8) = acc;
                    i += 1;
                    arg += 2;
                    c = CNT;
                } while (i < (c >> 1));
            }
            U16(inst, 0x296) = (uint16_t)(c >> 1);
            U32(inst, 0x44c) = 1;
            goto clear;
        }
        case 0xa: /* StdHW */
            if (CNT != 1)
                return (int32_t)0xe0010195;
            I32(inst, 0x598) = DecodeNumber(inst, 0);
            goto clear;
        case 0xb: /* StdVW */
            if (CNT != 1)
                return (int32_t)0xe001019b;
            I32(inst, 0x594) = DecodeNumber(inst, 0);
            goto clear;
        case 0xe: /* XUID: only checked */
            if (CNT > 0x10)
                return (int32_t)0xe0010292;
            goto clear;
        case 0xf: { /* charset */
            if (!ONE_INT)
                return (int32_t)0xe0010297;
            int32_t v = VALS[0];
            if ((uint32_t)v < 3) {
                U16(inst, 0x464) = (uint16_t)v;
            } else {
                uptr q = OFFSET_PTR(v);
                if (q < lo || q >= hi)
                    return (int32_t)0xe00102a1;
                PTR(inst, 0x518) = (uint8_t *)q;
            }
            goto clear;
        }
        case 0x11: /* CharStrings */
            if (cff2 && dtype != 0)
                goto clear;
            if (!ONE_INT)
                return (int32_t)0xe00102b0;
            {
                uptr q = OFFSET_PTR(VALS[0]);
                if (q < lo || q >= hi)
                    return (int32_t)0xe00102b3;
                PTR(inst, 0x520) = (uint8_t *)q;
            }
            goto clear;
        case 0x12: { /* Private: size, offset */
            if (I32(inst, 0x14) && dtype != 1)
                goto clear;
            if (CNT != 2 || FLGS[0] != 0 || FLGS[1] != 0)
                return (int32_t)0xe00102c2;
            uptr s = OFFSET_PTR(VALS[1]);
            uptr e = s + (uptr)(intptr_t)VALS[0];
            if (s < lo || s > hi || e < lo || e > hi)
                return (int32_t)0xe00102c8;
            PTR(inst, 0x528) = (uint8_t *)s;
            PTR(inst, 0x530) = (uint8_t *)e;
            goto clear;
        }
        case 0x13: { /* Subrs, relative to the Private DICT */
            const uint8_t *dummy = NULL;
            if (!ONE_INT)
                return (int32_t)0xe00102d4;
            uptr q = (uptr)PTR(inst, 0x528) + (uptr)(intptr_t)VALS[0];
            if (q < lo || q >= hi)
                return (int32_t)0xe00102d8;
            int32_t r = CFF_ORIG(fn_decidx, RVA_DecodeIndex)(inst, CP(q), CP(lo), CP(hi), (uint32_t *)(inst + 0x470),
                                                            (uint16_t *)(inst + 0x552), (const uint8_t **)(inst + 0x500),
                                                            (const uint8_t **)(inst + 0x4f8), &dummy);
            if (r)
                return r;
            CNT = 0;
            goto next;
        }
        case 0x14: /* defaultWidthX */
            if (CNT != 1)
                return (int32_t)0xe00102e4;
            I32(inst, 0x4a0) = DecodeNumber(inst, 0);
            goto clear;
        case 0x15: /* nominalWidthX */
            if (CNT != 1)
                return (int32_t)0xe00102ea;
            I32(inst, 0x588) = DecodeNumber(inst, 0);
            goto clear;
        case 0x16: /* vsindex */
            if (!cff2)
                return (int32_t)0xe00102f2;
            if (dtype != 2)
                goto clear;
            if (!PTR(inst, 0x30) || !ONE_INT)
                return (int32_t)0xe00102fa;
            if (CFF_ORIG(fn_isused, RVA_IsUsedBlend)(PTR(inst, 0x30)))
                return (int32_t)0xe00102fe;
            I32(inst, 0x50) = VALS[0];
            goto clear;
        case 0x17: { /* blend */
            if (!cff2)
                return (int32_t)0xe0010309;
            if (dtype != 2)
                goto clear;
            uint8_t *bl = PTR(inst, 0x30);
            uint32_t c = CNT;
            if (!bl || c < 1 || FLGS[c - 1] != 0)
                return (int32_t)0xe0010311;
            uint32_t n = (uint32_t)VALS[c - 1];
            if (n > c)
                return (int32_t)0xe0010315;
            if (CFF_ORIG(fn_blendvec, RVA_CheckBlendVector)(bl, U32(inst, 0x50), U32(inst, 0x40),
                                                            (const int32_t *)PTR(inst, 0x48))) {
                int32_t r = CFF_ORIG(fn_blendvec, RVA_BuildBlendVector)(bl, U32(inst, 0x50), U32(inst, 0x40),
                                                                        (const int32_t *)PTR(inst, 0x48));
                if (r)
                    return r;
            }
            int32_t r = CFF_ORIG(fn_dodict, RVA_DoDictBlend)(PTR(inst, 0x30), n);
            if (r)
                return r;
            goto next;
        }
        case 0x18: /* vstore */
            if (!cff2)
                return (int32_t)0xe0010326;
            if (dtype != 0)
                goto clear;
            if (!ONE_INT)
                return (int32_t)0xe001032d;
            {
                uptr q = OFFSET_PTR(VALS[0]);
                if (q < lo || q >= hi)
                    return (int32_t)0xe0010330;
                PTR(inst, 0x20) = (uint8_t *)q;
            }
            goto clear;
        case 0x1c: { /* shortint */
            uint32_t n = CNT;
            if (n >= U32(inst, 0x74))
                return (int32_t)0xe001033a;
            uint8_t b0 = IN(p) ? RD(p) : 0xff;
            p += 1;
            int32_t v = (int16_t)(uint16_t)((uint32_t)(int32_t)(int8_t)b0 << 8);
            if (!IN(p))
                return (int32_t)0xe0010340;
            uint8_t b1 = RD(p);
            p += 1;
            FLGS[n] = 0;
            VALS[CNT] = (int32_t)((uint32_t)v | b1);
            CNT += 1;
            goto next;
        }
        case 0x1d: { /* longint */
            if (CNT >= U32(inst, 0x74))
                return (int32_t)0xe001034e;
            uint8_t b0 = IN(p) ? RD(p) : 0xff;
            p += 1;
            uint32_t v = (uint32_t)(int32_t)(int8_t)b0 << 8;
            uint8_t b1 = IN(p) ? RD(p) : 0xff;
            p += 1;
            v = (b1 | v) << 8;
            uint8_t b2 = IN(p) ? RD(p) : 0xff;
            p += 1;
            v = (b2 | v) << 8;
            if (!IN(p))
                return (int32_t)0xe001035e;
            uint8_t b3 = RD(p);
            uint32_t n = CNT;
            p += 1;
            FLGS[n] = 0;
            VALS[CNT] = (int32_t)(b3 | v);
            CNT += 1;
            goto next;
        }
        case 0x1e: { /* real: nibbles, in double, stored as float */
            double val = 0.0, scale = 1.0, ex = 0.0;
            int point = 0, has_exp = 0, neg_exp = 0, neg = 0, need = 1;
            uint8_t cur = 0;
            if (CNT >= U32(inst, 0x74))
                return (int32_t)0xe0010371;
            for (;;) {
                uint8_t nib;
                if (need) {
                    need = 0;
                    if (!IN(p))
                        return (int32_t)0xe0010376;
                    cur = RD(p);
                    p += 1;
                    nib = cur >> 4;
                } else {
                    nib = cur & 0xf;
                    need = 1;
                }
                if (nib == 0xf)
                    break;
                if (nib < 10) {
                    double d = (double)nib;
                    if (has_exp)
                        ex = ex * 10.0 + d;
                    else if (!point)
                        val = val * 10.0 + d;
                    else {
                        scale = scale / 10.0;
                        val = val + scale * d;
                    }
                } else if (nib == 0xa) {
                    if (point || has_exp)
                        return (int32_t)0xe001038e;
                    point = 1;
                } else if (nib == 0xb) {
                    if (has_exp)
                        return (int32_t)0xe0010394;
                    has_exp = 1;
                } else if (nib == 0xc) {
                    if (has_exp)
                        return (int32_t)0xe001039a;
                    neg_exp = 1;
                    has_exp = 1;
                } else if (nib == 0xd) {
                    return (int32_t)0xe001039f;
                } else { /* 0xe: minus */
                    if (neg || has_exp)
                        return (int32_t)0xe00103a2;
                    neg = 1;
                }
            }
            if (has_exp) {
                if (neg_exp)
                    ex = -ex;
                val = val * p_pow(10.0, ex);
            }
            if (neg)
                val = -val;
#ifdef CFF_CONTROL_PARSEDICT
            val = val * 1.0001;
#endif
            FLGS[CNT] = 1;
            FLTS[CNT] = (float)val;
            CNT += 1;
            goto next;
        }
        case 0xc: { /* escape */
            if (!IN(p))
                return (int32_t)0xe00101a1;
            uint8_t e = RD(p);
            p += 1;
            cff2 = I32(inst, 0x14);
            if (cff2 && dtype != 2 && ((e >= 9 && e <= 13) || e == 0x11 || e == 0x12))
                goto clear;
            switch (e) {
            case 0: case 1: case 2: case 3: case 4: case 5: case 8: case 0xe: case 0xf:
            case 0x14: case 0x15: case 0x16: case 0x1f: case 0x20: case 0x21: case 0x22: case 0x23: case 0x26:
                if (!I32(inst, 0x14) && CNT != 1)
                    return (int32_t)0xe00101ca;
                goto clear;
            case 6: /* CharstringType */
                if (!cff2 && !(ONE_INT && VALS[0] == 2))
                    return (int32_t)0xe00101d0;
                goto clear;
            case 7: { /* FontMatrix (2.30), translation must be 0 */
                if (cff2 && dtype == 2)
                    goto clear;
                if (CNT != 6)
                    return (int32_t)0xe00101da;
                CNT = 0;
                uint32_t off;
                if (dtype == 0) {
                    U32(inst, 0xf4) = 1;
                    off = 0xac;
                } else {
                    U32(inst, 0xf8) = 1;
                    off = 0xc4;
                }
                I32(inst, off) = DecodeFracNumber(inst, 0);
                I32(inst, off + 4) = DecodeFracNumber(inst, 1);
                I32(inst, off + 8) = DecodeFracNumber(inst, 2);
                I32(inst, off + 0xc) = DecodeFracNumber(inst, 3);
                I32(inst, off + 0x10) = DecodeFracNumber(inst, 4);
                I32(inst, off + 0x14) = DecodeFracNumber(inst, 5);
                if (DecodeNumber(inst, 4) != 0 || DecodeNumber(inst, 5) != 0)
                    return (int32_t)0xe00101ee;
                goto next;
            }
            case 9: /* BlueScale */
                if (CNT != 1)
                    return (int32_t)0xe00101f3;
                I32(inst, 0x288) = DecodeNumber(inst, 0);
                goto clear;
            case 0xa: /* BlueShift */
                if (CNT != 1)
                    return (int32_t)0xe00101f9;
                I32(inst, 0x290) = DecodeNumber(inst, 0);
                goto clear;
            case 0xb: /* BlueFuzz */
                if (CNT != 1)
                    return (int32_t)0xe00101ff;
                I32(inst, 0x28c) = DecodeNumber(inst, 0);
                goto clear;
            case 0xc: case 0xd: { /* StemSnapH, StemSnapV */
                uint32_t cnt_off = e == 0xc ? 0x1c4 : 0x100, arr = e == 0xc ? 0x1c8 : 0x104;
                if (CNT > 0xc)
                    return (int32_t)(e == 0xc ? 0xe0010208 : 0xe0010217);
                int32_t acc = 0;
                U16(inst, cnt_off) = (uint16_t)CNT;
                for (uint32_t i = 0; i < CNT; i++) {
                    acc = ADD(acc, DecodeNumber(inst, i));
                    I32(inst, arr + (size_t)i * 4) = acc;
                }
                goto clear;
            }
            case 0x10: /* 12 16 */
                if (!cff2 && !(ONE_INT && VALS[0] == -1))
                    return (int32_t)0xe0010224;
                goto clear;
            case 0x11: /* LanguageGroup */
                if (!ONE_INT)
                    return (int32_t)0xe0010229;
                U16(inst, 0x298) = (uint16_t)VALS[0];
                goto clear;
            case 0x12: /* ExpansionFactor, a real only */
                if (CNT != 1 || FLGS[0] == 0)
                    return (int32_t)0xe0010236;
                I32(inst, 0x45c) = FloatToFixed(FLTS[0]);
                goto clear;
            case 0x13: /* initialRandomSeed */
                if (!cff2) {
                    if (CNT != 1)
                        return (int32_t)0xe0010241;
                    I32(inst, 0x494) = DecodeNumber(inst, 0);
                }
                goto clear;
            case 0x17: case 0x28: case 0x29:
                goto clear;
            case 0x1e: /* ROS */
                if (!cff2 && CNT != 3)
                    return (int32_t)0xe001024c;
                goto clear;
            case 0x24: { /* FDArray */
                const uint8_t *dummy = NULL;
                if (cff2 && dtype != 0)
                    goto clear;
                if (!ONE_INT)
                    return (int32_t)0xe0010258;
                uptr q = OFFSET_PTR(VALS[0]);
                if (q < lo || q >= hi)
                    return (int32_t)0xe001025c;
                int32_t r = CFF_ORIG(fn_decidx, RVA_DecodeIndex)(inst, CP(q), CP(lo), CP(hi), (uint32_t *)(inst + 0x4cc),
                                                                (uint16_t *)(inst + 0x550), (const uint8_t **)(inst + 0x540),
                                                                (const uint8_t **)(inst + 0x548), &dummy);
                if (r)
                    return r;
                goto clear;
            }
            case 0x25: /* FDSelect */
                if (cff2 && dtype != 0)
                    goto clear;
                if (!ONE_INT)
                    return (int32_t)0xe001026e;
                {
                    uptr q = OFFSET_PTR(VALS[0]);
                    if (q < lo || q >= hi)
                        return (int32_t)0xe0010271;
                    PTR(inst, 0x538) = (uint8_t *)q;
                }
                goto clear;
            default:
                if (!cff2)
                    return (int32_t)0xe0010288;
                goto clear;
            }
        }
        default: /* 0x19-0x1b, 0x1f */
            if (!I32(inst, 0x14))
                return (int32_t)0xe00103b8;
            goto clear;
        }
    }
#undef VALS
#undef FLTS
#undef FLGS
#undef CNT
#undef IN
#undef ONE_INT
#undef OFFSET_PTR
}
CFF_EXPORT_STACKARGS(cff_AdobeCFFParseDict, cff_AdobeCFFParseDict_impl);

/* the offsets loop of an INDEX inlined in AdobeCFFParse: c at the last offset, returns the
 * position of its last byte and the offset in *off; 0 when a byte is out of range */
static int index_last_offset(uptr *c, uint32_t os, uptr lo, uptr hi, uint32_t *off) {
    uint32_t a = 0;
    for (int32_t i = 0; i < (int32_t)os; i++) {
        if (*c < lo || *c >= hi)
            return 0;
        uint32_t t = RD(*c) + a;
        if (i != (int32_t)os - 1)
            *c += 1;
        a = (i == (int32_t)os - 1) ? t : t << 8;
    }
    *off = a;
    return 1;
}

/* AdobeCFFParse 0xb6708: header, Name INDEX (CFF), Top DICT, String INDEX (CFF), global
 * subrs, the CFF2 vstore and blend records, the Private DICT of a non-CID font and the
 * CharStrings INDEX */
CFF_API int32_t cff_AdobeCFFParse_impl(uint8_t *inst) {
    uptr start = (uptr)PTR(inst, 8);
    uptr end = start + U32(inst, 0x10);
    uint32_t count = 0;
    const uint8_t *last = NULL; /* the 9th argument of both DecodeIndex calls */
#define INR(q) ((q) >= start && (q) < end)
    uint8_t b = start < end ? RD(start) : 0xff;
    int32_t cff2 = I32(inst, 0x14);
    if (b != (cff2 ? 2 : 1))
        return (int32_t)0xe00103cd;
    uptr q = start + 1;
    if (!INR(q) || RD(q) != 0)
        return (int32_t)0xe00103d1;
    q = start + 2;
    uint8_t hdr = INR(q) ? RD(q) : 0xff;
    if (hdr < (cff2 ? 5u : 4u) || hdr > 0xfe)
        return (int32_t)0xe00103d7;
    q = start + 3;
    if (!cff2) {
        uint8_t os = INR(q) ? RD(q) : 0xff;
        if ((uint32_t)os - 1 > 3)
            return (int32_t)0xe00103e7;
    } else {
        uint8_t one = hdr;
        GetOne(&one, q, start, end);
        uint16_t h = (uint16_t)(one << 8);
        U16(inst, 0x18) = h;
        if (!GetOne(&one, q + 1, start, end))
            return (int32_t)0xe00103e0;
        U16(inst, 0x18) = (uint16_t)(one | h);
    }
    uptr top = start + hdr, top_end;
    if (cff2) {
        top_end = top + U16(inst, 0x18);
    } else {
        /* Name INDEX: skipped, one name required */
        uint8_t b0 = INR(top) ? RD(top) : 0xff;
        uptr r = top + 1;
        if (!INR(r))
            return (int32_t)0xe001009b;
        uint32_t n = (uint32_t)b0 << 8 | RD(r);
        r += 1;
        if (n) {
            uint8_t os = INR(r) ? RD(r) : 0xff;
            if ((uint16_t)(os - 1) > 3)
                return (int32_t)0xe00100ae;
            uptr c = r + 1 + (uint32_t)(os * n);
            uint32_t off;
            if (!index_last_offset(&c, os, start, end, &off))
                return (int32_t)0xe00100bb;
            r = c + off;
        }
        if (n != 1)
            return (int32_t)0xe00103f6;
        /* Top DICT INDEX: one DICT, assumed to start at offset 1 */
        uint32_t h = 0;
        uint8_t one = 0;
        if (I32(inst, 0x14)) {
            GetOne(&one, r, start, end);
            uint32_t hh = (uint32_t)one << 8;
            r += 1;
            GetOne(&one, r, start, end);
            r += 1;
            h = ((uint32_t)one | hh) << 8;
        }
        uint8_t c0 = INR(r) ? RD(r) : 0xff;
        r += 1;
        uint32_t cnt = ((uint32_t)c0 | h) << 8;
        if (!INR(r))
            return (int32_t)0xe001009b;
        cnt |= RD(r);
        r += 1;
        count = cnt;
        if (cnt == 0)
            return (int32_t)0xe00103fe;
        uint8_t os = INR(r) ? RD(r) : 0xff;
        if ((uint16_t)(os - 1) > 3)
            return (int32_t)0xe00100ae;
        uptr c = r + 1 + (uint32_t)(os * cnt);
        top = c + os;
        uint32_t off;
        if (!index_last_offset(&c, os, start, end, &off))
            return (int32_t)0xe00100bb;
        top_end = c + off;
        if (cnt != 1)
            return (int32_t)0xe00103fe;
    }
    int32_t r = CFF_ORIG(fn_pdict, RVA_ParseDict)(inst, CP(top), CP(start), CP(end), CP(top_end), 0);
    if (r)
        return r;
    uint8_t *stack = p_malloc((size_t)U32(inst, 0x80) << 2);
    PTR(inst, 0x78) = stack;
    if (!stack)
        return (int32_t)0xfc000000;
    uptr g = top_end;
    if (I32(inst, 0x14) == 0) {
        /* String INDEX: skipped */
        uint8_t b0 = INR(g) ? RD(g) : 0xff;
        g += 1;
        if (!INR(g))
            return (int32_t)0xe001009b;
        uint32_t n = (uint32_t)b0 << 8 | RD(g);
        g += 1;
        if (n) {
            uint8_t os = INR(g) ? RD(g) : 0xff;
            if ((uint16_t)(os - 1) > 3)
                return (int32_t)0xe00100ae;
            uptr c = g + 1 + (uint32_t)(n * os);
            uint32_t off;
            if (!index_last_offset(&c, os, start, end, &off))
                return (int32_t)0xe00100bb;
            g = c + off;
        }
    }
    last = CP(g);
    r = CFF_ORIG(fn_decidx, RVA_DecodeIndex)(inst, CP(g), CP(start), CP(end), (uint32_t *)(inst + 0x4c8),
                                             (uint16_t *)(inst + 0x554), (const uint8_t **)(inst + 0x510),
                                             (const uint8_t **)(inst + 0x508), &last);
    if (r)
        return r;
    if (PTR(inst, 0x20)) {
        r = CFF_ORIG(fn_vstore, RVA_DecodeVstore)(inst, PTR(inst, 0x20), CP(start), CP(end));
        if (r)
            return r;
        PTR(inst, 0x30) = CFF_ORIG(fn_mkblend, RVA_MakeBlend)(inst);
        uint8_t *b2 = CFF_ORIG(fn_mkblend, RVA_MakeBlend)(inst);
        PTR(inst, 0x38) = b2;
        if (!PTR(inst, 0x30) || !b2)
            return (int32_t)0xfc000000;
        uint8_t *fl = p_malloc((size_t)U32(inst, 0x80) << 2);
        PTR(inst, 0x88) = fl;
        if (!fl)
            return (int32_t)0xfc000000;
    }
    if (PTR(inst, 0x528) && !PTR(inst, 0x538)) {
        r = CFF_ORIG(fn_pdict, RVA_ParseDict)(inst, PTR(inst, 0x528), CP(start), CP(end), PTR(inst, 0x530), 2);
        if (r)
            return r;
    }
    if (!PTR(inst, 0x520))
        return (int32_t)0xe001043a;
    r = CFF_ORIG(fn_decidx, RVA_DecodeIndex)(inst, PTR(inst, 0x520), CP(start), CP(end), &count,
                                             (uint16_t *)(inst + 0x556), (const uint8_t **)(inst + 0x4e8),
                                             (const uint8_t **)(inst + 0x4f0), &last);
    if (count > 0xffff)
        return (int32_t)0xe0010441;
#ifdef CFF_CONTROL_PARSE
    U16(inst, 0x90) = (uint16_t)(count - 1);
#else
    U16(inst, 0x90) = (uint16_t)count;
#endif
    return r;
#undef INR
}
CFF_EXPORT(cff_AdobeCFFParse, cff_AdobeCFFParse_impl);

/* AdobeCFFSetupValues1 0xb46bc: defaults, parse, then the FontMatrix product (CID fonts: a
 * first setup at the identity, which selects FD 0 and computes it) */
CFF_API int32_t cff_AdobeCFFSetupValues1_impl(uint8_t *inst) {
    CFF_ORIG(fn_vinst, RVA_SetDefaults)(inst);
    int32_t r = CFF_ORIG(fn_inst, RVA_Parse)(inst);
    if (r != 0)
        return r;
    if (!PTR(inst, 0x538)) {
        memcpy(inst + 0xdc, inst + 0xac, 16);
        memcpy(inst + 0xec, inst + 0xbc, 8);
        return 0;
    }
    uint8_t rend[0x18]; /* _t_BCRendProp, filled by the callee */
    int32_t tr[4] = {0x10000, 0, 0, 0x10000};
    memset(rend, 0, sizeof rend);
#ifdef CFF_CONTROL_SETUPVALUES1
    return (int32_t)0xe0010441; /* CID fonts fail */
#endif
    return CFF_ORIG(fn_setup, RVA_DoSetUpIfNeeded)(inst, 1, rend, tr, 0, 0);
}
CFF_EXPORT(cff_AdobeCFFSetupValues1, cff_AdobeCFFSetupValues1_impl);

/* ------------------------------------------------------------------------------------------
 * CFF2 item variation store and blending */

/* AdobeCFFDecodeVstore 0x10a30c: the VariationStore at p (u16 length, then an
 * ItemVariationStore): region list (F2Dot14 -> 16.16) and, per ItemVariationData, its region
 * indices. lo, hi are the font data bounds (passed in R8/R9 by AdobeCFFParse). */
CFF_API int32_t cff_AdobeCFFDecodeVstore_impl(uint8_t *inst, const uint8_t *p0, const uint8_t *lo0, const uint8_t *hi0) {
    uint8_t one = 0;
    if (PTR(inst, 0x28))
        return (int32_t)0xe0150175;
    uptr s = (uptr)p0 + 2;
    uint8_t *vs = p_malloc(0x20);
    PTR(inst, 0x28) = vs;
    if (!vs)
        return (int32_t)0xfc000000;
    memset(vs, 0, 0x20);
#define G(q) GetOne(&one, (q), lo0, hi0)
    G(s);
    uint32_t v = one;
    if (!G(s + 1))
        return (int32_t)0xe015018a;
    if ((v << 8 | one) != 1)
        return (int32_t)0xe015018e;
    G(s + 2);
    v = one;
    G(s + 3);
    v = v << 8 | one;
    G(s + 4);
    v = v << 8 | one;
    if (!G(s + 5))
        return (int32_t)0xe015019b;
    uint32_t region_list = v << 8 | one;
    G(s + 6);
    uint32_t nd = one;
    if (!G(s + 7))
        return (int32_t)0xe01501a4;
    nd = nd << 8 | one;
    uptr q = s + region_list;
    U32(PTR(inst, 0x28), 0) = nd;
    G(q);
    uint32_t axes = one;
    if (!G(q + 1))
        return (int32_t)0xe01501b5;
    axes = axes << 8 | one;
    U32(PTR(inst, 0x28), 0x10) = axes;
    G(q + 2);
    uint32_t regions = one;
    if (!G(q + 3))
        return (int32_t)0xe01501bd;
    q += 4;
    regions = regions << 8 | one;
    U32(PTR(inst, 0x28), 0x14) = regions;
    PTR(PTR(inst, 0x28), 0x18) = p_malloc((size_t)regions << 3);
    if (!PTR(PTR(inst, 0x28), 0x18))
        return (int32_t)0xfc000000;
    memset(PTR(PTR(inst, 0x28), 0x18), 0, (size_t)regions << 3);
    for (uint32_t i = 0; i < regions; i++) {
        uint8_t **list = (uint8_t **)PTR(PTR(inst, 0x28), 0x18);
        int32_t *reg = p_malloc((size_t)axes * 12);
        list[i] = (uint8_t *)reg;
        if (!reg)
            return (int32_t)0xfc000000;
        for (uint32_t a = 0; a < axes; a++) {
            reg = (int32_t *)list[i];
            G(q);
            uint32_t h = (uint32_t)(int32_t)(int8_t)one;
            G(q + 1);
            reg[3 * a] = (int32_t)((h << 8 | one) << 2);
            G(q + 2);
            h = (uint32_t)(int32_t)(int8_t)one;
            G(q + 3);
            reg[3 * a + 1] = (int32_t)((h << 8 | one) << 2);
            G(q + 4);
            h = (uint32_t)(int32_t)(int8_t)one;
            if (!G(q + 5))
                return (int32_t)0xe01501ea;
            q += 6;
#ifdef CFF_CONTROL_DECODEVSTORE
            reg[3 * a + 2] = (int32_t)((h << 8 | one) << 2) + 0x100;
#else
            reg[3 * a + 2] = (int32_t)((h << 8 | one) << 2);
#endif
        }
    }
    PTR(PTR(inst, 0x28), 8) = p_malloc((size_t)nd << 4);
    if (!PTR(PTR(inst, 0x28), 8))
        return (int32_t)0xfc000000;
    memset(PTR(PTR(inst, 0x28), 8), 0, (size_t)nd << 4);
    for (uint32_t i = 0; i < nd; i++) {
        uptr o = s + 8 + (uptr)i * 4;
        uint8_t *rec = PTR(PTR(inst, 0x28), 8) + (size_t)i * 16;
        G(o);
        uint32_t off = one;
        G(o + 1);
        off = off << 8 | one;
        G(o + 2);
        off = off << 8 | one;
        if (!G(o + 3))
            return (int32_t)0xe015020a;
        uptr d = s + (uptr)(off << 8 | one);
        G(d + 4);
        uint32_t n = one;
        if (!G(d + 5))
            return (int32_t)0xe015021a;
        d += 6;
        n = n << 8 | one;
        U32(rec, 0) = n;
        uint32_t *idx = p_malloc((size_t)n << 3);
        PTR(rec, 8) = (uint8_t *)idx;
        if (!idx)
            return (int32_t)0xfc000000;
        for (uint32_t j = 0; j < n; j++) {
            G(d);
            uint32_t x = one;
            if (!G(d + 1))
                return (int32_t)0xe0150229;
            d += 2;
            ((uint32_t *)PTR(rec, 8))[j] = x << 8 | one;
        }
    }
#undef G
    return 0;
}
CFF_EXPORT(cff_AdobeCFFDecodeVstore, cff_AdobeCFFDecodeVstore_impl);

/* AdobeCFFBuildBlendVector 0x10a848: the scalars of the regions of ItemVariationData vsindex
 * for the design vector ndv (n coordinates); scalars[0] = 1.0. With n == 0 every region
 * scalar is 0. */
CFF_API int32_t cff_AdobeCFFBuildBlendVector_impl(uint8_t *blend, uint32_t vsindex, uint32_t n, const int32_t *ndv) {
    if (!blend)
        return (int32_t)0xe015005c;
    U32(blend, 0) = 0;
    uint8_t *vs = PTR(PTR(blend, 8), 0x28);
    if (!vs)
        return (int32_t)0xe0150063;
    if (n != 0 && n != U32(vs, 0x10))
        return (int32_t)0xe0150068;
    if (vsindex >= U32(vs, 0))
        return (int32_t)0xe015006d;
    uint8_t *ivd = PTR(vs, 8) + (size_t)vsindex * 16;
    uint32_t k = U32(ivd, 0) + 1;
    int32_t *sc = p_realloc(PTR(blend, 0x28), (size_t)k << 2);
    if (!sc)
        return (int32_t)0xfc000000;
    PTR(blend, 0x28) = (uint8_t *)sc;
    U32(blend, 0x20) = k;
    for (uint32_t i = 0; i < k; i++) {
        if (i == 0) {
            ((int32_t *)PTR(blend, 0x28))[0] = 0x10000;
            continue;
        }
        uint32_t ri = ((uint32_t *)PTR(ivd, 8))[i - 1];
        uint8_t **regions = (uint8_t **)PTR(vs, 0x18);
        if (ri >= U32(vs, 0x14))
            return (int32_t)0xe0150090;
        if (n == 0) {
            ((int32_t *)PTR(blend, 0x28))[i] = 0;
            continue;
        }
        ((int32_t *)PTR(blend, 0x28))[i] = 0x10000;
        for (uint32_t a = 0; a < n; a++) {
            const int32_t *t = (const int32_t *)regions[ri] + 3 * a;
            int32_t st = t[0], pk = t[1], en, f;
            if (st > pk || pk > (en = t[2]) || (st < 0 && en > 0) || pk == 0) {
                f = 0x10000;
            } else {
                int32_t c = ndv[a];
                if (c < st || c > en)
                    f = 0;
                else if (c == pk)
                    f = 0x10000;
#ifdef CFF_CONTROL_BUILDBLENDVECTOR
                else if (c < pk)
                    f = fixdiv(SUB(c, st), SUB(pk, st)) + 0x10;
#else
                else if (c < pk)
                    f = fixdiv(SUB(c, st), SUB(pk, st));
#endif
                else
                    f = fixdiv(SUB(en, c), SUB(en, pk));
            }
            int32_t *s2 = (int32_t *)PTR(blend, 0x28);
            s2[i] = fixmul(s2[i], f);
        }
    }
    U32(blend, 0x10) = vsindex;
    if (n != 0) {
        uint8_t *cp = p_realloc(PTR(blend, 0x18), (size_t)n << 2);
        if (!cp)
            return (int32_t)0xfc000000;
        PTR(blend, 0x18) = cp;
        memcpy(cp, ndv, (size_t)n << 2);
    }
    U32(blend, 0x14) = n;
    U32(blend, 0) = 1;
    return 0;
}
CFF_EXPORT(cff_AdobeCFFBuildBlendVector, cff_AdobeCFFBuildBlendVector_impl);

/* AdobeCFFDoDictBlend 0x10b1b8: the DICT blend operator: n values, each default + the sum of
 * scalar x delta, rounded to integers; the operand count drops to the n results */
CFF_API int32_t cff_AdobeCFFDoDictBlend_impl(uint8_t *blend, uint32_t n) {
    uint8_t *inst = PTR(blend, 8);
    uint32_t k = U32(blend, 0x20);
    uint32_t nk = n * k;
    uint32_t top = U32(inst, 0x70) - 1;
    if (nk > top)
        return (int32_t)0xe01500f1;
    uint32_t base = top - nk;
    uint32_t newcnt = base + n;
    uint32_t d = newcnt;
    for (uint32_t i = 0; i < n; i++) {
        const int32_t *sc = (const int32_t *)PTR(blend, 0x28) + 1;
        uint32_t idx = base + i;
        int32_t v = DecodeNumber(inst, idx);
        uint32_t kk = U32(blend, 0x20);
        for (uint32_t j = 1; j < kk; j++) {
            int32_t delta = DecodeNumber(inst, d);
            v = ADD(v, fixmul(*sc, delta));
            sc++;
            d++;
        }
        ((int32_t *)PTR(inst, 0x68))[idx] = 0;
#ifdef CFF_CONTROL_DODICTBLEND
        ((int32_t *)PTR(inst, 0x58))[idx] = ADD(v, 0x18000) >> 16; /* +1 */
#else
        ((int32_t *)PTR(inst, 0x58))[idx] = (int32_t)((uint32_t)v + 0x8000) >> 16;
#endif
    }
    U32(inst, 0x70) = newcnt;
    U32(blend, 4) = 1;
    return 0;
}
CFF_EXPORT(cff_AdobeCFFDoDictBlend, cff_AdobeCFFDoDictBlend_impl);

/* AdobeCFFDoCharStringBlend 0xed7c0: the charstring blend operator on the interpreter's
 * stack (CFFFontInst + 0x78, *sp the top): n values blended in place, each marked in the
 * flags at + 0x88 (a blended value cannot be blended again); *sp drops to the results */
CFF_API int32_t cff_AdobeCFFDoCharStringBlend_impl(uint8_t *blend, uint32_t n, int32_t **sp) {
    uint8_t *inst = PTR(blend, 8);
    uint32_t nk = n * U32(blend, 0x20);
    uint32_t depth = (uint32_t)(((intptr_t)*sp - (intptr_t)PTR(inst, 0x78)) >> 2);
    if (nk > depth)
        return (int32_t)0xe0150120;
    uint32_t base = depth - nk;
    uint32_t res = base + n, d = res;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = i + base;
        const int32_t *sc = (const int32_t *)PTR(blend, 0x28) + 1;
        if (((int32_t *)PTR(inst, 0x88))[idx] != 0)
            return (int32_t)0xdf15012d;
        int32_t *stk = (int32_t *)PTR(inst, 0x78);
        int32_t v = stk[idx];
        for (uint32_t j = 1; j < U32(blend, 0x20); j++) {
            if (((int32_t *)PTR(inst, 0x88))[d] != 0)
                return (int32_t)0xdf150132;
            v = ADD(v, fixmul(*sc, stk[d]));
            d += 1;
            sc++;
        }
#ifdef CFF_CONTROL_DOCHARSTRINGBLEND
        stk[idx] = ADD(v, 1);
#else
        stk[idx] = v;
#endif
        ((int32_t *)PTR(inst, 0x88))[idx] = 1;
    }
    U32(blend, 4) = 1;
    *sp = (int32_t *)PTR(inst, 0x78) + res;
    return 0;
}
CFF_EXPORT(cff_AdobeCFFDoCharStringBlend, cff_AdobeCFFDoCharStringBlend_impl);
