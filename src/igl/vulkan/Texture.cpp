/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <igl/vulkan/Common.h>
#include <igl/vulkan/Device.h>
#include <igl/vulkan/Texture.h>
#include <igl/vulkan/VulkanContext.h>
#include <igl/vulkan/VulkanImage.h>
#include <igl/vulkan/VulkanImageView.h>
#include <igl/vulkan/VulkanStagingDevice.h>
#include <igl/vulkan/VulkanTexture.h>

#include <format>

namespace igl {
namespace vulkan {

Texture::Texture(const igl::vulkan::Device& device, TextureFormat format) :
  ITexture(format), device_(device) {}

Result Texture::create(const TextureDesc& desc) {
  desc_ = desc;

  const VulkanContext& ctx = device_.getVulkanContext();

  const VkFormat vkFormat = isDepthOrStencilFormat(desc_.format)
                                ? ctx.getClosestDepthStencilFormat(desc_.format)
                                : textureFormatToVkFormat(desc_.format);

  const igl::TextureType type = desc_.type;
  if (!IGL_VERIFY(type == TextureType::TwoD || type == TextureType::Cube ||
                  type == TextureType::ThreeD)) {
    IGL_ASSERT_MSG(false, "Only 1D, 2D, 3D and Cube textures are supported");
    return Result(Result::Code::Unimplemented);
  }

  if (desc_.numMipLevels == 0) {
    IGL_ASSERT_MSG(false, "The number of mip levels specified must be greater than 0");
    desc_.numMipLevels = 1;
  }

  if (desc.numSamples > 1 && desc_.numMipLevels != 1) {
    IGL_ASSERT_MSG(false, "The number of mip levels for multisampled images should be 1");
    return Result(Result::Code::ArgumentOutOfRange,
                  "The number of mip levels for multisampled images should be 1");
  }

  if (desc.numSamples > 1 && type == TextureType::ThreeD) {
    IGL_ASSERT_MSG(false, "Multisampled 3D images are not supported");
    return Result(Result::Code::ArgumentOutOfRange, "Multisampled 3D images are not supported");
  }

  if (!IGL_VERIFY(desc_.numMipLevels <= TextureDesc::calcNumMipLevels(desc_.width, desc_.height))) {
    return Result(Result::Code::ArgumentOutOfRange,
                  "The number of specified mip levels is greater than the maximum possible "
                  "number of mip levels.");
  }

  if (desc_.usage == 0) {
    IGL_ASSERT_MSG(false, "Texture usage flags are not set");
    desc_.usage = TextureDesc::TextureUsageBits::Sampled;
  }

  /* Use staging device to transfer data into the image when the storage is private to the device */
  VkImageUsageFlags usageFlags =
      (desc_.storage == ResourceStorage::Private) ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0;

  if (desc_.usage & TextureDesc::TextureUsageBits::Sampled) {
    usageFlags |= VK_IMAGE_USAGE_SAMPLED_BIT;
  }
  if (desc_.usage & TextureDesc::TextureUsageBits::Storage) {
    IGL_ASSERT_MSG(desc_.numSamples <= 1, "Storage images cannot be multisampled");
    usageFlags |= VK_IMAGE_USAGE_STORAGE_BIT;
  }
  if (desc_.usage & TextureDesc::TextureUsageBits::Attachment) {
    usageFlags |= isDepthOrStencilFormat(desc_.format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                       : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }

  // For now, always set this flag so we can read it back
  usageFlags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

  IGL_ASSERT_MSG(usageFlags != 0, "Invalid usage flags");

  const VkMemoryPropertyFlags memFlags = resourceStorageToVkMemoryPropertyFlags(desc_.storage);

  const std::string debugNameImage =
      !desc_.debugName.empty() ? std::format("Image: {}", desc_.debugName.c_str()) : "";
  const std::string debugNameImageView =
      !desc_.debugName.empty() ? std::format("Image View: {}", desc_.debugName.c_str()) : "";

  VkImageCreateFlags createFlags = 0;
  uint32_t arrayLayerCount = static_cast<uint32_t>(desc_.numLayers);
  VkImageViewType imageViewType;
  VkImageType imageType;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  switch (desc_.type) {
  case TextureType::TwoD:
    imageViewType = VK_IMAGE_VIEW_TYPE_2D;
    imageType = VK_IMAGE_TYPE_2D;
    samples = getVulkanSampleCountFlags(desc_.numSamples);
    break;
  case TextureType::ThreeD:
    imageViewType = VK_IMAGE_VIEW_TYPE_3D;
    imageType = VK_IMAGE_TYPE_3D;
    break;
  case TextureType::Cube:
    imageViewType = VK_IMAGE_VIEW_TYPE_CUBE;
    arrayLayerCount *= 6;
    imageType = VK_IMAGE_TYPE_2D;
    createFlags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    break;
  default:
    IGL_ASSERT_NOT_REACHED();
    return Result(Result::Code::Unimplemented, "Unimplemented or unsupported texture type.");
  }

  Result result;
  auto image = ctx.createImage(
      imageType,
      VkExtent3D{(uint32_t)desc_.width, (uint32_t)desc_.height, (uint32_t)desc_.depth},
      vkFormat,
      (uint32_t)desc_.numMipLevels,
      arrayLayerCount,
      VK_IMAGE_TILING_OPTIMAL,
      usageFlags,
      memFlags,
      createFlags,
      samples,
      &result,
      debugNameImage.c_str());
  if (!IGL_VERIFY(result.isOk())) {
    return result;
  }
  if (!IGL_VERIFY(image.get())) {
    return Result(Result::Code::InvalidOperation, "Cannot create VulkanImage");
  }

  // TODO: use multiple image views to allow sampling from the STENCIL buffer
  const VkImageAspectFlags aspect = (usageFlags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
                                        ? VK_IMAGE_ASPECT_DEPTH_BIT
                                        : VK_IMAGE_ASPECT_COLOR_BIT;

  std::shared_ptr<VulkanImageView> imageView = image->createImageView(imageViewType,
                                                                      vkFormat,
                                                                      aspect,
                                                                      0,
                                                                      VK_REMAINING_MIP_LEVELS,
                                                                      0,
                                                                      arrayLayerCount,
                                                                      debugNameImageView.c_str());

  if (!IGL_VERIFY(imageView.get())) {
    return Result(Result::Code::InvalidOperation, "Cannot create VulkanImageView");
  }

  texture_ = ctx.createTexture(std::move(image), std::move(imageView));

  return Result();
}

Result Texture::upload(const TextureRangeDesc& range, const void* data, size_t bytesPerRow) const {
  if (!data) {
    return igl::Result();
  }
  const auto [result, _] = validateRange(range);
  if (!result.isOk()) {
    return result;
  }

  const void* uploadData = data;
  const size_t bytesPerPixel = isCompressedTextureFormat(vkFormatToTextureFormat(getVkFormat()))
                                   ? toBytesPerBlock(vkFormatToTextureFormat(getVkFormat()))
                                   : igl::vulkan::getBytesPerPixel(getVkFormat());

  const size_t imageRowWidth = desc_.width * bytesPerPixel;

  std::vector<uint8_t> linearData;

  const bool isAligned = isCompressedTextureFormat(vkFormatToTextureFormat(getVkFormat())) ||
                         bytesPerRow == 0 || imageRowWidth == bytesPerRow;

  if (!isAligned) {
    linearData.resize(imageRowWidth * desc_.height);
  }

  auto numLayers = std::max(range.numLayers, static_cast<size_t>(1));
  auto byteIncrement =
      numLayers > 1
          ? getTextureBytesPerSlice(range.width, range.height, range.depth, desc_.format, 0)
          : 0;
  if (range.numMipLevels > 1) {
    for (auto i = 1; i < range.numMipLevels; ++i) {
      byteIncrement +=
          getTextureBytesPerSlice(range.width, range.height, range.depth, desc_.format, i);
    }
  }

  for (auto i = 0; i < numLayers; ++i) {
    if (isAligned) {
      uploadData = data;
    } else {
      for (uint32_t h = 0; h < desc_.height; h++) {
        memcpy(static_cast<uint8_t*>(linearData.data()) + h * imageRowWidth,
               static_cast<const uint8_t*>(data) + h * bytesPerRow,
               imageRowWidth);
      }

      uploadData = linearData.data();
    }

    const VulkanContext& ctx = device_.getVulkanContext();

    const VkImageType type = texture_->getVulkanImage().type_;

    if (type == VK_IMAGE_TYPE_3D) {
      ctx.stagingDevice_->imageData3D(
          texture_->getVulkanImage(),
          VkOffset3D{(int32_t)range.x, (int32_t)range.y, (int32_t)range.z},
          VkExtent3D{(uint32_t)range.width, (uint32_t)range.height, (uint32_t)range.depth},
          getVkFormat(),
          uploadData);
    } else {
      const VkRect2D imageRegion = ivkGetRect2D(
          (uint32_t)range.x, (uint32_t)range.y, (uint32_t)range.width, (uint32_t)range.height);
      ctx.stagingDevice_->imageData2D(texture_->getVulkanImage(),
                                      imageRegion,
                                      (uint32_t)range.mipLevel,
                                      (uint32_t)range.numMipLevels,
                                      (uint32_t)range.layer + i,
                                      getVkFormat(),
                                      uploadData);
    }

    data = static_cast<const uint8_t*>(data) + byteIncrement;
  }

  return Result();
}

Result Texture::uploadCube(const TextureRangeDesc& range,
                           TextureCubeFace face,
                           const void* data,
                           size_t bytesPerRow) const {
  const auto [result, _] = validateRange(range);
  if (!result.isOk()) {
    return result;
  }

  const VulkanContext& ctx = device_.getVulkanContext();
  const VkRect2D imageRegion = ivkGetRect2D(
      (uint32_t)range.x, (uint32_t)range.y, (uint32_t)range.width, (uint32_t)range.height);
  ctx.stagingDevice_->imageData2D(texture_->getVulkanImage(),
                                  imageRegion,
                                  (uint32_t)range.mipLevel,
                                  (uint32_t)range.numMipLevels,
                                  (uint32_t)face - (uint32_t)TextureCubeFace::PosX,
                                  getVkFormat(),
                                  data);
  return Result();
}

Dimensions Texture::getDimensions() const {
  return Dimensions{desc_.width, desc_.height, desc_.depth};
}

VkFormat Texture::getVkFormat() const {
  IGL_ASSERT(texture_.get());
  return texture_ ? texture_->getVulkanImage().imageFormat_ : VK_FORMAT_UNDEFINED;
}

size_t Texture::getNumLayers() const {
  return desc_.numLayers;
}

TextureType Texture::getType() const {
  return desc_.type;
}

uint32_t Texture::getUsage() const {
  return desc_.usage;
}

size_t Texture::getSamples() const {
  return desc_.numSamples;
}

size_t Texture::getNumMipLevels() const {
  return desc_.numMipLevels;
}

void Texture::generateMipmap() const {
  if (desc_.numMipLevels > 1) {
    IGL_ASSERT(texture_.get());
    IGL_ASSERT(texture_->getVulkanImage().imageLayout_ != VK_IMAGE_LAYOUT_UNDEFINED);
    const auto& ctx = device_.getVulkanContext();
    const auto& wrapper = ctx.immediate_->acquire();
    texture_->getVulkanImage().generateMipmap(wrapper.cmdBuf_);
    ctx.immediate_->submit(wrapper);
  }
}

uint32_t Texture::getTextureId() const {
  return texture_ ? texture_->getTextureId() : 0;
}

VkImageView Texture::getVkImageView() const {
  return texture_ ? texture_->getVulkanImageView().vkImageView_ : VK_NULL_HANDLE;
}

VkImageView Texture::getVkImageViewForFramebuffer(uint32_t level) const {
  if (level < imageViewForFramebuffer_.size() && imageViewForFramebuffer_[level]) {
    return imageViewForFramebuffer_[level]->getVkImageView();
  }

  if (level >= imageViewForFramebuffer_.size()) {
    imageViewForFramebuffer_.resize(level + 1);
  }

  const VkImageAspectFlags flags = texture_->getVulkanImage().getImageAspectFlags();

  imageViewForFramebuffer_[level] = texture_->getVulkanImage().createImageView(
      VK_IMAGE_VIEW_TYPE_2D, textureFormatToVkFormat(desc_.format), flags, level, 1u, 0u, 1u);

  return imageViewForFramebuffer_[level]->vkImageView_;
}

VkImage Texture::getVkImage() const {
  return texture_ ? texture_->getVulkanImage().vkImage_ : VK_NULL_HANDLE;
}

bool Texture::isSwapchainTexture() const {
  return texture_ ? texture_->getVulkanImage().isExternallyManaged_ : false;
}

} // namespace vulkan
} // namespace igl
