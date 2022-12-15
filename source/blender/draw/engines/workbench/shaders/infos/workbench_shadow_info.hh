/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_defines.h"

#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Common
 * \{ */

GPU_SHADER_INTERFACE_INFO(workbench_shadow_iface, "vData")
    .smooth(Type::VEC3, "pos")
    .smooth(Type::VEC4, "frontPosition")
    .smooth(Type::VEC4, "backPosition");

GPU_SHADER_CREATE_INFO(workbench_shadow_common)
    .vertex_in(0, Type::VEC3, "pos")
    .vertex_out(workbench_shadow_iface)
    .push_constant(Type::FLOAT, "lightDistance")
    .push_constant(Type::VEC3, "lightDirection")
    .vertex_source("workbench_shadow_vert.glsl")
    .additional_info("draw_mesh");

GPU_SHADER_CREATE_INFO(workbench_next_shadow_common)
    .vertex_in(0, Type::VEC3, "pos")
    .vertex_out(workbench_shadow_iface)
    .define("WORKBENCH_NEXT")
    .uniform_buf(1, "ShadowPassData", "pass_data")
    .push_constant(Type::VEC3, "lightDirection")
    .typedef_source("workbench_shader_shared.h")
    .vertex_source("workbench_shadow_vert.glsl")
    .additional_info("draw_view")
    .additional_info("draw_modelmat_new")
    .additional_info("draw_resource_handle_new");

GPU_SHADER_CREATE_INFO(workbench_next_shadow_visibility_compute_common)
    .local_group_size(DRW_VISIBILITY_GROUP_SIZE)
    .define("DRW_VIEW_LEN", "64")
    .storage_buf(0, Qualifier::READ, "ObjectBounds", "bounds_buf[]")
    .uniform_buf(2, "ExtrudedFrustum", "extruded_frustum")
    .push_constant(Type::BOOL, "forced_fail_pass")
    .push_constant(Type::INT, "resource_len")
    .push_constant(Type::INT, "view_len")
    .push_constant(Type::INT, "visibility_word_per_draw")
    .push_constant(Type::VEC3, "shadow_direction")
    .typedef_source("workbench_shader_shared.h")
    .compute_source("workbench_shadow_visibility_comp.glsl")
    .additional_info("draw_view", "draw_view_culling");

GPU_SHADER_CREATE_INFO(workbench_next_shadow_visibility_compute_dynamic_pass_type)
    .additional_info("workbench_next_shadow_visibility_compute_common")
    .define("DYNAMIC_PASS_SELECTION")
    .storage_buf(1, Qualifier::READ_WRITE, "uint", "pass_visibility_buf[]")
    .storage_buf(2, Qualifier::READ_WRITE, "uint", "fail_visibility_buf[]")
    .do_static_compilation(true);

GPU_SHADER_CREATE_INFO(workbench_next_shadow_visibility_compute_static_pass_type)
    .additional_info("workbench_next_shadow_visibility_compute_common")
    .storage_buf(1, Qualifier::READ_WRITE, "uint", "visibility_buf[]")
    .do_static_compilation(true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Manifold Type
 * \{ */

GPU_SHADER_CREATE_INFO(workbench_shadow_manifold)
    .geometry_layout(PrimitiveIn::LINES_ADJACENCY, PrimitiveOut::TRIANGLE_STRIP, 4, 1)
    .geometry_source("workbench_shadow_geom.glsl");

GPU_SHADER_CREATE_INFO(workbench_shadow_no_manifold)
    .geometry_layout(PrimitiveIn::LINES_ADJACENCY, PrimitiveOut::TRIANGLE_STRIP, 4, 2)
    .geometry_source("workbench_shadow_geom.glsl");

/** \} */

/* -------------------------------------------------------------------- */
/** \name Caps Type
 * \{ */

GPU_SHADER_CREATE_INFO(workbench_shadow_caps)
    .geometry_layout(PrimitiveIn::TRIANGLES, PrimitiveOut::TRIANGLE_STRIP, 3, 2)
    .geometry_source("workbench_shadow_caps_geom.glsl");

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debug Type
 * \{ */

GPU_SHADER_CREATE_INFO(workbench_shadow_no_debug)
    .fragment_source("gpu_shader_depth_only_frag.glsl");

GPU_SHADER_CREATE_INFO(workbench_shadow_debug)
    .fragment_out(0, Type::VEC4, "materialData")
    .fragment_out(1, Type::VEC4, "normalData")
    .fragment_out(2, Type::UINT, "objectId")
    .fragment_source("workbench_shadow_debug_frag.glsl");

GPU_SHADER_CREATE_INFO(workbench_next_shadow_no_debug)
    .additional_info("workbench_shadow_no_debug");

GPU_SHADER_CREATE_INFO(workbench_next_shadow_debug).additional_info("workbench_shadow_debug");

/** \} */

/* -------------------------------------------------------------------- */
/** \name Variations Declaration
 * \{ */

#define WORKBENCH_SHADOW_VARIATIONS(common, prefix, suffix, ...) \
  GPU_SHADER_CREATE_INFO(prefix##_pass_manifold_no_caps##suffix) \
      .define("SHADOW_PASS") \
      .additional_info(common, "workbench_shadow_manifold", __VA_ARGS__) \
      .do_static_compilation(true); \
  GPU_SHADER_CREATE_INFO(prefix##_pass_no_manifold_no_caps##suffix) \
      .define("SHADOW_PASS") \
      .define("DOUBLE_MANIFOLD") \
      .additional_info(common, "workbench_shadow_no_manifold", __VA_ARGS__) \
      .do_static_compilation(true); \
  GPU_SHADER_CREATE_INFO(prefix##_fail_manifold_caps##suffix) \
      .define("SHADOW_FAIL") \
      .additional_info(common, "workbench_shadow_caps", __VA_ARGS__) \
      .do_static_compilation(true); \
  GPU_SHADER_CREATE_INFO(prefix##_fail_manifold_no_caps##suffix) \
      .define("SHADOW_FAIL") \
      .additional_info(common, "workbench_shadow_manifold", __VA_ARGS__) \
      .do_static_compilation(true); \
  GPU_SHADER_CREATE_INFO(prefix##_fail_no_manifold_caps##suffix) \
      .define("SHADOW_FAIL") \
      .define("DOUBLE_MANIFOLD") \
      .additional_info(common, "workbench_shadow_caps", __VA_ARGS__) \
      .do_static_compilation(true); \
  GPU_SHADER_CREATE_INFO(prefix##_fail_no_manifold_no_caps##suffix) \
      .define("SHADOW_FAIL") \
      .define("DOUBLE_MANIFOLD") \
      .additional_info(common, "workbench_shadow_no_manifold", __VA_ARGS__) \
      .do_static_compilation(true);

WORKBENCH_SHADOW_VARIATIONS("workbench_shadow_common",
                            workbench_shadow,
                            ,
                            "workbench_shadow_no_debug")

WORKBENCH_SHADOW_VARIATIONS("workbench_shadow_common",
                            workbench_shadow,
                            _debug,
                            "workbench_shadow_debug")

WORKBENCH_SHADOW_VARIATIONS("workbench_next_shadow_common",
                            workbench_next_shadow,
                            ,
                            "workbench_next_shadow_no_debug")

WORKBENCH_SHADOW_VARIATIONS("workbench_next_shadow_common",
                            workbench_next_shadow,
                            _debug,
                            "workbench_next_shadow_debug")

/** \} */
