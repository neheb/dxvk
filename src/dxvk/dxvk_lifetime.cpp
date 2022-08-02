#include "dxvk_lifetime.h"

namespace dxvk {
  
  DxvkLifetimeTracker:: DxvkLifetimeTracker() = default;
  DxvkLifetimeTracker::~DxvkLifetimeTracker() = default;
  
  
  void DxvkLifetimeTracker::notify() {
    m_resources.clear();
  }


  void DxvkLifetimeTracker::reset() {
    m_resources.clear();
  }
  
}