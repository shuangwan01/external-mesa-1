/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file iris_draw.c
 *
 * The main driver hooks for drawing and launching compute shaders.
 */

#include <stdio.h>
#include <errno.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "util/u_transfer.h"
#include "util/u_upload_mgr.h"
#include "intel/compiler/brw_compiler.h"
#include "iris_context.h"
#include "iris_defines.h"

/**
 * Implement workarounds for preemption:
 *    - WaDisableMidObjectPreemptionForGSLineStripAdj
 *    - WaDisableMidObjectPreemptionForTrifanOrPolygon
 *    - WaDisableMidObjectPreemptionForLineLoop
 *    - WA#0798
 */
static void
gen9_emit_preempt_wa(struct iris_context *ice, struct iris_batch *batch,
                     const struct pipe_draw_info *info)
{
   bool object_preemption = true;
   struct iris_screen *screen = (struct iris_screen *)ice->ctx.screen;

   /* Only apply these workarounds for gen9 */
   assert(screen->devinfo.gen == 9);

   /* WaDisableMidObjectPreemptionForGSLineStripAdj
    *
    *    WA: Disable mid-draw preemption when draw-call is a linestrip_adj and
    *    GS is enabled.
    */
   if (ice->state.prim_mode == PIPE_PRIM_LINE_STRIP_ADJACENCY &&
       ice->shaders.prog[MESA_SHADER_GEOMETRY])
      object_preemption = false;

   /* WaDisableMidObjectPreemptionForTrifanOrPolygon
    *
    *    TriFan miscompare in Execlist Preemption test. Cut index that is on a
    *    previous context. End the previous, the resume another context with a
    *    tri-fan or polygon, and the vertex count is corrupted. If we prempt
    *    again we will cause corruption.
    *
    *    WA: Disable mid-draw preemption when draw-call has a tri-fan.
    */
   if (ice->state.prim_mode == PIPE_PRIM_TRIANGLE_FAN)
      object_preemption = false;

   /* WaDisableMidObjectPreemptionForLineLoop
    *
    *    VF Stats Counters Missing a vertex when preemption enabled.
    *
    *    WA: Disable mid-draw preemption when the draw uses a lineloop
    *    topology.
    */
   if (ice->state.prim_mode == PIPE_PRIM_LINE_LOOP)
      object_preemption = false;

   /* WA#0798
    *
    *    VF is corrupting GAFS data when preempted on an instance boundary and
    *    replayed with instancing enabled.
    *
    *    WA: Disable preemption when using instanceing.
    */
   if (info->instance_count > 1)
      object_preemption = false;

   gen9_iris_enable_obj_preemption(ice, batch, object_preemption);
}

/**
 * Record the current primitive mode and restart information, flagging
 * related packets as dirty if necessary.
 */
