/* The CScan data structures (reimpl/cff/cscan.c, notes/cff.md group 8 "cscan"), shared with
 * the dropout group (EditBlackSpace, EditWhiteSpace, FixCrossFlags, ... walk the same
 * crossings and rows). Offsets are those of DWrite.dll 10.0.26100.9711; every structure has
 * the same layout on the 64-bit standalone targets. */
#ifndef CFF_CSCAN_H
#define CFF_CSCAN_H
#include <stddef.h>
#include <stdint.h>

/* A crossing (0x18 bytes) in the pool, the grow buffer the CScan state points to (+0x20).
 * The records of one path are contiguous in the pool, so the path neighbours of a crossing
 * are the records before and after it (ForwPathCross, BackPathCross). Records flagged
 * CS_EXTRA are not crossings: they are skipped by the path walks; the ones flagged CS_JUMP as
 * well (NewLinkCross, at both ends of each path) hold byte offsets in x (forward) and y
 * (backward) that join the two ends of a closed path. */
typedef struct CsCross {
    struct CsCross *next; /* 0x00 the row list (AddYCrosses), sorted by x; the turn lists */
    int32_t x;            /* 0x08 16.16; CS_JUMP: byte offset to the next record */
    int32_t y;            /* 0x0c 16.16, a pixel centre; +0x0e (y >> 16) is the row; CS_JUMP: byte offset back */
    uint16_t flags;       /* 0x10 */
    int16_t pix;          /* 0x12 the pixel column the crossing rounds to (BuildInitialRuns) */
    uint32_t pad;         /* 0x14 */
} CsCross;

enum {
    CS_DOWN = 0x0001,     /* the path goes on downward from here */
    CS_UP = 0x0002,       /* reached going upward */
    CS_SAMEY_RIGHT = 0x0004,
    CS_SAMEY_LEFT = 0x0008,
    CS_SLOPE = 0x00f0,    /* OffsetCenterSlope class (1..5) << 4, with the "center" option */
    CS_LINKED = 0x0100,   /* has a path neighbour crossing the next row */
    CS_ROWSTART = 0x0400, /* first crossing of a row (BuildInitialRuns) */
    CS_ORIG = 0x0800,
    CS_JUMP = 0x1000,
    CS_EXTRA = 0x2000,
    CS_WOUND = 0x4000,    /* dropped by the nonzero winding rule */
};

typedef struct CsPt {
    int32_t x, y;
} CsPt;

typedef struct CsGrowBuf { /* the grow buffers of CFFFontInst + 0x640 */
    void *ptr;
    uint32_t size;
    uint32_t pad;
} CsGrowBuf;

/* _t_ASZoneRec: a pointer to the allocator procs (internalMalloc, internalFree, internalRealloc) */
typedef struct CsZone CsZone;
typedef struct CsZoneProcs {
    void *(*alloc)(CsZone *zone, uint64_t size); /* 0x00 */
    void *unused08;
    void (*free)(CsZone *zone, void *p);         /* 0x10 */
    void *unused18, *unused20;
    void *(*realloc)(CsZone *zone, void *p, uint64_t size); /* 0x28 */
} CsZoneProcs;
struct CsZone {
    CsZoneProcs *procs;
};

/* the CScan state, 0x2e0 bytes in grow buffer 5 (IniCScan) */
typedef struct CsState {
    CsCross *list0;      /* 0x000 turns where the path starts going left (BuildCrosses) */
    CsCross *list1;      /* 0x008 turns where the path starts going right */
    CsCross *last;       /* 0x010 the last crossing (first: the sentinel at the 5th argument of CSPathPoints + 0x5a0) */
    CsCross **rows;      /* 0x018 the row list heads, indexed by row (biased pointer, BuildYCross) */
    CsGrowBuf *pool_buf; /* 0x020 the crossing pool (grow buffer 0) */
    CsCross *pool_start; /* 0x028 */
    CsCross *list6;      /* 0x030 list1 of every closed path */
    CsCross *list7;      /* 0x038 list0 of every closed path */
    CsCross *link;       /* 0x040 the link record that starts the current path */
    CsCross *prev_link;  /* 0x048 the one before */
    CsCross *pool_cur;   /* 0x050 */
    CsCross *pool_end;   /* 0x058 */
    uint8_t unk60[0x10]; /* 0x060 */
    int32_t npts;        /* 0x070 buffered points */
    CsPt pts[0x47];      /* 0x074 0x46 buffered points, and room for the closing one */
    CsPt prev;           /* 0x2ac the point the next segment starts from */
    CsPt first;          /* 0x2b4 the first point of the path */
    int32_t ymin;        /* 0x2bc 16.16 bounding box; +0x2be is its row */
    int32_t ymax;        /* 0x2c0 */
    int32_t xmin;        /* 0x2c4 */
    int32_t xmax;        /* 0x2c8 */
    uint16_t hdir;       /* 0x2cc 1 left, 2 right, 3 vertical */
    uint16_t vdir;       /* 0x2ce 4 up, 5 down, 6 horizontal */
    int16_t top;         /* 0x2d0 first row */
    int16_t bottom;      /* 0x2d2 last row (+1 after CScan) */
    int16_t left;        /* 0x2d4 first column */
    int16_t right;       /* 0x2d6 last column (+1 after CScan) */
    int32_t closed;      /* 0x2d8 the current path made crossings */
    int32_t newpath;     /* 0x2dc */
} CsState;

