/* SPDX-FileCopyrightText: 2011-2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "scene/attribute.h"
#include "scene/mesh.h"

#include "subd/interpolation.h"
#include "subd/osd.h"
#include "subd/patch.h"
#include "subd/split.h"

#include "util/algorithm.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

void Mesh::tessellate(SubdParams &params)
{
  /* reset the number of subdivision vertices, in case the Mesh was not cleared
   * between calls or data updates */
  num_subd_added_verts = 0;
  const int num_faces = get_num_subd_faces();

  /* If there are no faces there is nothing to subdivide, and the required
   * subd position attribute may not exist. */
  if (num_faces == 0) {
    return;
  }

#ifdef WITH_OPENSUBDIV
  OsdMesh osd_mesh(*this);
  OsdData osd_data;

  if (subdivision_type == SUBDIVISION_CATMULL_CLARK) {
    if (get_num_subd_faces()) {
      osd_data.build(osd_mesh);
    }
  }
  else
#endif
  {
    /* force linear subdivision if OpenSubdiv is unavailable to avoid
     * falling into catmull-clark code paths by accident
     */
    subdivision_type = SUBDIVISION_LINEAR;
  }

  vector<bool> hole(num_faces, false);
  for (const int face : get_subd_hole_faces()) {
    if (face >= 0 && face < num_faces) {
      hole[face] = true;
    }
  }
#ifdef WITH_OPENSUBDIV
  /* OpenSubdiv also leaves the boundary faces out when boundaries are not interpolated. */
  if (subdivision_type == SUBDIVISION_CATMULL_CLARK) {
    const Far::TopologyLevel &base = osd_data.refiner->GetLevel(0);
    for (int f = 0; f < num_faces; f++) {
      if (base.IsFaceHole(f)) {
        hole[f] = true;
      }
    }
  }
#endif

  /* count patches */
  int num_patches = 0;
  for (int f = 0; f < num_faces; f++) {
    SubdFace face = get_subd_face(f);

    if (face.is_quad()) {
      num_patches++;
    }
    else {
      num_patches += face.num_corners;
    }
  }

  /* build patches from faces */
#ifdef WITH_OPENSUBDIV
  if (subdivision_type == SUBDIVISION_CATMULL_CLARK) {
    vector<OsdPatch> osd_patches(num_patches, OsdPatch(osd_data));
    OsdPatch *patch = osd_patches.data();

    for (int f = 0; f < num_faces; f++) {
      SubdFace face = get_subd_face(f);

      if (face.is_quad()) {
        patch->patch_index = face.ptex_offset;
        patch->from_ngon = false;
        patch->shader = face.shader;
        patch->smooth = face.smooth;
        patch++;
      }
      else {
        for (int corner = 0; corner < face.num_corners; corner++) {
          patch->patch_index = face.ptex_offset + corner;
          patch->from_ngon = true;
          patch->shader = face.shader;
          patch->smooth = face.smooth;
          patch++;
        }
      }
    }

    /* Split patches. */
    DiagSplit split(params);
    split.split_patches(osd_patches.data(), sizeof(OsdPatch), hole);

    /* Setup interpolation. */
    SubdAttributeInterpolation interpolation(*this, osd_mesh, osd_data);

    /* Dice patches. */
    EdgeDice dice(params, split.get_num_verts(), split.get_num_triangles(), interpolation);
    dice.dice(split);
  }
  else
