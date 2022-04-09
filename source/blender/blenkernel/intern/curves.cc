/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_curves_types.h"
#include "DNA_defaults.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"

#include "BLI_bounds.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_rand.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BKE_anim_data.h"
#include "BKE_curves.hh"
#include "BKE_customdata.h"
#include "BKE_global.h"
#include "BKE_idtype.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_lib_remap.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_object.h"

#include "BLT_translation.h"

#include "DEG_depsgraph_query.h"

#include "BLO_read_write.h"

using blender::float3;
using blender::IndexRange;
using blender::MutableSpan;
using blender::RandomNumberGenerator;
using blender::Span;

static const char *ATTR_POSITION = "position";

static void update_custom_data_pointers(Curves &curves);

static void curves_init_data(ID *id)
{
  Curves *curves = (Curves *)id;
  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(curves, id));

  MEMCPY_STRUCT_AFTER(curves, DNA_struct_default_get(Curves), id);

  new (&curves->geometry) blender::bke::CurvesGeometry();
}

static void curves_copy_data(Main *UNUSED(bmain), ID *id_dst, const ID *id_src, const int flag)
{
  using namespace blender;

  Curves *curves_dst = (Curves *)id_dst;
  const Curves *curves_src = (const Curves *)id_src;
  curves_dst->mat = static_cast<Material **>(MEM_dupallocN(curves_src->mat));

  const bke::CurvesGeometry &src = bke::CurvesGeometry::wrap(curves_src->geometry);
  bke::CurvesGeometry &dst = bke::CurvesGeometry::wrap(curves_dst->geometry);

  /* We need special handling here because the generic ID management code has already done a
   * shallow copy from the source to the destination, and because the copy-on-write functionality
   * isn't supported more generically yet. */

  dst.point_size = src.point_size;
  dst.curve_size = src.curve_size;

  const eCDAllocType alloc_type = (flag & LIB_ID_COPY_CD_REFERENCE) ? CD_REFERENCE : CD_DUPLICATE;
  CustomData_copy(&src.point_data, &dst.point_data, CD_MASK_ALL, alloc_type, dst.point_size);
  CustomData_copy(&src.curve_data, &dst.curve_data, CD_MASK_ALL, alloc_type, dst.curve_size);

  dst.curve_offsets = static_cast<int *>(MEM_dupallocN(src.curve_offsets));

  dst.runtime = MEM_new<bke::CurvesGeometryRuntime>(__func__);

  dst.update_customdata_pointers();

  curves_dst->batch_cache = nullptr;
}

static void curves_free_data(ID *id)
{
  Curves *curves = (Curves *)id;
  BKE_animdata_free(&curves->id, false);

  blender::bke::CurvesGeometry::wrap(curves->geometry).~CurvesGeometry();

  BKE_curves_batch_cache_free(curves);

  MEM_SAFE_FREE(curves->mat);
}

static void curves_foreach_id(ID *id, LibraryForeachIDData *data)
{
  Curves *curves = (Curves *)id;
  for (int i = 0; i < curves->totcol; i++) {
    BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, curves->mat[i], IDWALK_CB_USER);
  }
  BKE_LIB_FOREACHID_PROCESS_IDSUPER(data, curves->surface, IDWALK_CB_NOP);
}

static void curves_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  Curves *curves = (Curves *)id;

  CustomDataLayer *players = nullptr, players_buff[CD_TEMP_CHUNK_SIZE];
  CustomDataLayer *clayers = nullptr, clayers_buff[CD_TEMP_CHUNK_SIZE];
  CustomData_blend_write_prepare(
      &curves->geometry.point_data, &players, players_buff, ARRAY_SIZE(players_buff));
  CustomData_blend_write_prepare(
      &curves->geometry.curve_data, &clayers, clayers_buff, ARRAY_SIZE(clayers_buff));

  /* Write LibData */
  BLO_write_id_struct(writer, Curves, id_address, &curves->id);
  BKE_id_blend_write(writer, &curves->id);

  /* Direct data */
  CustomData_blend_write(writer,
                         &curves->geometry.point_data,
                         players,
                         curves->geometry.point_size,
                         CD_MASK_ALL,
                         &curves->id);
  CustomData_blend_write(writer,
                         &curves->geometry.curve_data,
                         clayers,
                         curves->geometry.curve_size,
                         CD_MASK_ALL,
                         &curves->id);

  BLO_write_int32_array(writer, curves->geometry.curve_size + 1, curves->geometry.curve_offsets);

  BLO_write_pointer_array(writer, curves->totcol, curves->mat);
  if (curves->adt) {
    BKE_animdata_blend_write(writer, curves->adt);
  }

  /* Remove temporary data. */
  if (players && players != players_buff) {
    MEM_freeN(players);
  }
  if (clayers && clayers != clayers_buff) {
    MEM_freeN(clayers);
  }
}