/* the run buffer record (0x30 bytes): the output of CScan, the input of ConvertRuns. Per row
 * from top to bottom: a count n, then n (left, right) column pairs, all int16. */
typedef struct CsRunRec {
    int16_t left, right, top, bottom; /* 0x00 */
    int32_t count;                    /* 0x08 int16 entries in data */
    int32_t pad0c;
    int16_t *data;                    /* 0x10 */
    uint64_t q18;                     /* 0x18 */
    uint64_t q20;                     /* 0x20 */
    uint32_t cap;                     /* 0x28 bytes */
    uint32_t pad2c;
} CsRunRec;

/* the memory record (CFFFontInst + 0x610 and the copies ATMBuildBitMap and CScanFill make) */
typedef struct CsMemRec {
    void *p00;       /* 0x00 */
    int (*grow)(uint32_t delta, CsRunRec *rr, int keep, void *growctx); /* 0x08 GrowRunBuff */
    void *p10;       /* 0x10 ATM_ManageCString */
    uint32_t v18;    /* 0x18 */
    uint32_t pad1c;
    CsZone *zone;    /* 0x20 */
    void *p28;       /* 0x28 */
} CsMemRec;

/* the record GrowRunBuff gets (its 4th argument) */
typedef struct CsGrowCtx {
    void *p00;
    CsMemRec *mem;       /* 0x08 the allocator holder for ATM_GrowBuff */
    CsGrowBuf *runbuf;   /* 0x10 */
} CsGrowCtx;

/* BuildRuns' frame (rsp + 0x40 .. rbp - 9): the context every CScan proc gets */
typedef struct CsCtx {
    struct CsCtx *self;  /* 0x00 */
    int32_t minppem;     /* 0x08 min of the two ppems (ATM state + 0x2a8, 0x2ac) */
    int32_t ppem2e0;     /* 0x0c ATM state + 0x2e0 */
    int32_t st4;         /* 0x10 ATM state + 4 */
    int32_t z14;
    void **procs;        /* 0x18 -> procs5 */
    int32_t white;       /* 0x20 ATM state + 0x2bc bit 4 */
    int32_t white2;      /* 0x24 bit 30 or bit 31 */
    int32_t center;      /* 0x28 bit 6 */
    int32_t wind;        /* 0x2c the _t_CharIO flags (+0x18) bit 13: nonzero winding */
    CsState *cs;         /* 0x30 */
    uint32_t flags;      /* 0x38 BuildRuns' 7th argument, then Type1InterpretCharString's 7th */
    uint32_t z3c;
    void *tail_ptr;      /* 0x40 -> tail */
    void *growctx;       /* 0x48 BuildRuns' 6th argument */
    struct CsCtx *self2; /* 0x50 */
    void *procs5[5];     /* 0x58 ResetCScan, CSNewPoint, CSNewLine, FixedFltn, CSClose */
    void *z80;           /* 0x80 */
    uint8_t tail[16];    /* 0x88 */
} CsCtx;

#define CS_CHECK(t, f, o) _Static_assert(offsetof(t, f) == (o), #t "." #f)
CS_CHECK(CsCross, flags, 0x10);
CS_CHECK(CsCross, pix, 0x12);
_Static_assert(sizeof(CsCross) == 0x18, "CsCross");
CS_CHECK(CsState, pool_end, 0x58);
CS_CHECK(CsState, npts, 0x70);
CS_CHECK(CsState, pts, 0x74);
CS_CHECK(CsState, prev, 0x2ac);
CS_CHECK(CsState, ymin, 0x2bc);
CS_CHECK(CsState, hdir, 0x2cc);
CS_CHECK(CsState, top, 0x2d0);
CS_CHECK(CsState, closed, 0x2d8);
_Static_assert(sizeof(CsState) == 0x2e0, "CsState");
CS_CHECK(CsRunRec, data, 0x10);
CS_CHECK(CsRunRec, cap, 0x28);
_Static_assert(sizeof(CsRunRec) == 0x30, "CsRunRec");
CS_CHECK(CsMemRec, zone, 0x20);
_Static_assert(sizeof(CsMemRec) == 0x30, "CsMemRec");
CS_CHECK(CsCtx, cs, 0x30);
CS_CHECK(CsCtx, flags, 0x38);
CS_CHECK(CsCtx, self2, 0x50);
CS_CHECK(CsCtx, procs5, 0x58);
CS_CHECK(CsCtx, tail, 0x88);
_Static_assert(sizeof(CsCtx) == 0x98, "CsCtx");

#endif
