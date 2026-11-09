/*
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#define IDWR_STATIC 1
#include "src/ports/SkIdwrPathSink.h"

#include "idwr/idwr.h"
#include "src/utils/SkFloatUtils.h"

namespace {

struct Pt {
    float x, y;
};

bool approximately_equal(float a, float b) {
    const SkFloatingPoint<float, 10> lhs(a), rhs(b);
    return lhs.AlmostEquals(rhs);
}

bool check_quadratic(const Pt (&cubic)[4], Pt& quadraticP1) {
    float dx10 = cubic[1].x - cubic[0].x;
    float dx23 = cubic[2].x - cubic[3].x;
    float midX = cubic[0].x + dx10 * 3 / 2;
    if (!approximately_equal(midX, (dx23 * 3 / 2) + cubic[3].x)) {
        return false;
    }
    float dy10 = cubic[1].y - cubic[0].y;
    float dy23 = cubic[2].y - cubic[3].y;
    float midY = cubic[0].y + dy10 * 3 / 2;
    if (!approximately_equal(midY, (dy23 * 3 / 2) + cubic[3].y)) {
        return false;
    }
    quadraticP1 = {midX, midY};
    return true;
}

struct Sink {
    SkPathBuilder* fBuilder;
    bool fStarted = false;
    Pt fCurrent = {0, 0};

    void goingTo(Pt pt) {
        if (!fStarted) {
            fStarted = true;
            fBuilder->moveTo(fCurrent.x, fCurrent.y);
        }
        fCurrent = pt;
    }
    bool currentIsNot(Pt pt) const { return fCurrent.x != pt.x || fCurrent.y != pt.y; }

    void line(Pt p) {
        if (this->currentIsNot(p)) {
            this->goingTo(p);
            fBuilder->lineTo(p.x, p.y);
        }
    }
    void bezier(Pt p1, Pt p2, Pt p3) {
        if (this->currentIsNot(p1) || this->currentIsNot(p2) || this->currentIsNot(p3)) {
            Pt cubic[4] = {fCurrent, p1, p2, p3};
            this->goingTo(p3);
            Pt q;
            if (check_quadratic(cubic, q)) {
                fBuilder->quadTo(q.x, q.y, p3.x, p3.y);
            } else {
                fBuilder->cubicTo(p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
            }
        }
    }
};

}  // namespace

void SkIdwrAppendPath(SkPathBuilder* builder, const idwr_path_cmd* cmds, int count) {
    Sink sink{builder};
    for (int i = 0; i < count; ++i) {
        const idwr_path_cmd& c = cmds[i];
        switch (c.op) {
            case IDWR_PATH_FILL_WINDING:
                builder->setFillType(SkPathFillType::kWinding);
                break;
            case IDWR_PATH_BEGIN:
                sink.fStarted = false;
                sink.fCurrent = {c.p[0], c.p[1]};
                break;
            case IDWR_PATH_LINE:
                sink.line({c.p[0], c.p[1]});
                break;
            case IDWR_PATH_BEZIER:
                sink.bezier({c.p[0], c.p[1]}, {c.p[2], c.p[3]}, {c.p[4], c.p[5]});
                break;
            case IDWR_PATH_END:
                if (sink.fStarted) {
                    builder->close();
                }
                break;
            default:
                break;
        }
    }
}
