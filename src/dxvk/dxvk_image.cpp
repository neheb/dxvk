#include "dxvk_image.h"

#include "dxvk_device.h"

namespace dxvk {
  
  std::atomic<uint64_t> DxvkImageView::s_cookie = { 0ull };


  DxvkImage::DxvkImage(
    const DxvkDevice*           device,
    const DxvkImageCreateInfo&  createInfo,
          DxvkMemoryAllocator&  memAlloc,
          VkMemoryPropertyFlags memFlags)
  : m_vkd(device->vkd()), m_device(device), m_info(createInfo), m_memFlags(memFlags) {

    // Copy the compatible view formats to a persistent array
    m_viewFormats.resize(createInfo.viewFormatCount);
    for (uint32_t i = 0; i < createInfo.viewFormatCount; i++)
      m_viewFormats[i] = createInfo.viewFormats[i];
    m_info.viewFormats = m_viewFormats.data();

    // If defined, we should provide a format list, which
    // allows some drivers to enable image compression
    VkImageFormatListCreateInfo formatList = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
    formatList.viewFormatCount = createInfo.viewFormatCount;
    formatList.pViewFormats    = createInfo.viewFormats;
    
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &formatList };
    info.flags                 = createInfo.flags;
    info.imageType             = createInfo.type;
    info.format                = createInfo.format;
    info.extent                = createInfo.extent;
    info.mipLevels             = createInfo.mipLevels;
    info.arrayLayers           = createInfo.numLayers;
    info.samples               = createInfo.sampleCount;
    info.tiling                = createInfo.tiling;
    info.usage                 = createInfo.usage;
    info.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout         = createInfo.initialLayout;

    m_shared = canShareImage(info, createInfo.sharing);

    VkExternalMemoryImageCreateInfo externalInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };

    if (m_shared) {
      externalInfo.pNext = std::exchange(info.pNext, &externalInfo);
      externalInfo.handleTypes = createInfo.sharing.type;
    }
    
    if (m_vkd->vkCreateImage(m_vkd->device(),
          &info, nullptr, &m_image.image) != VK_SUCCESS) {
      throw DxvkError(str::format(
        "DxvkImage: Failed to create image:",
        "\n  Type:            ", info.imageType,
        "\n  Format:          ", info.format,
        "\n  Extent:          ", "(", info.extent.width,
                                 ",", info.extent.height,
                                 ",", info.extent.depth, ")",
        "\n  Mip levels:      ", info.mipLevels,
        "\n  Array layers:    ", info.arrayLayers,
        "\n  Samples:         ", info.samples,
        "\n  Usage:           ", info.usage,
        "\n  Tiling:          ", info.tiling));
    }
    
    // Get memory requirements for the image. We may enforce strict
    // alignment on non-linear images in order not to violate the
    // bufferImageGranularity limit, which may be greater than the
    // required resource memory alignment on some GPUs.
    VkMemoryDedicatedRequirements dedicatedRequirements = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 memReq = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicatedRequirements };
    
    VkImageMemoryRequirementsInfo2 memReqInfo = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
    memReqInfo.image = m_image.image;

    VkMemoryDedicatedAllocateInfo dedMemoryAllocInfo = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedMemoryAllocInfo.image  = m_image.image;

    VkExportMemoryAllocateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    if (m_shared && createInfo.sharing.mode == DxvkSharedHandleMode::Export) {
      exportInfo.pNext = std::exchange(dedMemoryAllocInfo.pNext, &exportInfo);
      exportInfo.handleTypes = createInfo.sharing.type;
    }

#ifdef _WIN32
    VkImportMemoryWin32HandleInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    if (m_shared && createInfo.sharing.mode == DxvkSharedHandleMode::Import) {
      importInfo.pNext = std::exchange(dedMemoryAllocInfo.pNext, &importInfo);
      importInfo.handleType = createInfo.sharing.type;
      importInfo.handle = createInfo.sharing.handle;
    }
