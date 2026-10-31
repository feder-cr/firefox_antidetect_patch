/* idwr_face, internal to idwr (idwr/face.c, idwr/gdimetrics.c) */
#ifndef IDWR_FACE_H
#define IDWR_FACE_H
#include "idwr.h"
#include "idwr_internal.h"

#define IDWR_FACE_MAX_AXES 64

struct idwr_face {
    const idwr_font *font;
    uint32_t simulations;
    uint16_t glyph_count;
    uint16_t axes;                  /* 0 for a face without fvar */
    float coords[IDWR_FACE_MAX_AXES];/* normalized, fvar order */
    int weight;                     /* idwr_face_weight of the axis values the face was created with */
    idwr_font_metrics metrics;       /* the instance's metrics (MVAR), before the oblique shear */
    void *gdi;                      /* gdimetrics.c: the scaler and the full-width glyph bits */
    void *color;                    /* color.c: the COLR/CPAL tables, parsed on first use */
};

void idwr_face_color_release(idwr_face *f);

void idwr_face_apply_mvar(const idwr_font *font, const float *coords, uint16_t axes, const idwr_metrics_info *info,
                         idwr_font_metrics *m);
void idwr_face_gdi_release(idwr_face *f);

#endif
