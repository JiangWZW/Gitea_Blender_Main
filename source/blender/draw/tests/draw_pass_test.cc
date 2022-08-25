/* SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "draw_manager.hh"
#include "draw_shader.h"
#include "draw_testing.hh"

namespace blender::draw {

static void test_draw_pass_all_commands()
{
  Texture tex;
  tex.ensure_2d(GPU_RGBA16, int2(1));

  UniformBuffer<uint4> ubo;
  ubo.push_update();

  StorageBuffer<uint4> ssbo;
  ssbo.push_update();

  float alpha = 0.0f;
  int3 dispatch_size(1);

  PassSimple pass = {"test.all_commands"};
  pass.init();
  pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_STENCIL);
  pass.clear_color_depth_stencil(float4(0.25f, 0.5f, 100.0f, -2000.0f), 0.5f, 0xF0);
  pass.state_stencil(0x80, 0x0F, 0x8F);
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_MODULATE_ALPHA));
  pass.bind("image", tex);
  pass.bind("image", &tex);
  pass.bind("missing_image", as_image(tex));  /* Should not crash. */
  pass.bind("missing_image", as_image(&tex)); /* Should not crash. */
  pass.bind("missing_ubo", ubo);              /* Should not crash. */
  pass.bind("missing_ubo", &ubo);             /* Should not crash. */
  pass.bind("missing_ssbo", ssbo);            /* Should not crash. */
  pass.bind("missing_ssbo", &ssbo);           /* Should not crash. */
  pass.push_constant("alpha", alpha);
  pass.push_constant("alpha", &alpha);
  pass.push_constant("ModelViewProjectionMatrix", float4x4::identity());
  pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);

  /* Should not crash even if shader is not a compute. This is because we only serialize. */
  /* TODO(fclem): Use real compute shader. */
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_MODULATE_ALPHA));
  pass.dispatch(dispatch_size);
  pass.dispatch(&dispatch_size);
  pass.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS);

  /* Change references. */
  alpha = 1.0f;
  dispatch_size = int3(2);

  std::string result = pass.serialize();
  std::stringstream expected;
  expected << ".test.all_commands" << std::endl;
  expected << "  .state_set(6)" << std::endl;
  expected << "  .clear(color=(0.25, 0.5, 100, -2000), depth=0.5, stencil=0b11110000))"
           << std::endl;
  expected << "  .stencil_set(write_mask=0b10000000, compare_mask=0b00001111, reference=0b10001111"
           << std::endl;
  expected << "  .shader_bind(gpu_shader_3D_image_modulate_alpha)" << std::endl;
  expected << "  .bind_texture(0)" << std::endl;
  expected << "  .bind_texture_ref(0)" << std::endl;
  expected << "  .bind_image(-1)" << std::endl;
  expected << "  .bind_image_ref(-1)" << std::endl;
  expected << "  .bind_uniform_buf(-1)" << std::endl;
  expected << "  .bind_uniform_buf_ref(-1)" << std::endl;
  expected << "  .bind_storage_buf(-1)" << std::endl;
  expected << "  .bind_storage_buf_ref(-1)" << std::endl;
  expected << "  .push_constant(2, data=0)" << std::endl;
  expected << "  .push_constant(2, data=1)" << std::endl;
  expected << "  .push_constant(0, data=(" << std::endl;
  expected << "(   1.000000,    0.000000,    0.000000,    0.000000)" << std::endl;
  expected << "(   0.000000,    1.000000,    0.000000,    0.000000)" << std::endl;
  expected << "(   0.000000,    0.000000,    1.000000,    0.000000)" << std::endl;
  expected << "(   0.000000,    0.000000,    0.000000,    1.000000)" << std::endl;
  expected << ")" << std::endl;
  expected << ")" << std::endl;
  expected << "  .draw(inst_len=1, vert_len=3, vert_first=0, res_id=0)" << std::endl;
  expected << "  .shader_bind(gpu_shader_3D_image_modulate_alpha)" << std::endl;
  expected << "  .dispatch(1, 1, 1)" << std::endl;
  expected << "  .dispatch_ref(2, 2, 2)" << std::endl;
  expected << "  .barrier(4)" << std::endl;

  EXPECT_EQ(result, expected.str());

  DRW_shape_cache_free();
}
DRAW_TEST(draw_pass_all_commands)

