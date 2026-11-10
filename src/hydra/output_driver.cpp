/* SPDX-FileCopyrightText: 2022 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/output_driver.h"
#include "hydra/render_buffer.h"
#include "hydra/session.h"

HDCYCLES_NAMESPACE_OPEN_SCOPE

HdCyclesOutputDriver::HdCyclesOutputDriver(HdCyclesSession *renderParam)
    : _renderParam(renderParam)
{
}

namespace {

void updateAovBuffers(const HdCyclesSession *renderParam, const OutputDriver::Tile &tile)
{
  const HdRenderPassAovBinding &displayAovBinding = renderParam->GetDisplayAovBindingLocked();

  std::vector<float> pixels;

  for (const HdRenderPassAovBinding &aovBinding : renderParam->GetAovBindingsLocked()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      if (aovBinding == displayAovBinding && renderBuffer->IsResourceUsed()) {
        continue;  // Display AOV binding is already updated by Cycles display driver
      }

      const HdFormat format = renderBuffer->GetFormat();
      if (format == HdFormatInvalid) {
        continue;  // Skip invalid AOV bindings
      }

      const size_t channels = HdGetComponentCount(format);
      // Avoid extra copy by mapping render buffer directly when dimensions/format match the tile
      if (tile.offset.x == 0 && tile.offset.y == 0 && tile.size.x == renderBuffer->GetWidth() &&
          tile.size.y == renderBuffer->GetHeight() &&
          (format >= HdFormatFloat32 && format <= HdFormatFloat32Vec4))
      {
        float *const data = static_cast<float *>(renderBuffer->Map());
        // Hydra may have reallocated the buffer since the check above; mapping freezes its size
        if (data && tile.size.x == renderBuffer->GetWidth() &&
            tile.size.y == renderBuffer->GetHeight() && renderBuffer->GetFormat() == format)
        {
          TF_VERIFY(tile.get_pass_pixels(aovBinding.aovName.GetString(), channels, data));
        }
        renderBuffer->Unmap();
      }
      else {
        pixels.resize(channels * tile.size.x * tile.size.y);
        if (tile.get_pass_pixels(aovBinding.aovName.GetString(), channels, pixels.data())) {
          const bool isId = aovBinding.aovName == HdAovTokens->primId ||
                            aovBinding.aovName == HdAovTokens->elementId ||
                            aovBinding.aovName == HdAovTokens->instanceId;
          renderBuffer->Map();
          renderBuffer->WritePixels(pixels.data(),
                                    GfVec2i(tile.offset.x, tile.offset.y),
                                    GfVec2i(tile.size.x, tile.size.y),
                                    channels,
                                    isId);
          renderBuffer->Unmap();
        }
        else {
          // Do not warn on missing elementId, which is a standard AOV but is not implememted
          if (aovBinding.aovName != HdAovTokens->elementId) {
            TF_RUNTIME_ERROR("Could not find pass for AOV '%s'", aovBinding.aovName.GetText());
          }
        }
      }
    }
  }
}

}  // namespace

void HdCyclesOutputDriver::write_render_tile(const Tile &tile)
{
  const thread_scoped_lock lock = _renderParam->LockAovBindings();

  updateAovBuffers(_renderParam, tile);

  // Update convergence state of all render buffers
  for (const HdRenderPassAovBinding &aovBinding : _renderParam->GetAovBindingsLocked()) {
    if (auto *const renderBuffer = static_cast<HdCyclesRenderBuffer *>(aovBinding.renderBuffer)) {
      renderBuffer->SetConverged(true);
    }
  }
}

bool HdCyclesOutputDriver::update_render_tile(const Tile &tile)
{
  const thread_scoped_lock lock = _renderParam->LockAovBindings();

  updateAovBuffers(_renderParam, tile);

  return true;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
