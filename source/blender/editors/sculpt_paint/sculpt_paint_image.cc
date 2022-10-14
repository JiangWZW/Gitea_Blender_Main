/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "ED_paint.h"
#include "ED_uvedit.h"

#include "BLI_math.h"
#include "BLI_math_color_blend.h"
#include "BLI_task.h"

#include "PIL_time_utildefines.h"

#include "GPU_capabilities.h"
#include "GPU_compute.h"
#include "GPU_debug.h"
#include "GPU_shader.h"
#include "GPU_uniform_buffer.h"

#include "IMB_colormanagement.h"
#include "IMB_imbuf.h"

#include "BKE_brush.h"
#include "BKE_image_wrappers.hh"
#include "BKE_material.h"
#include "BKE_pbvh.h"
#include "BKE_pbvh_pixels.hh"

#include "bmesh.h"

#include "NOD_shader.h"

#include "sculpt_intern.h"

namespace blender::ed::sculpt_paint::paint::image {

using namespace blender::bke::pbvh::pixels;
using namespace blender::bke::image;

struct ImageData {
  Image *image = nullptr;
  ImageUser *image_user = nullptr;

  ~ImageData() = default;

  static bool init_active_image(Object *ob,
                                ImageData *r_image_data,
                                PaintModeSettings *paint_mode_settings)
  {
    return BKE_paint_canvas_image_get(
        paint_mode_settings, ob, &r_image_data->image, &r_image_data->image_user);
  }
};

/* -------------------------------------------------------------------- */
/** \name CPU
 * \{ */

struct TexturePaintingUserData {
  Object *ob;
  Brush *brush;
  PBVHNode **nodes;
  ImageData image_data;
  int32_t nodes_len;
};

/** Reading and writing to image buffer with 4 float channels. */
class ImageBufferFloat4 {
 private:
  int pixel_offset;

 public:
  void set_image_position(ImBuf *image_buffer, ushort2 image_pixel_position)
  {
    pixel_offset = int(image_pixel_position.y) * image_buffer->x + int(image_pixel_position.x);
  }

  void next_pixel()
  {
    pixel_offset += 1;
  }

  float4 read_pixel(ImBuf *image_buffer) const
  {
    return &image_buffer->rect_float[pixel_offset * 4];
  }

  void write_pixel(ImBuf *image_buffer, const float4 pixel_data) const
  {
    copy_v4_v4(&image_buffer->rect_float[pixel_offset * 4], pixel_data);
  }

  const char *get_colorspace_name(ImBuf *image_buffer)
  {
    return IMB_colormanagement_get_float_colorspace(image_buffer);
  }
};

/** Reading and writing to image buffer with 4 byte channels. */
class ImageBufferByte4 {
 private:
  int pixel_offset;

 public:
  void set_image_position(ImBuf *image_buffer, ushort2 image_pixel_position)
  {
    pixel_offset = int(image_pixel_position.y) * image_buffer->x + int(image_pixel_position.x);
  }

  void next_pixel()
  {
    pixel_offset += 1;
  }

  float4 read_pixel(ImBuf *image_buffer) const
  {
    float4 result;
    rgba_uchar_to_float(result,
                        static_cast<const uchar *>(
                            static_cast<const void *>(&(image_buffer->rect[pixel_offset]))));
    return result;
  }

  void write_pixel(ImBuf *image_buffer, const float4 pixel_data) const
  {
    rgba_float_to_uchar(
        static_cast<uchar *>(static_cast<void *>(&image_buffer->rect[pixel_offset])), pixel_data);
  }

