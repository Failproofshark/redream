/*
 * tile accelerator rendering
 *
 * responsible for parsing a context generated by the ta frontend into draw
 * commands to be passed to the host's render backend
 */

#include "guest/pvr/tr.h"
#include "core/core.h"
#include "core/sort.h"
#include "guest/pvr/ta.h"
#include "guest/pvr/tex.h"

struct tr {
  struct render_backend *r;
  void *userdata;
  tr_find_texture_cb find_texture;

  /* current global state */
  const union vert_param *last_vertex;
  int list_type;
  int vert_type;
  /* poly params */
  uint8_t face_color[4];
  uint8_t face_offset_color[4];
  /* sprite params */
  uint8_t sprite_color[4];
  uint8_t sprite_offset_color[4];
};

static int compressed_mipmap_offsets[] = {
    0x00006, /* 8 x 8 */
    0x00016, /* 16 x 16 */
    0x00056, /* 32 x 32 */
    0x00156, /* 64 x 64 */
    0x00556, /* 128 x 128 */
    0x01556, /* 256 x 256 */
    0x05556, /* 512 x 512 */
    0x15556, /* 1024 x 1024 */
};

static int paletted_4bpp_mipmap_offsets[] = {
    0x0000c, /* 8 x 8 */
    0x0002c, /* 16 x 16 */
    0x000ac, /* 32 x 32 */
    0x002ac, /* 64 x 64 */
    0x00aac, /* 128 x 128 */
    0x02aac, /* 256 x 256 */
    0x0aaac, /* 512 x 512 */
    0x2aaac, /* 1024 x 1024 */
};

static int paletted_8bpp_mipmap_offsets[] = {
    0x00018, /* 8 x 8 */
    0x00058, /* 16 x 16 */
    0x00158, /* 32 x 32 */
    0x00558, /* 64 x 64 */
    0x01558, /* 128 x 128 */
    0x05558, /* 256 x 256 */
    0x15558, /* 512 x 512 */
    0x55558, /* 1024 x 1024 */
};

static int nonpaletted_mipmap_offsets[] = {
    0x00030, /* 8 x 8 */
    0x000b0, /* 16 x 16 */
    0x002b0, /* 32 x 32 */
    0x00ab0, /* 64 x 64 */
    0x02ab0, /* 128 x 128 */
    0x0aab0, /* 256 x 256 */
    0x2aab0, /* 512 x 512 */
    0xaaab0, /* 1024 x 1024 */
};

static inline enum depth_func translate_depth_func(uint32_t depth_func) {
  static enum depth_func depth_funcs[] = {
      DEPTH_NEVER, DEPTH_GREATER, DEPTH_EQUAL,  DEPTH_GEQUAL,
      DEPTH_LESS,  DEPTH_NEQUAL,  DEPTH_LEQUAL, DEPTH_ALWAYS};
  return depth_funcs[depth_func];
}

static inline enum cull_face translate_cull(uint32_t cull_mode) {
  static enum cull_face cull_modes[] = {CULL_NONE, CULL_NONE, CULL_BACK,
                                        CULL_FRONT};
  return cull_modes[cull_mode];
}

static inline enum blend_func translate_src_blend_func(uint32_t blend_func) {
  static enum blend_func src_blend_funcs[] = {
      BLEND_ZERO,      BLEND_ONE,
      BLEND_DST_COLOR, BLEND_ONE_MINUS_DST_COLOR,
      BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA,
      BLEND_DST_ALPHA, BLEND_ONE_MINUS_DST_ALPHA};
  return src_blend_funcs[blend_func];
}

static inline enum blend_func translate_dst_blend_func(uint32_t blend_func) {
  static enum blend_func dst_blend_funcs[] = {
      BLEND_ZERO,      BLEND_ONE,
      BLEND_SRC_COLOR, BLEND_ONE_MINUS_SRC_COLOR,
      BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA,
      BLEND_DST_ALPHA, BLEND_ONE_MINUS_DST_ALPHA};
  return dst_blend_funcs[blend_func];
}

static inline enum shade_mode translate_shade_mode(uint32_t shade_mode) {
  static enum shade_mode shade_modes[] = {
      SHADE_DECAL, SHADE_MODULATE, SHADE_DECAL_ALPHA, SHADE_MODULATE_ALPHA};
  return shade_modes[shade_mode];
}

