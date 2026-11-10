/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/render_buffer.h"
#include "hydra/session.h"
#include "util/half.h"

#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesRenderBuffer::HdCyclesRenderBuffer(const SdfPath &bprimId) : HdRenderBuffer(bprimId) {}

HdCyclesRenderBuffer::~HdCyclesRenderBuffer() = default;

void HdCyclesRenderBuffer::Finalize(HdRenderParam *renderParam)
{
  // Remove this render buffer from AOV bindings
  // This ensures that 'OutputDriver' does not attempt to write to it anymore
  static_cast<HdCyclesSession *>(renderParam)->RemoveAovBinding(this);

  HdRenderBuffer::Finalize(renderParam);
}

bool HdCyclesRenderBuffer::Allocate(const GfVec3i &dimensions,
                                    HdFormat format,
                                    bool /*multiSampled*/)
{
  if (dimensions[2] != 1) {
    TF_RUNTIME_ERROR("HdCyclesRenderBuffer::Allocate called with dimensions that are not 2D.");
    return false;
  }

  thread_scoped_lock lock(_mutex);

  const unsigned int width = dimensions[0];
  const unsigned int height = dimensions[1];
  if (width == _width && height == _height && format == _format) {
    return true;
  }

  if (_mapCount != 0 && _mapOwner == std::this_thread::get_id()) {
    TF_RUNTIME_ERROR("HdCyclesRenderBuffer::Allocate called while buffer is mapped.");
    return false;
  }
  _unmapped.wait(lock, [this] { return _mapCount == 0; });

  _width = width;
  _height = height;
  _format = format;
  _dataSize = size_t(width) * size_t(height) * HdDataSizeOfFormat(format);
  _data.resize(_dataSize);
  _resourceUsed = false;

  return true;
}

void HdCyclesRenderBuffer::_Deallocate()
{
  thread_scoped_lock lock(_mutex);

  if (_mapCount != 0 && _mapOwner == std::this_thread::get_id()) {
    TF_CODING_ERROR("HdCyclesRenderBuffer deallocated while mapped by the deallocating thread.");
  }
  else {
    _unmapped.wait(lock, [this] { return _mapCount == 0; });
  }

  _width = 0u;
  _height = 0u;
  _format = HdFormatInvalid;

  _data.clear();
  _data.shrink_to_fit();
  _dataSize = 0;

  _resource = VtValue();
}

bool HdCyclesRenderBuffer::_IsMappedByOtherThread() const
{
  return _mapCount != 0 && _mapOwner != std::this_thread::get_id();
}

unsigned int HdCyclesRenderBuffer::GetWidth() const
{
  const thread_scoped_lock lock(_mutex);
  return _width;
}

unsigned int HdCyclesRenderBuffer::GetHeight() const
{
  const thread_scoped_lock lock(_mutex);
  return _height;
}

HdFormat HdCyclesRenderBuffer::GetFormat() const
{
  const thread_scoped_lock lock(_mutex);
  return _format;
}

void *HdCyclesRenderBuffer::Map()
{
  thread_scoped_lock lock(_mutex);
  _unmapped.wait(lock, [this] { return !_IsMappedByOtherThread(); });

  // Mapping is not implemented when a resource is set
  if (!_resource.IsEmpty() || _data.empty()) {
    return nullptr;
  }

  _mapOwner = std::this_thread::get_id();
  ++_mapCount;

  return _data.data();
}

void HdCyclesRenderBuffer::Unmap()
{
  thread_scoped_lock lock(_mutex);

  // Callers may unmap after a Map that returned null, which took no ownership
  if (_mapCount == 0 || _mapOwner != std::this_thread::get_id()) {
    return;
  }

  if (--_mapCount == 0) {
    _mapOwner = std::thread::id();
    lock.unlock();
    _unmapped.notify_all();
  }
}

bool HdCyclesRenderBuffer::IsMapped() const
{
  const thread_scoped_lock lock(_mutex);
  return _mapCount != 0;
}

void HdCyclesRenderBuffer::Resolve() {}

bool HdCyclesRenderBuffer::IsConverged() const
{
  return _converged;
}

void HdCyclesRenderBuffer::SetConverged(bool converged)
{
  _converged = converged;
}

bool HdCyclesRenderBuffer::IsResourceUsed() const
{
  return _resourceUsed;
}

VtValue HdCyclesRenderBuffer::GetResource(bool multiSampled) const
{
  TF_UNUSED(multiSampled);

  const thread_scoped_lock lock(_mutex);

  _resourceUsed = true;

  return _resource;
}

void HdCyclesRenderBuffer::SetResource(const VtValue &resource)
{
  const thread_scoped_lock lock(_mutex);
  _resource = resource;
}