static void test_draw_pass_sub_ordering()
{
  PassSimple pass = {"test.sub_ordering"};
  pass.init();
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_MODULATE_ALPHA));
  pass.push_constant("test_pass", 1);

  PassSimple::Sub &sub1 = pass.sub("Sub1");
  sub1.push_constant("test_sub1", 11);

  PassSimple::Sub &sub2 = pass.sub("Sub2");
  sub2.push_constant("test_sub2", 21);

  /* Will execute after both sub. */
  pass.push_constant("test_pass", 2);

  /* Will execute after sub1. */
  sub2.push_constant("test_sub2", 22);

  /* Will execute before sub2. */
  sub1.push_constant("test_sub1", 12);

  /* Will execute before end of pass. */
  sub2.push_constant("test_sub2", 23);

  std::string result = pass.serialize();
  std::stringstream expected;
  expected << ".test.sub_ordering" << std::endl;
  expected << "  .shader_bind(gpu_shader_3D_image_modulate_alpha)" << std::endl;
  expected << "  .push_constant(-1, data=1)" << std::endl;
  expected << "  .Sub1" << std::endl;
  expected << "    .push_constant(-1, data=11)" << std::endl;
  expected << "  .Sub2" << std::endl;
  expected << "    .push_constant(-1, data=21)" << std::endl;
  expected << "    .push_constant(-1, data=22)" << std::endl;
  expected << "    .push_constant(-1, data=23)" << std::endl;
  expected << "  .push_constant(-1, data=2)" << std::endl;

  EXPECT_EQ(result, expected.str());
}
DRAW_TEST(draw_pass_sub_ordering)

static void test_draw_pass_simple_draw()
{
  PassSimple pass = {"test.simple_draw"};
  pass.init();
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_MODULATE_ALPHA));
  /* Each draw procedural type uses a different batch. Groups are drawn in correct order. */
  pass.draw_procedural(GPU_PRIM_TRIS, 1, 10, 1, {1});
  pass.draw_procedural(GPU_PRIM_POINTS, 4, 20, 2, {2});
  pass.draw_procedural(GPU_PRIM_TRIS, 2, 30, 3, {3});
  pass.draw_procedural(GPU_PRIM_POINTS, 5, 40, 4, ResourceHandle(4, true));
  pass.draw_procedural(GPU_PRIM_LINES, 1, 50, 5, {5});
  pass.draw_procedural(GPU_PRIM_POINTS, 6, 60, 6, {5});
  pass.draw_procedural(GPU_PRIM_TRIS, 3, 70, 7, {6});

  std::string result = pass.serialize();
  std::stringstream expected;
  expected << ".test.simple_draw" << std::endl;
  expected << "  .shader_bind(gpu_shader_3D_image_modulate_alpha)" << std::endl;
  expected << "  .draw(inst_len=1, vert_len=10, vert_first=1, res_id=1)" << std::endl;
  expected << "  .draw(inst_len=4, vert_len=20, vert_first=2, res_id=2)" << std::endl;
  expected << "  .draw(inst_len=2, vert_len=30, vert_first=3, res_id=3)" << std::endl;
  expected << "  .draw(inst_len=5, vert_len=40, vert_first=4, res_id=4)" << std::endl;
  expected << "  .draw(inst_len=1, vert_len=50, vert_first=5, res_id=5)" << std::endl;
  expected << "  .draw(inst_len=6, vert_len=60, vert_first=6, res_id=5)" << std::endl;
  expected << "  .draw(inst_len=3, vert_len=70, vert_first=7, res_id=6)" << std::endl;

  EXPECT_EQ(result, expected.str());

  DRW_shape_cache_free();
}
DRAW_TEST(draw_pass_simple_draw)

static void test_draw_pass_multi_draw()
{
  PassMain pass = {"test.multi_draw"};
  pass.init();
  pass.shader_set(GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE_MODULATE_ALPHA));
  /* Each draw procedural type uses a different batch. Groups are drawn in reverse order. */
  pass.draw_procedural(GPU_PRIM_TRIS, 1, -1, -1, {1});
  pass.draw_procedural(GPU_PRIM_POINTS, 4, -1, -1, {2});
  pass.draw_procedural(GPU_PRIM_TRIS, 2, -1, -1, {3});
  pass.draw_procedural(GPU_PRIM_POINTS, 5, -1, -1, ResourceHandle(4, true));
  pass.draw_procedural(GPU_PRIM_LINES, 1, -1, -1, {5});
  pass.draw_procedural(GPU_PRIM_POINTS, 6, -1, -1, {5});
  pass.draw_procedural(GPU_PRIM_TRIS, 3, -1, -1, {6});

  std::string result = pass.serialize();
  std::stringstream expected;
  expected << ".test.multi_draw" << std::endl;
  expected << "  .shader_bind(gpu_shader_3D_image_modulate_alpha)" << std::endl;
  expected << "  .draw_multi(3)" << std::endl;
  expected << "    .group(id=2, len=1)" << std::endl;
  expected << "      .proto(instance_len=1, resource_id=5, front_face)" << std::endl;
  expected << "    .group(id=1, len=15)" << std::endl;
  expected << "      .proto(instance_len=5, resource_id=4, back_face)" << std::endl;
  expected << "      .proto(instance_len=6, resource_id=5, front_face)" << std::endl;
  expected << "      .proto(instance_len=4, resource_id=2, front_face)" << std::endl;
  expected << "    .group(id=0, len=6)" << std::endl;
  expected << "      .proto(instance_len=3, resource_id=6, front_face)" << std::endl;
  expected << "      .proto(instance_len=2, resource_id=3, front_face)" << std::endl;
  expected << "      .proto(instance_len=1, resource_id=1, front_face)" << std::endl;

  EXPECT_EQ(result, expected.str());

  DRW_shape_cache_free();
}
DRAW_TEST(draw_pass_multi_draw)

}  // namespace blender::draw