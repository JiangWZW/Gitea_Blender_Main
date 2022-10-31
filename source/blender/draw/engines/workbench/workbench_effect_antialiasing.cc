/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "workbench_private.hh"

#include "BLI_jitter_2d.h"
#include "smaa_textures.h"

namespace blender::workbench {

class TaaSamples {
  void init_samples(blender::Array<float2> &samples, const int size)
  {
    samples = blender::Array<float2>(size);
    BLI_jitter_init((float(*)[2])samples.begin(), size);

    /* find closest element to center */
    int closest_index = 0;
    float closest_squared_distance = 1.0f;

    for (int i : samples.index_range()) {
      float2 sample = samples[i];
      const float squared_dist = len_squared_v2(sample);
      if (squared_dist < closest_squared_distance) {
        closest_squared_distance = squared_dist;
        closest_index = i;
      }
    }

    float2 closest_sample = samples[closest_index];

    for (float2 &sample : samples) {
      /* move jitter samples so that closest sample is in center */
      sample -= closest_sample;
      /* Avoid samples outside range (wrap around). */
      sample = {fmodf(sample.x + 0.5f, 1.0f), fmodf(sample.y + 0.5f, 1.0f)};
      /* Recenter the distribution[-1..1]. */
      sample = (sample * 2.0f) - 1.0f;
    }

    /* swap center sample to the start of the array */
    if (closest_index != 0) {
      swap_v2_v2(samples[0], samples[closest_index]);
    }

    /* Sort list based on farthest distance with previous. */
    for (int i = 0; i < size - 2; i++) {
      float squared_dist = 0.0;
      int index = i;
      for (int j = i + 1; j < size; j++) {
        const float _squared_dist = len_squared_v2(samples[i] - samples[j]);
        if (_squared_dist > squared_dist) {
          squared_dist = _squared_dist;
          index = j;
        }
      }
      swap_v2_v2(samples[i + 1], samples[index]);
    }
  }

 public:
  blender::Array<float2> x5;
  blender::Array<float2> x8;
  blender::Array<float2> x11;
  blender::Array<float2> x16;
  blender::Array<float2> x32;