namespace {

struct SimpleConversion {
  static float convert(const float value)
  {
    return value;
  }
};
struct IdConversion {
  static int32_t convert(const float value)
  {
    return static_cast<int32_t>(value) - 1;
  }
};
struct UInt8Conversion {
  static uint8_t convert(const float value)
  {
    return static_cast<uint8_t>(value * 255.f);
  }
};
struct SInt8Conversion {
  static int8_t convert(const float value)
  {
    return static_cast<int8_t>(value * 127.f);
  }
};
struct HalfConversion {
  static half convert(const float value)
  {
    return float_to_half_image(value);
  }
};

template<typename SrcT, typename DstT, typename Convertor = SimpleConversion>
void writePixels(const SrcT *srcPtr,
                 const GfVec2i &srcSize,
                 const int srcChannelCount,
                 DstT *dstPtr,
                 const GfVec2i &dstSize,
                 const int dstChannelCount,
                 const GfVec2i &writeSize,
                 const Convertor &convertor = {})
{
  const auto writeChannelCount = GfMin(srcChannelCount, dstChannelCount);

  for (int y = 0; y < writeSize[1]; ++y) {
    for (int x = 0; x < writeSize[0]; ++x) {
      for (int c = 0; c < writeChannelCount; ++c) {
        dstPtr[x * dstChannelCount + c] = convertor.convert(srcPtr[x * srcChannelCount + c]);
      }
    }
    srcPtr += srcSize[0] * srcChannelCount;
    dstPtr += dstSize[0] * dstChannelCount;
  }
}

}  // namespace

void HdCyclesRenderBuffer::WritePixels(const float *srcPixels,
                                       const PXR_NS::GfVec2i &srcOffset,
                                       const GfVec2i &srcDims,
                                       const int srcChannels,
                                       bool isId)
{
  thread_scoped_lock lock(_mutex);
  _unmapped.wait(lock, [this] { return !_IsMappedByOtherThread(); });

  const int width = int(_width);
  const int height = int(_height);
  if (_data.empty() || srcOffset[0] < 0 || srcOffset[1] < 0 || srcOffset[0] >= width ||
      srcOffset[1] >= height)
  {
    return;
  }
  const GfVec2i writeSize(GfMin(srcDims[0], width - srcOffset[0]),
                          GfMin(srcDims[1], height - srcOffset[1]));

  uint8_t *dstPixels = _data.data();

  const size_t formatSize = HdDataSizeOfFormat(_format);
  dstPixels += size_t(srcOffset[1]) * (formatSize * _width) + size_t(srcOffset[0]) * formatSize;

  switch (_format) {
    case HdFormatUNorm8:
    case HdFormatUNorm8Vec2:
    case HdFormatUNorm8Vec3:
    case HdFormatUNorm8Vec4:
      writePixels(srcPixels,
                  srcDims,
                  srcChannels,
                  dstPixels,
                  GfVec2i(_width, _height),
                  1 + (_format - HdFormatUNorm8),
                  writeSize,
                  UInt8Conversion());
      break;

    case HdFormatSNorm8:
    case HdFormatSNorm8Vec2:
    case HdFormatSNorm8Vec3:
    case HdFormatSNorm8Vec4:
      writePixels(srcPixels,
                  srcDims,
                  srcChannels,
                  dstPixels,
                  GfVec2i(_width, _height),
                  1 + (_format - HdFormatSNorm8),
                  writeSize,
                  SInt8Conversion());
      break;

    case HdFormatFloat16:
    case HdFormatFloat16Vec2:
    case HdFormatFloat16Vec3:
    case HdFormatFloat16Vec4:
      writePixels(srcPixels,
                  srcDims,
                  srcChannels,
                  reinterpret_cast<half *>(dstPixels),
                  GfVec2i(_width, _height),
                  1 + (_format - HdFormatFloat16),
                  writeSize,
                  HalfConversion());
      break;

    case HdFormatFloat32:
    case HdFormatFloat32Vec2:
    case HdFormatFloat32Vec3:
    case HdFormatFloat32Vec4:
      writePixels(srcPixels,
                  srcDims,
                  srcChannels,
                  reinterpret_cast<float *>(dstPixels),
                  GfVec2i(_width, _height),
                  1 + (_format - HdFormatFloat32),
                  writeSize);
      break;

    case HdFormatInt32:
      // Special case for ID AOVs (see 'HdCyclesMesh::Sync')
      if (isId) {
        writePixels(srcPixels,
                    srcDims,
                    srcChannels,
                    reinterpret_cast<int *>(dstPixels),
                    GfVec2i(_width, _height),
                    1,
                    writeSize,
                    IdConversion());
      }
      else {
        writePixels(srcPixels,
                    srcDims,
                    srcChannels,
                    reinterpret_cast<int *>(dstPixels),
                    GfVec2i(_width, _height),
                    1,
                    writeSize);
      }
      break;
    case HdFormatInt32Vec2:
    case HdFormatInt32Vec3:
    case HdFormatInt32Vec4:
      writePixels(srcPixels,
                  srcDims,
                  srcChannels,
                  reinterpret_cast<int *>(dstPixels),
                  GfVec2i(_width, _height),
                  1 + (_format - HdFormatInt32),
                  writeSize);
      break;

    default:
      TF_RUNTIME_ERROR("HdCyclesRenderBuffer::WritePixels called with unsupported format.");
      break;
  }
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
