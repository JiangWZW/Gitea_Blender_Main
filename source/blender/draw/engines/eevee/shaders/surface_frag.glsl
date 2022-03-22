
/* Required by some nodes. */
#pragma BLENDER_REQUIRE(common_hair_lib.glsl)
#pragma BLENDER_REQUIRE(common_utiltex_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_geom_lib.glsl)

#pragma BLENDER_REQUIRE(closure_eval_impl_lib.glsl)

#pragma BLENDER_REQUIRE(surface_lib.glsl)
#pragma BLENDER_REQUIRE(volumetric_lib.glsl)

#ifdef USE_ALPHA_BLEND
/* Use dual source blending to be able to make a whole range of effects. */
layout(location = 0, index = 0) out vec4 outRadiance;
layout(location = 0, index = 1) out vec4 outTransmittance;

#else /* OPAQUE */
layout(location = 0) out vec4 outRadiance;
layout(location = 1) out vec2 ssrNormals;
layout(location = 2) out vec4 ssrData;
layout(location = 3) out vec3 sssIrradiance;
layout(location = 4) out float sssRadius;
layout(location = 5) out vec3 sssAlbedo;

#endif

void main()
{
#if defined(WORLD_BACKGROUND) || defined(PROBE_CAPTURE)
  attrib_load();
#endif

  g_data = init_globals();

  Closure cl = nodetree_exec();

  float holdout = saturate(1.0 - cl.holdout);
  float transmit = saturate(avg(cl.transmittance));
  float alpha = 1.0 - transmit;

#ifdef USE_ALPHA_BLEND
  vec2 uvs = gl_FragCoord.xy * volCoordScale.zw;
  vec3 vol_transmit, vol_scatter;
  volumetric_resolve(uvs, gl_FragCoord.z, vol_transmit, vol_scatter);

  /* Removes part of the volume scattering that have
   * already been added to the destination pixels.
   * Since we do that using the blending pipeline we need to account for material transmittance. */
  vol_scatter -= vol_scatter * cl.transmittance;

  cl.radiance = cl.radiance * holdout * vol_transmit + vol_scatter;
  outRadiance = vec4(cl.radiance, alpha * holdout);
  outTransmittance = vec4(cl.transmittance, transmit) * holdout;
#else
  outRadiance = vec4(cl.radiance, holdout);
  ssrNormals = normal_encode(normalize(mat3(ViewMatrix) * out_ssr_N), vec3(0.0));
  ssrData = vec4(out_ssr_color, out_ssr_roughness);
  sssIrradiance = out_sss_radiance;
  sssRadius = out_sss_radius;
  sssAlbedo = out_sss_color;
#endif

#ifdef USE_REFRACTION
  /* SSRefraction pass is done after the SSS pass.
   * In order to not lose the diffuse light totally we
   * need to merge the SSS radiance to the main radiance. */
  const bool use_refraction = true;
#else
  const bool use_refraction = false;
#endif
  /* For Probe capture */
  if (!sssToggle || use_refraction) {
    outRadiance.rgb += out_sss_radiance * out_sss_color;
  }

#ifndef USE_ALPHA_BLEND
  float alpha_div = safe_rcp(alpha);
  outRadiance.rgb *= alpha_div;
  ssrData.rgb *= alpha_div;
  sssAlbedo.rgb *= alpha_div;
#endif

#ifdef LOOKDEV
  /* Lookdev spheres are rendered in front. */
  gl_FragDepth = 0.0;
#endif
}

vec3 attr_load_orco(vec4 orco)
{
  return -g_data.N;
}

/* Unsupported. */
vec4 attr_load_tangent(vec4 tangent)
{
  return vec4(0);
}
vec4 attr_load_vec4(vec4 attr)
{
  return vec4(0);
}
vec3 attr_load_vec3(vec3 attr)
{
  return vec3(0);
}
vec2 attr_load_vec2(vec2 attr)
{
  return vec2(0);
}
vec4 attr_load_color(vec4 attr)
{
  return vec4(0);
}
vec3 attr_load_uv(vec3 attr)
{
  return vec3(0);
}