#endif

    m_vkd->vkGetImageMemoryRequirements2(
      m_vkd->device(), &memReqInfo, &memReq);

    if (info.tiling != VK_IMAGE_TILING_LINEAR && !dedicatedRequirements.prefersDedicatedAllocation) {
      memReq.memoryRequirements.size      = align(memReq.memoryRequirements.size,       memAlloc.bufferImageGranularity());
      memReq.memoryRequirements.alignment = align(memReq.memoryRequirements.alignment , memAlloc.bufferImageGranularity());
    }

    // Use high memory priority for GPU-writable resources
    bool isGpuWritable = (m_info.access & (
      VK_ACCESS_SHADER_WRITE_BIT                  |
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT         |
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT        |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)) != 0;
    
    DxvkMemoryFlags hints(DxvkMemoryFlag::GpuReadable);

    if (isGpuWritable)
      hints.set(DxvkMemoryFlag::GpuWritable);

    if (m_shared) {
      dedicatedRequirements.prefersDedicatedAllocation  = VK_TRUE;
      dedicatedRequirements.requiresDedicatedAllocation = VK_TRUE;
    }

    // Ask driver whether we should be using a dedicated allocation
    m_image.memory = memAlloc.alloc(&memReq.memoryRequirements,
      dedicatedRequirements, dedMemoryAllocInfo, memFlags, hints);
    
    // Try to bind the allocated memory slice to the image
    if (m_vkd->vkBindImageMemory(m_vkd->device(), m_image.image,
          m_image.memory.memory(), m_image.memory.offset()) != VK_SUCCESS)
      throw DxvkError("DxvkImage::DxvkImage: Failed to bind device memory");
  }
  
  
  DxvkImage::DxvkImage(
    const DxvkDevice*           device,
    const DxvkImageCreateInfo&  info,
          VkImage               image)
  : m_vkd(device->vkd()), m_device(device), m_info(info), m_image({ image }) {
    
    m_viewFormats.resize(info.viewFormatCount);
    for (uint32_t i = 0; i < info.viewFormatCount; i++)
      m_viewFormats[i] = info.viewFormats[i];
    m_info.viewFormats = m_viewFormats.data();
  }
  
  
  DxvkImage::~DxvkImage() {
    // This is a bit of a hack to determine whether
    // the image is implementation-handled or not
    if (m_image.memory.memory() != VK_NULL_HANDLE)
      m_vkd->vkDestroyImage(m_vkd->device(), m_image.image, nullptr);
  }


  bool DxvkImage::canShareImage(const VkImageCreateInfo&  createInfo, const DxvkSharedHandleInfo& sharingInfo) const {
    if (sharingInfo.mode == DxvkSharedHandleMode::None)
      return false;

    if (!m_device->extensions().khrExternalMemoryWin32) {
      Logger::err("Failed to create shared resource: VK_KHR_EXTERNAL_MEMORY_WIN32 not supported");
      return false;
    }

    VkPhysicalDeviceExternalImageFormatInfo externalImageFormatInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
    externalImageFormatInfo.handleType = sharingInfo.type;

    VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &externalImageFormatInfo };
    imageFormatInfo.format = createInfo.format;
    imageFormatInfo.type = createInfo.imageType;
    imageFormatInfo.tiling = createInfo.tiling;
    imageFormatInfo.usage = createInfo.usage;
    imageFormatInfo.flags = createInfo.flags;

    VkExternalImageFormatProperties externalImageFormatProperties = { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 imageFormatProperties = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &externalImageFormatProperties };

    VkResult vr = m_device->adapter()->vki()->vkGetPhysicalDeviceImageFormatProperties2(
      m_device->adapter()->handle(), &imageFormatInfo, &imageFormatProperties);

    if (vr != VK_SUCCESS) {
      Logger::err(str::format("Failed to create shared resource: getImageProperties failed:", vr));
      return false;
    }

    if (sharingInfo.mode == DxvkSharedHandleMode::Export) {
      bool ret = externalImageFormatProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
      if (!ret)
        Logger::err("Failed to create shared resource: image cannot be exported");
      return ret;
    }

    if (sharingInfo.mode == DxvkSharedHandleMode::Import) {
      bool ret = externalImageFormatProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
      if (!ret)
        Logger::err("Failed to create shared resource: image cannot be imported");
      return ret;
    }

    return false;
  }


  HANDLE DxvkImage::sharedHandle() const {
    HANDLE handle = INVALID_HANDLE_VALUE;

    if (!m_shared)
      return INVALID_HANDLE_VALUE;

#ifdef _WIN32
    VkMemoryGetWin32HandleInfoKHR handleInfo = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
    handleInfo.handleType = m_info.sharing.type;
    handleInfo.memory = m_image.memory.memory();
    if (m_vkd->vkGetMemoryWin32HandleKHR(m_vkd->device(), &handleInfo, &handle) != VK_SUCCESS)
      Logger::warn("DxvkImage::DxvkImage: Failed to get shared handle for image");
#endif

    return handle;
  }


  DxvkImageView::DxvkImageView(
    const Rc<vk::DeviceFn>&         vkd,
    const Rc<DxvkImage>&            image,
    const DxvkImageViewCreateInfo&  info)
  : m_vkd(vkd), m_image(image), m_info(info), m_cookie(++s_cookie) {
    std::fill(std::begin(m_views), std::end(m_views), VK_NULL_HANDLE);
    
    switch (m_info.type) {
      case VK_IMAGE_VIEW_TYPE_1D:
      case VK_IMAGE_VIEW_TYPE_1D_ARRAY: {
        this->createView(VK_IMAGE_VIEW_TYPE_1D,       1);
        this->createView(VK_IMAGE_VIEW_TYPE_1D_ARRAY, m_info.numLayers);
      } break;
      
      case VK_IMAGE_VIEW_TYPE_2D:
      case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        this->createView(VK_IMAGE_VIEW_TYPE_2D, 1);
        [[fallthrough]];

      case VK_IMAGE_VIEW_TYPE_CUBE:
      case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: {
        this->createView(VK_IMAGE_VIEW_TYPE_2D_ARRAY, m_info.numLayers);
        
        if (m_image->info().flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) {
          uint32_t cubeCount = m_info.numLayers / 6;
        
          if (cubeCount > 0) {
            this->createView(VK_IMAGE_VIEW_TYPE_CUBE,       6);
            this->createView(VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6 * cubeCount);
          }
        }
      } break;
        
      case VK_IMAGE_VIEW_TYPE_3D: {
        this->createView(VK_IMAGE_VIEW_TYPE_3D, 1);
        
        if (m_image->info().flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT && m_info.numLevels == 1) {
          this->createView(VK_IMAGE_VIEW_TYPE_2D,       1);
          this->createView(VK_IMAGE_VIEW_TYPE_2D_ARRAY, m_image->mipLevelExtent(m_info.minLevel).depth);
        }
      } break;
      
      default:
        throw DxvkError(str::format("DxvkImageView: Invalid view type: ", m_info.type));
    }
  }
  
  
  DxvkImageView::~DxvkImageView() {
    for (auto &m_view : m_views)
      m_vkd->vkDestroyImageView(m_vkd->device(), m_view, nullptr);
  }

  
  void DxvkImageView::createView(VkImageViewType type, uint32_t numLayers) {
    VkImageSubresourceRange subresourceRange;
    subresourceRange.aspectMask     = m_info.aspect;
    subresourceRange.baseMipLevel   = m_info.minLevel;
    subresourceRange.levelCount     = m_info.numLevels;
    subresourceRange.baseArrayLayer = m_info.minLayer;
    subresourceRange.layerCount     = numLayers;

    VkImageViewUsageCreateInfo viewUsage = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
    viewUsage.usage           = m_info.usage;
    
    VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &viewUsage };
    viewInfo.image            = m_image->handle();
    viewInfo.viewType         = type;
    viewInfo.format           = m_info.format;
    viewInfo.components       = m_info.swizzle;
    viewInfo.subresourceRange = subresourceRange;

    if (m_info.usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      viewInfo.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    }
    
    if (m_vkd->vkCreateImageView(m_vkd->device(),
          &viewInfo, nullptr, &m_views[type]) != VK_SUCCESS) {
      throw DxvkError(str::format(
        "DxvkImageView: Failed to create image view:"
        "\n  View type:       ", viewInfo.viewType,
        "\n  View format:     ", viewInfo.format,
        "\n  Subresources:    ",
        "\n    Aspect mask:   ", std::hex, viewInfo.subresourceRange.aspectMask,
        "\n    Mip levels:    ", viewInfo.subresourceRange.baseMipLevel, " - ",
                                 viewInfo.subresourceRange.levelCount,
        "\n    Array layers:  ", viewInfo.subresourceRange.baseArrayLayer, " - ",
                                 viewInfo.subresourceRange.layerCount,
        "\n  Image properties:",
        "\n    Type:          ", m_image->info().type,
        "\n    Format:        ", m_image->info().format,
        "\n    Extent:        ", "(", m_image->info().extent.width,
                                 ",", m_image->info().extent.height,
                                 ",", m_image->info().extent.depth, ")",
        "\n    Mip levels:    ", m_image->info().mipLevels,
        "\n    Array layers:  ", m_image->info().numLayers,
        "\n    Samples:       ", m_image->info().sampleCount,
        "\n    Usage:         ", std::hex, m_image->info().usage,
        "\n    Tiling:        ", m_image->info().tiling));
    }
  }
  
}