  TaaSamples()
  {
    init_samples(x5, 5);
    init_samples(x8, 8);
    init_samples(x11, 11);
    init_samples(x16, 16);
    init_samples(x32, 32);
  }
};

static TaaSamples TAA_SAMPLES = TaaSamples();

static float filter_blackman_harris(float x, const float width)
{
  if (x > width * 0.5f) {
    return 0.0f;
  }
  x = 2.0f * M_PI * clamp_f((x / width + 0.5f), 0.0f, 1.0f);
  return 0.35875f - 0.48829f * cosf(x) + 0.14128f * cosf(2.0f * x) - 0.01168f * cosf(3.0f * x);
}

/* Compute weights for the 3x3 neighborhood using a 1.5px filter. */
static void setup_taa_weights(const float2 offset, float r_weights[9], float &r_weight_sum)
{
  /* NOTE: If filter width is bigger than 2.0f, then we need to sample more neighborhood. */
  const float filter_width = 2.0f;
  r_weight_sum = 0.0f;
  int i = 0;
  for (int x = -1; x <= 1; x++) {
    for (int y = -1; y <= 1; y++, i++) {
      float2 sample_co = float2(x, y) - offset;
      float r = len_v2(sample_co);
      /* fclem: is radial distance ok here? */
      float weight = filter_blackman_harris(r, filter_width);
      r_weight_sum += weight;
      r_weights[i] = weight;
    }
  }
}

AntiAliasingPass::AntiAliasingPass()
{
  taa_accumulation_sh = GPU_shader_create_from_info_name("workbench_taa");
  smaa_edge_detect_sh = GPU_shader_create_from_info_name("workbench_smaa_stage_0");
  smaa_aa_weight_sh = GPU_shader_create_from_info_name("workbench_smaa_stage_1");
  smaa_resolve_sh = GPU_shader_create_from_info_name("workbench_smaa_stage_2");

  smaa_search_tx.ensure_2d(GPU_R8, {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT});
  GPU_texture_update(smaa_search_tx, GPU_DATA_UBYTE, searchTexBytes);
  GPU_texture_filter_mode(smaa_search_tx, true);

  smaa_area_tx.ensure_2d(GPU_RG8, {AREATEX_WIDTH, AREATEX_HEIGHT});
  GPU_texture_update(smaa_area_tx, GPU_DATA_UBYTE, areaTexBytes);
  GPU_texture_filter_mode(smaa_area_tx, true);
}

AntiAliasingPass::~AntiAliasingPass()
{
  DRW_SHADER_FREE_SAFE(taa_accumulation_sh);
  DRW_SHADER_FREE_SAFE(smaa_edge_detect_sh);
  DRW_SHADER_FREE_SAFE(smaa_aa_weight_sh);
  DRW_SHADER_FREE_SAFE(smaa_resolve_sh);
}

void AntiAliasingPass::init(const SceneState &scene_state)
{
  if (scene_state.reset_taa) {
    sample = 0;
  }
  sample_len = scene_state.aa_samples;

  /*TODO(Miguel Pozo): This can probably be removed.*/
  /*
  if (sample_len > 0 && valid_history == false) {
    sample = 0;
  }
  */
}

void AntiAliasingPass::sync(SceneResources &resources, int2 resolution)
{
  if (sample_len > 0) {
    taa_accumulation_tx.ensure_2d(GPU_RGBA16F, resolution);
    sample0_depth_tx.ensure_2d(GPU_DEPTH24_STENCIL8, resolution);
  }
  else {
    taa_accumulation_tx.free();
    sample0_depth_tx.free();
  }

  taa_accumulation_ps_.init();
  taa_accumulation_ps_.state_set(sample == 0 ? DRW_STATE_WRITE_COLOR :
                                               DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ADD_FULL);
  taa_accumulation_ps_.shader_set(taa_accumulation_sh);
  taa_accumulation_ps_.bind_texture("colorBuffer", &resources.color_tx);
  taa_accumulation_ps_.push_constant("samplesWeights", weights, 9);
  taa_accumulation_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  smaa_edge_detect_ps_.init();
  smaa_edge_detect_ps_.state_set(DRW_STATE_WRITE_COLOR);
  smaa_edge_detect_ps_.shader_set(smaa_edge_detect_sh);
  smaa_edge_detect_ps_.bind_texture("colorTex", &taa_accumulation_tx);
  smaa_edge_detect_ps_.push_constant("viewportMetrics", &smaa_viewport_metrics, 1);
  smaa_edge_detect_ps_.clear_color(float4(0.0f));
  smaa_edge_detect_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  smaa_aa_weight_ps_.init();
  smaa_aa_weight_ps_.state_set(DRW_STATE_WRITE_COLOR);
  smaa_aa_weight_ps_.shader_set(smaa_aa_weight_sh);
  smaa_aa_weight_ps_.bind_texture("edgesTex", &smaa_edge_tx);
  smaa_aa_weight_ps_.bind_texture("areaTex", smaa_area_tx);
  smaa_aa_weight_ps_.bind_texture("searchTex", smaa_search_tx);
  smaa_aa_weight_ps_.push_constant("viewportMetrics", &smaa_viewport_metrics, 1);
  smaa_aa_weight_ps_.clear_color(float4(0.0f));
  smaa_aa_weight_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  smaa_resolve_ps_.init();
  smaa_resolve_ps_.state_set(DRW_STATE_WRITE_COLOR);
  smaa_resolve_ps_.shader_set(smaa_resolve_sh);
  smaa_resolve_ps_.bind_texture("blendTex", &smaa_weight_tx);
  smaa_resolve_ps_.bind_texture("colorTex", &taa_accumulation_tx);
  smaa_resolve_ps_.push_constant("viewportMetrics", &smaa_viewport_metrics, 1);
  smaa_resolve_ps_.push_constant("mixFactor", &smaa_mix_factor, 1);
  smaa_resolve_ps_.push_constant("taaAccumulatedWeight", &weight_accum, 1);
  smaa_resolve_ps_.clear_color(float4(0.0f));
  smaa_resolve_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);
}

