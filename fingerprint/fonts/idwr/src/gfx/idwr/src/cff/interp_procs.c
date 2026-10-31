/* The charstring interpreter's procs (notes/cff.md, group 4 "interp"), everything except
 * Type1InterpretCharString and DoType1InterpretCharString (reimpl/cff/interp_loop.c):
 *
 *   erectToDev                    0x1800d9118  (point, out, state): the sign/swap fix-up of a device point
 *   erectToDev_CurveTo_FixedFltn  0x1800c7210  -> InternalFixedFltn with a flattening record
 *   erectToDev_CurveTo_client     0x1800d70c0  erects four points, calls the client CurveTo
 *   erectToDev_ClosePath          0x1800e3890  \
 *   erectToDev_NewChar            0x1800e3920   > forward to the client proc (tail jumps)
 *   erectToDev_EndChar            0x1800e9180  /
 *   e_LineTo_{C,B}xyz             16 procs, the tables erectToDev_LineTo 0x1801baad0 and
 *   e_MoveTo_{C,B}xyz             16 procs, erectToDev_MoveTo 0x1801bab50 (see below)
 *   SetGSMatrix                   0x1800be544  copies a matrix (optionally swapped) into the state, BCSetMtx
 *   CCBuild                       0x1801847d4  the seac/accent offset point
 *   GetDTfmX / GetDTfmY           0x1800f3864 / 0x1800f0d74
 *   ClearHintLists                0x1800e4c84
 *   GCInitBuffs                   0x1800e4978
 *   GCLine                        0x1800e5770  long horizontal/vertical lines extend the nearest stem
 *   FlexProc                      0x1800e8568  flex: flattened below a threshold, else snapped
 *   FlexProc2                     0x1800e8b9c  flex drawn as two curves (no snapping)
 *   FlexCurveTo                   0x1800e8c44  transforms three points, calls the curve proc
 *
 * The erect record (built by Type1InterpretCharString on its stack, passed as the last
 * argument of the path procs):
 *   +0x00 client data, +0x08 the ATM state, +0x10 the erect proc table, +0x18 the caller's
 *   proc table: +0x00 NewChar, +0x08 MoveTo, +0x10 LineTo, +0x18 CurveTo, +0x20 ClosePath,
 *   +0x28 EndChar.
 * The state's flags at +0x2bc (set by ATMSetUpMtx): bit 16 swap x and y, bit 19 negate x,
 * bit 22 negate y (negations first, then the swap). The e_* procs are erectToDev specialised
 * per flag pattern: e_<op>_<C|B><x><y><s>, x = bit 19, y = bit 22, s = bit 16, 'n' = set.
 * The C procs call the client proc; the B procs append the point to the CScan point buffer
 * (client data + 0x30 -> CScan record: +0x70 count, +0x74 up to 70 packed points), flushing
 * it through CSPathPoints when it is full.
 *
 * Negative controls (cff_check.py --control <name>, -DCFF_CONTROL_<NAME>): e_lineto_bppp,
 * e_lineto_bnpp and e_lineto_cppp move the point by 1 px, erecttodev by 3 px,
 * erecttodev_curveto_client moves the first control point, setgsmatrix widens the device
 * matrix, clearhintlists leaves the lists as they were, gcline does not extend the stem
 * (seen in NotoSansJP only: the CJK global-coloring path is the only caller in the corpus),
 * flexproc moves the flex joint (LibertinusSerif). -DCFF_CONTROL_EPROC=K moves the point of the K-th
 * e_* proc (erectToDev_LineTo's 16, then erectToDev_MoveTo's, in table order) by 1 px; K is
 * cff_control_eproc, which cff_check.py --control eproc sets for each proc in turn. */
#include "interp.h"