static void
iris_update_draw_info(struct iris_context *ice,
                      const struct pipe_draw_info *info)
{
   if (ice->state.prim_mode != info->mode) {
      ice->state.prim_mode = info->mode;
      ice->state.dirty |= IRIS_DIRTY_VF_TOPOLOGY;
   }

   if (info->mode == PIPE_PRIM_PATCHES &&
       ice->state.vertices_per_patch != info->vertices_per_patch) {
      ice->state.vertices_per_patch = info->vertices_per_patch;
      ice->state.dirty |= IRIS_DIRTY_VF_TOPOLOGY;

      /* Flag constants dirty for gl_PatchVerticesIn if needed. */
      const struct shader_info *tcs_info =
         iris_get_shader_info(ice, MESA_SHADER_TESS_CTRL);
      if (tcs_info &&
          tcs_info->system_values_read & (1ull << SYSTEM_VALUE_VERTICES_IN)) {
         ice->state.dirty |= IRIS_DIRTY_CONSTANTS_TCS;
         ice->state.shaders[MESA_SHADER_TESS_CTRL].cbuf0_needs_upload = true;
      }
   }

   if (ice->state.primitive_restart != info->primitive_restart ||
       ice->state.cut_index != info->restart_index) {
      ice->state.dirty |= IRIS_DIRTY_VF;
      ice->state.primitive_restart = info->primitive_restart;
      ice->state.cut_index = info->restart_index;
   }

   if (info->indirect) {
      pipe_resource_reference(&ice->draw.draw_params_res,
                              info->indirect->buffer);
      ice->draw.draw_params_offset = info->indirect->offset +
                                     (info->index_size ? 12 : 8);
      ice->draw.params.firstvertex = 0;
      ice->draw.params.baseinstance = 0;
      ice->state.dirty |= IRIS_DIRTY_VERTEX_BUFFERS |
                          IRIS_DIRTY_VERTEX_ELEMENTS |
                          IRIS_DIRTY_VF_SGVS;
   } else if (ice->draw.is_indirect ||
              ice->draw.params.firstvertex !=
              (info->index_size ? info->index_bias : info->start) ||
              (ice->draw.params.baseinstance != info->start_instance)) {
      pipe_resource_reference(&ice->draw.draw_params_res, NULL);
      ice->draw.draw_params_offset = 0;
      ice->draw.params.firstvertex =
         info->index_size ? info->index_bias : info->start;
      ice->draw.params.baseinstance = info->start_instance;
      ice->state.dirty |= IRIS_DIRTY_VERTEX_BUFFERS |
                          IRIS_DIRTY_VERTEX_ELEMENTS |
                          IRIS_DIRTY_VF_SGVS;
   }
   ice->draw.is_indirect = info->indirect;

   if (ice->draw.derived_params.drawid != info->drawid ||
       ice->draw.derived_params.is_indexed_draw != (info->index_size ? ~0 : 0)) {
      ice->draw.derived_params.drawid = info->drawid;
      ice->draw.derived_params.is_indexed_draw = info->index_size ? ~0 : 0;
      ice->state.dirty |= IRIS_DIRTY_VERTEX_BUFFERS |
                          IRIS_DIRTY_VERTEX_ELEMENTS |
                          IRIS_DIRTY_VF_SGVS;
   }
}

/**
 * The pipe->draw_vbo() driver hook.  Performs a draw on the GPU.
 */
void
iris_draw_vbo(struct pipe_context *ctx, const struct pipe_draw_info *info)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_screen *screen = (struct iris_screen*)ice->ctx.screen;
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_RENDER];

   if (ice->state.predicate == IRIS_PREDICATE_STATE_DONT_RENDER)
      return;

   /* We can't safely re-emit 3DSTATE_SO_BUFFERS because it may zero the
    * write offsets, changing the behavior.
    */
   if (unlikely(INTEL_DEBUG & DEBUG_REEMIT))
      ice->state.dirty |= IRIS_ALL_DIRTY_FOR_RENDER & ~IRIS_DIRTY_SO_BUFFERS;

   iris_batch_maybe_flush(batch, 1500);

   iris_update_draw_info(ice, info);

   if (screen->devinfo.gen == 9)
     gen9_emit_preempt_wa(ice, batch, info);

   iris_update_compiled_shaders(ice);

   if (ice->state.dirty & IRIS_DIRTY_RENDER_RESOLVES_AND_FLUSHES) {
      bool draw_aux_buffer_disabled[BRW_MAX_DRAW_BUFFERS] = { };
      for (gl_shader_stage stage = 0; stage < MESA_SHADER_COMPUTE; stage++) {
         if (ice->shaders.prog[stage])
            iris_predraw_resolve_inputs(ice, batch, draw_aux_buffer_disabled,
                                        stage, true);
      }
      iris_predraw_resolve_framebuffer(ice, batch, draw_aux_buffer_disabled);
   }

   iris_binder_reserve_3d(ice);

   ice->vtbl.update_surface_base_address(batch, &ice->state.binder);
   ice->vtbl.upload_render_state(ice, batch, info);

   iris_postdraw_update_resolve_tracking(ice, batch);

   ice->state.dirty &= ~IRIS_ALL_DIRTY_FOR_RENDER;
}