bool AntiAliasingPass::setup_view(View &view, int2 resolution)
{
  if (sample_len == 0) {
    /* AA disabled. */
    return true;
  }

  if (sample >= sample_len) {
    /* TAA accumulation has finished. Just copy the result back */
    return false;
  }

  float2 sample_offset;
  switch (sample_len) {
    default:
    case 5:
      sample_offset = TAA_SAMPLES.x5[sample];
      break;
    case 8:
      sample_offset = TAA_SAMPLES.x8[sample];
      break;
    case 11:
      sample_offset = TAA_SAMPLES.x11[sample];
      break;
    case 16:
      sample_offset = TAA_SAMPLES.x16[sample];
      break;
    case 32:
      sample_offset = TAA_SAMPLES.x32[sample];
      break;
  }

  setup_taa_weights(sample_offset, weights, weights_sum);

  /* TODO(Miguel Pozo): New API equivalent? */
  const DRWView *default_view = DRW_view_default_get();
  float4x4 winmat, viewmat, persmat;
  /* construct new matrices from transform delta */
  DRW_view_winmat_get(default_view, winmat.ptr(), false);
  DRW_view_viewmat_get(default_view, viewmat.ptr(), false);
  DRW_view_persmat_get(default_view, persmat.ptr(), false);

  window_translate_m4(
      winmat.ptr(), persmat.ptr(), sample_offset.x / resolution.x, sample_offset.y / resolution.y);

  view.sync(viewmat, winmat);

  return true;
}

void AntiAliasingPass::draw(Manager &manager,
                            View &view,
                            SceneResources &resources,
                            int2 resolution,
                            GPUTexture *depth_tx,
                            GPUTexture *color_tx)
{
  if (sample_len == 0) {
    /* AA disabled. */
    // valid_history = false;
    /* TODO(Miguel Pozo): Should render to the input color_tx and depth_tx in the first place */
    GPU_texture_copy(color_tx, resources.color_tx);
    GPU_texture_copy(depth_tx, resources.depth_tx);
    return;
  }
  // valid_history = true;

  /**
   * We always do SMAA on top of TAA accumulation, unless the number of samples of TAA is already
   * high. This ensure a smoother transition.
   * If TAA accumulation is finished, we only blit the result.
   */
  const bool last_sample = sample + 1 == sample_len;
  const bool taa_finished = sample >= sample_len; /* TODO(Miguel Pozo): Why is this ever true ? */

  if (!taa_finished) {
    if (sample == 0) {
      weight_accum = 0;
    }
    /* Accumulate result to the TAA buffer. */
    taa_accumulation_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(taa_accumulation_tx));
    taa_accumulation_fb.bind();
    manager.submit(taa_accumulation_ps_, view);
    weight_accum += weights_sum;
  }

  if (sample == 0) {
    if (sample0_depth_tx.is_valid()) {
      GPU_texture_copy(sample0_depth_tx, resources.depth_tx);
    }
    /* TODO(Miguel Pozo): Should render to the input depth_tx in the first place */
    /* Copy back the saved depth buffer for correct overlays. */
    GPU_texture_copy(depth_tx, resources.depth_tx);
  }
  else {
    /* Copy back the saved depth buffer for correct overlays. */
    GPU_texture_copy(depth_tx, sample0_depth_tx);
  }

  if (!DRW_state_is_image_render() || last_sample) {
    smaa_weight_tx.acquire(resolution, GPU_RGBA8);
    smaa_mix_factor = 1.0f - clamp_f(sample / 4.0f, 0.0f, 1.0f);
    smaa_viewport_metrics = float4(float2(1.0f / float2(resolution)), resolution);

    /* After a certain point SMAA is no longer necessary. */
    if (smaa_mix_factor > 0.0f) {
      smaa_edge_tx.acquire(resolution, GPU_RG8);
      smaa_edge_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(smaa_edge_tx));
      smaa_edge_fb.bind();
      manager.submit(smaa_edge_detect_ps_, view);

      smaa_weight_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(smaa_weight_tx));
      smaa_weight_fb.bind();
      manager.submit(smaa_aa_weight_ps_, view);
      smaa_edge_tx.release();
    }
    smaa_resolve_fb.ensure(GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(color_tx));
    smaa_resolve_fb.bind();
    manager.submit(smaa_resolve_ps_, view);
    smaa_weight_tx.release();
  }

  if (!taa_finished) {
    sample++;
  }

  if (!DRW_state_is_image_render() && sample < sample_len) {
    DRW_viewport_request_redraw();
  }
}

}  // namespace blender::workbench