#endif
  {
    vector<LinearQuadPatch> linear_patches(num_patches);
    LinearQuadPatch *patch = linear_patches.data();
    const Attribute *subd_attr_P = subd_attributes.find(ATTR_STD_POSITION);
    const packed_float3 *verts = subd_attr_P->data<packed_float3>();

    for (int f = 0; f < num_faces; f++) {
      SubdFace face = get_subd_face(f);

      if (face.is_quad()) {
        /* Simple quad case. */
        float3 *hull = patch->hull;

        patch->patch_index = face.ptex_offset;
        patch->from_ngon = false;

        hull[0] = verts[subd_face_corners[face.start_corner + 0]];
        hull[1] = verts[subd_face_corners[face.start_corner + 1]];
        hull[2] = verts[subd_face_corners[face.start_corner + 3]];
        hull[3] = verts[subd_face_corners[face.start_corner + 2]];

        patch->shader = face.shader;
        patch->smooth = face.smooth;
        patch++;
      }
      else {
        /* N-gon split into N quads. */
        float3 center_vert = zero_float3();

        const float inv_num_corners = 1.0f / float(face.num_corners);
        for (int corner = 0; corner < face.num_corners; corner++) {
          center_vert += verts[subd_face_corners[face.start_corner + corner]] * inv_num_corners;
        }

        for (int corner = 0; corner < face.num_corners; corner++) {
          float3 *hull = patch->hull;

          patch->patch_index = face.ptex_offset + corner;
          patch->from_ngon = true;

          patch->shader = face.shader;
          patch->smooth = face.smooth;

          const int v0 = subd_face_corners[face.start_corner + mod(corner + 0, face.num_corners)];
          const int v1 = subd_face_corners[face.start_corner + mod(corner + 1, face.num_corners)];
          const int v3 = subd_face_corners[face.start_corner +
                                           mod(corner + face.num_corners - 1, face.num_corners)];

          hull[0] = verts[v0];
          hull[1] = 0.5f * (verts[v0] + verts[v1]);
          hull[2] = 0.5f * (verts[v3] + verts[v0]);
          hull[3] = center_vert;

          patch++;
        }
      }
    }

    /* Split patches. */
    DiagSplit split(params);
    split.split_patches(linear_patches.data(), sizeof(LinearQuadPatch), hole);

    /* Setup interpolation. */
#ifdef WITH_OPENSUBDIV
    SubdAttributeInterpolation interpolation(*this, osd_mesh, osd_data);
#else
    SubdAttributeInterpolation interpolation(*this);
#endif

    /* Dice patches. */
    EdgeDice dice(params, split.get_num_verts(), split.get_num_triangles(), interpolation);
    dice.dice(split);
  }

  if (std::find(hole.begin(), hole.end(), true) != hole.end()) {
    place_unused_base_verts(hole);
  }

  // TODO: Free subd base data? Or will this break interactive updates?
}

/* Dicing keeps every base vertex but places only those of faces it dices, so a vertex used by
 * holes alone would otherwise be left uninitialized and corrupt the mesh bounds. */
void Mesh::place_unused_base_verts(const vector<bool> &hole)
{
  const int num_faces = get_num_subd_faces();
  const size_t num_base_verts = get_num_subd_base_verts();

  vector<bool> used(num_base_verts, false);
  for (int f = 0; f < num_faces; f++) {
    if (hole[f]) {
      continue;
    }
    const SubdFace face = get_subd_face(f);
    for (int corner = 0; corner < face.num_corners; corner++) {
      const int vert = subd_face_corners[face.start_corner + corner];
      if (vert >= 0 && size_t(vert) < num_base_verts) {
        used[vert] = true;
      }
    }
  }

  const Attribute *subd_attr_P = subd_attributes.find(ATTR_STD_POSITION);
  Attribute *attr_P = attributes.find(ATTR_STD_POSITION);
  if (!subd_attr_P || !attr_P) {
    return;
  }

  for (int step = 0; step < attr_P->num_motion_steps(); step++) {
    const int subd_step = step < subd_attr_P->num_motion_steps() ? step : 0;
    const packed_float3 *cage = subd_attr_P->data<packed_float3>(subd_step);
    packed_float3 *verts = attr_P->data_for_write<packed_float3>(step);
    for (size_t vert = 0; vert < num_base_verts; vert++) {
      if (!used[vert]) {
        verts[vert] = cage[vert];
      }
    }
  }
}

CCL_NAMESPACE_END