static texture_handle_t tr_convert_texture(struct tr *tr,
                                           const struct ta_context *ctx,
                                           union tsp tsp, union tcw tcw) {
  /* TODO it's bad that textures are only cached based off tsp / tcw yet the
     TEXT_CONTROL registers and PAL_RAM_CTRL registers are used here to control
     texture generation */

  struct tr_texture *entry = tr->find_texture(tr->userdata, tsp, tcw);
  CHECK_NOTNULL(entry);

  /* if there's a non-dirty handle, return it */
  if (entry->handle && !entry->dirty) {
    return entry->handle;
  }

  /* if there's a dirty handle, destroy it before creating the new one */
  if (entry->handle && entry->dirty) {
    r_destroy_texture(tr->r, entry->handle);
    entry->handle = 0;
  }

  static uint8_t converted[1024 * 1024 * 4];
  const uint8_t *palette = entry->palette;
  const uint8_t *texture = entry->texture;

  /* get texture dimensions */
  int texture_fmt = ta_texture_format(tcw);
  int mipmaps = ta_texture_mipmaps(tcw);
  int width = ta_texture_width(tsp, tcw);
  int height = ta_texture_height(tsp, tcw);
  int stride = ta_texture_stride(tsp, tcw, ctx->stride);

  /* figure out the texture format */
  pvr_tex_decode(texture, width, height, stride, texture_fmt, tcw.pixel_fmt,
                 palette, ctx->palette_fmt, converted, sizeof(converted));

  /* ignore trilinear filtering for now */
  enum filter_mode filter =
      tsp.filter_mode == 0 ? FILTER_NEAREST : FILTER_BILINEAR;
  enum wrap_mode wrap_u =
      tsp.clamp_u ? WRAP_CLAMP_TO_EDGE
                  : (tsp.flip_u ? WRAP_MIRRORED_REPEAT : WRAP_REPEAT);
  enum wrap_mode wrap_v =
      tsp.clamp_v ? WRAP_CLAMP_TO_EDGE
                  : (tsp.flip_v ? WRAP_MIRRORED_REPEAT : WRAP_REPEAT);

  entry->handle = r_create_texture(tr->r, PXL_RGBA, filter, wrap_u, wrap_v,
                                   mipmaps, width, height, converted);
  entry->filter = filter;
  entry->wrap_u = wrap_u;
  entry->wrap_v = wrap_v;
  entry->format = texture_fmt;
  entry->width = width;
  entry->height = height;
  entry->dirty = 0;

  return entry->handle;
}

static struct ta_surface *tr_reserve_surf(struct tr *tr, struct tr_context *rc,
                                          int copy_from_prev) {
  int surf_index = rc->num_surfs;

  CHECK_LT(surf_index, ARRAY_SIZE(rc->surfs));
  struct ta_surface *surf = &rc->surfs[surf_index];

  if (copy_from_prev) {
    CHECK(rc->num_surfs);
    *surf = rc->surfs[rc->num_surfs - 1];
  } else {
    memset(surf, 0, sizeof(*surf));
  }

  surf->first_vert = rc->num_verts;
  surf->num_verts = 0;

  return surf;
}

static struct ta_vertex *tr_reserve_vert(struct tr *tr, struct tr_context *rc) {
  struct ta_surface *curr_surf = &rc->surfs[rc->num_surfs];

  int vert_index = rc->num_verts + curr_surf->num_verts;
  CHECK_LT(vert_index, ARRAY_SIZE(rc->verts));
  struct ta_vertex *vert = &rc->verts[vert_index];

  memset(vert, 0, sizeof(*vert));

  curr_surf->num_verts++;

  return vert;
}

static void tr_commit_surf(struct tr *tr, struct tr_context *rc) {
  struct tr_list *list = &rc->lists[tr->list_type];
  struct ta_surface *new_surf = &rc->surfs[rc->num_surfs];

  /* track original number of surfaces, before sorting, merging, etc. */
  list->num_orig_surfs++;

  /* for translucent lists, commit a surf for each tri to make sorting easier */
  if (tr->list_type == TA_LIST_TRANSLUCENT ||
      tr->list_type == TA_LIST_PUNCH_THROUGH) {
    /* ignore the last two verts as polygons are fed to the TA as tristrips */
    int num_verts = new_surf->num_verts;

    for (int i = 0; i < num_verts - 2; i++) {
      struct ta_surface *surf = NULL;
      if (i == 0) {
        surf = new_surf;
      } else {
        surf = tr_reserve_surf(tr, rc, 1);
      }

      /* track triangle strip offset so winding order can be consistent when
         generating indices */
      surf->strip_offset = i;
      surf->first_vert = rc->num_verts;
      surf->num_verts = 3;

      /* default sort the new surface */
      list->surfs[list->num_surfs++] = rc->num_surfs;

      /* commit the new surface */
      rc->num_verts += 1;
      rc->num_surfs++;
    }

    /* commit the last two verts */
    rc->num_verts += 2;
  }
  /* for opaque lists, commit surface as is */
  else {
    /* default sort the new surface */
    list->surfs[list->num_surfs++] = rc->num_surfs;

    /* commit the new surface */
    rc->num_verts += new_surf->num_verts;
    rc->num_surfs += 1;
  }
}

