
#pragma BLENDER_REQUIRE(common_hair_lib.glsl)
#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(common_math_lib.glsl)
#pragma BLENDER_REQUIRE(common_utiltex_lib.glsl)
#pragma BLENDER_REQUIRE(closure_eval_impl_lib.glsl)
#pragma BLENDER_REQUIRE(surface_lib.glsl)

#ifndef HAIR_SHADER
in vec3 pos;
in vec3 nor;
#endif

RESOURCE_ID_VARYING

void main()
{
  GPU_INTEL_VERTEX_SHADER_WORKAROUND

  PASS_RESOURCE_ID

#ifdef HAIR_SHADER
  hairStrandID = hair_get_strand_id();
  vec3 pos, binor;
  hair_get_pos_tan_binor_time((ProjectionMatrix[3][3] == 0.0),
                              ModelMatrixInverse,
                              ViewMatrixInverse[3].xyz,
                              ViewMatrixInverse[2].xyz,
                              pos,
                              hairTangent,
                              binor,
                              hairTime,
                              hairThickness,
                              hairThickTime);
  worldNormal = cross(hairTangent, binor);
  vec3 world_pos = pos;
#elif defined(POINTCLOUD_SHADER)
  pointcloud_get_pos_and_radius(pointPosition, pointRadius);
  pointID = gl_VertexID;
#else
  vec3 world_pos = point_object_to_world(pos);
#endif

  gl_Position = point_world_to_ndc(world_pos);

  /* Used for planar reflections */
  gl_ClipDistance[0] = dot(vec4(world_pos, 1.0), clipPlanes[0]);

#ifdef MESH_SHADER
  worldPosition = world_pos;
  viewPosition = point_world_to_view(worldPosition);

#  ifndef HAIR_SHADER
  worldNormal = normalize(normal_object_to_world(nor));
#  endif

  /* No need to normalize since this is just a rotation. */
  viewNormal = normal_world_to_view(worldNormal);

  attrib_load();
#endif
}

#ifdef HAIR_SHADER
#  ifdef OBINFO_LIB
vec3 attr_load_orco(samplerBuffer cd_buf)
{
  vec3 P = hair_get_strand_pos();
  vec3 lP = transform_point(ModelMatrixInverse, P);
  return OrcoTexCoFactors[0].xyz + lP * OrcoTexCoFactors[1].xyz;
}
#  endif

vec4 attr_load_tangent(samplerBuffer cd_buf)
{
  /* Not supported. */
  return vec4(0.0, 0.0, 0.0, 1.0);
}

vec3 attr_load_uv(samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, interp.hair_strand_id).rgb;
}

vec4 attr_load_color(samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, interp.hair_strand_id).rgba;
}

vec4 attr_load_vec4(samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, interp.hair_strand_id).rgba;
}

vec3 attr_load_vec3(samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, interp.hair_strand_id).rgb;
}

vec2 attr_load_vec2(samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, interp.hair_strand_id).rg;
}

#else

#  ifdef OBINFO_LIB
vec3 attr_load_orco(vec4 orco)
{
  /* We know when there is no orco layer when orco.w is 1.0 because it uses the generic vertex
   * attrib (which is [0,0,0,1]). */
  if (orco.w == 0.0) {
    return orco.xyz * 0.5 + 0.5;
  }
  else {
    /* If the object does not have any deformation, the orco layer calculation is done on the fly
     * using the orco_madd factors. */
    return OrcoTexCoFactors[0].xyz + pos * OrcoTexCoFactors[1].xyz;
  }
}
#  endif

vec4 attr_load_tangent(vec4 tangent)
{
  tangent.xyz = safe_normalize(normal_object_to_world(tangent.xyz));
  return tangent;
}

/* Simple passthrough. */
vec4 attr_load_vec4(vec4 attr)
{
  return attr;
}
vec3 attr_load_vec3(vec3 attr)
{
  return attr;
}
vec2 attr_load_vec2(vec2 attr)
{
  return attr;
}
vec4 attr_load_color(vec4 attr)
{
  return attr;
}
vec3 attr_load_uv(vec3 attr)
{
  return attr;
}
#endif
