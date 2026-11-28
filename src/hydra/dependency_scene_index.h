/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "hydra/config.h"

#include <pxr/imaging/hd/sceneIndex.h>

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Declares, on each volume, a dependency on every field bound to it. A volume copies its fields'
 * image handles when it syncs, so an HdDependencyForwardingSceneIndex downstream of this one has
 * to dirty the volume's field binding whenever a bound field changes. */
PXR_NS::HdSceneIndexBaseRefPtr NewDependencySceneIndex(
    const PXR_NS::HdSceneIndexBaseRefPtr &inputSceneIndex);

HDCYCLES_NAMESPACE_CLOSE_SCOPE