static void curves_blend_read_data(BlendDataReader *reader, ID *id)
{
  Curves *curves = (Curves *)id;
  BLO_read_data_address(reader, &curves->adt);
  BKE_animdata_blend_read_data(reader, curves->adt);

  /* Geometry */
  CustomData_blend_read(reader, &curves->geometry.point_data, curves->geometry.point_size);
  CustomData_blend_read(reader, &curves->geometry.curve_data, curves->geometry.curve_size);
  update_custom_data_pointers(*curves);

  BLO_read_int32_array(reader, curves->geometry.curve_size + 1, &curves->geometry.curve_offsets);

  curves->geometry.runtime = MEM_new<blender::bke::CurvesGeometryRuntime>(__func__);

  /* Materials */
  BLO_read_pointer_array(reader, (void **)&curves->mat);
}

static void curves_blend_read_lib(BlendLibReader *reader, ID *id)
{
  Curves *curves = (Curves *)id;
  for (int a = 0; a < curves->totcol; a++) {
    BLO_read_id_address(reader, curves->id.lib, &curves->mat[a]);
  }
  BLO_read_id_address(reader, curves->id.lib, &curves->surface);
}

static void curves_blend_read_expand(BlendExpander *expander, ID *id)
{
  Curves *curves = (Curves *)id;
  for (int a = 0; a < curves->totcol; a++) {
    BLO_expand(expander, curves->mat[a]);
  }
  BLO_expand(expander, curves->surface);
}

IDTypeInfo IDType_ID_CV = {
    /*id_code */ ID_CV,
    /*id_filter */ FILTER_ID_CV,
    /*main_listbase_index */ INDEX_ID_CV,
    /*struct_size */ sizeof(Curves),
    /*name */ "Hair Curves",
    /*name_plural */ "Hair Curves",
    /*translation_context */ BLT_I18NCONTEXT_ID_CURVES,
    /*flags */ IDTYPE_FLAGS_APPEND_IS_REUSABLE,
    /*asset_type_info */ nullptr,

    /*init_data */ curves_init_data,
    /*copy_data */ curves_copy_data,
    /*free_data */ curves_free_data,
    /*make_local */ nullptr,
    /*foreach_id */ curves_foreach_id,
    /*foreach_cache */ nullptr,
    /*foreach_path */ nullptr,
    /*owner_get */ nullptr,

    /*blend_write */ curves_blend_write,
    /*blend_read_data */ curves_blend_read_data,
    /*blend_read_lib */ curves_blend_read_lib,
    /*blend_read_expand */ curves_blend_read_expand,

    /*blend_read_undo_preserve */ nullptr,

    /*lib_override_apply_post */ nullptr,
};

static void update_custom_data_pointers(Curves &curves)
{
  blender::bke::CurvesGeometry::wrap(curves.geometry).update_customdata_pointers();
}

void *BKE_curves_add(Main *bmain, const char *name)
{
  Curves *curves = static_cast<Curves *>(BKE_id_new(bmain, ID_CV, name));

  return curves;
}

BoundBox *BKE_curves_boundbox_get(Object *ob)
{
  BLI_assert(ob->type == OB_CURVES);
  Curves *curves = static_cast<Curves *>(ob->data);

  if (ob->runtime.bb != nullptr && (ob->runtime.bb->flag & BOUNDBOX_DIRTY) == 0) {
    return ob->runtime.bb;
  }

  if (ob->runtime.bb == nullptr) {
    ob->runtime.bb = MEM_cnew<BoundBox>(__func__);

    blender::bke::CurvesGeometry &geometry = blender::bke::CurvesGeometry::wrap(curves->geometry);

    float3 min(FLT_MAX);
    float3 max(-FLT_MAX);
    if (!geometry.bounds_min_max(min, max)) {
      min = float3(-1);
      max = float3(1);
    }

    BKE_boundbox_init_from_minmax(ob->runtime.bb, min, max);
  }

  return ob->runtime.bb;
}

