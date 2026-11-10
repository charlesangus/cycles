/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"
#include "util/thread.h"

#include <pxr/imaging/hd/renderBuffer.h>

#include <thread>

HDCYCLES_NAMESPACE_OPEN_SCOPE

class HdCyclesRenderBuffer final : public PXR_NS::HdRenderBuffer {
 public:
  HdCyclesRenderBuffer(const PXR_NS::SdfPath &bprimId);
  ~HdCyclesRenderBuffer() override;

  void Finalize(PXR_NS::HdRenderParam *renderParam) override;

  bool Allocate(const PXR_NS::GfVec3i &dimensions,
                PXR_NS::HdFormat format,
                bool multiSampled) override;

  unsigned int GetWidth() const override;

  unsigned int GetHeight() const override;

  unsigned int GetDepth() const override
  {
    return 1u;
  }

  PXR_NS::HdFormat GetFormat() const override;

  bool IsMultiSampled() const override
  {
    return false;
  }

  void *Map() override;

  void Unmap() override;

  bool IsMapped() const override;

  void Resolve() override;

  bool IsConverged() const override;

  void SetConverged(bool converged);

  bool IsResourceUsed() const;

  PXR_NS::VtValue GetResource(bool multiSampled = false) const override;

  void SetResource(const PXR_NS::VtValue &resource);

  void WritePixels(const float *pixels,
                   const PXR_NS::GfVec2i &offset,
                   const PXR_NS::GfVec2i &dims,
                   const int channels,
                   bool isId = false);

 private:
  void _Deallocate() override;

  bool _IsMappedByOtherThread() const;

  /* Hydra's host and the Cycles session thread map the buffer from different threads, so a
   * mapping is exclusive to one thread for as long as it is held: the storage and size cannot
   * change and no other thread reads or writes the pixels under it. The mapping thread may map
   * again, so ownership is counted. The mutex guards every other member, and nothing holding it
   * may take another lock, so it stays the innermost lock after the session's AOV mutex. */
  mutable CCL_NS::thread_mutex _mutex;
  CCL_NS::thread_condition_variable _unmapped;
  std::thread::id _mapOwner;
  int _mapCount = 0;

  unsigned int _width = 0u;
  unsigned int _height = 0u;
  PXR_NS::HdFormat _format = PXR_NS::HdFormatInvalid;
  size_t _dataSize = 0;

  std::vector<uint8_t> _data;
  PXR_NS::VtValue _resource;
  mutable std::atomic_bool _resourceUsed = false;

  std::atomic_bool _converged = false;
};

HDCYCLES_NAMESPACE_CLOSE_SCOPE