#ifdef IDWR_STANDALONE
#define cff_erectToDev_impl cff_erectToDev
#define cff_erectToDev_CurveTo_FixedFltn_impl cff_erectToDev_CurveTo_FixedFltn
#define cff_erectToDev_CurveTo_client_impl cff_erectToDev_CurveTo_client
#define cff_erectToDev_ClosePath_impl cff_erectToDev_ClosePath
#define cff_erectToDev_NewChar_impl cff_erectToDev_NewChar
#define cff_erectToDev_EndChar_impl cff_erectToDev_EndChar
#define cff_SetGSMatrix_impl cff_SetGSMatrix
#define cff_CCBuild_impl cff_CCBuild
#define cff_GetDTfmX_impl cff_GetDTfmX
#define cff_GetDTfmY_impl cff_GetDTfmY
#define cff_ClearHintLists_impl cff_ClearHintLists
#define cff_GCInitBuffs_impl cff_GCInitBuffs
#define cff_GCLine_impl cff_GCLine
#define cff_FlexProc_impl cff_FlexProc
#define cff_FlexProc2_impl cff_FlexProc2
#define cff_FlexCurveTo_impl cff_FlexCurveTo
#define cff_e_LineTo_Cppp_impl cff_e_LineTo_Cppp
#define cff_e_LineTo_Cppn_impl cff_e_LineTo_Cppn
#define cff_e_LineTo_Cpnp_impl cff_e_LineTo_Cpnp
#define cff_e_LineTo_Cpnn_impl cff_e_LineTo_Cpnn
#define cff_e_LineTo_Cnpp_impl cff_e_LineTo_Cnpp
#define cff_e_LineTo_Cnpn_impl cff_e_LineTo_Cnpn
#define cff_e_LineTo_Cnnp_impl cff_e_LineTo_Cnnp
#define cff_e_LineTo_Cnnn_impl cff_e_LineTo_Cnnn
#define cff_e_MoveTo_Cppp_impl cff_e_MoveTo_Cppp
#define cff_e_MoveTo_Cppn_impl cff_e_MoveTo_Cppn
#define cff_e_MoveTo_Cpnp_impl cff_e_MoveTo_Cpnp
#define cff_e_MoveTo_Cpnn_impl cff_e_MoveTo_Cpnn
#define cff_e_MoveTo_Cnpp_impl cff_e_MoveTo_Cnpp
#define cff_e_MoveTo_Cnpn_impl cff_e_MoveTo_Cnpn
#define cff_e_MoveTo_Cnnp_impl cff_e_MoveTo_Cnnp
#define cff_e_MoveTo_Cnnn_impl cff_e_MoveTo_Cnnn
#define cff_e_LineTo_Bppp_impl cff_e_LineTo_Bppp
#define cff_e_LineTo_Bppn_impl cff_e_LineTo_Bppn
#define cff_e_LineTo_Bpnp_impl cff_e_LineTo_Bpnp
#define cff_e_LineTo_Bpnn_impl cff_e_LineTo_Bpnn
#define cff_e_LineTo_Bnpp_impl cff_e_LineTo_Bnpp
#define cff_e_LineTo_Bnpn_impl cff_e_LineTo_Bnpn
#define cff_e_LineTo_Bnnp_impl cff_e_LineTo_Bnnp
#define cff_e_LineTo_Bnnn_impl cff_e_LineTo_Bnnn
#define cff_e_MoveTo_Bppp_impl cff_e_MoveTo_Bppp
#define cff_e_MoveTo_Bppn_impl cff_e_MoveTo_Bppn
#define cff_e_MoveTo_Bpnp_impl cff_e_MoveTo_Bpnp
#define cff_e_MoveTo_Bpnn_impl cff_e_MoveTo_Bpnn
#define cff_e_MoveTo_Bnpp_impl cff_e_MoveTo_Bnpp
#define cff_e_MoveTo_Bnpn_impl cff_e_MoveTo_Bnpn
#define cff_e_MoveTo_Bnnp_impl cff_e_MoveTo_Bnnp
#define cff_e_MoveTo_Bnnn_impl cff_e_MoveTo_Bnnn
#endif


/* ------------------------------------------------------------------------------------------
 * helpers */

#define RD32(p, off) (*(int32_t *)((uint8_t *)(p) + (off)))
#define RDU32(p, off) (*(uint32_t *)((uint8_t *)(p) + (off)))
#define RDPTR(p, off) (*(void **)((uint8_t *)(p) + (off)))

static inline uint64_t pack(int32_t x, int32_t y) { return (uint32_t)x | (uint64_t)(uint32_t)y << 32; }
static inline int32_t px(uint64_t p) { return (int32_t)(uint32_t)p; }
static inline int32_t py(uint64_t p) { return (int32_t)(uint32_t)(p >> 32); }
static inline int32_t neg32(int32_t v) { return (int32_t)(0u - (uint32_t)v); }
static inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
/* neg + cmovs: |v|, with |INT_MIN| = INT_MIN */
static inline int32_t abs32(int32_t v) {
    int32_t t = neg32(v);
    return t < 0 ? v : t;
}

typedef void (*tfm_fn)(uint64_t pt, uint64_t *out, void *state);         /* state + 0x580/0x588/0x590 */
typedef int32_t (*fixop_fn)(int32_t, int32_t);
typedef int64_t (*client_pt_fn)(int32_t *pt, void *arg2, void *client); /* client MoveTo / LineTo */
typedef int64_t (*client_fwd_fn)(void *a1, void *client, void *a3, void *a4);
typedef int64_t (*client_curve_fn)(int32_t *p0, int32_t *p1, int32_t *p2, int32_t *p3, void *a5, void *client);
typedef int32_t (*curve_fn)(uint64_t *p0, uint64_t *p1, uint64_t *p2, uint64_t *p3, void *state, void *a6);
typedef int32_t (*line_fn)(uint64_t *pt, void *state, void *a3);
typedef int32_t (*cspathpoints_fn)(void *client, void *pts, uint64_t n, int32_t flag, void *a5);
typedef uint64_t (*erect_fn)(uint64_t pt, uint64_t *out, void *state);
typedef int32_t (*flexcurve_fn)(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t *cur, void *proc, void *state, void *a7);
typedef int64_t (*fltn_fn)(void *p0, void *p1, void *p2, void *p3, void *a5, void *rec, int32_t depth);
typedef int64_t (*bcsetmtx_fn)(void *mtx, void *state);