bool BKE_curves_customdata_required(Curves *UNUSED(curves), CustomDataLayer *layer)
{
  return layer->type == CD_PROP_FLOAT3 && STREQ(layer->name, ATTR_POSITION);
}

Curves *BKE_curves_copy_for_eval(Curves *curves_src, bool reference)
{
  int flags = LIB_ID_COPY_LOCALIZE;

  if (reference) {
    flags |= LIB_ID_COPY_CD_REFERENCE;
  }

  Curves *result = (Curves *)BKE_id_copy_ex(nullptr, &curves_src->id, nullptr, flags);
  return result;
}

static Curves *curves_evaluate_modifiers(struct Depsgraph *depsgraph,
                                         struct Scene *scene,
                                         Object *object,
                                         Curves *curves_input)
{
  Curves *curves = curves_input;

  /* Modifier evaluation modes. */
  const bool use_render = (DEG_get_mode(depsgraph) == DAG_EVAL_RENDER);
  const int required_mode = use_render ? eModifierMode_Render : eModifierMode_Realtime;
  ModifierApplyFlag apply_flag = use_render ? MOD_APPLY_RENDER : MOD_APPLY_USECACHE;
  const ModifierEvalContext mectx = {depsgraph, object, apply_flag};

  /* Get effective list of modifiers to execute. Some effects like shape keys
   * are added as virtual modifiers before the user created modifiers. */
  VirtualModifierData virtualModifierData;
  ModifierData *md = BKE_modifiers_get_virtual_modifierlist(object, &virtualModifierData);

  /* Evaluate modifiers. */
  for (; md; md = md->next) {
    const ModifierTypeInfo *mti = BKE_modifier_get_info(static_cast<ModifierType>(md->type));

    if (!BKE_modifier_is_enabled(scene, md, required_mode)) {
      continue;
    }

    if ((mti->type == eModifierTypeType_OnlyDeform) &&
        (mti->flags & eModifierTypeFlag_AcceptsVertexCosOnly)) {
      /* Ensure we are not modifying the input. */
      if (curves == curves_input) {
        curves = BKE_curves_copy_for_eval(curves, true);
      }

      /* Created deformed coordinates array on demand. */
      blender::bke::CurvesGeometry &geometry = blender::bke::CurvesGeometry::wrap(
          curves->geometry);
      MutableSpan<float3> positions = geometry.positions_for_write();

      mti->deformVerts(md,
                       &mectx,
                       nullptr,
                       reinterpret_cast<float(*)[3]>(positions.data()),
                       curves->geometry.point_size);
    }
  }

  return curves;
}

void BKE_curves_data_update(struct Depsgraph *depsgraph, struct Scene *scene, Object *object)
{
  /* Free any evaluated data and restore original data. */
  BKE_object_free_derived_caches(object);

  /* Evaluate modifiers. */
  Curves *curves = static_cast<Curves *>(object->data);
  Curves *curves_eval = curves_evaluate_modifiers(depsgraph, scene, object, curves);

  /* Assign evaluated object. */
  const bool is_owned = (curves != curves_eval);
  BKE_object_eval_assign_data(object, &curves_eval->id, is_owned);
}

/* Draw Cache */

void (*BKE_curves_batch_cache_dirty_tag_cb)(Curves *curves, int mode) = nullptr;
void (*BKE_curves_batch_cache_free_cb)(Curves *curves) = nullptr;

void BKE_curves_batch_cache_dirty_tag(Curves *curves, int mode)
{
  if (curves->batch_cache) {
    BKE_curves_batch_cache_dirty_tag_cb(curves, mode);
  }
}

void BKE_curves_batch_cache_free(Curves *curves)
{
  if (curves->batch_cache) {
    BKE_curves_batch_cache_free_cb(curves);
  }
}

namespace blender::bke {

Curves *curves_new_nomain(const int points_num, const int curves_num)
{
  Curves *curves = static_cast<Curves *>(BKE_id_new_nomain(ID_CV, nullptr));
  CurvesGeometry &geometry = CurvesGeometry::wrap(curves->geometry);
  geometry.resize(points_num, curves_num);
  return curves;
}

Curves *curves_new_nomain_single(const int points_num, const CurveType type)
{
  Curves *curves = curves_new_nomain(points_num, 1);
  CurvesGeometry &geometry = CurvesGeometry::wrap(curves->geometry);
  geometry.offsets_for_write().last() = points_num;
  geometry.curve_types_for_write().first() = type;
  return curves;
}

}  // namespace blender::bke
