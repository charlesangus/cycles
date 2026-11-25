/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/field.h"
#include "hydra/util.h"
#include "hydra/volume.h"

#include "util/log.h"

#ifdef WITH_OPENVDB
#  include "hydra/session.h"
#  include "scene/image_vdb.h"
#  include "scene/scene.h"
#endif

#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/volumeFieldSchema.h>
#include <pxr/usd/sdf/assetPath.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

#ifdef WITH_OPENVDB
class HdCyclesVolumeLoader : public VDBImageLoader {
 public:
  HdCyclesVolumeLoader(const std::string &filePath, const std::string &gridName)
      : VDBImageLoader(gridName)
  {
    /* Disable delay loading and file copying, this has poor performance on network drives. */
    const bool delay_load = false;
    try {
      /* Blender initialises OpenVDB itself, so nothing in Cycles does. Without it no grid type
       * is registered and readGrid throws. The call is idempotent and thread-safe. */
      openvdb::initialize();
      openvdb::io::File file(filePath);
#  ifdef OPENVDB_USE_DELAYED_LOADING
      file.setCopyMaxBytes(0);
#  endif
      if (file.open(delay_load)) {
        grid = file.readGrid(gridName);
      }
    }
    catch (const openvdb::IoError &e) {
      LOG_ERROR << "Error loading OpenVDB file: " << e.what();
    }
    catch (...) {
      LOG_ERROR << "Error loading OpenVDB file: Unknown error";
    }
  }
};
#endif

HdCyclesField::HdCyclesField(const SdfPath &bprimId, const TfToken & /*typeId*/) : HdField(bprimId)
{
}

HdCyclesField::~HdCyclesField() = default;

HdDirtyBits HdCyclesField::GetInitialDirtyBitsMask() const
{
  return DirtyBits::DirtyParams;
}

void HdCyclesField::Sync(HdSceneDelegate *sceneDelegate,
                         HdRenderParam *renderParam,
                         HdDirtyBits *dirtyBits)
{
#ifdef WITH_OPENVDB
  const SdfPath &id = GetId();

  if (*dirtyBits & DirtyBits::DirtyParams) {
    const HdSceneIndexPrim prim = GetPrim(sceneDelegate, id);
    HdVolumeFieldSchema schema = HdVolumeFieldSchema::GetFromParent(prim.dataSource);

    SdfAssetPath assetPath;
    if (auto ds = schema.GetFilePath()) {
      assetPath = ds->GetTypedValue(0.0f);
    }

    std::string filename = assetPath.GetResolvedPath();
    if (filename.empty()) {
      filename = assetPath.GetAssetPath();
    }

    TfToken fieldName;
    if (auto ds = schema.GetFieldName()) {
      fieldName = ds->GetTypedValue(0.0f);
    }

    {
      const SceneLock lock(renderParam);

      if (!filename.empty() && !fieldName.IsEmpty()) {
        unique_ptr<ImageLoader> loader = make_unique<HdCyclesVolumeLoader>(filename,
                                                                           fieldName.GetString());

        ImageParams params;
        params.frame = 0.0f;

        _handle = lock.scene->image_manager->add_image(std::move(loader), params, false);
      }
      else {
        _handle.clear();
      }
    }

    MarkVolumesDirty(sceneDelegate);
  }
#else
  (void)sceneDelegate;
  (void)renderParam;
#endif

  *dirtyBits = DirtyBits::Clean;
}

void HdCyclesField::AddVolume(const SdfPath &volumeId)
{
  const std::lock_guard<std::mutex> guard(_volumesMutex);
  _volumes.insert(volumeId);
}

/* A volume copies the field's image handle when it syncs, and nothing else dirties the volume
 * when only the field changes (a time-sampled filePath, say), so it would keep drawing the old
 * grid. Bprims sync before rprims, so the volumes marked here pick up the new handle in this
 * same sync. */
void HdCyclesField::MarkVolumesDirty(HdSceneDelegate *sceneDelegate)
{
  HdRenderIndex &renderIndex = sceneDelegate->GetRenderIndex();
  HdChangeTracker &tracker = renderIndex.GetChangeTracker();
  const SdfPath &id = GetId();

  const std::lock_guard<std::mutex> guard(_volumesMutex);
  for (auto it = _volumes.begin(); it != _volumes.end();) {
    const auto *const volume = dynamic_cast<const HdCyclesVolume *>(renderIndex.GetRprim(*it));
    if (volume && volume->UsesField(id)) {
      tracker.MarkRprimDirty(*it, HdChangeTracker::DirtyVolumeField);
      ++it;
    }
    else {
      it = _volumes.erase(it);
    }
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