#define FIXMUL CFF_ORIG(fixop_fn, 0xb3340)
#define FIXDIV CFF_ORIG(fixop_fn, 0xb316c)

#define ST_FLAGS 0x2bc
#define F_SWAP (1u << 16)
#define F_NEGX (1u << 19)
#define F_NEGY (1u << 22)

/* ------------------------------------------------------------------------------------------
 * erectToDev and the path procs of the erect record */

/* returns the erected point in RAX, as the original (and BCTfmP, which tail-calls it) does */
CFF_API uint64_t cff_erectToDev_impl(uint64_t pt, uint64_t *out, void *state) {
    uint32_t f = RDU32(state, ST_FLAGS);
    int32_t x = px(pt), y = py(pt);
    if (f & F_NEGX)
        x = neg32(x);
    if (f & F_NEGY)
        y = neg32(y);
#ifdef CFF_CONTROL_ERECTTODEV
    x = add32(x, 0x30000); /* every erected point 3 px to the right */
#endif
    uint64_t r = (f & F_SWAP) ? pack(y, x) : pack(x, y);
    *out = r;
    return r;
}
CFF_EXPORT(cff_erectToDev, cff_erectToDev_impl);

/* rcx..r9 and the fifth argument go to InternalFixedFltn unchanged; the sixth is the record */
CFF_API int64_t cff_erectToDev_CurveTo_FixedFltn_impl(void *p0, void *p1, void *p2, void *p3, void *a5, void **rec) {
    uint8_t fr[0x38];
    memset(fr, 0, sizeof fr);
    void *r = rec;
    memcpy(fr + 0x00, &r, 8);
    memcpy(fr + 0x10, (uint8_t *)rec[0] + 0x10, 4);
    memcpy(fr + 0x18, &rec[2], 8);
    return CFF_ORIG(fltn_fn, 0xc7284)(p0, p1, p2, p3, a5, fr, 0);
}
CFF_EXPORT_N(cff_erectToDev_CurveTo_FixedFltn, cff_erectToDev_CurveTo_FixedFltn_impl, 2);

static inline void erect_in(uint32_t f, const int32_t *in, int32_t *o) {
    int32_t x = in[0], y = in[1];
    if (f & F_NEGX)
        x = neg32(x);
    if (f & F_NEGY)
        y = neg32(y);
    if (f & F_SWAP) {
        o[0] = y;
        o[1] = x;
    } else {
        o[0] = x;
        o[1] = y;
    }
}

CFF_API int64_t cff_erectToDev_CurveTo_client_impl(const int32_t *p0, const int32_t *p1, const int32_t *p2,
                                                   const int32_t *p3, void *a5, void **rec) {
    void *state = rec[1];
    int32_t t0[2], t1[2], t2[2], t3[2];
    erect_in(RDU32(state, ST_FLAGS), p0, t0);
    erect_in(RDU32(state, ST_FLAGS), p1, t1);
    erect_in(RDU32(state, ST_FLAGS), p2, t2);
    erect_in(RDU32(state, ST_FLAGS), p3, t3);
#ifdef CFF_CONTROL_ERECTTODEV_CURVETO_CLIENT
    t1[0] = add32(t1[0], 0x100);
#endif
    client_curve_fn cb = (client_curve_fn)RDPTR(rec[3], 0x18);
    if (!cb)
        return 1;
    return cb(t0, t1, t2, t3, a5, rec[0]);
}
CFF_EXPORT_N(cff_erectToDev_CurveTo_client, cff_erectToDev_CurveTo_client_impl, 2);