static void
iris_update_grid_size_resource(struct iris_context *ice,
                               const struct pipe_grid_info *grid)
{
   const struct iris_screen *screen = (void *) ice->ctx.screen;
   const struct isl_device *isl_dev = &screen->isl_dev;
   struct iris_state_ref *grid_ref = &ice->state.grid_size;
   struct iris_state_ref *state_ref = &ice->state.grid_surf_state;

   // XXX: if the shader doesn't actually care about the grid info,
   // don't bother uploading the surface?

   if (grid->indirect) {
      pipe_resource_reference(&grid_ref->res, grid->indirect);
      grid_ref->offset = grid->indirect_offset;

      /* Zero out the grid size so that the next non-indirect grid launch will
       * re-upload it properly.
       */
      memset(ice->state.last_grid, 0, sizeof(ice->state.last_grid));
   } else {
      /* If the size is the same, we don't need to upload anything. */
      if (memcmp(ice->state.last_grid, grid->grid, sizeof(grid->grid)) == 0)
         return;

      memcpy(ice->state.last_grid, grid->grid, sizeof(grid->grid));

      u_upload_data(ice->state.dynamic_uploader, 0, sizeof(grid->grid), 4,
                    grid->grid, &grid_ref->offset, &grid_ref->res);
   }

   void *surf_map = NULL;
   u_upload_alloc(ice->state.surface_uploader, 0, isl_dev->ss.size,
                  isl_dev->ss.align, &state_ref->offset, &state_ref->res,
                  &surf_map);
   state_ref->offset +=
      iris_bo_offset_from_base_address(iris_resource_bo(state_ref->res));
   isl_buffer_fill_state(&screen->isl_dev, surf_map,
                         .address = grid_ref->offset +
                            iris_resource_bo(grid_ref->res)->gtt_offset,
                         .size_B = sizeof(grid->grid),
                         .format = ISL_FORMAT_RAW,
                         .stride_B = 1,
                         .mocs = 4); // XXX: MOCS

   ice->state.dirty |= IRIS_DIRTY_BINDINGS_CS;
}

void
iris_launch_grid(struct pipe_context *ctx, const struct pipe_grid_info *grid)
{
   struct iris_context *ice = (struct iris_context *) ctx;
   struct iris_batch *batch = &ice->batches[IRIS_BATCH_COMPUTE];

   if (ice->state.predicate == IRIS_PREDICATE_STATE_DONT_RENDER)
      return;

   if (unlikely(INTEL_DEBUG & DEBUG_REEMIT))
      ice->state.dirty |= IRIS_ALL_DIRTY_FOR_COMPUTE;

   /* We can't do resolves on the compute engine, so awkwardly, we have to
    * do them on the render batch...
    */
   if (ice->state.dirty & IRIS_DIRTY_COMPUTE_RESOLVES_AND_FLUSHES) {
      iris_predraw_resolve_inputs(ice, &ice->batches[IRIS_BATCH_RENDER], NULL,
                                  MESA_SHADER_COMPUTE, false);
   }

   iris_batch_maybe_flush(batch, 1500);

   //if (dirty & IRIS_DIRTY_UNCOMPILED_CS)
      iris_update_compiled_compute_shader(ice);

   iris_update_grid_size_resource(ice, grid);

   iris_binder_reserve_compute(ice);
   ice->vtbl.update_surface_base_address(batch, &ice->state.binder);

   if (ice->state.compute_predicate) {
      ice->vtbl.load_register_mem64(batch, MI_PREDICATE_RESULT,
                                    ice->state.compute_predicate, 0);
      ice->state.compute_predicate = NULL;
   }

   ice->vtbl.upload_compute_state(ice, batch, grid);

   ice->state.dirty &= ~IRIS_ALL_DIRTY_FOR_COMPUTE;

   /* Note: since compute shaders can't access the framebuffer, there's
    * no need to call iris_postdraw_update_resolve_tracking.
    */
}