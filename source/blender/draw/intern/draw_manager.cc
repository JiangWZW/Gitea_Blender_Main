/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. */

/** \file
 * \ingroup draw
 */

#include "GPU_compute.h"

#include "draw_manager.h"
#include "draw_manager.hh"
#include "draw_shader.h"

namespace blender::draw {

void Manager::begin_sync()
{
#ifdef DEBUG
  /* Detect non-init data. */
  memset(matrix_buf.data(), 0xF0, resource_len * sizeof(*matrix_buf.data()));
  memset(bounds_buf.data(), 0xF0, resource_len * sizeof(*bounds_buf.data()));
  memset(infos_buf.data(), 0xF0, resource_len * sizeof(*infos_buf.data()));
#endif
  resource_len = 0;
  /* TODO(fclem): Resize buffers if too big, but with an hysteresis threshold. */

  object_active = DST.draw_ctx.obact;

  /* Init the 0 resource. */
  resource_handle(float4x4::identity());
}

void Manager::end_sync()
{
  /* Make sure all buffers have the right amount of data. */
  matrix_buf.get_or_resize(resource_len - 1);
  bounds_buf.get_or_resize(resource_len - 1);
  infos_buf.get_or_resize(resource_len - 1);

  /* Dispatch compute to finalize the resources on GPU. Save a bit of CPU time. */
  uint thread_groups = divide_ceil_u(resource_len, DRW_FINALIZE_GROUP_SIZE);
  GPUShader *shader = DRW_shader_draw_resource_finalize_get();
  GPU_shader_bind(shader);
  GPU_shader_uniform_1i(shader, "resource_len", resource_len);
  GPU_storagebuf_bind(matrix_buf, 0);
  GPU_storagebuf_bind(bounds_buf, 1);
  GPU_storagebuf_bind(infos_buf, 2);
  GPU_compute_dispatch(shader, thread_groups, 1, 1);
}

void Manager::submit(const PassSimple &pass)
{
  command::RecordingState state;
  pass.submit(state);
}

void Manager::submit(const PassMain &pass, View &view)
{
  view.bind();

  view.compute_visibility(bounds_buf, resource_len);

  GPU_storagebuf_bind(matrix_buf, DRW_OBJ_MAT_SLOT);
  GPU_storagebuf_bind(infos_buf, DRW_OBJ_INFOS_SLOT);
  // GPU_storagebuf_bind(attribute_buf, DRW_OBJ_ATTR_SLOT); /* TODO */

  command::RecordingState state;

  pass.draw_commands_buf_.bind(resource_id_buf);

  // GPU_storagebuf_bind(resource_id_buf, DRW_COMMAND_SLOT);

  pass.submit(state);
}

}  // namespace blender::draw