/* tail jumps: rcx, r8 and r9 pass through, rdx becomes the client data */
#define FORWARD_PROC(NAME, OFF)                                                                  \
    CFF_API int64_t cff_##NAME##_impl(void *a1, void **rec, void *a3, void *a4) {                \
        client_fwd_fn cb = (client_fwd_fn)RDPTR(rec[3], OFF);                                    \
        if (!cb)                                                                                 \
            return 1;                                                                            \
        return cb(a1, rec[0], a3, a4);                                                           \
    }                                                                                            \
    CFF_EXPORT(cff_##NAME, cff_##NAME##_impl);

FORWARD_PROC(erectToDev_NewChar, 0x00)
FORWARD_PROC(erectToDev_ClosePath, 0x20)
FORWARD_PROC(erectToDev_EndChar, 0x28)

#ifdef CFF_CONTROL_EPROC
#ifdef _WIN32
__declspec(dllexport)
#endif
int cff_control_eproc = CFF_CONTROL_EPROC;
#define CTRL_EPROC(k) (cff_control_eproc == (k))
#else
#define CTRL_EPROC(k) 0
#endif

/* e_<op>_C<x><y><s>: erect the point, call the client's MoveTo (+0x08) or LineTo (+0x10) */
#define E_CLIENT(NAME, OFF, NX, NY, SW, CTRL)                                                    \
    CFF_API int64_t cff_##NAME##_impl(const int32_t *pt, void *a2, void **rec) {                 \
        int32_t x = pt[0], y = pt[1], o[2];                                                      \
        if (NX)                                                                                  \
            x = neg32(x);                                                                        \
        if (NY)                                                                                  \
            y = neg32(y);                                                                        \
        o[0] = SW ? y : x;                                                                       \
        o[1] = SW ? x : y;                                                                       \
        if (CTRL)                                                                                \
            o[1] = add32(o[1], 0x10000);                                                         \
        client_pt_fn cb = (client_pt_fn)RDPTR(rec[3], OFF);                                      \
        if (!cb)                                                                                 \
            return 1;                                                                            \
        return cb(o, a2, rec[0]);                                                                \
    }                                                                                            \
    CFF_EXPORT(cff_##NAME, cff_##NAME##_impl);

#ifdef CFF_CONTROL_E_LINETO_CPPP
#define CTRL_LCPPP 1
#else
#define CTRL_LCPPP 0
#endif
#ifdef CFF_CONTROL_E_LINETO_BPPP
#define CTRL_LBPPP 1
#else
#define CTRL_LBPPP 0
#endif
#ifdef CFF_CONTROL_E_LINETO_BNPP
#define CTRL_LBNPP 1
#else
#define CTRL_LBNPP 0
#endif

E_CLIENT(e_LineTo_Cppp, 0x10, 0, 0, 0, (CTRL_LCPPP || CTRL_EPROC(0)))
E_CLIENT(e_LineTo_Cppn, 0x10, 0, 0, 1, (0 || CTRL_EPROC(1)))
E_CLIENT(e_LineTo_Cpnp, 0x10, 0, 1, 0, (0 || CTRL_EPROC(2)))
E_CLIENT(e_LineTo_Cpnn, 0x10, 0, 1, 1, (0 || CTRL_EPROC(3)))
E_CLIENT(e_LineTo_Cnpp, 0x10, 1, 0, 0, (0 || CTRL_EPROC(4)))
E_CLIENT(e_LineTo_Cnpn, 0x10, 1, 0, 1, (0 || CTRL_EPROC(5)))
E_CLIENT(e_LineTo_Cnnp, 0x10, 1, 1, 0, (0 || CTRL_EPROC(6)))
E_CLIENT(e_LineTo_Cnnn, 0x10, 1, 1, 1, (0 || CTRL_EPROC(7)))
E_CLIENT(e_MoveTo_Cppp, 0x08, 0, 0, 0, (0 || CTRL_EPROC(16)))
E_CLIENT(e_MoveTo_Cppn, 0x08, 0, 0, 1, (0 || CTRL_EPROC(17)))
E_CLIENT(e_MoveTo_Cpnp, 0x08, 0, 1, 0, (0 || CTRL_EPROC(18)))
E_CLIENT(e_MoveTo_Cpnn, 0x08, 0, 1, 1, (0 || CTRL_EPROC(19)))
E_CLIENT(e_MoveTo_Cnpp, 0x08, 1, 0, 0, (0 || CTRL_EPROC(20)))
E_CLIENT(e_MoveTo_Cnpn, 0x08, 1, 0, 1, (0 || CTRL_EPROC(21)))
E_CLIENT(e_MoveTo_Cnnp, 0x08, 1, 1, 0, (0 || CTRL_EPROC(22)))
E_CLIENT(e_MoveTo_Cnnn, 0x08, 1, 1, 1, (0 || CTRL_EPROC(23)))

/* e_<op>_B<x><y><s>: append to the CScan point buffer. A MoveTo while the buffer holds one or
 * two points replaces the last one. */
#define CS_COUNT 0x70
#define CS_PTS 0x74
#define CS_MAX 0x46

#define E_CSCAN(NAME, MOVE, NX, NY, SW, CTRL)                                                    \
    CFF_API int32_t cff_##NAME##_impl(const int32_t *pt, void *a2, void **rec) {                 \
        int32_t x = pt[0], y = pt[1];                                                            \
        if (NX)                                                                                  \
            x = neg32(x);                                                                        \
        if (NY)                                                                                  \
            y = neg32(y);                                                                        \
        uint64_t o = SW ? pack(y, x) : pack(x, y);                                               \
        if (CTRL)                                                                                \
            o += 0x10000;                                                                        \
        uint8_t *cs = (uint8_t *)RDPTR(rec[0], 0x30);                                            \
        int32_t n = RD32(cs, CS_COUNT);                                                          \
        if (n >= CS_MAX) {                                                                       \
            if (!CFF_ORIG(cspathpoints_fn, 0xc0208)(rec[0], cs + CS_PTS, (uint64_t)(int64_t)n, 0, a2)) \
                return 0;                                                                        \
            n = 0;                                                                               \
            RD32(cs, CS_COUNT) = 0;                                                              \
        } else if (MOVE && (n == 1 || n == 2)) {                                                 \
            memcpy(cs + CS_PTS + 8 * (int64_t)(n - 1), &o, 8);                                   \
            return 1;                                                                            \
        }                                                                                        \
        memcpy(cs + CS_PTS + 8 * (int64_t)n, &o, 8);                                             \
        RD32(cs, CS_COUNT) = RD32(cs, CS_COUNT) + 1;                                             \
        return 1;                                                                                \
    }                                                                                            \
    CFF_EXPORT(cff_##NAME, cff_##NAME##_impl);

E_CSCAN(e_LineTo_Bppp, 0, 0, 0, 0, (CTRL_LBPPP || CTRL_EPROC(8)))
E_CSCAN(e_LineTo_Bppn, 0, 0, 0, 1, (0 || CTRL_EPROC(9)))
E_CSCAN(e_LineTo_Bpnp, 0, 0, 1, 0, (0 || CTRL_EPROC(10)))
E_CSCAN(e_LineTo_Bpnn, 0, 0, 1, 1, (0 || CTRL_EPROC(11)))
E_CSCAN(e_LineTo_Bnpp, 0, 1, 0, 0, (CTRL_LBNPP || CTRL_EPROC(12)))
E_CSCAN(e_LineTo_Bnpn, 0, 1, 0, 1, (0 || CTRL_EPROC(13)))
E_CSCAN(e_LineTo_Bnnp, 0, 1, 1, 0, (0 || CTRL_EPROC(14)))
E_CSCAN(e_LineTo_Bnnn, 0, 1, 1, 1, (0 || CTRL_EPROC(15)))
E_CSCAN(e_MoveTo_Bppp, 1, 0, 0, 0, (0 || CTRL_EPROC(24)))
E_CSCAN(e_MoveTo_Bppn, 1, 0, 0, 1, (0 || CTRL_EPROC(25)))
E_CSCAN(e_MoveTo_Bpnp, 1, 0, 1, 0, (0 || CTRL_EPROC(26)))
E_CSCAN(e_MoveTo_Bpnn, 1, 0, 1, 1, (0 || CTRL_EPROC(27)))
E_CSCAN(e_MoveTo_Bnpp, 1, 1, 0, 0, (0 || CTRL_EPROC(28)))
E_CSCAN(e_MoveTo_Bnpn, 1, 1, 0, 1, (0 || CTRL_EPROC(29)))
E_CSCAN(e_MoveTo_Bnnp, 1, 1, 1, 0, (0 || CTRL_EPROC(30)))
E_CSCAN(e_MoveTo_Bnnn, 1, 1, 1, 1, (0 || CTRL_EPROC(31)))

/* ------------------------------------------------------------------------------------------
 * matrix and transform helpers */

/* (matrix a b c d tx ty, swap, state): state+0x578 = state+0x540, zeroes its first two
 * words, copies the matrix to +0x548 (b a d c ty tx when swap), then BCSetMtx. Returns 0. */
CFF_API int32_t cff_SetGSMatrix_impl(const int32_t *m, int32_t swap, void *state) {
    uint8_t *g = (uint8_t *)state + 0x540;
    RDPTR(state, 0x578) = g;
    RD32(g, 4) = 0;
    RD32(g, 0) = 0;
    int32_t *d = (int32_t *)(g + 8);
    if (!swap) {
        d[0] = m[0];
        d[1] = m[1];
        d[2] = m[2];
        d[3] = m[3];
        d[4] = m[4];
        d[5] = m[5];
    } else {
        d[0] = m[1];
        d[1] = m[0];
        d[2] = m[3];
        d[3] = m[2];
        d[4] = m[5];
        d[5] = m[4];
    }
#ifdef CFF_CONTROL_SETGSMATRIX
    d[0] = add32(d[0], d[0] >> 3); /* the device matrix 12.5% wider */
#endif
    CFF_ORIG(bcsetmtx_fn, 0xb24ec)(d, state);
    return 0;
}
CFF_EXPORT(cff_SetGSMatrix, cff_SetGSMatrix_impl);

/* (accent record, out, dx, dy, state) */
CFF_API int32_t cff_CCBuild_impl(const uint8_t *a, int32_t *out, int32_t dx, int32_t dy, void *state) {
    int32_t x = add32(sub32(RD32(a, 4), RD32(a, 0)), dx);
    int32_t y = add32(RD32(a, 8), dy);
    out[3] = *(const int16_t *)(a + 0xe);
    out[2] = *(const int16_t *)(a + 0x12);
    RD32(state, 0x3d0) = x;
    uint64_t t = pack(x, y);
    ((tfm_fn)RDPTR(state, 0x590))(t, &t, state);
    CFF_ORIG(erect_fn, 0xd9118)(t, &t, state);
    out[1] = py(t);
    out[0] = px(t);
    RD32(state, 0x3d4) = px(t);
    return py(t);
}
CFF_EXPORT_N(cff_CCBuild, cff_CCBuild_impl, 1);

/* (v, out, state): the device x of (v, 0), or the device y of (0, v), through state+0x590 */
CFF_API int32_t cff_GetDTfmX_impl(int32_t v, int32_t *out, void *state) {
    uint64_t t = pack(v, 0);
    ((tfm_fn)RDPTR(state, 0x590))(t, &t, state);
    return *out = px(t);
}
CFF_EXPORT(cff_GetDTfmX, cff_GetDTfmX_impl);

CFF_API int32_t cff_GetDTfmY_impl(int32_t v, int32_t *out, void *state) {
    uint64_t t = pack(0, v);
    ((tfm_fn)RDPTR(state, 0x590))(t, &t, state);
    return *out = py(t);
}
CFF_EXPORT(cff_GetDTfmY, cff_GetDTfmY_impl);

/* ------------------------------------------------------------------------------------------
 * hint lists */

CFF_API void *cff_ClearHintLists_impl(void *state) {
    int32_t *a = (int32_t *)RDPTR(state, 0x468);
    RDPTR(state, 0x428) = 0;
#ifndef CFF_CONTROL_CLEARHINTLISTS /* the control leaves the two lists as they were */
    a[0] = 1;
    ((int32_t *)RDPTR(state, 0x470))[0] = 1;
    ((int32_t *)RDPTR(state, 0x468))[0x10] = (int32_t)0xd8300001;
    ((int32_t *)RDPTR(state, 0x470))[0x10] = (int32_t)0xd8300001;
#endif
    return RDPTR(state, 0x470);
}
CFF_EXPORT(cff_ClearHintLists, cff_ClearHintLists_impl);

/* (buffer of 48-byte records, buffer of 80-byte records, x, state): each buffer is
 * {pointer, u32 size}; the state gets the start and the end of the last whole record */
CFF_API int32_t cff_GCInitBuffs_impl(void **b48, void **b80, void *v, void *state) {
    RDPTR(state, 0x488) = b80;
    RDPTR(state, 0x4a0) = b48;
    uint8_t *p80 = (uint8_t *)b80[0];
    RDPTR(state, 0x480) = p80;
    uint32_t n80 = *(uint32_t *)((uint8_t *)b80 + 8);
    RDPTR(state, 0x478) = p80 + (uint64_t)(n80 / 80) * 80;
    uint8_t *p48 = (uint8_t *)b48[0];
    RDPTR(state, 0x498) = p48;
    uint32_t n48 = *(uint32_t *)((uint8_t *)b48 + 8);
    RDPTR(state, 0x3f8) = v;
    RDPTR(state, 0x490) = p48 + (uint64_t)(n48 / 48) * 48;
    uint64_t z = 0;
    RDPTR(state, 0x4b0) = 0;
    memcpy((uint8_t *)state + 0x3ec, &z, 8);
    RD32(state, 0x408) = 0;
    RDPTR(state, 0x400) = 0;
    RDPTR(state, 0x458) = 0;
    return 0;
}
CFF_EXPORT(cff_GCInitBuffs, cff_GCInitBuffs_impl);

/* (p0, p1, state): a line longer than 15 px (16.16) and within 2 px of horizontal or vertical
 * extends the nearest stem of the list at state+0x4a8 (flag 4 set, flag 1 = direction, no
 * 0x60) whose edge is within 3 px of the line's middle: its range [+0x1c, +0x20] grows to
 * cover the line. */
CFF_API void cff_GCLine_impl(uint64_t p0, uint64_t p1, void *state) {
    int32_t x0 = px(p0), y0 = py(p0), x1 = px(p1), y1 = py(p1);
    int32_t dx = sub32(x1, x0), dy = sub32(y1, y0);
    int32_t adx = abs32(dx), ady = abs32(dy);
    int32_t dir, side, mid, lo, hi;
    if (adx <= 0x20000 && ady >= 0xf0000) {
        /* vertical */
        dir = 0;
        mid = dx == 0 ? x0 : add32(dx >> 1, x0);
        side = (int32_t)((uint32_t)dy >> 31);
        if (dy >= 0) {
            lo = y0;
            hi = y1;
        } else {
            lo = y1;
            hi = y0;
        }
    } else {
        if (ady > 0x20000 || adx < 0xf0000)
            return;
        /* horizontal */
        dir = 1;
        mid = dy != 0 ? add32(dy >> 1, y0) : y0;
        if (dx >= 0) {
            lo = x0;
            hi = x1;
            side = 1;
        } else {
            lo = x1;
            hi = x0;
            side = 0;
        }
    }
    uint8_t *best = 0;
    int32_t bestd = 0x27100000;
    uint8_t *s = (uint8_t *)RDPTR(state, 0x4a8);
    if (!s)
        return;
    for (; s; s = (uint8_t *)RDPTR(s, 0)) {
        if (!(s[0x48] & 4) || (int32_t)(RDU32(s, 0x48) & 1) != dir || (s[0x48] & 0x60))
            continue;
        int32_t e = side ? RD32(s, 0xc) : RD32(s, 0x10);
        int32_t d = abs32(sub32(mid, e));
        if (d < bestd) {
            bestd = d;
            best = s;
        }
    }
    if (bestd > 0x30000)
        return;
#ifdef CFF_CONTROL_GCLINE
    return; /* the stem's range is not extended */
#endif
    if (lo < RD32(best, 0x1c))
        RD32(best, 0x1c) = lo;
    if (hi > RD32(best, 0x20))
        RD32(best, 0x20) = hi;
}
CFF_EXPORT(cff_GCLine, cff_GCLine_impl);

/* ------------------------------------------------------------------------------------------
 * flex
 *
 * The flex record F (on DoType1InterpretCharString's stack): ten packed points (x low, y high)
 * F[0] the reference point, F[1] the current point, F[2..8] the two curves (F[5] the joint,
 * F[8] the end), F[9] the point that becomes the current point afterwards. */

/* (p1, p2, p3, current point, curve proc, state, arg): transforms p1..p3 through state+0x580
 * and calls curve(current, &p1, &p2, &p3, state, arg); on success the current point is p3 */
CFF_API int32_t cff_FlexCurveTo_impl(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t *cur, void *proc, void *state,
                                     void *a7) {
    ((tfm_fn)RDPTR(state, 0x580))(p1, &p1, state);
    ((tfm_fn)RDPTR(state, 0x580))(p2, &p2, state);
    ((tfm_fn)RDPTR(state, 0x580))(p3, &p3, state);
    if (!((curve_fn)proc)(cur, &p1, &p2, &p3, state, a7))
        return 0;
    *cur = p3;
    return 1;
}
CFF_EXPORT_N(cff_FlexCurveTo, cff_FlexCurveTo_impl, 3);

CFF_API int32_t cff_FlexProc2_impl(uint64_t *F, uint64_t *cur, void *proc, void *state, void *a5) {
    flexcurve_fn fc = CFF_ORIG(flexcurve_fn, 0xe8c44);
    if (!fc(F[2], F[3], F[5], cur, proc, state, a5))
        return 0;
    if (!fc(F[6], F[7], F[8], cur, proc, state, a5))
        return 0;
    F[1] = F[9];
    return 1;
}
CFF_EXPORT_N(cff_FlexProc2, cff_FlexProc2_impl, 1);

/* the snapped coordinate of the flex joint: c is the transformed current point's coordinate
 * along the flex, r the transformed (rounded) joint offset */
static int32_t flex_snap(int32_t c, int32_t r, int32_t arg8, int32_t arg9) {
    int32_t e = (int32_t)(((uint32_t)r + 0x8000) & 0xffff0000u);
    if (e == 0)
        e = (int32_t)(((uint32_t)(r >> 31) & 0xfffe0000u) + 0x10000);
    int half = 0;
    if (arg9 == 0 && arg8 >= 0x8000) {
        half = 1;
        c = sub32(c, 0x8000);
    }
    uint32_t s = (uint32_t)c + (uint32_t)e;
    uint32_t v = ((s + 0xffff) & 0xffff0000u) + (s & 0xffff0000u) - s;
    return (int32_t)(half ? v + 0x8000 : v);
}

/* (F, threshold, current point, line proc, curve proc, arg, -, arg8, arg9, state) */
CFF_API int32_t cff_FlexProc_impl(uint64_t *F, int32_t thr, uint64_t *cur, void *lineproc, void *curveproc, void *a6,
                                  int32_t a7, int32_t arg8, int32_t arg9, void *state) {
    (void)a7;
    uint64_t P0 = F[0], P1 = F[1], P2 = F[2], P3 = F[3], P4 = F[4], P5 = F[5], P6 = F[6], P7 = F[7], P8 = F[8];
    int32_t save_x = RD32(F, 0x48), save_y = RD32(F, 0x4c);
    int32_t lim = abs32(thr) / 100;
    int32_t adx = abs32(sub32(px(P1), px(P5)));
    int32_t ady = abs32(sub32(py(P1), py(P5)));
    int32_t c = *(int32_t *)RDPTR(state, 0x578);
    int32_t dir = c == 1 ? 1 : c == 2 ? -1 : 0;
    /* the curves' points after the adjustments: q2 q3 joint, q6 q7 P8 */
    uint64_t q2 = P2, q3 = P3, j = P5, q6 = P6, q7 = P7;
    int32_t x8 = px(P8), y8 = py(P8);
    uint64_t t;
    if (ady > adx) {
        /* the flex runs vertically: its depth is in y */
        if (dir != 0 && py(P5) != y8) {
            int32_t s = abs32(FIXDIV(sub32(py(P1), y8), sub32(py(P5), y8)));
            uint64_t r2 = pack(px(P2), add32(FIXMUL(sub32(py(P2), y8), s), y8));
            uint64_t r3 = pack(px(P3), add32(FIXMUL(sub32(py(P3), y8), s), y8));
            uint64_t r6 = pack(px(P6), add32(FIXMUL(sub32(py(P6), y8), s), y8));
            uint64_t r7 = pack(px(P7), add32(FIXMUL(sub32(py(P7), y8), s), y8));
            t = pack(0, (int32_t)(((uint32_t)sub32(py(P4), py(P0)) + 0x8000) & 0xffff0000u));
            ((tfm_fn)RDPTR(state, 0x590))(t, &t, state);
            int32_t v = dir == 1 ? py(t) : px(t);
            if (abs32(v) < lim) {
                q2 = r2;
                j = P1;
                q3 = r3;
                q6 = r6;
                q7 = r7;
            }
            int32_t d = sub32(py(j), py(P1));
            if (abs32(d) > 0x42) {
                ((tfm_fn)RDPTR(state, 0x580))(P1, &t, state);
                int32_t keep, along;
                if (dir == 1) {
                    keep = px(t);
                    along = py(t);
                } else {
                    keep = py(t);
                    along = px(t);
                }
                t = pack(0, (int32_t)(((uint32_t)d + 0x8000) & 0xffff0000u));
                ((tfm_fn)RDPTR(state, 0x590))(t, &t, state);
                int32_t r = dir == 1 ? py(t) : px(t);
                int32_t sn = flex_snap(along, r, arg8, arg9);
                t = dir == 1 ? pack(keep, sn) : pack(sn, keep);
                ((tfm_fn)RDPTR(state, 0x588))(t, &t, state);
                int32_t k = sub32(py(t), py(j));
                q3 = pack(px(q3), add32(py(q3), k));
                j = pack(px(j), add32(py(j), k));
                q6 = pack(px(q6), add32(py(q6), k));
            }
        }
    } else {
        /* the flex runs horizontally: its depth is in x */
        if (dir != 0 && px(P5) != x8) {
            int32_t s = abs32(FIXDIV(sub32(px(P1), x8), sub32(px(P5), x8)));
            uint64_t r2 = pack(add32(FIXMUL(sub32(px(P2), x8), s), x8), py(P2));
            uint64_t r3 = pack(add32(FIXMUL(sub32(px(P3), x8), s), x8), py(P3));
            uint64_t r6 = pack(add32(FIXMUL(sub32(px(P6), x8), s), x8), py(P6));
            uint64_t r7 = pack(add32(FIXMUL(sub32(px(P7), x8), s), x8), py(P7));
            t = pack((int32_t)(((uint32_t)sub32(px(P4), px(P0)) + 0x8000) & 0xffff0000u), 0);
            ((tfm_fn)RDPTR(state, 0x590))(t, &t, state);
            int32_t v = dir == -1 ? py(t) : px(t);
            if (abs32(v) < lim) {
                q2 = r2;
                j = P1;
                q3 = r3;
                q6 = r6;
                q7 = r7;
            }
            int32_t d = sub32(px(j), px(P1));
            if (abs32(d) > 0x42) {
                ((tfm_fn)RDPTR(state, 0x580))(P1, &t, state);
                int32_t keep, along;
                if (dir == -1) {
                    keep = px(t);
                    along = py(t);
                } else {
                    keep = py(t);
                    along = px(t);
                }
                t = pack((int32_t)(((uint32_t)d + 0x8000) & 0xffff0000u), 0);
                ((tfm_fn)RDPTR(state, 0x590))(t, &t, state);
                int32_t r = dir == -1 ? py(t) : px(t);
                int32_t sn = flex_snap(along, r, arg8, arg9);
                t = dir == -1 ? pack(keep, sn) : pack(sn, keep);
                ((tfm_fn)RDPTR(state, 0x588))(t, &t, state);
                int32_t k = sub32(px(t), px(j));
                q3 = pack(add32(px(q3), k), py(q3));
                j = pack(add32(px(j), k), py(j));
                q6 = pack(add32(px(q6), k), py(q6));
            }
        }
    }
#ifdef CFF_CONTROL_FLEXPROC
    j = pack(add32(px(j), 0x20000), add32(py(j), 0x20000)); /* the joint moved by (2, 2) */
#endif
    int32_t ok;
    if (px(j) == x8 || py(j) == y8) {
        /* the joint is level with the end: a line */
        ((tfm_fn)RDPTR(state, 0x580))(P8, cur, state);
        ok = ((line_fn)lineproc)(cur, state, a6);
    } else {
        flexcurve_fn fc = CFF_ORIG(flexcurve_fn, 0xe8c44);
        if (!fc(q2, q3, j, cur, curveproc, state, a6))
            return 0;
        ok = fc(q6, q7, P8, cur, curveproc, state, a6);
    }
    if (!ok)
        return 0;
    RD32(F, 8) = save_x;
    RD32(F, 0xc) = save_y;
    return 1;
}
CFF_EXPORT_N(cff_FlexProc, cff_FlexProc_impl, 6);