/*
* polygon parsing helpers
*/
static inline uint8_t ftou8(float x) {
  /* saturating floating point to uint8_t conversion */
  return MIN(MAX((int32_t)(x * 255.0f), 0), 255);
}

static inline uint8_t fmulu8(uint8_t a, uint8_t b) {
  /* fixed point multiply */
  return (uint8_t)((uint32_t)a * (uint32_t)b / 255);
}

#define PARSE_XYZ(xyz, out) \
  {                         \
    (out)[0] = (xyz)[0];    \
    (out)[1] = (xyz)[1];    \
    (out)[2] = (xyz)[2];    \
  }

#define PARSE_UV(uv, out) \
  {                       \
    (out)[0] = (uv)[0];   \
    (out)[1] = (uv)[1];   \
  }

#define PARSE_UV16(uv, out)     \
  {                             \
    uint32_t u = (uv)[1] << 16; \
    uint32_t v = (uv)[0] << 16; \
    (out)[0] = *(float *)&u;    \
    (out)[1] = *(float *)&v;    \
  }

#define PARSE_FLOAT_COLOR(color, out)                                 \
  {                                                                   \
    /* when converting from float point to a packed color, clamp each \
       component to 0-255 */                                          \
    ((uint8_t *)out)[0] = ftou8(color##_r);                           \
    ((uint8_t *)out)[1] = ftou8(color##_g);                           \
    ((uint8_t *)out)[2] = ftou8(color##_b);                           \
    ((uint8_t *)out)[3] = ftou8(color##_a);                           \
  }

#define PARSE_PACKED_COLOR(color, out)                \
  {                                                   \
    ((uint8_t *)out)[0] = (color & 0x00ff0000) >> 16; \
    ((uint8_t *)out)[1] = (color & 0x0000ff00) >> 8;  \
    ((uint8_t *)out)[2] = (color & 0x000000ff);       \
    ((uint8_t *)out)[3] = (color & 0xff000000) >> 24; \
  }

#define PARSE_INTENSITY(color, intensity, out)                           \
  {                                                                      \
    /* when converting from intensity to a packed color, each operand is \
       clamped to 0-255 before multiplication */                         \
    uint8_t i = ftou8(intensity);                                        \
    ((uint8_t *)out)[0] = fmulu8(color[0], i);                           \
    ((uint8_t *)out)[1] = fmulu8(color[1], i);                           \
    ((uint8_t *)out)[2] = fmulu8(color[2], i);                           \
    ((uint8_t *)out)[3] = color[3];                                      \
  }

#define PARSE_BASE_INTENSITY(base_intensity, out) \
  PARSE_INTENSITY(tr->face_color, base_intensity, out)

#define PARSE_OFFSET_INTENSITY(offset_intensity, out) \
  PARSE_INTENSITY(tr->face_offset_color, offset_intensity, out)

static int tr_parse_bg_vert(const struct ta_context *ctx, struct tr_context *rc,
                            int offset, struct ta_vertex *v) {
  PARSE_XYZ((float *)&ctx->bg_vertices[offset], v->xyz);
  offset += 12;

  if (ctx->bg_isp.texture) {
    float *uv = (float *)&ctx->bg_vertices[offset];
    PARSE_UV(uv, v->uv);
    offset += 8;
  }

  uint32_t base_color = *(uint32_t *)&ctx->bg_vertices[offset];
  PARSE_PACKED_COLOR(base_color, &v->color);
  offset += 4;

  if (ctx->bg_isp.offset) {
    uint32_t offset_color = *(uint32_t *)&ctx->bg_vertices[offset];
    PARSE_PACKED_COLOR(offset_color, &v->offset_color);
    offset += 4;
  }

  return offset;
}

static void tr_parse_bg(struct tr *tr, const struct ta_context *ctx,
                        struct tr_context *rc) {
  tr->list_type = TA_LIST_OPAQUE;

  /* translate the surface */
  struct ta_surface *surf = tr_reserve_surf(tr, rc, 0);

  surf->params.texture =
      ctx->bg_isp.texture
          ? tr_convert_texture(tr, ctx, ctx->bg_tsp, ctx->bg_tcw)
          : 0;
  surf->params.depth_write = !ctx->bg_isp.z_write_disable;
  surf->params.depth_func =
      translate_depth_func(ctx->bg_isp.depth_compare_mode);
  surf->params.cull = translate_cull(ctx->bg_isp.culling_mode);
  surf->params.src_blend = BLEND_NONE;
  surf->params.dst_blend = BLEND_NONE;

  /* translate the first 3 vertices */
  struct ta_vertex *va = tr_reserve_vert(tr, rc);
  struct ta_vertex *vb = tr_reserve_vert(tr, rc);
  struct ta_vertex *vc = tr_reserve_vert(tr, rc);
  struct ta_vertex *vd = tr_reserve_vert(tr, rc);

  int offset = 0;
  offset = tr_parse_bg_vert(ctx, rc, offset, va);
  offset = tr_parse_bg_vert(ctx, rc, offset, vb);
  offset = tr_parse_bg_vert(ctx, rc, offset, vc);

  /* not exactly sure how ISP_BACKGND_D is supposed to be honored. would be nice
     to find a game that actually uses the texture parameter to see how the uv
     coordinates look */
  /*va->xyz[2] = ctx->bg_depth;
  vb->xyz[2] = ctx->bg_depth;
  vc->xyz[2] = ctx->bg_depth;*/

  /* 4th vertex isn't supplied, fill it out automatically */
  float xyz_ab[3], xyz_ac[3];
  vec3_sub(xyz_ab, vb->xyz, va->xyz);
  vec3_sub(xyz_ac, vc->xyz, va->xyz);
  vec3_add(vd->xyz, vb->xyz, xyz_ab);
  vec3_add(vd->xyz, vd->xyz, xyz_ac);

  float uv_ab[2], uv_ac[2];
  vec2_sub(uv_ab, vb->uv, va->uv);
  vec2_sub(uv_ac, vc->uv, va->uv);
  vec2_add(vd->uv, vb->uv, uv_ab);
  vec2_add(vd->uv, vd->uv, uv_ac);

  /* TODO interpolate this properly when a game is found to test with */
  vd->color = va->color;
  vd->offset_color = va->offset_color;

  tr_commit_surf(tr, rc);

  tr->list_type = TA_NUM_LISTS;
}

/* this offset color implementation is not correct at all, see the
   Texture/Shading Instruction in the union tsp instruction word */
static void tr_parse_poly_param(struct tr *tr, const struct ta_context *ctx,
                                struct tr_context *rc, const uint8_t *data) {
  const union poly_param *param = (const union poly_param *)data;

  /* reset state */
  tr->last_vertex = NULL;
  tr->vert_type = ta_vert_type(param->type0.pcw);

  int poly_type = ta_poly_type(param->type0.pcw);

  if (poly_type == 6) {
    /* FIXME handle modifier volumes */
    return;
  }

  switch (poly_type) {
    case 0: {
      /*uint32_t sdma_data_size;
      uint32_t sdma_next_addr;*/
    } break;

    case 1: {
      PARSE_FLOAT_COLOR(param->type1.face_color, &tr->face_color);
    } break;

    case 2: {
      PARSE_FLOAT_COLOR(param->type2.face_color, &tr->face_color);
      PARSE_FLOAT_COLOR(param->type2.face_offset_color, &tr->face_offset_color);
    } break;

    case 5: {
      PARSE_PACKED_COLOR(param->sprite.base_color, &tr->sprite_color);
      PARSE_PACKED_COLOR(param->sprite.offset_color, &tr->sprite_offset_color);
    } break;

    default:
      LOG_FATAL("unsupported poly type %d", poly_type);
      break;
  }

  /* setup the new surface

     note, bits 0-3 of the global pcw override the respective bits in the global
     isp/tsp instruction word, so use the pcw for the uv_16bit, gouraud, offset,
     and texture settings */
  struct ta_surface *surf = tr_reserve_surf(tr, rc, 0);
  surf->params.depth_write = !param->type0.isp.z_write_disable;
  surf->params.depth_func =
      translate_depth_func(param->type0.isp.depth_compare_mode);
  surf->params.cull = translate_cull(param->type0.isp.culling_mode);
  surf->params.src_blend =
      translate_src_blend_func(param->type0.tsp.src_alpha_instr);
  surf->params.dst_blend =
      translate_dst_blend_func(param->type0.tsp.dst_alpha_instr);
  surf->params.shade =
      translate_shade_mode(param->type0.tsp.texture_shading_instr);
  surf->params.ignore_alpha = !param->type0.tsp.use_alpha;
  surf->params.ignore_texture_alpha = param->type0.tsp.ignore_tex_alpha;
  surf->params.offset_color = param->type0.pcw.offset;
  surf->params.alpha_test = tr->list_type == TA_LIST_PUNCH_THROUGH;
  surf->params.alpha_ref = ctx->alpha_ref;

  /* override a few surface parameters based on the list type */
  if (tr->list_type != TA_LIST_TRANSLUCENT &&
      tr->list_type != TA_LIST_TRANSLUCENT_MODVOL) {
    surf->params.src_blend = BLEND_NONE;
    surf->params.dst_blend = BLEND_NONE;
  } else if ((tr->list_type == TA_LIST_TRANSLUCENT ||
              tr->list_type == TA_LIST_TRANSLUCENT_MODVOL) &&
             ctx->autosort) {
    surf->params.depth_func = DEPTH_LEQUAL;
  } else if (tr->list_type == TA_LIST_PUNCH_THROUGH) {
    surf->params.depth_func = DEPTH_GEQUAL;
  }

  surf->params.texture =
      param->type0.pcw.texture
          ? tr_convert_texture(tr, ctx, param->type0.tsp, param->type0.tcw)
          : 0;
}

static void tr_parse_vert_param(struct tr *tr, const struct ta_context *ctx,
                                struct tr_context *rc, const uint8_t *data) {
  const union vert_param *param = (const union vert_param *)data;

  if (tr->vert_type == 17) {
    /* FIXME handle modifier volumes */
    return;
  }

  /* if there is no need to change the Global Parameters, a Vertex Parameter
     for the next polygon may be input immediately after inputting a Vertex
     Parameter for which "End of Strip" was specified */
  if (tr->last_vertex && tr->last_vertex->type0.pcw.end_of_strip) {
    tr_reserve_surf(tr, rc, 1);
  }
  tr->last_vertex = param;

  switch (tr->vert_type) {
    case 0: {
      struct ta_vertex *vert = tr_reserve_vert(tr, rc);
      PARSE_XYZ(param->type0.xyz, vert->xyz);
      PARSE_PACKED_COLOR(param->type0.base_color, &vert->color);
    } break;

    case 1: {
      struct ta_vertex *vert = tr_reserve_vert(tr, rc);
      PARSE_XYZ(param->type1.xyz, vert->xyz);
      PARSE_FLOAT_COLOR(param->type1.base_color, &vert->color);
    } break;

    case 2: {
      struct ta_vertex *vert = tr_reserve_vert(tr, rc);
      PARSE_XYZ(param->type2.xyz, vert->xyz);
      PARSE_BASE_INTENSITY(param->type2.base_intensity, &vert->color);
    } break;

    case 3: {
      struct ta_vertex *vert = tr_reserve_vert(tr, rc);
      PARSE_XYZ(param->type3.xyz, vert->xyz);
      PARSE_UV(param->type3.uv, vert->uv);
      PARSE_PACKED_COLOR(param->type3.base_color, &vert->color);
      PARSE_PACKED_COLOR(param->type3.offset_color, &vert->offset_color);
    } break;

    case 4: {
      struct ta_vertex *vert = tr_reserve_vert(tr, rc);
      PARSE_XYZ(param->type4.xyz, vert->xyz);
      PARSE_UV16(param->type4.uv, vert->uv);
      PARSE_PACKED_COLOR(param->type4.base_color, &vert->color);
      PARSE_PACKED_COLOR(param->type4.offset_color, &vert->offset_color);
    } break;

    case 5: {
      struct ta_vertex *vert = tr_reserve_vert(tr, rc);
      PARSE_XYZ(param->type5.xyz, vert->xyz);
      PARSE_UV(param->type5.uv, vert->uv);
      PARSE_FLOAT_COLOR(param->type5.base_color, &vert->color);
      PARSE_FLOAT_COLOR(param->type5.offset_color, &vert->offset_color);
    } break;

    case 6: {
      struct ta_vertex *vert = tr_reserve_vert(tr, rc);
      PARSE_XYZ(param->type6.xyz, vert->xyz);
      PARSE_UV16(param->type6.uv, vert->uv);
      PARSE_FLOAT_COLOR(param->type6.base_color, &vert->color);
      PARSE_FLOAT_COLOR(param->type6.offset_color, &vert->offset_color);
    } break;

    case 7: {
      struct ta_vertex *vert = tr_reserve_vert(tr, rc);
      PARSE_XYZ(param->type7.xyz, vert->xyz);
      PARSE_UV(param->type7.uv, vert->uv);
      PARSE_BASE_INTENSITY(param->type7.base_intensity, &vert->color);
      PARSE_OFFSET_INTENSITY(param->type7.offset_intensity,
                             &vert->offset_color);
    } break;

    case 8: {
      struct ta_vertex *vert = tr_reserve_vert(tr, rc);
      PARSE_XYZ(param->type8.xyz, vert->xyz);
      PARSE_UV16(param->type8.uv, vert->uv);
      PARSE_BASE_INTENSITY(param->type8.base_intensity, &vert->color);
      PARSE_OFFSET_INTENSITY(param->type8.offset_intensity,
                             &vert->offset_color);
    } break;

    case 15:
    case 16: {
      CHECK(param->type0.pcw.end_of_strip);

      /*
       * sprites are input as a quad in a clockwise order:
       *
       * b (x,y,z,u,v) ---> c (x,y,z,u,v)
       *       ^                  |
       *       |                  |
       *       |                  |
       *       |                  v
       * a (x,y,z,u,v) <--- d (x,y,0,0,0)
       *
       * note that the z, u, v components aren't specified for the final vertex.
       * these need to be calculated, and the quad needs to be converted into a
       * tristrip to match the rest of the ta input
       */
      struct ta_vertex *va = tr_reserve_vert(tr, rc); /* bottom left */
      struct ta_vertex *vb = tr_reserve_vert(tr, rc); /* top left */
      struct ta_vertex *vd = tr_reserve_vert(tr, rc); /* bottom right */
      struct ta_vertex *vc = tr_reserve_vert(tr, rc); /* top right */

      PARSE_XYZ(param->sprite1.xyz[0], va->xyz);
      PARSE_UV16(param->sprite1.uv[0], va->uv);
      va->color = *(uint32_t *)&tr->sprite_color;
      va->offset_color = *(uint32_t *)&tr->sprite_offset_color;

      PARSE_XYZ(param->sprite1.xyz[1], vb->xyz);
      PARSE_UV16(param->sprite1.uv[1], vb->uv);
      vb->color = *(uint32_t *)&tr->sprite_color;
      vb->offset_color = *(uint32_t *)&tr->sprite_offset_color;

      PARSE_XYZ(param->sprite1.xyz[2], vc->xyz);
      PARSE_UV16(param->sprite1.uv[2], vc->uv);
      vc->color = *(uint32_t *)&tr->sprite_color;
      vc->offset_color = *(uint32_t *)&tr->sprite_offset_color;

      vd->xyz[0] = param->sprite1.xyz[3][0];
      vd->xyz[1] = param->sprite1.xyz[3][1];
      vd->color = *(uint32_t *)&tr->sprite_color;
      vd->offset_color = *(uint32_t *)&tr->sprite_offset_color;

      /* calculate the sprite's plane from the three complete vertices */
      float xyz_ba[3], xyz_bc[3];
      float n[3], len, d;
      vec3_sub(xyz_ba, va->xyz, vb->xyz);
      vec3_sub(xyz_bc, vc->xyz, vb->xyz);
      vec3_cross(n, xyz_ba, xyz_bc);
      len = vec3_normalize(n);
      d = vec3_dot(n, vb->xyz);

      /* don't commit surf if quad is degenerate or perpendicular to our view */
      if (len == 0.0f || n[2] == 0.0f) {
        return;
      }

      /*
       * for all points on a plane, the following must hold true:
       * dot(n, p) - d = 0
       *
       * using this, the missing corner's z can be solved with:
       * n.x * p.x + n.y * p.y + n.z * p.z - d = 0
       * n.x * p.x + n.y * p.y + n.z * p.z = d
       * n.z * p.z = d - n.x * p.x - n.y * p.y
       * p.z = (d - n.x * p.y - n.y * p.y) / n.z
       */
      vd->xyz[2] = (d - n[0] * vd->xyz[0] - n[1] * vd->xyz[1]) / n[2];

      /* calculate the missing corner's uv */
      float uv_ba[2], uv_bc[2];
      vec2_sub(uv_ba, va->uv, vb->uv);
      vec2_sub(uv_bc, vc->uv, vb->uv);

      vec2_add(vd->uv, vb->uv, uv_ba);
      vec2_add(vd->uv, vd->uv, uv_bc);
    } break;

    default:
      LOG_FATAL("unsupported vertex type %d", tr->vert_type);
      break;
  }

  /* in the case of the Polygon type, the last Vertex Parameter for an object
     must have "End of Strip" specified.  If Vertex Parameters with the "End of
     Strip" specification were not input, but parameters other than the Vertex
     Parameters were input, the polygon data in question is ignored and
     an interrupt signal is output */
  if (param->type0.pcw.end_of_strip) {
    tr_commit_surf(tr, rc);
  }
}

static void tr_parse_eol(struct tr *tr, const struct ta_context *ctx,
                         struct tr_context *rc, const uint8_t *data) {
  tr->last_vertex = NULL;
  tr->list_type = TA_NUM_LISTS;
  tr->vert_type = TA_NUM_VERTS;
}

static inline int tr_can_merge_surfs(struct ta_surface *a,
                                     struct ta_surface *b) {
  return a->params.full == b->params.full;
}

static void tr_generate_indices(struct tr *tr, struct tr_context *rc,
                                int list_type) {
  /* polygons are fed to the TA as triangle strips, with the vertices being fed
     in a CW order, so a given quad looks like:

     1----3----5
     |\   |\   |
     | \  | \  |
     |  \ |  \ |
     |   \|   \|
     0----2----4

     convert from these triangle strips to triangles, and convert to CCW to
     match OpenGL defaults */
  struct tr_list *list = &rc->lists[list_type];

  int num_merged = 0;

  for (int i = 0, j = 0; i < list->num_surfs; i = j) {
    struct ta_surface *root = &rc->surfs[list->surfs[i]];
    int first_index = rc->num_indices;

    /* merge adjacent surfaces at this time */
    for (j = i; j < list->num_surfs; j++) {
      struct ta_surface *surf = &rc->surfs[list->surfs[j]];

      if (surf != root) {
        if (!tr_can_merge_surfs(root, surf)) {
          break;
        }

        num_merged++;
      }

      int num_indices = (surf->num_verts - 2) * 3;
      CHECK_LT(rc->num_indices + num_indices, ARRAY_SIZE(rc->indices));

      for (int j = 0; j < surf->num_verts - 2; j++) {
        int strip_offset = surf->strip_offset + j;
        int vertex_offset = surf->first_vert + j;

        /* be careful to maintain a CCW winding order */
        if (strip_offset & 1) {
          rc->indices[rc->num_indices++] = vertex_offset + 0;
          rc->indices[rc->num_indices++] = vertex_offset + 1;
          rc->indices[rc->num_indices++] = vertex_offset + 2;
        } else {
          rc->indices[rc->num_indices++] = vertex_offset + 0;
          rc->indices[rc->num_indices++] = vertex_offset + 2;
          rc->indices[rc->num_indices++] = vertex_offset + 1;
        }
      }
    }

    /* update to point at triangle indices instead of the raw tristrip verts */
    root->first_vert = first_index;
    root->num_verts = rc->num_indices - first_index;

    /* shift the list to account for merges */
    list->surfs[j - num_merged - 1] = list->surfs[i];
  }

  list->num_surfs -= num_merged;
}

static int sort_tmp[TR_MAX_SURFS];
static float sort_minz[TR_MAX_SURFS];

static int tr_compare_surf(const void *a, const void *b) {
  int i = *(const int *)a;
  int j = *(const int *)b;
  return sort_minz[i] <= sort_minz[j];
}

static void tr_sort_surfaces(struct tr *tr, struct tr_context *rc,
                             int list_type) {
  struct tr_list *list = &rc->lists[list_type];

  /* sort each surface from back to front based on its minz */
  for (int i = 0; i < list->num_surfs; i++) {
    int surf_index = list->surfs[i];
    struct ta_surface *surf = &rc->surfs[surf_index];
    float *minz = &sort_minz[surf_index];

    struct ta_vertex *verts = &rc->verts[surf->first_vert];
    CHECK_EQ(surf->num_verts, 3);

    *minz = MIN(verts[0].xyz[2], verts[1].xyz[2]);
    *minz = MIN(*minz, verts[2].xyz[2]);
  }

  msort_noalloc(list->surfs, sort_tmp, list->num_surfs, sizeof(int),
                &tr_compare_surf);
}

static void tr_reset(struct tr *tr, struct tr_context *rc) {
  /* reset global state */
  tr->last_vertex = NULL;
  tr->list_type = TA_NUM_LISTS;
  tr->vert_type = TA_NUM_VERTS;
  memset(tr->face_color, 0, sizeof(tr->face_color));
  memset(tr->face_offset_color, 0, sizeof(tr->face_offset_color));
  memset(tr->sprite_color, 0, sizeof(tr->sprite_color));
  memset(tr->sprite_offset_color, 0, sizeof(tr->sprite_offset_color));

  /* reset render context state */
  rc->num_params = 0;
  rc->num_surfs = 0;
  rc->num_verts = 0;
  rc->num_indices = 0;
  for (int i = 0; i < TA_NUM_LISTS; i++) {
    struct tr_list *list = &rc->lists[i];
    list->num_surfs = 0;
    list->num_orig_surfs = 0;
  }
}

static void tr_render_list(struct render_backend *r,
                           const struct tr_context *rc, int list_type,
                           int end_surf, int *stopped) {
  if (*stopped) {
    return;
  }

  const struct tr_list *list = &rc->lists[list_type];
  const int *sorted_surf = list->surfs;
  const int *sorted_surf_end = list->surfs + list->num_surfs;

  while (sorted_surf < sorted_surf_end) {
    int surf = *(sorted_surf++);

    r_draw_ta_surface(r, &rc->surfs[surf]);

    if (surf == end_surf) {
      *stopped = 1;
      break;
    }
  }
}

void tr_render_context_until(struct render_backend *r,
                             const struct tr_context *rc, int end_surf) {
  int stopped = 0;

  r_begin_ta_surfaces(r, rc->width, rc->height, rc->verts, rc->num_verts,
                      rc->indices, rc->num_indices);

  tr_render_list(r, rc, TA_LIST_OPAQUE, end_surf, &stopped);
  tr_render_list(r, rc, TA_LIST_PUNCH_THROUGH, end_surf, &stopped);
  tr_render_list(r, rc, TA_LIST_TRANSLUCENT, end_surf, &stopped);

  r_end_ta_surfaces(r);
}

void tr_render_context(struct render_backend *r, const struct tr_context *rc) {
  tr_render_context_until(r, rc, -1);
}

void tr_convert_context(struct render_backend *r, void *userdata,
                        tr_find_texture_cb find_texture,
                        const struct ta_context *ctx, struct tr_context *rc) {
  struct tr tr;
  tr.r = r;
  tr.userdata = userdata;
  tr.find_texture = find_texture;

  const uint8_t *data = ctx->params;
  const uint8_t *end = ctx->params + ctx->size;

  ta_init_tables();

  tr_reset(&tr, rc);

  rc->width = ctx->video_width;
  rc->height = ctx->video_height;

  tr_parse_bg(&tr, ctx, rc);

  while (data < end) {
    union pcw pcw = *(union pcw *)data;

    if (ta_pcw_list_type_valid(pcw, tr.list_type)) {
      tr.list_type = pcw.list_type;
    }

    switch (pcw.para_type) {
      /* control params */
      case TA_PARAM_END_OF_LIST:
        tr_parse_eol(&tr, ctx, rc, data);
        break;

      case TA_PARAM_USER_TILE_CLIP:
        break;

      case TA_PARAM_OBJ_LIST_SET:
        LOG_FATAL("TA_PARAM_OBJ_LIST_SET unsupported");
        break;

      /* global params */
      case TA_PARAM_POLY_OR_VOL:
      case TA_PARAM_SPRITE:
        tr_parse_poly_param(&tr, ctx, rc, data);
        break;

      /* vertex params */
      case TA_PARAM_VERTEX:
        tr_parse_vert_param(&tr, ctx, rc, data);
        break;
    }

    /* track info about the parse state for tracer debugging */
    struct tr_param *rp = &rc->params[rc->num_params++];
    rp->offset = (int)(data - ctx->params);
    rp->list_type = tr.list_type;
    rp->vert_type = tr.vert_type;
    rp->last_surf = rc->num_surfs - 1;
    rp->last_vert = rc->num_verts - 1;

    data += ta_param_size(pcw, tr.vert_type);
  }

  /* sort surfaces if requested */
  if (ctx->autosort) {
    tr_sort_surfaces(&tr, rc, TA_LIST_TRANSLUCENT);
    tr_sort_surfaces(&tr, rc, TA_LIST_PUNCH_THROUGH);
  }

  for (int i = 0; i < TA_NUM_LISTS; i++) {
    tr_generate_indices(&tr, rc, i);
  }
}
