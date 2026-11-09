/*
 * The glyph outline commands of idwr as an SkPath, with the logic of SkDWriteGeometrySink
 * (lazy moveTo, repeated points dropped, a degree-raised quadratic back to a quad, close only
 * if started). idwr records DirectWrite's ID2D1SimplifiedGeometrySink calls in order.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef SkIdwrPathSink_DEFINED
#define SkIdwrPathSink_DEFINED

#include "include/core/SkPathBuilder.h"

#define IDWR_STATIC 1
#include "idwr/idwr.h"

void SkIdwrAppendPath(SkPathBuilder* builder, const idwr_path_cmd* cmds, int count);

#endif
