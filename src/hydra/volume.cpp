/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/volume.h"
#include "hydra/field.h"
#include "hydra/geometry.inl"
#include "hydra/util.h"
#include "scene/volume.h"

#include <algorithm>

#include <pxr/imaging/hd/volumeFieldBindingSchema.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens,
   (openvdbAsset)
);
// clang-format on

HdCyclesVolume::HdCyclesVolume(const SdfPath &rprimId) : HdCyclesGeometry(rprimId) {}

HdCyclesVolume::~HdCyclesVolume() = default;

HdDirtyBits HdCyclesVolume::GetInitialDirtyBitsMask() const
{
  HdDirtyBits bits = HdCyclesGeometry::GetInitialDirtyBitsMask();
  bits |= HdChangeTracker::DirtyVolumeField;
  return bits;
}

bool HdCyclesVolume::UsesField(const SdfPath &fieldId) const
{
  return std::find(_fieldIds.begin(), _fieldIds.end(), fieldId) != _fieldIds.end();
}

void HdCyclesVolume::Populate(HdSceneDelegate *sceneDelegate, HdDirtyBits dirtyBits, bool &rebuild)
{
  Scene *const scene = (Scene *)_geom->get_owner();

  if (dirtyBits & HdChangeTracker::DirtyVolumeField) {
    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, GetId());
    HdVolumeFieldBindingSchema bindings = HdVolumeFieldBindingSchema::GetFromParent(
        prim.dataSource);

    _fieldIds.clear();

    /* The kernel finds a voxel attribute's image slot through the attribute map, which is only
     * rebuilt when an attribute is added or removed. Giving an existing attribute a new handle
     * would leave the map on the old image's slot, and the old image is freed once nothing holds
     * it, so the kernel would read freed grid memory. Bindings that went away are dropped too. */
    for (auto it = _geom->attributes.attributes.begin();
         it != _geom->attributes.attributes.end();)
    {
      if (it->element & ATTR_ELEMENT_VOXEL) {
        _geom->attributes.remove(it++);
      }
      else {
        ++it;
      }
    }

    for (const TfToken &fieldName : bindings.GetVolumeFieldBindingNames()) {
      auto pathDs = bindings.GetVolumeFieldBinding(fieldName);
      if (!pathDs) {
        continue;
      }
      const SdfPath fieldId = pathDs->GetTypedValue(0.0f);

      if (auto *const openvdbAsset = static_cast<HdCyclesField *>(
              sceneDelegate->GetRenderIndex().GetBprim(_tokens->openvdbAsset, fieldId)))
      {
        _fieldIds.push_back(fieldId);
        openvdbAsset->AddVolume(GetId());

        const ustring name(fieldName.GetString());

        AttributeStandard std = ATTR_STD_NONE;
        if (name == Attribute::standard_name(ATTR_STD_VOLUME_DENSITY)) {
          std = ATTR_STD_VOLUME_DENSITY;
        }
        else if (name == Attribute::standard_name(ATTR_STD_VOLUME_COLOR)) {
          std = ATTR_STD_VOLUME_COLOR;
        }
        else if (name == Attribute::standard_name(ATTR_STD_VOLUME_FLAME)) {
          std = ATTR_STD_VOLUME_FLAME;
        }
        else if (name == Attribute::standard_name(ATTR_STD_VOLUME_HEAT)) {
          std = ATTR_STD_VOLUME_HEAT;
        }
        else if (name == Attribute::standard_name(ATTR_STD_VOLUME_TEMPERATURE)) {
          std = ATTR_STD_VOLUME_TEMPERATURE;
        }
        else if (name == Attribute::standard_name(ATTR_STD_VOLUME_VELOCITY)) {
          std = ATTR_STD_VOLUME_VELOCITY;
        }

        // Skip attributes that are not needed
        if ((std != ATTR_STD_NONE && _geom->need_attribute(scene, std)) ||
            _geom->need_attribute(scene, name))
        {
          Attribute *const attr = (std != ATTR_STD_NONE) ?
                                      _geom->attributes.add(std) :
                                      _geom->attributes.add(name, TypeFloat, ATTR_ELEMENT_VOXEL);
          attr->data_voxel_for_write() = openvdbAsset->GetImageHandle();
        }
      }
    }

    _geom->merge_grids(scene);

    /* Only voxel attributes changed, and they are not sockets, so without this the geometry
     * manager would see an unmodified volume: it would keep the bounding mesh built from the
     * previous grid and the volume manager would keep that grid's octree, so a grid that moved
     * would be drawn only where the old one was. */
    _geom->tag_modified();

    rebuild = true;
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
