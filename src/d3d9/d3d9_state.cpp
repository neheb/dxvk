#include "d3d9_state.h"

#include "d3d9_texture.h"

namespace dxvk {

  D3D9CapturableState::D3D9CapturableState() {
    streamFreq.fill(1);
    enabledLightIndices.fill(UINT32_MAX);
  }

  D3D9CapturableState::~D3D9CapturableState() {
    for (auto &texture : textures)
      TextureChangePrivate(texture, nullptr);
  }

}