  const char *get_colorspace_name(ImBuf *image_buffer)
  {
    return IMB_colormanagement_get_rect_colorspace(image_buffer);
  }
};

template<typename ImageBuffer> class PaintingKernel {
  ImageBuffer image_accessor;

  SculptSession *ss;
  const Brush *brush;
  const int thread_id;
  const MVert *mvert;

  float4 brush_color;
  float brush_strength;

  SculptBrushTestFn brush_test_fn;
  SculptBrushTest test;
  /* Pointer to the last used image buffer to detect when buffers are switched. */
  void *last_used_image_buffer_ptr = nullptr;
  const char *last_used_color_space = nullptr;

 public:
  explicit PaintingKernel(SculptSession *ss,
                          const Brush *brush,
                          const int thread_id,
                          const MVert *mvert)
      : ss(ss), brush(brush), thread_id(thread_id), mvert(mvert)
  {
    init_brush_strength();
    init_brush_test();
  }

  bool paint(const Triangles &triangles,
             const PackedPixelRow &pixel_row,
             ImBuf *image_buffer,
             AutomaskingNodeData *automask_data)
  {
    image_accessor.set_image_position(image_buffer, pixel_row.start_image_coordinate);
    const TrianglePaintInput triangle = triangles.get_paint_input(pixel_row.triangle_index);
    float3 pixel_pos = get_start_pixel_pos(triangle, pixel_row);
    const float3 delta_pixel_pos = get_delta_pixel_pos(triangle, pixel_row, pixel_pos);
    bool pixels_painted = false;
    for (int x = 0; x < pixel_row.num_pixels; x++) {
      if (!brush_test_fn(&test, pixel_pos)) {
        pixel_pos += delta_pixel_pos;
        image_accessor.next_pixel();
        continue;
      }

      float4 color = image_accessor.read_pixel(image_buffer);
      const float3 normal(0.0f, 0.0f, 0.0f);
      const float3 face_normal(0.0f, 0.0f, 0.0f);
      const float mask = 0.0f;

      const float falloff_strength = SCULPT_brush_strength_factor(
          ss,
          brush,
          pixel_pos,
          sqrtf(test.dist),
          normal,
          face_normal,
          mask,
          BKE_pbvh_make_vref(PBVH_REF_NONE),
          thread_id,
          automask_data);
      float4 paint_color = brush_color * falloff_strength * brush_strength;
      float4 buffer_color;
      blend_color_mix_float(buffer_color, color, paint_color);
      buffer_color *= brush->alpha;
      IMB_blend_color_float(color, color, buffer_color, static_cast<IMB_BlendMode>(brush->blend));
      image_accessor.write_pixel(image_buffer, color);
      pixels_painted = true;

      image_accessor.next_pixel();
      pixel_pos += delta_pixel_pos;
    }
    return pixels_painted;
  }

  void init_brush_color(ImBuf *image_buffer)
  {
    const char *to_colorspace = image_accessor.get_colorspace_name(image_buffer);
    if (last_used_color_space == to_colorspace) {
      return;
    }
    copy_v3_v3(brush_color,
               ss->cache->invert ? BKE_brush_secondary_color_get(ss->scene, brush) :
                                   BKE_brush_color_get(ss->scene, brush));
    /* NOTE: Brush colors are stored in sRGB. We use math color to follow other areas that
     * use brush colors. From there on we use IMB_colormanagement to convert the brush color to the
     * colorspace of the texture. This isn't ideal, but would need more refactoring to make sure
     * that brush colors are stored in scene linear by default. */
    srgb_to_linearrgb_v3_v3(brush_color, brush_color);
    brush_color[3] = 1.0f;

    const char *from_colorspace = IMB_colormanagement_role_colorspace_name_get(
        COLOR_ROLE_SCENE_LINEAR);
    ColormanageProcessor *cm_processor = IMB_colormanagement_colorspace_processor_new(
        from_colorspace, to_colorspace);
    IMB_colormanagement_processor_apply_v4(cm_processor, brush_color);
    IMB_colormanagement_processor_free(cm_processor);
    last_used_color_space = to_colorspace;
  }

 private:
  void init_brush_strength()
  {
    brush_strength = ss->cache->bstrength;
  }
  void init_brush_test()
  {
    brush_test_fn = SCULPT_brush_test_init_with_falloff_shape(ss, &test, brush->falloff_shape);
  }

  /**
   * Extract the starting pixel position from the given encoded_pixels belonging to the triangle.
   */
  float3 get_start_pixel_pos(const TrianglePaintInput &triangle,
                             const PackedPixelRow &encoded_pixels) const
  {
    return init_pixel_pos(triangle, encoded_pixels.start_barycentric_coord);
  }

  /**
   * Extract the delta pixel position that will be used to advance a Pixel instance to the next
   * pixel.
   */
  float3 get_delta_pixel_pos(const TrianglePaintInput &triangle,
                             const PackedPixelRow &encoded_pixels,
                             const float3 &start_pixel) const
  {
    float3 result = init_pixel_pos(
        triangle, encoded_pixels.start_barycentric_coord + triangle.delta_barycentric_coord);
    return result - start_pixel;
  }

  float3 init_pixel_pos(const TrianglePaintInput &triangle,
                        const float2 &barycentric_weights) const
  {
    const int3 &vert_indices = triangle.vert_indices;
    float3 result;
    const float3 barycentric(barycentric_weights.x,
                             barycentric_weights.y,
                             1.0f - barycentric_weights.x - barycentric_weights.y);
    interp_v3_v3v3v3(result,
                     mvert[vert_indices[0]].co,
                     mvert[vert_indices[1]].co,
                     mvert[vert_indices[2]].co,
                     barycentric);
    return result;
  }
};

static std::vector<bool> init_triangle_brush_test(SculptSession *ss,
                                                  Triangles &triangles,
                                                  const MVert *mvert)
{
  std::vector<bool> brush_test(triangles.size());
  SculptBrushTest test;
  SCULPT_brush_test_init(ss, &test);
  float3 brush_min_bounds(test.location[0] - test.radius,
                          test.location[1] - test.radius,
                          test.location[2] - test.radius);
  float3 brush_max_bounds(test.location[0] + test.radius,
                          test.location[1] + test.radius,
                          test.location[2] + test.radius);
  for (int triangle_index = 0; triangle_index < triangles.size(); triangle_index++) {
    TrianglePaintInput &triangle = triangles.get_paint_input(triangle_index);

    float3 triangle_min_bounds(mvert[triangle.vert_indices[0]].co);
    float3 triangle_max_bounds(triangle_min_bounds);
    for (int i = 1; i < 3; i++) {
      const float3 &pos = mvert[triangle.vert_indices[i]].co;
      triangle_min_bounds.x = min_ff(triangle_min_bounds.x, pos.x);
      triangle_min_bounds.y = min_ff(triangle_min_bounds.y, pos.y);
      triangle_min_bounds.z = min_ff(triangle_min_bounds.z, pos.z);
      triangle_max_bounds.x = max_ff(triangle_max_bounds.x, pos.x);
      triangle_max_bounds.y = max_ff(triangle_max_bounds.y, pos.y);
      triangle_max_bounds.z = max_ff(triangle_max_bounds.z, pos.z);
    }
    brush_test[triangle_index] = isect_aabb_aabb_v3(
        brush_min_bounds, brush_max_bounds, triangle_min_bounds, triangle_max_bounds);
  }
  return brush_test;
}

static void do_paint_pixels(void *__restrict userdata,
                            const int n,
                            const TaskParallelTLS *__restrict tls)
{
  TexturePaintingUserData *data = static_cast<TexturePaintingUserData *>(userdata);
  Object *ob = data->ob;
  SculptSession *ss = ob->sculpt;
  const Brush *brush = data->brush;
  PBVHNode *node = data->nodes[n];

  NodeData &node_data = BKE_pbvh_pixels_node_data_get(*node);
  const int thread_id = BLI_task_parallel_thread_id(tls);
  MVert *mvert = SCULPT_mesh_deformed_mverts_get(ss);

  std::vector<bool> brush_test = init_triangle_brush_test(ss, node_data.triangles, mvert);

  PaintingKernel<ImageBufferFloat4> kernel_float4(ss, brush, thread_id, mvert);
  PaintingKernel<ImageBufferByte4> kernel_byte4(ss, brush, thread_id, mvert);

  AutomaskingNodeData automask_data;
  SCULPT_automasking_node_begin(ob, ss, ss->cache->automasking, &automask_data, data->nodes[n]);

  ImageUser image_user = *data->image_data.image_user;
  bool pixels_updated = false;
  for (UDIMTilePixels &tile_data : node_data.tiles) {
    LISTBASE_FOREACH (ImageTile *, tile, &data->image_data.image->tiles) {
      ImageTileWrapper image_tile(tile);
      if (image_tile.get_tile_number() == tile_data.tile_number) {
        image_user.tile = image_tile.get_tile_number();

        ImBuf *image_buffer = BKE_image_acquire_ibuf(data->image_data.image, &image_user, nullptr);
        if (image_buffer == nullptr) {
          continue;
        }

        if (image_buffer->rect_float != nullptr) {
          kernel_float4.init_brush_color(image_buffer);
        }
        else {
          kernel_byte4.init_brush_color(image_buffer);
        }

        for (const PackedPixelRow &pixel_row : tile_data.pixel_rows) {
          if (!brush_test[pixel_row.triangle_index]) {
            continue;
          }
          bool pixels_painted = false;
          if (image_buffer->rect_float != nullptr) {
            pixels_painted = kernel_float4.paint(
                node_data.triangles, pixel_row, image_buffer, &automask_data);
          }
          else {
            pixels_painted = kernel_byte4.paint(
                node_data.triangles, pixel_row, image_buffer, &automask_data);
          }

          if (pixels_painted) {
            tile_data.mark_dirty(pixel_row);
          }
        }

        BKE_image_release_ibuf(data->image_data.image, image_buffer, nullptr);
        pixels_updated |= tile_data.flags.dirty;
        break;
      }
    }
  }

  node_data.flags.dirty |= pixels_updated;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Undo
 * \{ */

static void undo_region_tiles(
    ImBuf *ibuf, int x, int y, int w, int h, int *tx, int *ty, int *tw, int *th)
{
  int srcx = 0, srcy = 0;
  IMB_rectclip(ibuf, nullptr, &x, &y, &srcx, &srcy, &w, &h);
  *tw = ((x + w - 1) >> ED_IMAGE_UNDO_TILE_BITS);
  *th = ((y + h - 1) >> ED_IMAGE_UNDO_TILE_BITS);
  *tx = (x >> ED_IMAGE_UNDO_TILE_BITS);
  *ty = (y >> ED_IMAGE_UNDO_TILE_BITS);
}

static void push_undo(const NodeData &node_data,
                      Image &image,
                      ImageUser &image_user,
                      const image::ImageTileWrapper &image_tile,
                      ImBuf &image_buffer,
                      ImBuf **tmpibuf)
{
  for (const UDIMTileUndo &tile_undo : node_data.undo_regions) {
    if (tile_undo.tile_number != image_tile.get_tile_number()) {
      continue;
    }
    int tilex, tiley, tilew, tileh;
    PaintTileMap *undo_tiles = ED_image_paint_tile_map_get();
    undo_region_tiles(&image_buffer,
                      tile_undo.region.xmin,
                      tile_undo.region.ymin,
                      BLI_rcti_size_x(&tile_undo.region),
                      BLI_rcti_size_y(&tile_undo.region),
                      &tilex,
                      &tiley,
                      &tilew,
                      &tileh);
    for (int ty = tiley; ty <= tileh; ty++) {
      for (int tx = tilex; tx <= tilew; tx++) {
        ED_image_paint_tile_push(undo_tiles,
                                 &image,
                                 &image_buffer,
                                 tmpibuf,
                                 &image_user,
                                 tx,
                                 ty,
                                 nullptr,
                                 nullptr,
                                 true,
                                 true);
      }
    }
  }
}

static void do_push_undo_tile(void *__restrict userdata,
                              const int n,
                              const TaskParallelTLS *__restrict UNUSED(tls))
{
  TexturePaintingUserData *data = static_cast<TexturePaintingUserData *>(userdata);
  PBVHNode *node = data->nodes[n];

  NodeData &node_data = BKE_pbvh_pixels_node_data_get(*node);
  Image *image = data->image_data.image;
  ImageUser *image_user = data->image_data.image_user;

  ImBuf *tmpibuf = nullptr;
  ImageUser local_image_user = *image_user;
  LISTBASE_FOREACH (ImageTile *, tile, &image->tiles) {
    image::ImageTileWrapper image_tile(tile);
    local_image_user.tile = image_tile.get_tile_number();
    ImBuf *image_buffer = BKE_image_acquire_ibuf(image, &local_image_user, nullptr);
    if (image_buffer == nullptr) {
      continue;
    }

    push_undo(node_data, *image, *image_user, image_tile, *image_buffer, &tmpibuf);
    BKE_image_release_ibuf(image, image_buffer, nullptr);
  }
  if (tmpibuf) {
    IMB_freeImBuf(tmpibuf);
  }
}

static void do_mark_dirty_regions(void *__restrict userdata,
                                  const int n,
                                  const TaskParallelTLS *__restrict UNUSED(tls))
{
  TexturePaintingUserData *data = static_cast<TexturePaintingUserData *>(userdata);
  PBVHNode *node = data->nodes[n];
  BKE_pbvh_pixels_mark_image_dirty(*node, *data->image_data.image, *data->image_data.image_user);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU
 * \{ */
static GPUStorageBuf *gpu_painting_vert_coord_create(SculptSession &ss)
{
  Vector<float4> vert_coords;

  vert_coords.reserve(ss.totvert);
  for (const MVert &mvert : Span<MVert>(ss.mvert, ss.totvert)) {
    float3 co(mvert.co);
    vert_coords.append(float4(co.x, co.y, co.z, 0.0f));
  }
  GPUStorageBuf *result = GPU_storagebuf_create_ex(
      sizeof(float4) * ss.totvert, vert_coords.data(), GPU_USAGE_STATIC, __func__);
  return result;
}

static void init_paint_brush_color(const SculptSession &ss,
                                   const Brush &brush,
                                   PaintBrushData &r_paint_brush)
{
  if (ss.cache->invert) {
    copy_v3_v3(r_paint_brush.color, BKE_brush_secondary_color_get(ss.scene, &brush));
  }
  else {
    copy_v3_v3(r_paint_brush.color, BKE_brush_color_get(ss.scene, &brush));
  }
  /* NOTE: Brush colors are stored in sRGB. We use math color to follow other areas that use
       brush colors. */
  srgb_to_linearrgb_v3_v3(r_paint_brush.color, r_paint_brush.color);
  r_paint_brush.color[3] = 1.0f;
}

static void init_paint_brush_alpha(const Brush &brush, PaintBrushData &r_paint_brush)
{
  r_paint_brush.alpha = brush.alpha;
}

static void init_paint_brush_test(const SculptSession &ss, PaintBrushData &r_paint_brush)
{
  r_paint_brush.test.symm_rot_mat_inv = ss.cache->symm_rot_mat_inv;
}

static void init_paint_brush_falloff(const Brush &brush, PaintBrushData &r_paint_brush)
{
  r_paint_brush.falloff_shape = brush.curve_preset;
}

static void init_paint_brush(const SculptSession &ss,
                             const Brush &brush,
                             PaintBrushData &r_paint_brush)
{
  init_paint_brush_color(ss, brush, r_paint_brush);
  init_paint_brush_alpha(brush, r_paint_brush);
  init_paint_brush_test(ss, r_paint_brush);
  init_paint_brush_falloff(brush, r_paint_brush);
}

/**
 * Tiles are split on the GPU in sub-tiles.
 *
 * Sub tiles are used to reduce the needed memory on the GPU.
 * - Only tiles that are painted on are loaded in memory, painted on and merged back to the actual
 * texture.
 */
template<int32_t Size, int32_t Depth = 512> class GPUSubTileTexture {
  struct Info {
    struct {
      bool in_use_stroke : 1;
      bool in_use_frame : 1;
      /* Does this sub tile needs to be updated (CPU->GPU transfer).*/
      bool needs_update : 1;
      bool should_be_removed : 1;
    } flags;
  };
  const int32_t LayerIdUnused = -1;
  const int32_t LayerIdMarkRemoval = -2;

  Vector<PaintTileData> paint_tiles_;
  Vector<Info> infos_;

  std::array<int32_t, Depth> layer_lookup_;

  GPUTexture *gpu_texture_ = nullptr;
  GPUStorageBuf *tile_buf_ = nullptr;
  int64_t tile_buf_size_ = 0;

 public:
  GPUSubTileTexture()
  {
    paint_tiles_.reserve(Depth);
    infos_.reserve(Depth);

    for (int i = 0; i < Depth; i++) {
      layer_lookup_[i] = LayerIdUnused;
    }
  }

  ~GPUSubTileTexture()
  {
    if (gpu_texture_) {
      GPU_texture_free(gpu_texture_);
      gpu_texture_ = nullptr;
    }

    if (tile_buf_) {
      GPU_storagebuf_free(tile_buf_);
      tile_buf_ = nullptr;
    }
  }

  void reset_usage()
  {
    printf("%s\n", __func__);
    for (Info &info : infos_) {
      info.flags.in_use = false;
    }
  }

  void reset_usage_stroke()
  {
    printf("%s\n", __func__);
    for (Info &info : infos_) {
      info.flags.in_use_stroke = false;
    }
  }

  void reset_usage_frame()
  {
    printf("%s\n", __func__);
    for (Info &info : infos_) {
      info.flags.in_use_frame = false;
    }
  }

  void mark_usage(TileNumber tile_number, int2 sub_tile_id)
  {
    validate();
    for (int index : paint_tiles_.index_range()) {
      PaintTileData &tile = paint_tiles_[index];
      if (tile.tile_number == tile_number && tile.sub_tile_id == sub_tile_id) {
        Info &info = infos_[index];
        if (!info.flags.in_use_stroke) {
          printf("%s: mark existing {tile:%d, sub_tile:%d,%d}\n",
                 __func__,
                 tile_number,
                 UNPACK2(sub_tile_id));
        }
        info.flags.in_use_stroke = true;
        info.flags.in_use_frame = true;
        info.flags.should_be_removed = false;
        validate();
        return;
      }
    }

    /* Tile not yet added, add a new one.*/
    Info info;
    info.flags.in_use_stroke = true;
    info.flags.in_use_frame = true;
    info.flags.needs_update = true;
    info.flags.should_be_removed = false;
    infos_.append(info);

    PaintTileData tile;
    tile.tile_number = tile_number;
    tile.sub_tile_id = sub_tile_id;
    tile.layer_id = LayerIdUnused;
    paint_tiles_.append(tile);

    printf(
        "%s: mark new {tile:%d, sub_tile:%d,%d}\n", __func__, tile_number, UNPACK2(sub_tile_id));
    validate();
  }

  /** Remove all sub tiles that are currently flagged not to be used (flags.in_use = false). */
  void remove_unused()
  {
    validate();
    Vector<int64_t> index_changes;
    for (int layer_id = 0; layer_id < Depth; layer_id++) {
      int index = layer_lookup_[layer_id];
      if (index == -1) {
        continue;
      }
      infos_[index].flags.should_be_removed = false;
      if (infos_[index].flags.in_use_stroke == false) {
        infos_[index].flags.should_be_removed = true;
        PaintTileData &paint_tile = paint_tiles_[index];
        BLI_assert(paint_tile.layer_id == layer_id);
        paint_tile.layer_id = LayerIdMarkRemoval;
        printf("%s: remove sub tile at layer %d->%d {tile:%d, sub_tile:%d,%d}\n",
               __func__,
               layer_id,
               index,
               paint_tile.tile_number,
               UNPACK2(paint_tile.sub_tile_id));
        layer_lookup_[layer_id] = LayerIdUnused;
        index_changes.append(index);
      }
    }

    /* Early exit when no removals where marked. */
    if (index_changes.is_empty()) {
      return;
    }

    for (int layer_id = 0; layer_id < Depth; layer_id++) {
      int decrement = 0;
      int index = layer_lookup_[layer_id];
      if (index == LayerIdUnused) {
        continue;
      }
      for (int64_t change : index_changes) {
        if (index > change) {
          decrement += 1;
        }
      }
      if (decrement == 0) {
        continue;
      }
      printf("%s: correct index of %d->%d to %d\n", __func__, layer_id, index, index - decrement);
      int corrected_index = index - decrement;
      layer_lookup_[layer_id] = corrected_index;
    }

    infos_.remove_if([&](Info &info) { return info.flags.should_be_removed; });
    paint_tiles_.remove_if(
        [&](PaintTileData &tile) { return tile.layer_id == LayerIdMarkRemoval; });
    validate();
  }

  void assign_layer_ids()
  {
    validate();
    for (int64_t index : paint_tiles_.index_range()) {
      PaintTileData &tile = paint_tiles_[index];

      if (tile.layer_id != LayerIdUnused) {
        continue;
      }

      tile.layer_id = first_empty_layer_id();
      layer_lookup_[tile.layer_id] = index;
      printf("%s: assign {tile:%d, sub_tile:%d,%d} to layer %d\n",
             __func__,
             tile.tile_number,
             UNPACK2(tile.sub_tile_id),
             tile.layer_id);
    }
    validate();
  }

  int first_empty_layer_id() const
  {
    for (int i = 0; i < Depth; i++) {
      if (layer_lookup_[i] == LayerIdUnused) {
        return i;
      }
    }

    BLI_assert_unreachable();
    return LayerIdUnused;
  }

  void ensure_gpu_texture()
  {
    if (gpu_texture_ != nullptr) {
      return;
    }
    gpu_texture_ = GPU_texture_create_3d(
        "GPUSubTileTexture", Size, Size, Depth, 1, GPU_RGBA16F, GPU_DATA_FLOAT, nullptr);
  }

  void update_gpu_texture(TileNumber tile_number, ImBuf &UNUSED(image_buffer))
  {
    BLI_assert(gpu_texture_);
    float *buffer = nullptr;
    for (int64_t index : infos_.index_range()) {
      Info &info = infos_[index];
      PaintTileData &tile = paint_tiles_[index];
      if (!info.flags.needs_update) {
        continue;
      }

      if (tile.tile_number != tile_number) {
        continue;
      }

      if (buffer == nullptr) {
        buffer = static_cast<float *>(MEM_callocN(Size * Size * 4 * sizeof(float), __func__));
      }

      /* TODO: Copy correct data from ImBuf.*/

      // GPU_texture_update_sub(
      //    gpu_texture_, GPU_DATA_FLOAT, buffer, 0, 0, tile.layer_id, Size, Size, 1);
      info.flags.needs_update = false;
    }

    if (buffer) {
      MEM_freeN(buffer);
    }
  }

  GPUTexture *gpu_texture_get()
  {
    return gpu_texture_;
  }

  void ensure_tile_buf()
  {
    int64_t needed_size = paint_tiles_.capacity() * sizeof(PaintTileData);

    /* Reuse previous buffer only when exact size, due to potentional read out of bound errors.*/
    if (tile_buf_ && tile_buf_size_ == needed_size) {
      return;
    }

    if (tile_buf_) {
      GPU_storagebuf_free(tile_buf_);
      tile_buf_ = nullptr;
    }
    tile_buf_ = GPU_storagebuf_create(needed_size);
  }

  void update_tile_buf()
  {
    BLI_assert(tile_buf_);
    GPU_storagebuf_update(tile_buf_, paint_tiles_.data());
  }

  GPUStorageBuf *tile_buf_get()
  {
    BLI_assert(tile_buf_);
    return tile_buf_;
  }

  int32_t paint_tiles_len()
  {
    return paint_tiles_.size();
  }

  void bind(GPUShader *shader)
  {
    GPU_texture_image_bind(gpu_texture_get(),
                           GPU_shader_get_texture_binding(shader, "paint_tiles_img"));
    GPU_storagebuf_bind(tile_buf_get(), GPU_shader_get_ssbo(shader, "paint_tile_buf"));
    GPU_shader_uniform_1i(shader, "paint_tile_buf_len", paint_tiles_len());
  }

  /* Go over each paint tile that is currently in use for the current frame.*/
  template<typename Predicate> void foreach_in_frame(Predicate &&predicate)
  {
    for (int64_t index : infos_.index_range()) {
      Info &info = infos_[index];
      if (!info.flags.in_use_frame) {
        continue;
      }
      predicate(paint_tiles_[index]);
    }
  }

  /* Checks if the structure is still consistent.*/
  void validate()
  {
    BLI_assert(paint_tiles_.size() == infos_.size());
    int num_filled_layers = 0;
    for (int index : paint_tiles_.index_range()) {
      PaintTileData &paint_tile = paint_tiles_[index];
      // Info &info = infos_[index];
      BLI_assert(paint_tile.layer_id == LayerIdUnused ||
                 layer_lookup_[paint_tile.layer_id] == index);
      if (paint_tile.layer_id != LayerIdUnused) {
        num_filled_layers += 1;
      }
    }

    int num_filled_lookups = 0;
    for (int index : IndexRange(Depth)) {
      if (layer_lookup_[index] != LayerIdUnused) {
        num_filled_lookups += 1;
      }
    }
    BLI_assert(num_filled_layers == num_filled_lookups);
  }
};

struct GPUSculptPaintData {
  Vector<PaintStepData> steps;
  GPUStorageBuf *step_buf = nullptr;
  size_t step_buf_alloc_size = 0;
  GPUStorageBuf *vert_coord_buf = nullptr;
  GPUUniformBuf *paint_brush_buf = nullptr;

  GPUSubTileTexture<TEXTURE_STREAMING_TILE_SIZE> tile_texture;

  ~GPUSculptPaintData()
  {
    if (vert_coord_buf) {
      GPU_storagebuf_free(vert_coord_buf);
      vert_coord_buf = nullptr;
    }

    if (paint_brush_buf) {
      GPU_uniformbuf_free(paint_brush_buf);
      paint_brush_buf = nullptr;
    }

    if (step_buf) {
      GPU_storagebuf_free(step_buf);
      step_buf = nullptr;
    }
  }

  void update_step_buf()
  {
    int requested_size = sizeof(PaintStepData) * steps.size();
    /* Reallocate buffer when it doesn't fit, or is to big to correct reading from
     * uninitialized memory. */
    const bool reallocate_buf = (requested_size > step_buf_alloc_size) ||
                                (sizeof(PaintStepData) * steps.capacity() < step_buf_alloc_size);

    if (step_buf && reallocate_buf) {
      GPU_storagebuf_free(step_buf);
      step_buf = nullptr;
    }

    if (step_buf == nullptr) {
      step_buf = GPU_storagebuf_create_ex(
          requested_size, nullptr, GPU_USAGE_STATIC, "PaintStepData");
      step_buf_alloc_size = requested_size;
    }

    BLI_assert_msg(sizeof(PaintStepData) * steps.capacity() >= step_buf_alloc_size,
                   "Possible read from unallocated memory as storage buffer is larger than the "
                   "step capacity.");
    GPU_storagebuf_update(step_buf, steps.data());
  }

  void ensure_vert_coord_buf(SculptSession &ss)
  {
    if (!vert_coord_buf) {
      vert_coord_buf = gpu_painting_vert_coord_create(ss);
    }
  }

  void ensure_paint_brush_buf(SculptSession &ss, Brush &brush)
  {
    PaintBrushData paint_brush;
    init_paint_brush(ss, brush, paint_brush);

    if (!paint_brush_buf) {
      paint_brush_buf = GPU_uniformbuf_create_ex(
          sizeof(PaintBrushData), nullptr, "PaintBrushData");
    }

    GPU_uniformbuf_update(paint_brush_buf, &paint_brush);
  }
};

static void ensure_gpu_buffers(TexturePaintingUserData &data)
{
  SculptSession &ss = *data.ob->sculpt;
  if (!ss.mode.texture_paint.gpu_data) {
    printf("%s: new gpu_data\n", __func__);
    ss.mode.texture_paint.gpu_data = MEM_new<GPUSculptPaintData>(__func__);
  }

  GPUSculptPaintData &paint_data = *static_cast<GPUSculptPaintData *>(
      ss.mode.texture_paint.gpu_data);
  if (paint_data.steps.is_empty()) {
    PBVH *pbvh = ss.pbvh;
    BKE_pbvh_frame_selection_clear(pbvh);
  }

  for (PBVHNode *node : MutableSpan<PBVHNode *>(data.nodes, data.nodes_len)) {
    NodeData &node_data = BKE_pbvh_pixels_node_data_get(*node);
    node_data.ensure_gpu_buffers();
  }
}

static BrushVariationFlags determine_shader_variation_flags(const Brush &brush)
{
  BrushVariationFlags result = static_cast<BrushVariationFlags>(0);

  if (brush.falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
    result = static_cast<BrushVariationFlags>(result | BRUSH_TEST_CIRCLE);
  }

  return result;
}

static void gpu_painting_paint_step(TexturePaintingUserData &data,
                                    GPUSculptPaintData &batches,
                                    TileNumber tile_number,
                                    int2 paint_step_range)
{
  BrushVariationFlags variation_flags = determine_shader_variation_flags(*data.brush);
  GPUShader *shader = SCULPT_shader_paint_image_get(variation_flags);
  GPU_shader_bind(shader);
  batches.tile_texture.bind(shader);
  GPU_storagebuf_bind(batches.step_buf, GPU_shader_get_ssbo(shader, "paint_step_buf"));
  GPU_shader_uniform_2iv(shader, "paint_step_range", paint_step_range);
  GPU_uniformbuf_bind(batches.paint_brush_buf,
                      GPU_shader_get_uniform_block(shader, "paint_brush_buf"));
  GPU_storagebuf_bind(batches.vert_coord_buf, GPU_shader_get_ssbo(shader, "vert_coord_buf"));

  /* Dispatch all nodes that paint on the active tile. */
  for (PBVHNode *node : MutableSpan<PBVHNode *>(data.nodes, data.nodes_len)) {
    NodeData &node_data = BKE_pbvh_pixels_node_data_get(*node);
    for (UDIMTilePixels &tile_pixels : node_data.tiles) {
      if (tile_pixels.tile_number != tile_number) {
        continue;
      }

      GPU_storagebuf_bind(node_data.triangles.gpu_buffer,
                          GPU_shader_get_ssbo(shader, "paint_input"));
      GPU_storagebuf_bind(node_data.gpu_buffers.pixels,
                          GPU_shader_get_ssbo(shader, "pixel_row_buf"));

      int pixel_row_len = tile_pixels.pixel_rows.size();
      const int compute_batch_size = GPU_max_work_group_count(0);
      for (int batch_offset = 0; batch_offset != pixel_row_len;
           batch_offset += min_ii(pixel_row_len - batch_offset, compute_batch_size)) {
        const int batch_size = min_ii(pixel_row_len - batch_offset, compute_batch_size);
        GPU_shader_uniform_1i(
            shader, "pixel_row_offset", tile_pixels.gpu_buffer_offset + batch_offset);
        GPU_compute_dispatch(shader, batch_size, 1, 1);
      }
    }
    node_data.ensure_gpu_buffers();
  }
}

/** Merge the changes from the current frame into the GPU texture. */
static void gpu_painting_image_merge(GPUSculptPaintData &batches,
                                     Image &image,
                                     ImageUser &image_user,
                                     ImBuf &image_buffer)
{
  GPUTexture *canvas_tex = BKE_image_get_gpu_texture(&image, &image_user, &image_buffer);
  GPUShader *shader = SCULPT_shader_paint_image_merge_get();
  GPU_shader_bind(shader);
  batches.tile_texture.bind(shader);
  GPU_texture_image_bind(canvas_tex, GPU_shader_get_texture_binding(shader, "texture_img"));
  batches.tile_texture.foreach_in_frame([shader](PaintTileData &paint_tile) {
    printf("%s: merging tile stored on layer %d {tile:%d sub_tile:%d,%d} \n",
           __func__,
           paint_tile.layer_id,
           paint_tile.tile_number,
           UNPACK2(paint_tile.sub_tile_id));
    GPU_shader_uniform_1i(shader, "layer_id", paint_tile.layer_id);
    GPU_compute_dispatch(shader, TEXTURE_STREAMING_TILE_SIZE, TEXTURE_STREAMING_TILE_SIZE, 1);
  });
}

static void init_paint_step(const SculptSession &ss,
                            const Brush &brush,
                            PaintStepData &r_paint_step)
{
  r_paint_step.location = ss.cache->location;
  r_paint_step.radius = ss.cache->radius;
  r_paint_step.mirror_symmetry_pass = ss.cache->mirror_symmetry_pass;
  r_paint_step.hardness = ss.cache->paint_brush.hardness;
  r_paint_step.strength = ss.cache->bstrength;

  if (brush.falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
    plane_from_point_normal_v3(
        r_paint_step.plane_view, r_paint_step.location, ss.cache->view_normal);
  }
  else {
    r_paint_step.plane_view = float4(0.0f);
  }
}

static void dispatch_gpu_painting(TexturePaintingUserData &data)
{
  SculptSession &ss = *data.ob->sculpt;

  GPUSculptPaintData &batches = *static_cast<GPUSculptPaintData *>(ss.mode.texture_paint.gpu_data);

  PaintStepData paint_step;
  init_paint_step(ss, *data.brush, paint_step);
  batches.steps.append(paint_step);
}

/* This should be done based on the frame_selection nodes, otherwise we might be over
 * committing.
 */
static void paint_tiles_mark_used(TexturePaintingUserData &data)
{
  SculptSession &ss = *data.ob->sculpt;
  GPUSculptPaintData &batches = *static_cast<GPUSculptPaintData *>(ss.mode.texture_paint.gpu_data);

  for (PBVHNode *node : MutableSpan<PBVHNode *>(data.nodes, data.nodes_len)) {
    NodeData &node_data = BKE_pbvh_pixels_node_data_get(*node);
    for (UDIMTilePixels &tile : node_data.tiles) {
      for (int x = tile.gpu_sub_tiles.xmin; x <= tile.gpu_sub_tiles.xmax; x++) {
        for (int y = tile.gpu_sub_tiles.ymin; y <= tile.gpu_sub_tiles.ymax; y++) {
          int2 sub_tile_id(x, y);
          batches.tile_texture.mark_usage(tile.tile_number, sub_tile_id);
        }
      }
    }
  }
}

/** Mark all nodes that are used when drawing this frame. */
static void update_frame_selection(TexturePaintingUserData &data)
{
  for (PBVHNode *node : MutableSpan<PBVHNode *>(data.nodes, data.nodes_len)) {
    BKE_pbvh_node_frame_selection_mark(node);
  }
}

using TileNumbers = Vector<TileNumber, 8>;

/* Collect all tile numbers that the node selection is using. This will reduce the read misses
 * when handling multiple Tiles. Most likely only a small amount of tiles are actually used. */
static TileNumbers collect_active_tile_numbers(const TexturePaintingUserData &data)
{
  Vector<TileNumber, 8> result;
  for (PBVHNode *node : Span<PBVHNode *>(data.nodes, data.nodes_len)) {
    NodeData &node_data = BKE_pbvh_pixels_node_data_get(*node);
    for (const UDIMTilePixels &tile : node_data.tiles) {
      result.append_non_duplicates(tile.tile_number);
    }
  }
  return result;
}

static void dispatch_gpu_batches(TexturePaintingUserData &data)
{
  SculptSession &ss = *data.ob->sculpt;
  if (!ss.mode.texture_paint.gpu_data) {
    return;
  }

  GPUSculptPaintData &batches = *static_cast<GPUSculptPaintData *>(ss.mode.texture_paint.gpu_data);
  const int64_t steps_len = batches.steps.size();
  int2 paint_step_range(0, steps_len);
  batches.update_step_buf();
  batches.ensure_vert_coord_buf(ss);
  batches.ensure_paint_brush_buf(ss, *data.brush);
  batches.tile_texture.ensure_gpu_texture();
  batches.tile_texture.remove_unused();
  batches.tile_texture.assign_layer_ids();
  batches.tile_texture.ensure_tile_buf();
  batches.tile_texture.update_tile_buf();

  Image &image = *data.image_data.image;
  ImageUser local_image_user = *data.image_data.image_user;

  TileNumbers tile_numbers = collect_active_tile_numbers(data);
  for (TileNumber tile_number : tile_numbers) {
    local_image_user.tile = tile_number;

    ImBuf *image_buffer = BKE_image_acquire_ibuf(&image, &local_image_user, nullptr);
    if (image_buffer == nullptr) {
      continue;
    }

    TIMEIT_START(upload);
    batches.tile_texture.update_gpu_texture(tile_number, *image_buffer);
    GPU_flush();
    TIMEIT_END(upload);

    GPU_debug_group_begin("Paint tile");
    TIMEIT_START(paint_step);
    gpu_painting_paint_step(data, batches, tile_number, paint_step_range);
    GPU_flush();
    TIMEIT_END(paint_step);
    TIMEIT_START(merge);
    gpu_painting_image_merge(batches, *data.image_data.image, local_image_user, *image_buffer);
    GPU_flush();
    TIMEIT_END(merge);
    GPU_debug_group_end();

    BKE_image_release_ibuf(data.image_data.image, image_buffer, nullptr);
  }
}

static void gpu_frame_end(TexturePaintingUserData &data)
{
  SculptSession &ss = *data.ob->sculpt;
  if (!ss.mode.texture_paint.gpu_data) {
    return;
  }

  GPUSculptPaintData &batches = *static_cast<GPUSculptPaintData *>(ss.mode.texture_paint.gpu_data);

  /* Reset GPU data for next frame. */
  batches.steps.clear();
  batches.tile_texture.reset_usage_frame();
}

/** \} */

}  // namespace blender::ed::sculpt_paint::paint::image

extern "C" {

using namespace blender::ed::sculpt_paint::paint::image;

bool SCULPT_paint_image_canvas_get(PaintModeSettings *paint_mode_settings,
                                   Object *ob,
                                   Image **r_image,
                                   ImageUser **r_image_user)
{
  *r_image = nullptr;
  *r_image_user = nullptr;

  ImageData image_data;
  if (!ImageData::init_active_image(ob, &image_data, paint_mode_settings)) {
    return false;
  }

  *r_image = image_data.image;
  *r_image_user = image_data.image_user;
  return true;
}

bool SCULPT_use_image_paint_brush(PaintModeSettings *settings, Object *ob)
{
  if (!U.experimental.use_sculpt_texture_paint) {
    return false;
  }
  if (ob->type != OB_MESH) {
    return false;
  }
  Image *image;
  ImageUser *image_user;
  return BKE_paint_canvas_image_get(settings, ob, &image, &image_user);
}

/** Can the sculpt paint be performed on the GPU? */
static bool SCULPT_use_image_paint_compute()
{
#if 0
  return false;
#else
  return GPU_compute_shader_support() && GPU_shader_storage_buffer_objects_support() &&
         GPU_shader_image_load_store_support();
#endif
}

void SCULPT_do_paint_brush_image(
    PaintModeSettings *paint_mode_settings, Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode)
{
  Brush *brush = BKE_paint_brush(&sd->paint);

  TexturePaintingUserData data = {nullptr};
  data.ob = ob;
  data.brush = brush;
  data.nodes = nodes;
  data.nodes_len = totnode;

  if (!ImageData::init_active_image(ob, &data.image_data, paint_mode_settings)) {
    return;
  }

  if (SCULPT_use_image_paint_compute()) {
    ensure_gpu_buffers(data);
    update_frame_selection(data);
    dispatch_gpu_painting(data);
    paint_tiles_mark_used(data);
  }
  else {
    TaskParallelSettings settings;
    BKE_pbvh_parallel_range_settings(&settings, true, totnode);
    BLI_task_parallel_range(0, totnode, &data, do_push_undo_tile, &settings);
    TIMEIT_START(paint_image_cpu);
    BLI_task_parallel_range(0, totnode, &data, do_paint_pixels, &settings);
    TIMEIT_END(paint_image_cpu);

    TaskParallelSettings settings_flush;
    BKE_pbvh_parallel_range_settings(&settings_flush, false, totnode);
    BLI_task_parallel_range(0, totnode, &data, do_mark_dirty_regions, &settings_flush);
  }
}

void SCULPT_paint_image_batches_flush(PaintModeSettings *paint_mode_settings,
                                      Sculpt *sd,
                                      Object *ob)
{
  if (!SCULPT_use_image_paint_compute()) {
    return;
  }

  Brush *brush = BKE_paint_brush(&sd->paint);
  TexturePaintingUserData data = {nullptr};
  data.ob = ob;
  data.brush = brush;
  BKE_pbvh_search_gather_frame_selected(ob->sculpt->pbvh, &data.nodes, &data.nodes_len);
  if (data.nodes_len == 0) {
    return;
  }

  if (ImageData::init_active_image(ob, &data.image_data, paint_mode_settings)) {
    TIMEIT_START(paint_image_gpu);
    GPU_debug_group_begin("SCULPT_paint_brush");
    dispatch_gpu_batches(data);
    gpu_frame_end(data);
    GPU_debug_group_end();
    TIMEIT_END(paint_image_gpu);
  }

  MEM_freeN(data.nodes);
}

void SCULPT_paint_image_batches_finalize(PaintModeSettings *UNUSED(paint_mode_settings),
                                         Sculpt *UNUSED(sd),
                                         Object *ob)
{
  if (!SCULPT_use_image_paint_compute()) {
    return;
  }

  // TODO(jbakker): record undo steps.
  // TODO(jbakker): download results and update the image data-block.

  /* TODO: move this to sculpt tool switch and sculpt session free. */
  // SCULPT_paint_image_sculpt_data_free(ob->sculpt);
  SculptSession &ss = *ob->sculpt;
  GPUSculptPaintData &batches = *static_cast<GPUSculptPaintData *>(ss.mode.texture_paint.gpu_data);
  batches.tile_texture.reset_usage_stroke();
}

void SCULPT_paint_image_sculpt_data_free(SculptSession *ss)
{
  GPUSculptPaintData *batches = static_cast<GPUSculptPaintData *>(ss->mode.texture_paint.gpu_data);
  if (batches) {
    MEM_delete(batches);
    ss->mode.texture_paint.gpu_data = nullptr;
  }
}
}
