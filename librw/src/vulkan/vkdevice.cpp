#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <vector>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwvk.h"
#include "rwvkimpl.h"

#define PLUGIN_ID ID_DRIVER

#ifdef RW_VULKAN
#include <android/log.h>
#define VKLOG(...) __android_log_print(ANDROID_LOG_INFO, "librw-vk", __VA_ARGS__)
#define VKERR(...) __android_log_print(ANDROID_LOG_ERROR, "librw-vk", __VA_ARGS__)

static const uint32_t kPostFxVertSpv[] =
#include "rw_postfx_vert.h"
;
static const uint32_t kPostFxFragSpv[] =
#include "rw_postfx_frag.h"
;
static const uint32_t kMotionFragSpv[] =
#include "rw_motion_frag.h"
;
static const uint32_t kSgsr2ConvertFragSpv[] =
#include "rw_sgsr2_convert_frag.h"
;
#endif

namespace rw {
namespace vulkan {

static bool32 gLastDeviceOpenRenderTargetFailure;

#ifdef RW_VULKAN

Globals gvk;
RenderState gstate;

struct RetiredBuffer
{
	VkBuffer buffer;
	VkDeviceMemory memory;
};

struct RetiredImage
{
	VkImageView view;
	VkImage image;
	VkDeviceMemory memory;
};

// These vectors deliberately live outside Globals. Globals is a C-style
// backend record that is cleared with memset when the OpenXR device closes.
static std::vector<RetiredBuffer> retiredBuffers[NUM_FRAME_CONTEXTS];
static std::vector<RetiredImage> retiredImages[NUM_FRAME_CONTEXTS];
// Streaming uploads are recorded while the main frame command buffer is open,
// then submitted immediately before it in the same batch. The frame fence
// therefore owns both their temporary resources and their command buffers.
static std::vector<VkCommandBuffer> frameUploadCommands[NUM_FRAME_CONTEXTS];

static uint32
retirementFrame(void)
{
	if(gvk.inFrame)
		return gvk.activeFrame;
	if(gvk.hasSubmittedFrame)
		return gvk.lastSubmittedFrame;
	return gvk.activeFrame;
}

void
retireBuffer(VkBuffer buffer, VkDeviceMemory memory)
{
	if(buffer == VK_NULL_HANDLE && memory == VK_NULL_HANDLE)
		return;
	if(!gvk.initialised || (!gvk.inFrame && !gvk.hasSubmittedFrame)){
		if(buffer) vkDestroyBuffer(gvk.device, buffer, nil);
		if(memory) vkFreeMemory(gvk.device, memory, nil);
		return;
	}
	RetiredBuffer retired = { buffer, memory };
	retiredBuffers[retirementFrame()].push_back(retired);
}

void
retireImage(VkImageView view, VkImage image, VkDeviceMemory memory)
{
	if(view == VK_NULL_HANDLE && image == VK_NULL_HANDLE &&
	   memory == VK_NULL_HANDLE)
		return;
	if(!gvk.initialised || (!gvk.inFrame && !gvk.hasSubmittedFrame)){
		if(view) vkDestroyImageView(gvk.device, view, nil);
		if(image) vkDestroyImage(gvk.device, image, nil);
		if(memory) vkFreeMemory(gvk.device, memory, nil);
		return;
	}
	RetiredImage retired = { view, image, memory };
	retiredImages[retirementFrame()].push_back(retired);
}

static std::vector<VkDescriptorSet> retiredDescriptorSets[NUM_FRAME_CONTEXTS];

void
retireTextureDescriptorSets(VkDescriptorSet *sets, uint32 count)
{
	uint32 live = 0;
	for(uint32 i = 0; i < count; i++)
		if(sets[i] != VK_NULL_HANDLE)
			live++;
	if(live == 0)
		return;
	if(!gvk.initialised || (!gvk.inFrame && !gvk.hasSubmittedFrame)){
		freeTextureDescriptorSets(sets, count);
		return;
	}
	std::vector<VkDescriptorSet> &queue =
		retiredDescriptorSets[retirementFrame()];
	for(uint32 i = 0; i < count; i++){
		if(sets[i] == VK_NULL_HANDLE)
			continue;
		queue.push_back(sets[i]);
		sets[i] = VK_NULL_HANDLE;
	}
}

static void
flushRetired(uint32 frameIndex)
{
	if(!frameUploadCommands[frameIndex].empty()){
		vkFreeCommandBuffers(gvk.device, gvk.commandPool,
		                     (uint32)frameUploadCommands[frameIndex].size(),
		                     frameUploadCommands[frameIndex].data());
		frameUploadCommands[frameIndex].clear();
	}

	for(size_t i = 0; i < retiredBuffers[frameIndex].size(); i++){
		const RetiredBuffer &retired = retiredBuffers[frameIndex][i];
		if(retired.buffer) vkDestroyBuffer(gvk.device, retired.buffer, nil);
		if(retired.memory) vkFreeMemory(gvk.device, retired.memory, nil);
	}
	retiredBuffers[frameIndex].clear();

	for(size_t i = 0; i < retiredImages[frameIndex].size(); i++){
		const RetiredImage &retired = retiredImages[frameIndex][i];
		if(retired.view) vkDestroyImageView(gvk.device, retired.view, nil);
		if(retired.image) vkDestroyImage(gvk.device, retired.image, nil);
		if(retired.memory) vkFreeMemory(gvk.device, retired.memory, nil);
	}
	retiredImages[frameIndex].clear();

	if(!retiredDescriptorSets[frameIndex].empty()){
		freeTextureDescriptorSets(retiredDescriptorSets[frameIndex].data(),
			(uint32)retiredDescriptorSets[frameIndex].size());
		retiredDescriptorSets[frameIndex].clear();
	}
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

bool32
findMemoryType(uint32 typeBits, VkMemoryPropertyFlags properties, uint32 *indexOut)
{
	for(uint32 i = 0; i < gvk.memoryProperties.memoryTypeCount; i++){
		if((typeBits & (1u << i)) == 0)
			continue;
		if((gvk.memoryProperties.memoryTypes[i].propertyFlags & properties) == properties){
			*indexOut = i;
			return 1;
		}
	}
	VKERR("no memory type for bits 0x%x properties 0x%x", typeBits, properties);
	return 0;
}

bool32
createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
             VkMemoryPropertyFlags properties, VkBuffer *bufferOut,
             VkDeviceMemory *memoryOut)
{
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if(vkCreateBuffer(gvk.device, &bufferInfo, nil, bufferOut) != VK_SUCCESS)
		return 0;

	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(gvk.device, *bufferOut, &requirements);

	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits, properties, &typeIndex)){
		vkDestroyBuffer(gvk.device, *bufferOut, nil);
		*bufferOut = VK_NULL_HANDLE;
		return 0;
	}

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil, memoryOut) != VK_SUCCESS){
		vkDestroyBuffer(gvk.device, *bufferOut, nil);
		*bufferOut = VK_NULL_HANDLE;
		return 0;
	}
	vkBindBufferMemory(gvk.device, *bufferOut, *memoryOut, 0);
	return 1;
}

VkCommandBuffer
beginOneShot(void)
{
	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = gvk.commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	if(vkAllocateCommandBuffers(gvk.device, &allocInfo, &commandBuffer) != VK_SUCCESS)
		return VK_NULL_HANDLE;

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(commandBuffer, &beginInfo);
	return commandBuffer;
}

void
endOneShot(VkCommandBuffer commandBuffer)
{
	if(commandBuffer == VK_NULL_HANDLE)
		return;
	if(vkEndCommandBuffer(commandBuffer) != VK_SUCCESS){
		vkFreeCommandBuffers(gvk.device, gvk.commandPool, 1, &commandBuffer);
		return;
	}

	if(gvk.inFrame){
		// Do not submit (or wait) once for every streamed texture/mesh. endFrame
		// prepends all of these command buffers to the main frame in one batch;
		// its fence then provides an exact lifetime boundary for staging data.
		frameUploadCommands[gvk.activeFrame].push_back(commandBuffer);
		return;
	}

	// Startup uploads have no later frame fence to inherit. Keep the
	// synchronous fallback for that uncommon path.
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &commandBuffer;
	const VkResult submitted =
		vkQueueSubmit(gvk.queue, 1, &submit, VK_NULL_HANDLE);
	if(submitted != VK_SUCCESS){
		VKERR("one-shot submit failed: %d", (int)submitted);
		vkFreeCommandBuffers(gvk.device, gvk.commandPool, 1, &commandBuffer);
		return;
	}
	vkQueueWaitIdle(gvk.queue);
	vkFreeCommandBuffers(gvk.device, gvk.commandPool, 1, &commandBuffer);
}

void
transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image,
                      VkImageAspectFlags aspect, uint32 levelCount,
                      VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspect;
	barrier.subresourceRange.levelCount = levelCount;
	barrier.subresourceRange.layerCount = 1;

	VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

	if(from == VK_IMAGE_LAYOUT_UNDEFINED &&
	   to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL){
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	}else if(from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
	         to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL){
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}else if(from == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
	         to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL){
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		sourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}

	vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0,
	                     0, nil, 0, nil, 1, &barrier);
}

bool32
deviceSupportsBC(void)
{
	return gvk.supportsBC;
}

// ---------------------------------------------------------------------------
// Attachments
// ---------------------------------------------------------------------------

static bool32
createDepthBuffer(FrameContext &frame)
{
	static const VkFormat candidates[] = {
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D16_UNORM,
	};
	if(gvk.depthFormat == VK_FORMAT_UNDEFINED){
		for(uint32 i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++){
			VkFormatProperties properties;
			vkGetPhysicalDeviceFormatProperties(
				gvk.physicalDevice, candidates[i], &properties);
			VkFormatFeatureFlags required =
				VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			if(gvk.sgsrMode != SGSR_OFF)
				required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
			if((properties.optimalTilingFeatures & required) == required){
				gvk.depthFormat = candidates[i];
				break;
			}
		}
	}
	if(gvk.depthFormat == VK_FORMAT_UNDEFINED){
		VKERR("no depth format supported");
		return 0;
	}

	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = gvk.depthFormat;
	imageInfo.extent.width = gvk.sceneWidth;
	imageInfo.extent.height = gvk.sceneHeight;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = gvk.viewCount;
	imageInfo.samples = gvk.sceneSamples;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if(gvk.sgsrMode != SGSR_OFF)
		imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(vkCreateImage(gvk.device, &imageInfo, nil, &frame.depthImage) != VK_SUCCESS)
		return 0;

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, frame.depthImage, &requirements);
	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
		return 0;

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil, &frame.depthMemory) != VK_SUCCESS)
		return 0;
	vkBindImageMemory(gvk.device, frame.depthImage, frame.depthMemory, 0);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = frame.depthImage;
	viewInfo.viewType = gvk.viewCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
	                                      : VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = gvk.depthFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = gvk.viewCount;
	return vkCreateImageView(gvk.device, &viewInfo, nil,
	                         &frame.depthView) == VK_SUCCESS;
}

static bool32
createSceneColour(FrameContext &frame)
{
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = gvk.colourFormat;
	imageInfo.extent.width = gvk.sceneWidth;
	imageInfo.extent.height = gvk.sceneHeight;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = gvk.viewCount;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	// TRANSFER_SRC as well: the reduced copy the reflection samples is
	// blitted out of this image once the frame is finished.
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                  VK_IMAGE_USAGE_SAMPLED_BIT |
	                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(vkCreateImage(gvk.device, &imageInfo, nil,
	                 &frame.sceneColourImage) != VK_SUCCESS){
		VKERR("failed to create post-FX scene colour image");
		return 0;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, frame.sceneColourImage,
	                             &requirements);

	// FXAA samples neighbouring texels in a separate pass, so this image must
	// be backed by ordinary device-local memory rather than transient/lazy
	// tile storage.
	uint32 typeIndex = UINT32_MAX;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex)){
		vkDestroyImage(gvk.device, frame.sceneColourImage, nil);
		frame.sceneColourImage = VK_NULL_HANDLE;
		return 0;
	}

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil,
	                    &frame.sceneColourMemory) != VK_SUCCESS){
		vkDestroyImage(gvk.device, frame.sceneColourImage, nil);
		frame.sceneColourImage = VK_NULL_HANDLE;
		return 0;
	}
	if(vkBindImageMemory(gvk.device, frame.sceneColourImage,
	                     frame.sceneColourMemory, 0) != VK_SUCCESS)
		return 0;

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = frame.sceneColourImage;
	// The post shader always uses sampler2DArray so gl_ViewIndex can select the
	// correct eye. A one-layer array is valid for the mono fallback too.
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = gvk.colourFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = gvk.viewCount;
	if(vkCreateImageView(gvk.device, &viewInfo, nil,
	                     &frame.sceneColourView) != VK_SUCCESS){
		VKERR("failed to create post-FX scene colour view");
		return 0;
	}
	return 1;
}

// How far down the finished frame is taken before the reflection block
// samples it, when the player has not chosen. One reads the frame
// itself and needs no copy at all, which is the default: reducing it
// was tried against the frame time and bought nothing, and the water
// mirror is the sharper for reading the full picture.
enum { SCENE_REFLECTION_DIVISOR = 1 };

// Decides how big the reduced copy is and whether it can be produced at all.
// A format that cannot be blitted with a linear filter leaves the reflection
// on the full image rather than losing it.
static void
chooseSceneReflectionSize(void)
{
	VkFormatProperties properties = {};
	vkGetPhysicalDeviceFormatProperties(gvk.physicalDevice,
	                                    gvk.colourFormat, &properties);
	const VkFormatFeatureFlags needed =
		VK_FORMAT_FEATURE_BLIT_SRC_BIT |
		VK_FORMAT_FEATURE_BLIT_DST_BIT |
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	uint32 divisor = gvk.sceneReflectionDivisor;
	if(divisor != 1 && divisor != 2 && divisor != 4 && divisor != 8)
		divisor = SCENE_REFLECTION_DIVISOR;
	// A divisor of one is the reflection reading the frame itself, so
	// there is no copy to make and none to allocate.
	gvk.sceneReflectionBlit = divisor > 1 &&
		(properties.optimalTilingFeatures & needed) == needed;
	gvk.sceneReflectionWidth = gvk.sceneWidth/divisor;
	gvk.sceneReflectionHeight = gvk.sceneHeight/divisor;
	if(gvk.sceneReflectionWidth < 1)
		gvk.sceneReflectionWidth = 1;
	if(gvk.sceneReflectionHeight < 1)
		gvk.sceneReflectionHeight = 1;
	if(gvk.sceneReflectionBlit)
		VKLOG("reflection scene copy %ux%u from %ux%u (1/%u)",
		      gvk.sceneReflectionWidth, gvk.sceneReflectionHeight,
		      gvk.sceneWidth, gvk.sceneHeight, divisor);
	else
		VKLOG("reflection reads the full %ux%u frame (%s)",
		      gvk.sceneWidth, gvk.sceneHeight,
		      divisor == 1 ? "by request" : "format cannot blit");
}

static bool32
createSceneReflection(FrameContext &frame)
{
	if(!gvk.sceneReflectionBlit)
		return 1;
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = gvk.colourFormat;
	imageInfo.extent.width = gvk.sceneReflectionWidth;
	imageInfo.extent.height = gvk.sceneReflectionHeight;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = gvk.viewCount;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                  VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(vkCreateImage(gvk.device, &imageInfo, nil,
	                 &frame.sceneReflectionImage) != VK_SUCCESS){
		VKERR("failed to create reflection scene copy");
		return 0;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, frame.sceneReflectionImage,
	                             &requirements);
	uint32 typeIndex = UINT32_MAX;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex)){
		vkDestroyImage(gvk.device, frame.sceneReflectionImage, nil);
		frame.sceneReflectionImage = VK_NULL_HANDLE;
		return 0;
	}
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil,
	                    &frame.sceneReflectionMemory) != VK_SUCCESS){
		vkDestroyImage(gvk.device, frame.sceneReflectionImage, nil);
		frame.sceneReflectionImage = VK_NULL_HANDLE;
		return 0;
	}
	if(vkBindImageMemory(gvk.device, frame.sceneReflectionImage,
	                     frame.sceneReflectionMemory, 0) != VK_SUCCESS)
		return 0;

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = frame.sceneReflectionImage;
	// Bound where the full-resolution scene used to be, so the shader still
	// declares a sampler2DArray and nothing there changes.
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = gvk.colourFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = gvk.viewCount;
	if(vkCreateImageView(gvk.device, &viewInfo, nil,
	                     &frame.sceneReflectionView) != VK_SUCCESS){
		VKERR("failed to create reflection scene copy view");
		return 0;
	}
	return 1;
}

// Reduces the finished frame into that copy. Runs after the world pass and
// before the post pass, while the scene image holds the world and nothing
// else. Returns true when it ran, which leaves the scene image in
// TRANSFER_SRC rather than COLOR_ATTACHMENT for the barrier that follows.
static bool32
reduceSceneForReflection(FrameContext &frame)
{
	if(!sceneReflectionsActive() || !gvk.sceneReflectionBlit ||
	   frame.sceneReflectionImage == VK_NULL_HANDLE)
		return 0;

	VkImageMemoryBarrier barriers[2] = {};
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = frame.sceneColourImage;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].subresourceRange.levelCount = 1;
	barriers[0].subresourceRange.layerCount = gvk.viewCount;
	// The copy is overwritten whole, so its old contents are discarded; only
	// the previous frame's sampling of it has to have finished.
	barriers[1] = barriers[0];
	barriers[1].srcAccessMask = frame.sceneReflectionInitialised ?
		VK_ACCESS_SHADER_READ_BIT : 0;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].image = frame.sceneReflectionImage;
	vkCmdPipelineBarrier(gvk.frameCommands,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nil, 0, nil, 2, barriers);

	VkImageBlit blit = {};
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = gvk.viewCount;
	blit.srcOffsets[1].x = (int32)gvk.sceneWidth;
	blit.srcOffsets[1].y = (int32)gvk.sceneHeight;
	blit.srcOffsets[1].z = 1;
	blit.dstSubresource = blit.srcSubresource;
	blit.dstOffsets[1].x = (int32)gvk.sceneReflectionWidth;
	blit.dstOffsets[1].y = (int32)gvk.sceneReflectionHeight;
	blit.dstOffsets[1].z = 1;
	vkCmdBlitImage(gvk.frameCommands,
		frame.sceneColourImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		frame.sceneReflectionImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, VK_FILTER_LINEAR);

	VkImageMemoryBarrier readBarrier = barriers[1];
	readBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	readBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	readBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	readBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(gvk.frameCommands,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, nil, 0, nil, 1, &readBarrier);
	frame.sceneReflectionInitialised = 1;
	return 1;
}

static bool32
createSceneMsaaColour(FrameContext &frame)
{
	if(gvk.sceneSamples == VK_SAMPLE_COUNT_1_BIT)
		return 1;
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = gvk.colourFormat;
	imageInfo.extent = { gvk.sceneWidth, gvk.sceneHeight, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = gvk.viewCount;
	imageInfo.samples = gvk.sceneSamples;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                  VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(vkCreateImage(gvk.device, &imageInfo, nil,
	                 &frame.sceneMsaaImage) != VK_SUCCESS)
		return 0;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, frame.sceneMsaaImage,
	                             &requirements);
	uint32 typeIndex = UINT32_MAX;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
		return 0;
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil,
	                    &frame.sceneMsaaMemory) != VK_SUCCESS ||
	   vkBindImageMemory(gvk.device, frame.sceneMsaaImage,
	                     frame.sceneMsaaMemory, 0) != VK_SUCCESS)
		return 0;
	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = frame.sceneMsaaImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = gvk.colourFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = gvk.viewCount;
	return vkCreateImageView(gvk.device, &viewInfo, nil,
	                         &frame.sceneMsaaView) == VK_SUCCESS;
}

static bool32
createResolvedHistory(FrameContext &frame)
{
	if(gvk.sgsrMode != SGSR2_RESOLVED_TEMPORAL_V3 &&
	   gvk.sgsrMode != SGSR2_OFFICIAL_QUALITY)
		return 1;

	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = gvk.colourFormat;
	imageInfo.extent = { gvk.width, gvk.height, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = gvk.viewCount;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                  VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(vkCreateImage(gvk.device, &imageInfo, nil,
	                 &frame.resolvedHistoryImage) != VK_SUCCESS){
		VKERR("failed to create resolved temporal history image");
		return 0;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, frame.resolvedHistoryImage,
	                             &requirements);
	uint32 typeIndex = UINT32_MAX;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
		return 0;
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil,
	                    &frame.resolvedHistoryMemory) != VK_SUCCESS ||
	   vkBindImageMemory(gvk.device, frame.resolvedHistoryImage,
	                     frame.resolvedHistoryMemory, 0) != VK_SUCCESS)
		return 0;

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = frame.resolvedHistoryImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = gvk.colourFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = gvk.viewCount;
	return vkCreateImageView(gvk.device, &viewInfo, nil,
	                         &frame.resolvedHistoryView) == VK_SUCCESS;
}

static bool32
createSgsr2ConvertImage(FrameContext &frame)
{
	if(gvk.sgsrMode != SGSR2_OFFICIAL_QUALITY)
		return 1;
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	imageInfo.extent = { gvk.sceneWidth, gvk.sceneHeight, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = gvk.viewCount;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                  VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(vkCreateImage(gvk.device, &imageInfo, nil,
	                 &frame.sgsrConvertImage) != VK_SUCCESS)
		return 0;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, frame.sgsrConvertImage,
	                             &requirements);
	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
		return 0;
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil,
	                    &frame.sgsrConvertMemory) != VK_SUCCESS ||
	   vkBindImageMemory(gvk.device, frame.sgsrConvertImage,
	                     frame.sgsrConvertMemory, 0) != VK_SUCCESS)
		return 0;
	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = frame.sgsrConvertImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = gvk.viewCount;
	return vkCreateImageView(gvk.device, &viewInfo, nil,
	                         &frame.sgsrConvertView) == VK_SUCCESS;
}

static bool32
createMotionImage(FrameContext &frame)
{
	if(gvk.sgsrMode == SGSR_OFF)
		return 1;
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = gvk.motionFormat;
	imageInfo.extent = { gvk.sceneWidth, gvk.sceneHeight, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = gvk.viewCount;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                  VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(vkCreateImage(gvk.device, &imageInfo, nil, &frame.motionImage) != VK_SUCCESS)
		return 0;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, frame.motionImage, &requirements);
	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
		return 0;
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil,
	                    &frame.motionMemory) != VK_SUCCESS)
		return 0;
	if(vkBindImageMemory(gvk.device, frame.motionImage,
	                     frame.motionMemory, 0) != VK_SUCCESS)
		return 0;
	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = frame.motionImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = gvk.motionFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = gvk.viewCount;
	return vkCreateImageView(gvk.device, &viewInfo, nil,
	                         &frame.motionView) == VK_SUCCESS;
}

static bool32
createRenderPass(void)
{
	VkAttachmentDescription attachments[3] = {};
	const bool32 multisampled = gvk.sceneSamples != VK_SAMPLE_COUNT_1_BIT;
	// World/HUD target. It is stored because the following post pass samples
	// neighbouring texels for FXAA.
	attachments[0].format = gvk.colourFormat;
	attachments[0].samples = gvk.sceneSamples;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = multisampled ? VK_ATTACHMENT_STORE_OP_DONT_CARE :
	                                      VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	// beginFrame performs an explicit read->write transition. This is required
	// once the alternating frame slot also serves as temporal history for the
	// following submission; UNDEFINED here would discard it without ordering
	// the preceding fragment read.
	attachments[0].initialLayout = multisampled ? VK_IMAGE_LAYOUT_UNDEFINED :
	                                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[1].format = gvk.depthFormat;
	attachments[1].samples = gvk.sceneSamples;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	// Nothing reads depth after the pass. Discarding it spares the tiler a
	// writeback of a full two-layer depth surface every single frame, which is
	// one of the larger easy wins on a mobile GPU.
	attachments[1].storeOp = gvk.sgsrMode != SGSR_OFF ?
		VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = gvk.sgsrMode != SGSR_OFF ?
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	if(multisampled){
		attachments[2].format = gvk.colourFormat;
		attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[2].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[2].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}else if(gvk.sgsrMode != SGSR_OFF){
		attachments[2].format = gvk.motionFormat;
		attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkAttachmentReference colourRefs[2] = {};
	colourRefs[0].attachment = 0;
	colourRefs[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colourRefs[1].attachment = 2;
	colourRefs[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference depthRef = {};
	depthRef.attachment = 1;
	depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount =
		gvk.sgsrMode != SGSR_OFF ? 2 : 1;
	subpass.pColorAttachments = colourRefs;
	subpass.pDepthStencilAttachment = &depthRef;
	VkAttachmentReference resolveRef = {};
	if(multisampled){
		resolveRef.attachment = 2;
		resolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		subpass.pResolveAttachments = &resolveRef;
	}

	const uint32 viewMask = gvk.viewCount > 1 ? 0x3u : 0x1u;
	const uint32 correlationMask = viewMask;
	const uint32 viewMasks[1] = { viewMask };
	VkRenderPassMultiviewCreateInfo multiview = {};
	multiview.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
	multiview.subpassCount = 1;
	multiview.pViewMasks = viewMasks;
	multiview.correlationMaskCount = 1;
	multiview.pCorrelationMasks = &correlationMask;

	VkRenderPassCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.pNext = gvk.viewCount > 1 ? &multiview : nil;
	info.attachmentCount =
		(multisampled || gvk.sgsrMode != SGSR_OFF) ? 3 : 2;
	info.pAttachments = attachments;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	if(vkCreateRenderPass(gvk.device, &info, nil, &gvk.renderPass) != VK_SUCCESS)
		return 0;

	// V3 writes the same resolve pass to the OpenXR target and to a private
	// unfiltered temporal-history attachment. Older modes keep the exact
	// single-target render pass they used before.
	const bool32 resolvedHistory =
		gvk.sgsrMode == SGSR2_RESOLVED_TEMPORAL_V3 ||
		gvk.sgsrMode == SGSR2_OFFICIAL_QUALITY;
	VkAttachmentDescription destinations[2] = {};
	destinations[0].format = gvk.colourFormat;
	destinations[0].samples = VK_SAMPLE_COUNT_1_BIT;
	destinations[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	destinations[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	destinations[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	destinations[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	destinations[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	destinations[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	if(resolvedHistory){
		destinations[1] = destinations[0];
		destinations[1].initialLayout =
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		destinations[1].finalLayout =
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
	VkAttachmentReference destinationRefs[2] = {};
	destinationRefs[0].attachment = 0;
	destinationRefs[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	destinationRefs[1].attachment = 1;
	destinationRefs[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkSubpassDescription postSubpass = {};
	postSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	postSubpass.colorAttachmentCount = resolvedHistory ? 2 : 1;
	postSubpass.pColorAttachments = destinationRefs;
	VkRenderPassMultiviewCreateInfo postMultiview = multiview;
	VkRenderPassCreateInfo postInfo = {};
	postInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	postInfo.pNext = gvk.viewCount > 1 ? &postMultiview : nil;
	postInfo.attachmentCount = resolvedHistory ? 2 : 1;
	postInfo.pAttachments = destinations;
	postInfo.subpassCount = 1;
	postInfo.pSubpasses = &postSubpass;
	if(vkCreateRenderPass(gvk.device, &postInfo, nil,
	                      &gvk.postFxRenderPass) != VK_SUCCESS){
		vkDestroyRenderPass(gvk.device, gvk.renderPass, nil);
		gvk.renderPass = VK_NULL_HANDLE;
		return 0;
	}

	if(gvk.sgsrMode != SGSR_OFF){
		VkAttachmentDescription motion = {};
		motion.format = gvk.motionFormat;
		motion.samples = VK_SAMPLE_COUNT_1_BIT;
		motion.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		motion.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		motion.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		motion.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		motion.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		motion.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkAttachmentReference motionRef = {};
		motionRef.attachment = 0;
		motionRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkSubpassDescription motionSubpass = {};
		motionSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		motionSubpass.colorAttachmentCount = 1;
		motionSubpass.pColorAttachments = &motionRef;
		VkRenderPassMultiviewCreateInfo motionMultiview = multiview;
		VkRenderPassCreateInfo motionInfo = {};
		motionInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		motionInfo.pNext = gvk.viewCount > 1 ? &motionMultiview : nil;
		motionInfo.attachmentCount = 1;
		motionInfo.pAttachments = &motion;
		motionInfo.subpassCount = 1;
		motionInfo.pSubpasses = &motionSubpass;
		if(vkCreateRenderPass(gvk.device, &motionInfo, nil,
		                      &gvk.motionRenderPass) != VK_SUCCESS)
			return 0;
	}
	if(gvk.sgsrMode == SGSR2_OFFICIAL_QUALITY){
		VkAttachmentDescription convert = {};
		convert.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		convert.samples = VK_SAMPLE_COUNT_1_BIT;
		convert.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		convert.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		convert.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		convert.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		convert.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		convert.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkAttachmentReference convertRef = {};
		convertRef.attachment = 0;
		convertRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkSubpassDescription convertSubpass = {};
		convertSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		convertSubpass.colorAttachmentCount = 1;
		convertSubpass.pColorAttachments = &convertRef;
		VkRenderPassMultiviewCreateInfo convertMultiview = multiview;
		VkRenderPassCreateInfo convertInfo = {};
		convertInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		convertInfo.pNext = gvk.viewCount > 1 ? &convertMultiview : nil;
		convertInfo.attachmentCount = 1;
		convertInfo.pAttachments = &convert;
		convertInfo.subpassCount = 1;
		convertInfo.pSubpasses = &convertSubpass;
		if(vkCreateRenderPass(gvk.device, &convertInfo, nil,
		                      &gvk.sgsrConvertRenderPass) != VK_SUCCESS)
			return 0;
	}
	return 1;
}

static VkShaderModule
createPostFxModule(const uint32_t *code, size_t byteSize)
{
	VkShaderModuleCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = byteSize;
	info.pCode = code;

	VkShaderModule module = VK_NULL_HANDLE;
	if(vkCreateShaderModule(gvk.device, &info, nil, &module) != VK_SUCCESS)
		VKERR("failed to create post-FX shader module");
	return module;
}

static bool32
createSceneFramebuffer(FrameContext &frame)
{
	const VkImageView attachments[3] = {
		gvk.sceneSamples != VK_SAMPLE_COUNT_1_BIT ? frame.sceneMsaaView :
			frame.sceneColourView,
		frame.depthView,
		gvk.sceneSamples != VK_SAMPLE_COUNT_1_BIT ? frame.sceneColourView :
			frame.motionView
	};
	VkFramebufferCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	info.renderPass = gvk.renderPass;
	info.attachmentCount =
		(gvk.sceneSamples != VK_SAMPLE_COUNT_1_BIT ||
		 gvk.sgsrMode != SGSR_OFF) ? 3 : 2;
	info.pAttachments = attachments;
	info.width = gvk.sceneWidth;
	info.height = gvk.sceneHeight;
	// Multiview derives the layers from the render-pass view mask.
	info.layers = 1;
	return vkCreateFramebuffer(gvk.device, &info, nil,
	                           &frame.sceneFramebuffer) == VK_SUCCESS;
}

static bool32
createMotionFramebuffer(FrameContext &frame)
{
	if(gvk.sgsrMode == SGSR_OFF)
		return 1;
	VkFramebufferCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	info.renderPass = gvk.motionRenderPass;
	info.attachmentCount = 1;
	info.pAttachments = &frame.motionView;
	info.width = gvk.sceneWidth;
	info.height = gvk.sceneHeight;
	info.layers = 1;
	return vkCreateFramebuffer(gvk.device, &info, nil,
	                           &frame.motionFramebuffer) == VK_SUCCESS;
}

static bool32
createSgsr2ConvertFramebuffer(FrameContext &frame)
{
	if(gvk.sgsrMode != SGSR2_OFFICIAL_QUALITY)
		return 1;
	VkFramebufferCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	info.renderPass = gvk.sgsrConvertRenderPass;
	info.attachmentCount = 1;
	info.pAttachments = &frame.sgsrConvertView;
	info.width = gvk.sceneWidth;
	info.height = gvk.sceneHeight;
	info.layers = 1;
	return vkCreateFramebuffer(gvk.device, &info, nil,
	                           &frame.sgsrConvertFramebuffer) == VK_SUCCESS;
}

static bool32
createPostFxResources(void)
{
	VkSamplerCreateInfo samplerInfo = {};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;
	if(vkCreateSampler(gvk.device, &samplerInfo, nil,
	                   &gvk.postFxSampler) != VK_SUCCESS){
		VKERR("failed to create post-FX sampler");
		return 0;
	}

	VkDescriptorSetLayoutBinding inputBindings[5] = {};
	for(uint32 binding = 0; binding < 3; binding++){
		inputBindings[binding].binding = binding;
		inputBindings[binding].descriptorType =
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		inputBindings[binding].descriptorCount = 1;
		inputBindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	inputBindings[3].binding = 3;
	inputBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	inputBindings[3].descriptorCount = 1;
	inputBindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	inputBindings[4].binding = 4;
	inputBindings[4].descriptorType =
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	inputBindings[4].descriptorCount = 1;
	inputBindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo = {};
	descriptorLayoutInfo.sType =
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptorLayoutInfo.bindingCount = 5;
	descriptorLayoutInfo.pBindings = inputBindings;
	if(vkCreateDescriptorSetLayout(gvk.device, &descriptorLayoutInfo, nil,
	                               &gvk.postFxDescriptorLayout) != VK_SUCCESS){
		VKERR("failed to create post-FX descriptor layout");
		return 0;
	}

	VkDescriptorPoolSize poolSizes[2] = {};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[0].descriptorCount = NUM_FRAME_CONTEXTS*4;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[1].descriptorCount = NUM_FRAME_CONTEXTS;
	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = NUM_FRAME_CONTEXTS;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = poolSizes;
	if(vkCreateDescriptorPool(gvk.device, &poolInfo, nil,
	                          &gvk.postFxDescriptorPool) != VK_SUCCESS){
		VKERR("failed to create post-FX descriptor pool");
		return 0;
	}

	VkDescriptorSetAllocateInfo descriptorInfo = {};
	descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorInfo.descriptorPool = gvk.postFxDescriptorPool;
	descriptorInfo.descriptorSetCount = NUM_FRAME_CONTEXTS;
	VkDescriptorSetLayout descriptorLayouts[NUM_FRAME_CONTEXTS];
	VkDescriptorSet descriptorSets[NUM_FRAME_CONTEXTS];
	for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
		descriptorLayouts[i] = gvk.postFxDescriptorLayout;
	descriptorInfo.pSetLayouts = descriptorLayouts;
	if(vkAllocateDescriptorSets(gvk.device, &descriptorInfo,
	                            descriptorSets) != VK_SUCCESS){
		VKERR("failed to allocate post-FX descriptor set");
		return 0;
	}

	VkDescriptorImageInfo imageDescriptors[NUM_FRAME_CONTEXTS][4] = {};
	VkDescriptorBufferInfo bufferDescriptors[NUM_FRAME_CONTEXTS] = {};
	VkWriteDescriptorSet writes[NUM_FRAME_CONTEXTS][5] = {};
	for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
		gvk.frames[i].postFxDescriptor = descriptorSets[i];
		imageDescriptors[i][0].sampler = gvk.postFxSampler;
		imageDescriptors[i][0].imageView = gvk.frames[i].sceneColourView;
		imageDescriptors[i][0].imageLayout =
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		// Binding 1 is always valid. OFF points it at the ordinary scene image;
		// MOTION DEBUG points at the private RG16F vector image.
		imageDescriptors[i][1].sampler = gvk.postFxSampler;
		imageDescriptors[i][1].imageView =
			gvk.sgsrMode == SGSR2_OFFICIAL_QUALITY ?
			gvk.frames[i].sgsrConvertView :
			(gvk.frames[i].motionView ? gvk.frames[i].motionView :
			 gvk.frames[i].sceneColourView);
		imageDescriptors[i][1].imageLayout =
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageDescriptors[i][2].sampler = gvk.motionDepthSampler;
		imageDescriptors[i][2].imageView = gvk.frames[i].depthView;
		imageDescriptors[i][2].imageLayout =
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		// Frame contexts rotate 0/1. The other slot therefore contains the
		// immediately preceding submitted scene and remains alive until this
		// queue submission has finished sampling it.
		const uint32 historyIndex =
			(i+NUM_FRAME_CONTEXTS-1)%NUM_FRAME_CONTEXTS;
		imageDescriptors[i][3].sampler = gvk.postFxSampler;
		imageDescriptors[i][3].imageView =
			(gvk.sgsrMode == SGSR2_RESOLVED_TEMPORAL_V3 ||
			 gvk.sgsrMode == SGSR2_OFFICIAL_QUALITY) ?
			gvk.frames[historyIndex].resolvedHistoryView :
			gvk.frames[historyIndex].sceneColourView;
		imageDescriptors[i][3].imageLayout =
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		for(uint32 binding = 0; binding < 3; binding++){
			writes[i][binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i][binding].dstSet = descriptorSets[i];
			writes[i][binding].dstBinding = binding;
			writes[i][binding].descriptorCount = 1;
			writes[i][binding].descriptorType =
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i][binding].pImageInfo = &imageDescriptors[i][binding];
		}
		bufferDescriptors[i].buffer = gvk.frames[i].motionBuffer;
		bufferDescriptors[i].offset = 0;
		bufferDescriptors[i].range = sizeof(MotionUniformData);
		writes[i][3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i][3].dstSet = descriptorSets[i];
		writes[i][3].dstBinding = 3;
		writes[i][3].descriptorCount = 1;
		writes[i][3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[i][3].pBufferInfo = &bufferDescriptors[i];
		writes[i][4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i][4].dstSet = descriptorSets[i];
		writes[i][4].dstBinding = 4;
		writes[i][4].descriptorCount = 1;
		writes[i][4].descriptorType =
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i][4].pImageInfo = &imageDescriptors[i][3];
	}
	vkUpdateDescriptorSets(gvk.device, NUM_FRAME_CONTEXTS*5,
	                      &writes[0][0], 0, nil);

	VkPushConstantRange pushRange = {};
	pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(PostFxPushConstants);

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &gvk.postFxDescriptorLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	if(vkCreatePipelineLayout(gvk.device, &pipelineLayoutInfo, nil,
	                          &gvk.postFxPipelineLayout) != VK_SUCCESS){
		VKERR("failed to create post-FX pipeline layout");
		return 0;
	}

	VkShaderModule vertexModule =
		createPostFxModule(kPostFxVertSpv, sizeof(kPostFxVertSpv));
	VkShaderModule fragmentModule =
		createPostFxModule(kPostFxFragSpv, sizeof(kPostFxFragSpv));
	if(vertexModule == VK_NULL_HANDLE || fragmentModule == VK_NULL_HANDLE){
		if(vertexModule) vkDestroyShaderModule(gvk.device, vertexModule, nil);
		if(fragmentModule) vkDestroyShaderModule(gvk.device, fragmentModule, nil);
		return 0;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertexModule;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragmentModule;
	stages[1].pName = "main";

	VkPipelineVertexInputStateCreateInfo vertexInput = {};
	vertexInput.sType =
		VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
	inputAssembly.sType =
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState = {};
	viewportState.sType =
		VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasteriser = {};
	rasteriser.sType =
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasteriser.polygonMode = VK_POLYGON_MODE_FILL;
	rasteriser.cullMode = VK_CULL_MODE_NONE;
	rasteriser.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasteriser.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample = {};
	multisample.sType =
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil = {};
	depthStencil.sType =
		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_FALSE;
	depthStencil.depthWriteEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState blendAttachments[2] = {};
	blendAttachments[0].blendEnable = VK_FALSE;
	blendAttachments[0].colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	blendAttachments[1] = blendAttachments[0];
	VkPipelineColorBlendStateCreateInfo colourBlend = {};
	colourBlend.sType =
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colourBlend.attachmentCount =
		(gvk.sgsrMode == SGSR2_RESOLVED_TEMPORAL_V3 ||
		 gvk.sgsrMode == SGSR2_OFFICIAL_QUALITY) ? 2 : 1;
	colourBlend.pAttachments = blendAttachments;

	const VkDynamicState dynamicStates[2] = {
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamicState = {};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = stages;
	pipelineInfo.pVertexInputState = &vertexInput;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasteriser;
	pipelineInfo.pMultisampleState = &multisample;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colourBlend;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = gvk.postFxPipelineLayout;
	pipelineInfo.renderPass = gvk.postFxRenderPass;
	pipelineInfo.subpass = 0;

	const VkResult pipelineResult =
		vkCreateGraphicsPipelines(gvk.device, VK_NULL_HANDLE, 1,
		                          &pipelineInfo, nil, &gvk.postFxPipeline);
	vkDestroyShaderModule(gvk.device, vertexModule, nil);
	vkDestroyShaderModule(gvk.device, fragmentModule, nil);
	if(pipelineResult != VK_SUCCESS){
		VKERR("failed to create post-FX graphics pipeline");
		return 0;
	}

	VKLOG("sampled multiview FXAA/post-FX resolve enabled%s",
	      gvk.sgsrMode == SGSR2_OFFICIAL_QUALITY ?
	      " (official Qualcomm SGSR2 Quality)" :
	      (gvk.sgsrMode == SGSR2_RESOLVED_TEMPORAL_V3 ?
	       " (stable resolved history)" : ""));
	return 1;
}

static bool32
createMotionResources(void)
{
	// The small temporal UBO and depth sampler are kept in every mode because
	// the shared post shader declares them. The full-resolution RG16F image is
	// still debug-only, and normal gameplay never writes depth back to memory.
	VkSamplerCreateInfo samplerInfo = {};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;
	if(vkCreateSampler(gvk.device, &samplerInfo, nil,
	                   &gvk.motionDepthSampler) != VK_SUCCESS)
		return 0;
	for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
		FrameContext &frame = gvk.frames[i];
		VkBufferCreateInfo bufferInfo = {};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = sizeof(MotionUniformData);
		bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if(vkCreateBuffer(gvk.device, &bufferInfo, nil,
		                  &frame.motionBuffer) != VK_SUCCESS)
			return 0;
		VkMemoryRequirements requirements;
		vkGetBufferMemoryRequirements(gvk.device, frame.motionBuffer,
		                              &requirements);
		uint32 memoryType = 0;
		if(!findMemoryType(requirements.memoryTypeBits,
		                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                   &memoryType))
			return 0;
		VkMemoryAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = requirements.size;
		allocInfo.memoryTypeIndex = memoryType;
		if(vkAllocateMemory(gvk.device, &allocInfo, nil,
		                    &frame.motionBufferMemory) != VK_SUCCESS ||
		   vkBindBufferMemory(gvk.device, frame.motionBuffer,
		                      frame.motionBufferMemory, 0) != VK_SUCCESS ||
		   vkMapMemory(gvk.device, frame.motionBufferMemory, 0,
		               sizeof(MotionUniformData), 0,
			               &frame.motionMapped) != VK_SUCCESS)
			return 0;
	}
	VKLOG("temporal motion uniform resources enabled%s",
	      gvk.sgsrMode == SGSR2_MOTION_DEBUG ? " (debug target active)" :
	      (gvk.sgsrMode == SGSR2_TEMPORAL_STABILIZER ?
	       " (temporal stabilizer active)" : ""));
	return 1;
}

static bool32
createSgsr2ConvertResources(void)
{
	if(gvk.sgsrMode != SGSR2_OFFICIAL_QUALITY)
		return 1;

	VkDescriptorSetLayoutBinding bindings[3] = {};
	for(uint32 i = 0; i < 2; i++){
		bindings[i].binding = i;
		bindings[i].descriptorType =
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	bindings[2].binding = 2;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 3;
	layoutInfo.pBindings = bindings;
	if(vkCreateDescriptorSetLayout(gvk.device, &layoutInfo, nil,
	                               &gvk.sgsrConvertDescriptorLayout) !=
	   VK_SUCCESS)
		return 0;

	VkDescriptorPoolSize sizes[2] = {};
	sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[0].descriptorCount = NUM_FRAME_CONTEXTS*2;
	sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	sizes[1].descriptorCount = NUM_FRAME_CONTEXTS;
	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = NUM_FRAME_CONTEXTS;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = sizes;
	if(vkCreateDescriptorPool(gvk.device, &poolInfo, nil,
	                          &gvk.sgsrConvertDescriptorPool) != VK_SUCCESS)
		return 0;

	VkDescriptorSetLayout layouts[NUM_FRAME_CONTEXTS];
	for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
		layouts[i] = gvk.sgsrConvertDescriptorLayout;
	VkDescriptorSet sets[NUM_FRAME_CONTEXTS] = {};
	VkDescriptorSetAllocateInfo alloc = {};
	alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool = gvk.sgsrConvertDescriptorPool;
	alloc.descriptorSetCount = NUM_FRAME_CONTEXTS;
	alloc.pSetLayouts = layouts;
	if(vkAllocateDescriptorSets(gvk.device, &alloc, sets) != VK_SUCCESS)
		return 0;

	VkDescriptorImageInfo images[NUM_FRAME_CONTEXTS][2] = {};
	VkDescriptorBufferInfo buffers[NUM_FRAME_CONTEXTS] = {};
	VkWriteDescriptorSet writes[NUM_FRAME_CONTEXTS][3] = {};
	for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
		FrameContext &frame = gvk.frames[i];
		frame.sgsrConvertDescriptor = sets[i];
		images[i][0].sampler = gvk.motionDepthSampler;
		images[i][0].imageView = frame.motionView;
		images[i][0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		images[i][1].sampler = gvk.motionDepthSampler;
		images[i][1].imageView = frame.depthView;
		images[i][1].imageLayout =
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		for(uint32 binding = 0; binding < 2; binding++){
			writes[i][binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i][binding].dstSet = sets[i];
			writes[i][binding].dstBinding = binding;
			writes[i][binding].descriptorCount = 1;
			writes[i][binding].descriptorType =
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i][binding].pImageInfo = &images[i][binding];
		}
		buffers[i].buffer = frame.motionBuffer;
		buffers[i].range = sizeof(MotionUniformData);
		writes[i][2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i][2].dstSet = sets[i];
		writes[i][2].dstBinding = 2;
		writes[i][2].descriptorCount = 1;
		writes[i][2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writes[i][2].pBufferInfo = &buffers[i];
	}
	vkUpdateDescriptorSets(gvk.device, NUM_FRAME_CONTEXTS*3,
	                      &writes[0][0], 0, nil);

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &gvk.sgsrConvertDescriptorLayout;
	if(vkCreatePipelineLayout(gvk.device, &pipelineLayoutInfo, nil,
	                          &gvk.sgsrConvertPipelineLayout) != VK_SUCCESS)
		return 0;

	VkShaderModule vertex =
		createPostFxModule(kPostFxVertSpv, sizeof(kPostFxVertSpv));
	VkShaderModule fragment = createPostFxModule(
		kSgsr2ConvertFragSpv, sizeof(kSgsr2ConvertFragSpv));
	if(!vertex || !fragment)
		return 0;
	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = stages[1].sType =
		VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment;
	stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vertexInput = {};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	VkPipelineInputAssemblyStateCreateInfo assembly = {};
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport = {};
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo raster = {};
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample = {};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState blendAttachment = {};
	blendAttachment.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo blend = {};
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blendAttachment;
	VkDynamicState dynamicStates[2] = {
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamic = {};
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamicStates;
	VkGraphicsPipelineCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = gvk.sgsrConvertPipelineLayout;
	info.renderPass = gvk.sgsrConvertRenderPass;
	const VkResult result = vkCreateGraphicsPipelines(
		gvk.device, VK_NULL_HANDLE, 1, &info, nil,
		&gvk.sgsrConvertPipeline);
	vkDestroyShaderModule(gvk.device, vertex, nil);
	vkDestroyShaderModule(gvk.device, fragment, nil);
	if(result != VK_SUCCESS)
		return 0;
	VKLOG("official Qualcomm SGSR2 Convert pass enabled");
	return 1;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

static void
readCompletedFrameTimestamps(FrameContext &frame)
{
	if(frame.timestampQueryPool == VK_NULL_HANDLE || !frame.timestampPending)
		return;

	uint64 timestamps[2] = { 0, 0 };
	const VkResult result = vkGetQueryPoolResults(
		gvk.device, frame.timestampQueryPool, 0, 2,
		sizeof(timestamps), timestamps, sizeof(uint64),
		VK_QUERY_RESULT_64_BIT);
	frame.timestampPending = 0;
	if(result != VK_SUCCESS)
		return;

	const uint32 validBits = gvk.frameTimestampValidBits;
	uint64 delta = timestamps[1]-timestamps[0];
	if(validBits > 0 && validBits < 64){
		const uint64 mask = (1ULL << validBits)-1ULL;
		delta = (timestamps[1]-timestamps[0]) & mask;
	}
	gvk.frameGpuMilliseconds =
		(float32)((double)delta*
		          (double)gvk.deviceProperties.limits.timestampPeriod/
		          1000000.0);
	gvk.frameTimestampAvailable = 1;
}

static bool32
invertMatrix4(float32 out[16], const float32 input[16])
{
	// Gauss-Jordan in row form. The backend stores matrices column-major, so
	// transpose while loading/storing the augmented matrix.
	float32 augmented[4][8] = {};
	for(int32 row = 0; row < 4; row++)
		for(int32 col = 0; col < 4; col++){
			augmented[row][col] = input[col*4 + row];
			augmented[row][4+col] = row == col ? 1.0f : 0.0f;
		}
	for(int32 pivot = 0; pivot < 4; pivot++){
		int32 best = pivot;
		for(int32 row = pivot+1; row < 4; row++)
			if(fabsf(augmented[row][pivot]) >
			   fabsf(augmented[best][pivot]))
				best = row;
		if(!isfinite(augmented[best][pivot]) ||
		   fabsf(augmented[best][pivot]) < 1.0e-8f)
			return 0;
		if(best != pivot)
			for(int32 col = 0; col < 8; col++){
				const float32 value = augmented[pivot][col];
				augmented[pivot][col] = augmented[best][col];
				augmented[best][col] = value;
			}
		const float32 inversePivot = 1.0f/augmented[pivot][pivot];
		for(int32 col = 0; col < 8; col++)
			augmented[pivot][col] *= inversePivot;
		for(int32 row = 0; row < 4; row++){
			if(row == pivot)
				continue;
			const float32 scale = augmented[row][pivot];
			for(int32 col = 0; col < 8; col++)
				augmented[row][col] -= scale*augmented[pivot][col];
		}
	}
	for(int32 row = 0; row < 4; row++)
		for(int32 col = 0; col < 4; col++){
			const float32 value = augmented[row][4+col];
			if(!isfinite(value))
				return 0;
			out[col*4 + row] = value;
		}
	return 1;
}

static void
setIdentityMatrix(float32 matrix[16])
{
	memset(matrix, 0, sizeof(float32)*16);
	matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

static void
prepareMotionUniform(FrameContext &frame)
{
	if(gvk.sgsrMode == SGSR_OFF || frame.motionMapped == nil)
		return;
	MotionUniformData data = {};
	// Frontend, loading and cutscene frames do not have a stable immersive
	// world camera. Never feed those frames into gameplay history.
	bool32 historyValid =
		gvk.temporalHistoryValid && gvk.firstPersonActive;
	for(int32 eye = 0; eye < 2; eye++){
		float32 currentWorldClip[16];
		float32 inverseCurrent[16];
		// Qualcomm's reference path explicitly uses ProjectionMatrixNoJitter
		// here. The jitter is consumed separately by the Upscale pass; folding it
		// into this transform as well double-counts sub-pixel motion and softens
		// distant static detail.
		multiplyMatrix(currentWorldClip,
		               gvk.stereoViewProjectionUnjittered[eye],
		               gvk.worldToPlay);
		if(historyValid && invertMatrix4(inverseCurrent, currentWorldClip))
			multiplyMatrix(data.clipToPrevClip[eye],
			               gvk.previousWorldClip[eye], inverseCurrent);
		else{
			setIdentityMatrix(data.clipToPrevClip[eye]);
			historyValid = 0;
		}
		memcpy(gvk.previousWorldClip[eye], currentWorldClip,
		       sizeof(currentWorldClip));
	}
	data.temporalParams[0] = (float32)gvk.sceneWidth;
	data.temporalParams[1] = (float32)gvk.sceneHeight;
	data.temporalParams[2] = historyValid ? 1.0f : 0.0f;
	data.temporalParams[3] = (float32)gvk.sgsrMode;
	data.sgsrParams[0] = gvk.sgsrJitter[0];
	data.sgsrParams[1] = gvk.sgsrJitter[1];
	data.sgsrParams[2] = gvk.sgsrHorizontalTanHalfFov > 0.0f ?
		gvk.sgsrHorizontalTanHalfFov : 1.0f;
	data.sgsrParams[3] = historyValid ? 0.0f : 1.0f;
	memcpy(frame.motionMapped, &data, sizeof(data));
	gvk.temporalHistoryValid = gvk.firstPersonActive;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Wrist panel targets
//
// Small private colour targets that pieces of the interface render into once
// per frame, before the frame's own multiview pass opens: the minimap on one
// arm, the money/health/armour/wanted readout on the other.
//
// They reuse gvk.renderPass rather than creating a plain single-view one.
// Every cached pipeline was created against that pass and the vertex shaders
// declare ViewIndex, so a second pass would mean a second set of pipelines and
// a driver that has to be trusted with ViewIndex outside multiview. The cost
// of reusing it is the second layer of a very small surface. Layer 0 is
// blitted into the sampled raster once the pass closes.
// ---------------------------------------------------------------------------
static const struct {
	int32 width, height;
} gWristPanelSize[WRIST_PANEL_COUNT] = {
	{ 256, 256 },	// the map is square
	{ 512, 192 },	// the status readout is a wide strip of small text
	{ 256, 128 },	// the clock is four digits and a colon
	{ 256, 128 }	// the ammo counter is reserve, a dash and a clip
};

static struct WristPanelTarget {
	VkImage colour;
	VkDeviceMemory colourMemory;
	VkImageView colourView;
	VkImage depth;
	VkDeviceMemory depthMemory;
	VkImageView depthView;
	// Third attachment of the scene pass: the resolve target under MSAA, the
	// motion vectors under SGSR. Only the first is ever read back.
	VkImage extra;
	VkDeviceMemory extraMemory;
	VkImageView extraView;
	VkFramebuffer framebuffer;
	// The pass the framebuffer was built for. Render scale, MSAA and SGSR
	// changes all rebuild it, and the targets have to follow.
	VkRenderPass pass;
	Raster *raster;
	bool32 rasterFilled;
	bool32 sourceLayoutValid;
	bool32 failed;
} gWristPanels[WRIST_PANEL_COUNT];

static bool32
createWristPanelImage(const WristPanelTarget &panel, int32 width, int32 height,
                      VkFormat format, VkSampleCountFlagBits samples,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect,
                      VkImage *imageOut, VkDeviceMemory *memoryOut,
                      VkImageView *viewOut)
{
	(void)panel;
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = format;
	imageInfo.extent.width = (uint32)width;
	imageInfo.extent.height = (uint32)height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = gvk.viewCount;
	imageInfo.samples = samples;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = usage;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if(vkCreateImage(gvk.device, &imageInfo, nil, imageOut) != VK_SUCCESS)
		return 0;

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, *imageOut, &requirements);
	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex))
		return 0;
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil, memoryOut) != VK_SUCCESS)
		return 0;
	if(vkBindImageMemory(gvk.device, *imageOut, *memoryOut, 0) != VK_SUCCESS)
		return 0;

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = *imageOut;
	viewInfo.viewType = gvk.viewCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
	                                        VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = aspect;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = gvk.viewCount;
	return vkCreateImageView(gvk.device, &viewInfo, nil, viewOut) == VK_SUCCESS;
}

static void
destroyWristPanelTarget(WristPanelTarget &panel)
{
	if(gvk.device == VK_NULL_HANDLE)
		return;
	if(panel.framebuffer)
		vkDestroyFramebuffer(gvk.device, panel.framebuffer, nil);
	if(panel.colourView)
		vkDestroyImageView(gvk.device, panel.colourView, nil);
	if(panel.colour)
		vkDestroyImage(gvk.device, panel.colour, nil);
	if(panel.colourMemory)
		vkFreeMemory(gvk.device, panel.colourMemory, nil);
	if(panel.depthView)
		vkDestroyImageView(gvk.device, panel.depthView, nil);
	if(panel.depth)
		vkDestroyImage(gvk.device, panel.depth, nil);
	if(panel.depthMemory)
		vkFreeMemory(gvk.device, panel.depthMemory, nil);
	if(panel.extraView)
		vkDestroyImageView(gvk.device, panel.extraView, nil);
	if(panel.extra)
		vkDestroyImage(gvk.device, panel.extra, nil);
	if(panel.extraMemory)
		vkFreeMemory(gvk.device, panel.extraMemory, nil);
	// The raster is not tied to the render pass and outlives a rebuild.
	Raster *raster = panel.raster;
	const bool32 failed = panel.failed;
	memset(&panel, 0, sizeof(panel));
	panel.raster = raster;
	panel.failed = failed;
}

static bool32
ensureWristPanelTarget(int32 index)
{
	WristPanelTarget &panel = gWristPanels[index];
	if(panel.pass == gvk.renderPass && panel.framebuffer != VK_NULL_HANDLE)
		return 1;
	if(panel.failed || gvk.renderPass == VK_NULL_HANDLE)
		return 0;
	destroyWristPanelTarget(panel);

	const int32 width = gWristPanelSize[index].width;
	const int32 height = gWristPanelSize[index].height;
	const bool32 multisampled = gvk.sceneSamples != VK_SAMPLE_COUNT_1_BIT;
	const bool32 hasThird = multisampled || gvk.sgsrMode != SGSR_OFF;
	VkImageUsageFlags colourUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if(!multisampled)
		colourUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	bool32 ok = createWristPanelImage(panel, width, height, gvk.colourFormat,
		gvk.sceneSamples, colourUsage, VK_IMAGE_ASPECT_COLOR_BIT,
		&panel.colour, &panel.colourMemory, &panel.colourView);
	VkImageUsageFlags depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if(gvk.sgsrMode != SGSR_OFF)
		depthUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	ok = ok && createWristPanelImage(panel, width, height, gvk.depthFormat,
		gvk.sceneSamples, depthUsage, VK_IMAGE_ASPECT_DEPTH_BIT,
		&panel.depth, &panel.depthMemory, &panel.depthView);
	if(ok && hasThird){
		// Under MSAA this is the resolve target, and the surface the finished
		// panel is read back from; under SGSR it is a motion buffer nothing
		// ever looks at.
		VkImageUsageFlags extraUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		if(multisampled)
			extraUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		else
			extraUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
		ok = createWristPanelImage(panel, width, height,
			multisampled ? gvk.colourFormat : gvk.motionFormat,
			VK_SAMPLE_COUNT_1_BIT, extraUsage, VK_IMAGE_ASPECT_COLOR_BIT,
			&panel.extra, &panel.extraMemory, &panel.extraView);
	}
	if(ok){
		const VkImageView attachments[3] = {
			panel.colourView, panel.depthView, panel.extraView
		};
		VkFramebufferCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		info.renderPass = gvk.renderPass;
		info.attachmentCount = hasThird ? 3 : 2;
		info.pAttachments = attachments;
		info.width = (uint32)width;
		info.height = (uint32)height;
		info.layers = 1;	// multiview takes the layers from the view mask
		ok = vkCreateFramebuffer(gvk.device, &info, nil,
			&panel.framebuffer) == VK_SUCCESS;
	}
	if(ok && panel.raster == nil){
		panel.raster = Raster::create(width, height, 32,
			Raster::C8888 | Raster::TEXTURE);
		ok = panel.raster != nil;
	}
	if(!ok){
		VKERR("wrist panel %d target could not be created", index);
		panel.failed = 1;
		destroyWristPanelTarget(panel);
		return 0;
	}
	panel.pass = gvk.renderPass;
	return 1;
}

// Moves the finished panel out of the pass target and into the raster the game
// binds as a texture. A blit rather than a copy: the scene format is whatever
// OpenXR handed out, and blit is the operation that converts between formats
// instead of reinterpreting the bits.
static void
copyWristPanelToRaster(WristPanelTarget &panel, VkImage source,
                       int32 width, int32 height)
{
	VulkanRaster *native = PLUGINOFFSET(VulkanRaster, panel.raster,
	                                    nativeRasterOffset);
	if(native->image == VK_NULL_HANDLE)
		return;

	VkImageMemoryBarrier barriers[2] = {};
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = source;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].subresourceRange.levelCount = 1;
	barriers[0].subresourceRange.layerCount = gvk.viewCount;
	barriers[1] = barriers[0];
	barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].oldLayout = panel.rasterFilled ?
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].image = native->image;
	barriers[1].subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(gvk.frameCommands,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nil, 0, nil, 2, barriers);

	VkImageBlit blit = {};
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[1].x = width;
	blit.srcOffsets[1].y = height;
	blit.srcOffsets[1].z = 1;
	blit.dstSubresource = blit.srcSubresource;
	blit.dstOffsets[1] = blit.srcOffsets[1];
	vkCmdBlitImage(gvk.frameCommands, source,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, native->image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

	barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(gvk.frameCommands, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, nil, 0, nil, 2, barriers);
	native->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	panel.rasterFilled = 1;
}

static void
renderWristPanelTarget(int32 index)
{
	WristPanelTarget &panel = gWristPanels[index];
	if(!ensureWristPanelTarget(index))
		return;

	const int32 width = gWristPanelSize[index].width;
	const int32 height = gWristPanelSize[index].height;
	const bool32 multisampled = gvk.sceneSamples != VK_SAMPLE_COUNT_1_BIT;
	VkImage source = multisampled ? panel.extra : panel.colour;
	if(!panel.sourceLayoutValid){
		// The pass expects to be handed back the layout it left behind, so the
		// blit source starts out where the pass will want it.
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.image = source;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = gvk.viewCount;
		vkCmdPipelineBarrier(gvk.frameCommands,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0, 0, nil, 0, nil, 1, &barrier);
		panel.sourceLayoutValid = 1;
	}

	// Cleared to nothing, alpha included: the interface writes its own alpha
	// where it draws, so what it does not cover stays transparent.
	VkClearValue clears[3];
	memset(clears, 0, sizeof(clears));
	clears[1].depthStencil.depth = 1.0f;
	VkRenderPassBeginInfo passInfo = {};
	passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	passInfo.renderPass = gvk.renderPass;
	passInfo.framebuffer = panel.framebuffer;
	passInfo.renderArea.extent.width = (uint32)width;
	passInfo.renderArea.extent.height = (uint32)height;
	passInfo.clearValueCount =
		(multisampled || gvk.sgsrMode != SGSR_OFF) ? 3 : 2;
	passInfo.pClearValues = clears;
	vkCmdBeginRenderPass(gvk.frameCommands, &passInfo,
	                     VK_SUBPASS_CONTENTS_INLINE);

	VkDescriptorSet sceneSet = getSceneDescriptor();
	vkCmdBindDescriptorSets(gvk.frameCommands, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                        getPipelineLayout(), 0, 1, &sceneSet, 0, nil);
	VkViewport viewport = {};
	viewport.width = (float32)width;
	viewport.height = (float32)height;
	viewport.maxDepth = 1.0f;
	VkRect2D scissor = {};
	scissor.extent.width = (uint32)width;
	scissor.extent.height = (uint32)height;
	vkCmdSetViewport(gvk.frameCommands, 0, 1, &viewport);
	vkCmdSetScissor(gvk.frameCommands, 0, 1, &scissor);

	gvk.wristPanelOffscreen = 1;
	gvk.wristPanelRenderer[index]();
	gvk.wristPanelOffscreen = 0;
	vkCmdEndRenderPass(gvk.frameCommands);
	copyWristPanelToRaster(panel, source, width, height);
}

// Called from beginFrame with the command buffer recording and no render pass
// open yet. Everything the callbacks draw lands in their own texture.
static void
renderWristPanels(void)
{
	bool32 any = 0;
	for(int32 index = 0; index < WRIST_PANEL_COUNT; index++)
		if(gvk.wristPanelWanted[index] && gvk.wristPanelRenderer[index])
			any = 1;
	if(!any){
		for(int32 index = 0; index < WRIST_PANEL_COUNT; index++)
			gvk.wristPanelWanted[index] = 0;
		return;
	}

	// The draw paths refuse to record outside a frame, and a pipeline miss has
	// to defer rather than compile -- this driver does not return from a
	// compile while a pass is open. Both are keyed off inFrame, and from here
	// on both are true.
	gvk.inFrame = 1;
	for(int32 index = 0; index < WRIST_PANEL_COUNT; index++){
		const bool32 wanted = gvk.wristPanelWanted[index];
		// One-shot: the game arms this every frame the panel is on screen, so
		// a menu or loading frame stops feeding it without extra bookkeeping.
		gvk.wristPanelWanted[index] = 0;
		if(wanted && gvk.wristPanelRenderer[index])
			renderWristPanelTarget(index);
	}
}

void
setWristPanelRenderer(int32 panel, void (*renderer)(void))
{
	if(panel >= 0 && panel < WRIST_PANEL_COUNT)
		gvk.wristPanelRenderer[panel] = renderer;
}

void
setWristPanelWanted(int32 panel, bool32 wanted)
{
	if(panel >= 0 && panel < WRIST_PANEL_COUNT)
		gvk.wristPanelWanted[panel] = wanted;
}

Raster*
getWristPanelRaster(int32 panel)
{
	if(panel < 0 || panel >= WRIST_PANEL_COUNT)
		return nil;
	return gWristPanels[panel].rasterFilled ? gWristPanels[panel].raster : nil;
}

bool32
beginFrame(VkImage colourImage, VkImageView colourView)
{
	if(!gvk.initialised || gvk.inFrame)
		return 0;
	(void)colourImage;

	// Anything a previous frame had to defer is built here, before a render
	// pass is open. Compiling inside one does not return on this driver.
	compilePendingPipelines();

	const uint32 frameIndex = gvk.nextFrame;
	FrameContext &frame = gvk.frames[frameIndex];
	if(frame.submissionPending){
		// Wait only when this particular slot comes around again. The other
		// slot remains on the GPU while the CPU records the new frame.
		if(vkWaitForFences(gvk.device, 1, &frame.fence, VK_TRUE,
		                   UINT64_MAX) != VK_SUCCESS)
			return 0;
		frame.submissionPending = 0;
		// Both timestamp readback and deferred resource destruction are now
		// non-blocking and safe for this retired slot.
		readCompletedFrameTimestamps(frame);
		flushRetired(frameIndex);
	}
	gvk.activeFrame = frameIndex;
	frame.reflectionCoverageComplete = 1;
	gvk.frameCommands = frame.commandBuffer;
	setStateFrame(frameIndex);

	// OpenXR rotates through a fixed swapchain image set. Cache one post-pass
	// framebuffer for each (frame slot, colour view) pair. The private
	// scene/depth framebuffer belongs permanently to the frame slot.
	frame.framebuffer = VK_NULL_HANDLE;
	for(uint32 i = 0; i < frame.numFramebuffers; i++)
		if(frame.framebufferColourViews[i] == colourView){
			frame.framebuffer = frame.framebuffers[i];
			break;
		}
	if(frame.framebuffer == VK_NULL_HANDLE){
		if(frame.numFramebuffers >= MAX_FRAMEBUFFERS_PER_CONTEXT){
			VKERR("OpenXR framebuffer cache exhausted");
			return 0;
		}
		const VkImageView attachments[2] = {
			colourView, frame.resolvedHistoryView
		};
		VkFramebufferCreateInfo framebufferInfo = {};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = gvk.postFxRenderPass;
		framebufferInfo.attachmentCount =
			(gvk.sgsrMode == SGSR2_RESOLVED_TEMPORAL_V3 ||
			 gvk.sgsrMode == SGSR2_OFFICIAL_QUALITY) ? 2 : 1;
		framebufferInfo.pAttachments = attachments;
		framebufferInfo.width = gvk.width;
		framebufferInfo.height = gvk.height;
		// Multiview derives the layer count from the view mask, so the
		// framebuffer itself is declared single-layer.
		framebufferInfo.layers = 1;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		if(vkCreateFramebuffer(gvk.device, &framebufferInfo, nil,
		                       &framebuffer) != VK_SUCCESS)
			return 0;
		const uint32 cacheSlot = frame.numFramebuffers++;
		frame.framebufferColourViews[cacheSlot] = colourView;
		frame.framebuffers[cacheSlot] = framebuffer;
		frame.framebuffer = framebuffer;
		VKLOG("cached OpenXR framebuffer frame=%u image=%u",
		      frameIndex, cacheSlot);
	}

	vkResetCommandBuffer(gvk.frameCommands, 0);

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(gvk.frameCommands, &beginInfo);
	VkImageMemoryBarrier sceneWriteBarrier = {};
	sceneWriteBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	sceneWriteBarrier.srcAccessMask = frame.sceneColourInitialised ?
		VK_ACCESS_SHADER_READ_BIT : 0;
	sceneWriteBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	sceneWriteBarrier.oldLayout = frame.sceneColourInitialised ?
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	sceneWriteBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	sceneWriteBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sceneWriteBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sceneWriteBarrier.image = frame.sceneColourImage;
	sceneWriteBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	sceneWriteBarrier.subresourceRange.levelCount = 1;
	sceneWriteBarrier.subresourceRange.layerCount = gvk.viewCount;
	vkCmdPipelineBarrier(gvk.frameCommands,
		frame.sceneColourInitialised ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT :
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, nil, 0, nil, 1, &sceneWriteBarrier);
	frame.sceneColourInitialised = 1;
	if(gvk.sgsrMode == SGSR2_RESOLVED_TEMPORAL_V3 ||
	   gvk.sgsrMode == SGSR2_OFFICIAL_QUALITY){
		VkImageMemoryBarrier historyWriteBarrier = {};
		historyWriteBarrier.sType =
			VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		historyWriteBarrier.srcAccessMask =
			frame.resolvedHistoryInitialised ? VK_ACCESS_SHADER_READ_BIT : 0;
		historyWriteBarrier.dstAccessMask =
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		historyWriteBarrier.oldLayout =
			frame.resolvedHistoryInitialised ?
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
			VK_IMAGE_LAYOUT_UNDEFINED;
		historyWriteBarrier.newLayout =
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		historyWriteBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		historyWriteBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		historyWriteBarrier.image = frame.resolvedHistoryImage;
		historyWriteBarrier.subresourceRange.aspectMask =
			VK_IMAGE_ASPECT_COLOR_BIT;
		historyWriteBarrier.subresourceRange.levelCount = 1;
		historyWriteBarrier.subresourceRange.layerCount = gvk.viewCount;
		vkCmdPipelineBarrier(gvk.frameCommands,
			frame.resolvedHistoryInitialised ?
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT :
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0, 0, nil, 0, nil, 1, &historyWriteBarrier);
		frame.resolvedHistoryInitialised = 1;
	}
	if(gvk.frameTimestampEnabled &&
	   frame.timestampQueryPool != VK_NULL_HANDLE){
		vkCmdResetQueryPool(gvk.frameCommands,
		                    frame.timestampQueryPool, 0, 2);
		vkCmdWriteTimestamp(gvk.frameCommands,
		                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                    frame.timestampQueryPool, 0);
	}

	// Safe to rewind the immediate-mode allocator here: the fence wait above
	// already guarantees the previous frame finished reading it.
	resetDynamic();
	gvk.motionFrameSerial++;
	SceneData *scene = getSceneData();
	for(int32 eye = 0; eye < 2; eye++){
		memcpy(scene->previousViewProj[eye],
		       gvk.temporalHistoryValid ?
		       gvk.previousStereoViewProjectionUnjittered[eye] :
		       gvk.stereoViewProjectionUnjittered[eye], sizeof(float32)*16);
		memcpy(gvk.previousStereoViewProjection[eye],
		       gvk.stereoViewProjection[eye], sizeof(float32)*16);
		memcpy(gvk.previousStereoViewProjectionUnjittered[eye],
		       gvk.stereoViewProjectionUnjittered[eye], sizeof(float32)*16);
	}
	// Fog is linear between the two planes the game supplies. Without it
	// the world simply stops at the far clip, which reads as half-drawn
	// buildings hanging in clear air from anything with altitude.
	{
		const float32 inverseScale = 1.0f/(gvk.worldScale > 0.0001f ? gvk.worldScale : 1.0f);
		const float32 fogStart = gvk.fogStart*inverseScale;
		const float32 fogEnd = gvk.fogEnd*inverseScale;
		const float32 range = fogEnd-fogStart;
		const bool32 usable = range > 0.001f;
		scene->fogParams[0] = fogStart;
		scene->fogParams[1] = fogEnd;
		scene->fogParams[2] = usable ? 1.0f/range : 0.0f;
		scene->fogParams[3] = usable ? 1.0f : 0.0f;
		scene->fogColour[0] = gstate.fogColor.red/255.0f;
		scene->fogColour[1] = gstate.fogColor.green/255.0f;
		scene->fogColour[2] = gstate.fogColor.blue/255.0f;
		scene->fogColour[3] = 1.0f;
	}
	// The vehicle env reflection reads the same sky the pass clears to.
	scene->skyColour[0] = gvk.clearColour[0];
	scene->skyColour[1] = gvk.clearColour[1];
	scene->skyColour[2] = gvk.clearColour[2];
	scene->skyColour[3] = 1.0f;
	// Where the interface drew two frames ago -- the same frame whose image
	// the reflections sample -- so a help box never lands on a car body.
	readAndResetCoverage(scene->im2dCoverage);
	scene->lightCount[2] = (float32)gvk.sceneWidth;
	scene->lightCount[3] = (float32)gvk.sceneHeight;
	scene->reflectionParams[0] = gvk.carReflectionIntensity;
	const FrameContext &reflectionPrevious = gvk.frames[
		(frameIndex + NUM_FRAME_CONTEXTS - 1)%NUM_FRAME_CONTEXTS];
	const bool32 reflectionHistoryValid = sceneReflectionsActive() &&
		reflectionPrevious.reflectionHistoryValid;
	scene->reflectionParams[1] = reflectionHistoryValid ? gvk.carReflectionSsr : 0.0f;
	scene->reflectionParams[2] = gvk.carReflectionSsrDistance;
	scene->reflectionParams[3] = gvk.effectsTime;
	memcpy(scene->waterParams1, gvk.waterParams,
	       sizeof(scene->waterParams1));
	memcpy(scene->waterParams2, gvk.waterParams+4,
	       sizeof(scene->waterParams2));
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	scene->vizParams[0] = gvk.debugVizActive ? 1.0f : 0.0f;
	scene->vizParams[1] = 0.0f;
	scene->vizParams[2] = 0.0f;
	scene->vizParams[3] = 0.0f;
#endif
	if(!reflectionHistoryValid)
		scene->waterParams1[3] = 0.0f;
	// Under the previous frame's camera fold for now; the first beginUpdate of
	// this frame re-derives them under the real one.
	refreshScenePointLights();
	refreshSceneReprojection();
	uploadSceneData();

	// The wrist panels render into their own targets here, while the frame is
	// recording but no pass is open yet. It cannot be done later: the frame
	// opens exactly one pass and runs every game draw inside it.
	renderWristPanels();

	// Open the multiview pass for the whole frame. Every draw the game issues
	// has to land inside a render pass; without this they are recorded outside
	// one and the driver faults on the first vkCmdDraw.
	// The clear colour doubles as the sky. Vice City paints its sky as
	// full-screen 2D gradient quads, which on the headset land on the floating
	// panel as a grey glass pane mid-view; the Quest build skips those quads
	// and clears the whole view to the timecycle colour instead.
	VkClearValue clears[3];
	memset(clears, 0, sizeof(clears));
	clears[0].color.float32[0] = gvk.clearColour[0];
	clears[0].color.float32[1] = gvk.clearColour[1];
	clears[0].color.float32[2] = gvk.clearColour[2];
	clears[0].color.float32[3] = 1.0f;
	clears[1].depthStencil.depth = 1.0f;
	clears[2].color.float32[0] = 0.0f;
	clears[2].color.float32[1] = 0.0f;

	VkRenderPassBeginInfo passInfo = {};
	passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	passInfo.renderPass = gvk.renderPass;
	passInfo.framebuffer = frame.sceneFramebuffer;
	passInfo.renderArea.extent.width = gvk.sceneWidth;
	passInfo.renderArea.extent.height = gvk.sceneHeight;
	passInfo.clearValueCount =
		(gvk.sceneSamples != VK_SAMPLE_COUNT_1_BIT ||
		 gvk.sgsrMode != SGSR_OFF) ? 3 : 2;
	passInfo.pClearValues = clears;
	vkCmdBeginRenderPass(gvk.frameCommands, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

	// Set 0 is the same scene buffer for every world and immediate-mode draw
	// in this frame. All librw scene pipelines share getPipelineLayout(), so
	// bind it once instead of repeating the command for every atomic/primitive.
	VkDescriptorSet sceneSet = getSceneDescriptor();
	vkCmdBindDescriptorSets(gvk.frameCommands, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                        getPipelineLayout(), 0, 1, &sceneSet, 0, nil);
	// Set 3: the vehicle env streak texture plus the previous frame's scene
	// colour, which the reflection block reprojects so a car shows the
	// street that actually stands around it. With two frame contexts the
	// other slot's image holds the frame the GPU finished last; the queue
	// serialises against its writes, and after its post pass the image sits
	// in SHADER_READ until that slot begins again.
	{
		const uint32 previousIndex =
			(frameIndex + NUM_FRAME_CONTEXTS - 1)%NUM_FRAME_CONTEXTS;
		const FrameContext &previous = gvk.frames[previousIndex];
		// The reduced copy when there is one, and the frame itself when
		// there is not -- either the player asked for full resolution
		// or the colour format cannot be blitted.
		VkImageView reflectionSource = VK_NULL_HANDLE;
		if(reflectionHistoryValid && previous.sceneReflectionInitialised)
			reflectionSource = previous.sceneReflectionView;
		else if(reflectionHistoryValid && previous.sceneColourInitialised)
			reflectionSource = previous.sceneColourView;
		bindEnvironmentDescriptor(gvk.frameCommands, reflectionSource);
	}

	VkViewport viewport = {};
	viewport.width = (float32)gvk.sceneWidth;
	viewport.height = (float32)gvk.sceneHeight;
	viewport.maxDepth = 1.0f;
	VkRect2D scissor = {};
	scissor.extent.width = gvk.sceneWidth;
	scissor.extent.height = gvk.sceneHeight;
	vkCmdSetViewport(gvk.frameCommands, 0, 1, &viewport);
	vkCmdSetScissor(gvk.frameCommands, 0, 1, &scissor);

	gvk.inFrame = 1;
	return 1;
}

void
endFrame(void)
{
	if(!gvk.inFrame)
		return;
	FrameContext &frame = gvk.frames[gvk.activeFrame];
	reportImStats();
	// The fold this frame's world draws went out under; the next frame's
	// reflection reprojection moves its probes back into this space.
	memcpy(gvk.worldToPlaySubmitted, gvk.worldToPlay,
	       sizeof(gvk.worldToPlaySubmitted));
	// And the matrices its image was projected with -- see the field comment
	// for why scene.previousViewProj cannot serve here.
	memcpy(gvk.reflectionPrevViewProj, gvk.stereoViewProjectionUnjittered,
	       sizeof(gvk.reflectionPrevViewProj));
	gvk.reflectionPrevValid = sceneReflectionsActive();

	// Make the stored world image visible to the sampled post pass. This is a
	// full-image dependency rather than BY_REGION because FXAA reads adjacent
	// pixels.
	vkCmdEndRenderPass(gvk.frameCommands);
	if(gvk.sgsrMode != SGSR_OFF){
		prepareMotionUniform(frame);
		VkImageMemoryBarrier depthBarrier = {};
		depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		depthBarrier.image = frame.depthImage;
		depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		depthBarrier.subresourceRange.levelCount = 1;
		depthBarrier.subresourceRange.layerCount = gvk.viewCount;
		vkCmdPipelineBarrier(gvk.frameCommands,
		                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                     0, 0, nil, 0, nil, 1, &depthBarrier);

		VkImageMemoryBarrier motionBarrier = {};
		motionBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		motionBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		motionBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		motionBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		motionBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		motionBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		motionBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		motionBarrier.image = frame.motionImage;
		motionBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		motionBarrier.subresourceRange.levelCount = 1;
		motionBarrier.subresourceRange.layerCount = gvk.viewCount;
		vkCmdPipelineBarrier(gvk.frameCommands,
		                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                     0, 0, nil, 0, nil, 1, &motionBarrier);

		if(gvk.sgsrMode == SGSR2_OFFICIAL_QUALITY){
			VkRenderPassBeginInfo convertPass = {};
			convertPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			convertPass.renderPass = gvk.sgsrConvertRenderPass;
			convertPass.framebuffer = frame.sgsrConvertFramebuffer;
			convertPass.renderArea.extent.width = gvk.sceneWidth;
			convertPass.renderArea.extent.height = gvk.sceneHeight;
			vkCmdBeginRenderPass(gvk.frameCommands, &convertPass,
			                     VK_SUBPASS_CONTENTS_INLINE);
			VkViewport convertViewport = {};
			convertViewport.width = (float32)gvk.sceneWidth;
			convertViewport.height = (float32)gvk.sceneHeight;
			convertViewport.maxDepth = 1.0f;
			VkRect2D convertScissor = {};
			convertScissor.extent.width = gvk.sceneWidth;
			convertScissor.extent.height = gvk.sceneHeight;
			vkCmdSetViewport(gvk.frameCommands, 0, 1, &convertViewport);
			vkCmdSetScissor(gvk.frameCommands, 0, 1, &convertScissor);
			vkCmdBindPipeline(gvk.frameCommands,
			                  VK_PIPELINE_BIND_POINT_GRAPHICS,
			                  gvk.sgsrConvertPipeline);
			vkCmdBindDescriptorSets(gvk.frameCommands,
			                        VK_PIPELINE_BIND_POINT_GRAPHICS,
			                        gvk.sgsrConvertPipelineLayout, 0, 1,
			                        &frame.sgsrConvertDescriptor, 0, nil);
			vkCmdDraw(gvk.frameCommands, 3, 1, 0, 0);
			vkCmdEndRenderPass(gvk.frameCommands);
		}
	}
	// Take the reduced copy for the reflection first: it reads the scene
	// image as a transfer source, which is a different layout from the
	// one the post pass wants it in.
	const bool32 reduced = reduceSceneForReflection(frame);
	frame.reflectionHistoryValid = sceneReflectionsActive() &&
		frame.reflectionCoverageComplete;
	VkImageMemoryBarrier sceneBarrier = {};
	sceneBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	sceneBarrier.srcAccessMask = reduced ? VK_ACCESS_TRANSFER_READ_BIT :
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	sceneBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	sceneBarrier.oldLayout = reduced ?
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL :
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	sceneBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	sceneBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sceneBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sceneBarrier.image = frame.sceneColourImage;
	sceneBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	sceneBarrier.subresourceRange.levelCount = 1;
	sceneBarrier.subresourceRange.layerCount = gvk.viewCount;
	vkCmdPipelineBarrier(gvk.frameCommands,
	                     reduced ? VK_PIPELINE_STAGE_TRANSFER_BIT :
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                     0, 0, nil, 0, nil, 1, &sceneBarrier);

	VkRenderPassBeginInfo postPass = {};
	postPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	postPass.renderPass = gvk.postFxRenderPass;
	postPass.framebuffer = frame.framebuffer;
	postPass.renderArea.extent.width = gvk.width;
	postPass.renderArea.extent.height = gvk.height;
	vkCmdBeginRenderPass(gvk.frameCommands, &postPass,
	                     VK_SUBPASS_CONTENTS_INLINE);

	// Resolve the sampled scene into the OpenXR swapchain. This always runs:
	// mode 0 is a byte-for-byte colour pass-through, while mode 1 applies the
	// Vice City colour filter. FXAA is selected with a push-constant flag, so
	// toggling it never rebuilds or switches a graphics pipeline.
	VkViewport viewport = {};
	viewport.width = (float32)gvk.width;
	viewport.height = (float32)gvk.height;
	viewport.maxDepth = 1.0f;
	VkRect2D scissor = {};
	scissor.extent.width = gvk.width;
	scissor.extent.height = gvk.height;
	vkCmdSetViewport(gvk.frameCommands, 0, 1, &viewport);
	vkCmdSetScissor(gvk.frameCommands, 0, 1, &scissor);
	vkCmdBindPipeline(gvk.frameCommands, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                  gvk.postFxPipeline);
	vkCmdBindDescriptorSets(gvk.frameCommands,
	                        VK_PIPELINE_BIND_POINT_GRAPHICS,
	                        gvk.postFxPipelineLayout, 0, 1,
	                        &frame.postFxDescriptor, 0, nil);
	vkCmdPushConstants(gvk.frameCommands, gvk.postFxPipelineLayout,
	                   VK_SHADER_STAGE_FRAGMENT_BIT, 0,
	                   sizeof(gvk.postFxConstants), &gvk.postFxConstants);
	vkCmdDraw(gvk.frameCommands, 3, 1, 0, 0);
	vkCmdEndRenderPass(gvk.frameCommands);
	if(gvk.frameTimestampEnabled &&
	   frame.timestampQueryPool != VK_NULL_HANDLE)
		vkCmdWriteTimestamp(gvk.frameCommands,
		                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		                    frame.timestampQueryPool, 1);
	vkEndCommandBuffer(gvk.frameCommands);

	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	// Copies must be available before this frame consumes newly streamed
	// textures and geometry. Put every upload before the render command buffer
	// in one queue batch; this also reduces vkQueueSubmit traffic to once/frame.
	std::vector<VkCommandBuffer> &uploads =
		frameUploadCommands[gvk.activeFrame];
	uploads.push_back(gvk.frameCommands);
	submit.commandBufferCount = (uint32)uploads.size();
	submit.pCommandBuffers = uploads.data();
	VkResult submitted = vkResetFences(gvk.device, 1, &frame.fence);
	if(submitted == VK_SUCCESS)
		submitted = vkQueueSubmit(gvk.queue, 1, &submit, frame.fence);
	// Keep only the upload buffers in the retirement list. The persistent main
	// frame command buffer belongs to FrameContext and is reset on slot reuse.
	uploads.pop_back();
	if(submitted == VK_SUCCESS){
		frame.submissionPending = 1;
		frame.timestampPending =
			gvk.frameTimestampEnabled &&
			frame.timestampQueryPool != VK_NULL_HANDLE;
		gvk.lastSubmittedFrame = gvk.activeFrame;
		gvk.hasSubmittedFrame = 1;
		gvk.nextFrame = (gvk.activeFrame + 1) % NUM_FRAME_CONTEXTS;
	}else{
		VKERR("frame submit failed: %d", (int)submitted);
		// The fence was reset but no work owns it. Draining the queue makes
		// every previously submitted slot and every retirement queue safe.
		vkQueueWaitIdle(gvk.queue);
		for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
			gvk.frames[i].submissionPending = 0;
			gvk.frames[i].timestampPending = 0;
			flushRetired(i);
		}
		gvk.hasSubmittedFrame = 0;
	}
	gvk.inFrame = 0;
}

void
setGpuFrameTimingEnabled(bool32 enabled)
{
	const bool32 value = enabled ? 1 : 0;
	if(gvk.frameTimestampEnabled == value)
		return;
	gvk.frameTimestampEnabled = value;
	gvk.frameTimestampAvailable = 0;
}

bool32
getGpuFrameTimeMs(float32 *milliseconds)
{
	if(milliseconds != nil)
		*milliseconds = gvk.frameGpuMilliseconds;
	return gvk.frames[0].timestampQueryPool != VK_NULL_HANDLE &&
	       gvk.frameTimestampAvailable;
}

void
multiplyMatrix(float32 out[16], const float32 a[16], const float32 b[16])
{
	for(int32 col = 0; col < 4; col++)
		for(int32 row = 0; row < 4; row++)
			out[col*4 + row] =
				a[0*4 + row]*b[col*4 + 0] +
				a[1*4 + row]*b[col*4 + 1] +
				a[2*4 + row]*b[col*4 + 2] +
				a[3*4 + row]*b[col*4 + 3];
}

void
setStereoViewProjection(const float32 left[16], const float32 right[16])
{
	memcpy(gvk.stereoViewProjection[0], left, sizeof(float32)*16);
	memcpy(gvk.stereoViewProjection[1], right, sizeof(float32)*16);
	memcpy(gvk.stereoViewProjectionUnjittered[0], left, sizeof(float32)*16);
	memcpy(gvk.stereoViewProjectionUnjittered[1], right, sizeof(float32)*16);
	gvk.stereoCullPlanesValid = 1;
	for(int32 eye = 0; eye < 2; eye++){
		const float32 *matrix = gvk.stereoViewProjection[eye];
		static const int32 rows[5][2] = {
			{ 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { 3, 0 }
		};
		for(int32 planeIndex = 0; planeIndex < 5; planeIndex++){
			float32 *plane = gvk.stereoCullPlanes[eye][planeIndex];
			if(rows[planeIndex][0] == 3){
				plane[0] = matrix[3];
				plane[1] = matrix[7];
				plane[2] = matrix[11];
				plane[3] = matrix[15];
			}else{
				const int32 row = rows[planeIndex][0];
				const float32 sign = (float32)rows[planeIndex][1];
				plane[0] = matrix[3]  + sign*matrix[row];
				plane[1] = matrix[7]  + sign*matrix[4+row];
				plane[2] = matrix[11] + sign*matrix[8+row];
				plane[3] = matrix[15] + sign*matrix[12+row];
			}
			const float32 length = sqrtf(plane[0]*plane[0] +
				plane[1]*plane[1] + plane[2]*plane[2]);
			if(!isfinite(length) || length <= 0.000001f){
				gvk.stereoCullPlanesValid = 0;
				continue;
			}
			for(int32 component = 0; component < 4; component++)
				plane[component] /= length;
		}
	}
	gvk.sgsrJitter[0] = gvk.sgsrJitter[1] = 0.0f;
	if(gvk.sgsrMode == SGSR2_JITTERED_TAA_2X &&
	   gvk.firstPersonActive && gvk.temporalHistoryValid){
		// Two opposite quarter-pixel samples. Adding jitter*clip.w to clip.xy
		// works for the already-composed asymmetric per-eye view-projection.
		// Cull planes above intentionally remain unjittered.
		const float32 phaseX =
			(gvk.motionFrameSerial & 1u) ? 0.25f : -0.25f;
		const float32 phaseY = phaseX;
		const float32 jitterNdcX = 2.0f*phaseX/(float32)gvk.sceneWidth;
		// projectionFromFov already flips clip Y and our viewport height is
		// positive, so NDC and framebuffer UV have the same Y direction.
		const float32 jitterNdcY = 2.0f*phaseY/(float32)gvk.sceneHeight;
		for(int32 eye = 0; eye < 2; eye++)
			for(int32 col = 0; col < 4; col++){
				gvk.stereoViewProjection[eye][col*4+0] +=
					jitterNdcX*gvk.stereoViewProjection[eye][col*4+3];
				gvk.stereoViewProjection[eye][col*4+1] +=
					jitterNdcY*gvk.stereoViewProjection[eye][col*4+3];
			}
	}
	if(gvk.sgsrMode == SGSR2_OFFICIAL_QUALITY &&
	   gvk.firstPersonActive){
		// Match the stable PC temporal path: a short eight-sample Halton cycle.
		// The Convert shader removes this exact offset before reconstructing
		// camera motion from depth, so jitter contributes new edge samples but
		// cannot move the resolved world or leak into either eye's history lookup.
		auto halton = [](uint32 index, uint32 base) -> float32 {
			float32 value = 0.0f;
			float32 fraction = 1.0f/(float32)base;
			while(index > 0){
				value += (float32)(index%base)*fraction;
				index /= base;
				fraction /= (float32)base;
			}
			return value;
		};
		const uint32 sample = (uint32)(gvk.motionFrameSerial%8u)+1u;
		gvk.sgsrJitter[0] = halton(sample, 2u)-0.5f;
		gvk.sgsrJitter[1] = halton(sample, 3u)-0.5f;
		const float32 jitterNdcX =
			2.0f*gvk.sgsrJitter[0]/(float32)gvk.sceneWidth;
		const float32 jitterNdcY =
			2.0f*gvk.sgsrJitter[1]/(float32)gvk.sceneHeight;
		for(int32 eye = 0; eye < 2; eye++)
			for(int32 col = 0; col < 4; col++){
				gvk.stereoViewProjection[eye][col*4+0] +=
					jitterNdcX*gvk.stereoViewProjection[eye][col*4+3];
				gvk.stereoViewProjection[eye][col*4+1] +=
					jitterNdcY*gvk.stereoViewProjection[eye][col*4+3];
			}
	}

	SceneData *scene = getSceneData();
	memcpy(scene->viewProj[0], gvk.stereoViewProjection[0], sizeof(float32)*16);
	memcpy(scene->viewProj[1], gvk.stereoViewProjection[1], sizeof(float32)*16);
}

void
setSgsrHorizontalFovDegrees(float32 degrees)
{
	if(!isfinite(degrees) || degrees < 1.0f || degrees > 179.0f){
		gvk.sgsrHorizontalTanHalfFov = 1.0f;
		return;
	}
	gvk.sgsrHorizontalTanHalfFov =
		tanf(degrees*0.5f*3.14159265358979323846f/180.0f);
}

// Plants play space on the game camera, so world geometry reaches clip space
// through the same viewpoint the flat game would use.
//
// Two conventions meet here. OpenXR reports the head in play space: metres,
// Y up, looking down -Z, right handed. RenderWare has the world Z up with a
// mirrored X -- the same flip the D3D backends bake into their view matrix
// (see beginUpdate in d3d12device.cpp) and the same one the desktop VR layer
// applies in ToGameVector. So the play space basis, written in world space, is
//
//     x -> -right    y -> up    z -> -at    origin -> pos
//
// and world geometry is transformed by the inverse of that before the per-eye
// matrices are applied. The head pose then rides on top of the game camera
// instead of replacing it.
//
// Skipping this step draws the world in RenderWare's frame through an OpenXR
// camera, which is a 90 degree rotation about X: the ground stands up vertical.
//
// The result goes into a global rather than the scene uniform block on purpose.
// Vice City runs more than one camera per frame, and every draw recorded into a
// frame reads that block at whatever it holds when the frame is submitted, so a
// second camera would silently retransform geometry already recorded under the
// first. Folding it into each draw's model matrix captures it at record time.
static void
getFirstPersonPlayBasis(float32 px[3], float32 py[3], float32 pz[3])
{
	if(gvk.fpUseFullBasis){
		memcpy(px, gvk.fpPlayX, sizeof(gvk.fpPlayX));
		memcpy(py, gvk.fpPlayY, sizeof(gvk.fpPlayY));
		memcpy(pz, gvk.fpPlayZ, sizeof(gvk.fpPlayZ));
		return;
	}
	const float32 a = gvk.fpAnchorYaw;
	px[0] =  sinf(a); px[1] = -cosf(a); px[2] = 0.0f;
	py[0] =  0.0f;    py[1] =  0.0f;    py[2] = 1.0f;
	pz[0] = -cosf(a); pz[1] = -sinf(a); pz[2] = 0.0f;
}

static void
updateWorldToPlay(Camera *cam)
{
	// First person: the anchor is the player's head in the world, not the
	// follow camera. The basis is a fixed yaw latched at activation, so the
	// game world stays still while the player physically turns; the head
	// pose in the per-eye view matrices does all the looking.
	if(gvk.firstPersonActive){
		// Play axes expressed in world space. Normally these are reconstructed
		// from yaw and world-up; cockpit mode can instead provide the full
		// vehicle basis so pitch/roll remain coherent in both eyes.
		float32 px[3], py[3], pz[3];
		getFirstPersonPlayBasis(px, py, pz);
		memcpy(gvk.cameraWorldPos, gvk.fpHeadWorld,
		       sizeof(gvk.cameraWorldPos));
		const float32 inverseScale = 1.0f/(gvk.worldScale > 0.0001f ? gvk.worldScale : 1.0f);
		float32 *m = gvk.worldToPlay;
		m[0]  = px[0]*inverseScale; m[4] = px[1]*inverseScale; m[8]  = px[2]*inverseScale;
		m[1]  = py[0]*inverseScale; m[5] = py[1]*inverseScale; m[9]  = py[2]*inverseScale;
		m[2]  = pz[0]*inverseScale; m[6] = pz[1]*inverseScale; m[10] = pz[2]*inverseScale;
		m[3]  = 0.0f;  m[7] = 0.0f;  m[11] = 0.0f;
		// The anchor lands on the LATCHED head position; the per-eye poses
		// carry the live one. Their difference is exactly the physical head
		// movement, so leaning towards the dashboard or ducking behind cover
		// moves the viewpoint in the world. Anchoring on the live position
		// cancelled that out and made the build feel 3DOF.
		const float32 *w = gvk.fpHeadWorld;
		const float32 *l = gvk.fpLatchedHeadPos;
		m[12] = -(px[0]*w[0] + px[1]*w[1] + px[2]*w[2])*inverseScale + l[0];
		m[13] = -(py[0]*w[0] + py[1]*w[1] + py[2]*w[2])*inverseScale + l[1];
		m[14] = -(pz[0]*w[0] + pz[1]*w[1] + pz[2]*w[2])*inverseScale + l[2];
		m[15] = 1.0f;
		return;
	}

	// The frontend runs before a camera frame exists. Identity leaves world
	// space equal to play space there, which keeps anything drawn in 3D around
	// the head rather than flinging it away by an uninitialised transform.
	if(cam == nil || cam->getFrame() == nil){
		memset(gvk.worldToPlay, 0, sizeof(gvk.worldToPlay));
		gvk.worldToPlay[0] = gvk.worldToPlay[5] =
		gvk.worldToPlay[10] = gvk.worldToPlay[15] = 1.0f;
		memset(gvk.cameraWorldPos, 0, sizeof(gvk.cameraWorldPos));
		return;
	}

	Matrix *ltm = cam->getFrame()->getLTM();

	const V3d bx = neg(ltm->right);
	const V3d by = ltm->up;
	const V3d bz = neg(ltm->at);
	const V3d origin = ltm->pos;
	gvk.cameraWorldPos[0] = origin.x;
	gvk.cameraWorldPos[1] = origin.y;
	gvk.cameraWorldPos[2] = origin.z;

	// Uniformly scaled orthonormal basis; camera-space coordinates are metres.
	//
	// The camera point lands on the player's HEAD, not on the play space
	// origin. The origin is wherever the Guardian was set up: anchoring there
	// puts the viewpoint at floor height and makes every turn of the head
	// sweep an arc whose radius is the player's distance from the middle of
	// their room. Pinning the head instead means turning in place pivots
	// around the player, at the cost of positional head movement -- an
	// acceptable trade until the full VR layer is ported.
	float32 *m = gvk.worldToPlay;
	const float32 inverseScale = 1.0f/(gvk.worldScale > 0.0001f ? gvk.worldScale : 1.0f);
	m[0]  = bx.x*inverseScale; m[4] = bx.y*inverseScale; m[8]  = bx.z*inverseScale;
	m[1]  = by.x*inverseScale; m[5] = by.y*inverseScale; m[9]  = by.z*inverseScale;
	m[2]  = bz.x*inverseScale; m[6] = bz.y*inverseScale; m[10] = bz.z*inverseScale;
	m[3]  = 0.0f; m[7] = 0.0f; m[11] = 0.0f;
	m[12] = -dot(bx, origin)*inverseScale + gvk.headPosition[0];
	m[13] = -dot(by, origin)*inverseScale + gvk.headPosition[1];
	m[14] = -dot(bz, origin)*inverseScale + gvk.headPosition[2];
	m[15] = 1.0f;
}

void
setPointLights(const PointLight *lights, uint32 count, float32 glowStrength)
{
	if(lights == nil)
		count = 0;
	if(count > MAX_POINT_LIGHTS)
		count = MAX_POINT_LIGHTS;
	if(count > 0)
		memcpy(gvk.pointLights, lights, count*sizeof(PointLight));
	gvk.pointLightCount = count;
	gvk.pointLightGlowStrength = glowStrength > 0.0f ? glowStrength : 0.0f;
}

void
setCarReflectionsEnabled(bool32 enabled)
{
	gvk.carReflections = enabled;
}

void
setModernWaterEnabled(bool32 enabled)
{
	gvk.modernWater = enabled;
}

void
setIm3DWater(bool32 water)
{
	gvk.im3dWaterDraw = water;
}

#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
void
setDebugVizActive(bool32 active)
{
	gvk.debugVizActive = active;
}

void
setDebugTint(uint32 index)
{
	gvk.debugTint = index;
}
#endif

void
setWaterParams(float32 waves, float32 speed, float32 distortion,
               float32 reflection, float32 sheen, float32 glint,
               float32 sparks)
{
	gvk.waterParams[0] = waves > 0.0f ? waves : 0.0f;
	gvk.waterParams[1] = speed > 0.0f ? speed : 0.0f;
	gvk.waterParams[2] = distortion > 0.0f ? distortion : 0.0f;
	gvk.waterParams[3] = reflection > 0.0f ? reflection : 0.0f;
	gvk.waterParams[4] = sheen > 0.0f ? sheen : 0.0f;
	gvk.waterParams[5] = glint > 0.0f ? glint : 0.0f;
	gvk.waterParams[6] = sparks > 0.0f ? sparks : 0.0f;
}

void
setEffectsTime(float32 seconds)
{
	gvk.effectsTime = seconds;
}

void
setWaterSun(const float32 directionWorld[3], float32 red, float32 green,
            float32 blue)
{
	if(directionWorld != nil)
		memcpy(gvk.waterSunDirWorld, directionWorld,
		       sizeof(gvk.waterSunDirWorld));
	gvk.waterSunColour[0] = red;
	gvk.waterSunColour[1] = green;
	gvk.waterSunColour[2] = blue;
}

void
setCarReflectionParams(float32 intensity, float32 ssrStrength,
                       float32 ssrDistance)
{
	gvk.carReflectionIntensity = intensity > 0.0f ? intensity : 0.0f;
	gvk.carReflectionSsr = ssrStrength > 0.0f ? ssrStrength : 0.0f;
	gvk.carReflectionSsrDistance = ssrDistance > 1.0f ? ssrDistance : 1.0f;
}

void
setPlayerVehicle(const float32 position[3], float32 radius, bool32 active)
{
	if(active && position != nil)
		memcpy(gvk.playerVehiclePos, position,
		       sizeof(gvk.playerVehiclePos));
	gvk.playerVehicleRadius = radius > 2.0f ? radius : 2.0f;
	gvk.playerVehicleActive = active && position != nil;
}

// Play-space copy of the frame's dynamic lights. The shaders compare these
// against play-space geometry, and geometry reaches play space through
// whatever worldToPlay holds when its draw is recorded -- so this is redone
// after every camera update, not just once a frame, or the light pools would
// trail the head by a frame.
void
refreshScenePointLights(void)
{
	SceneData *scene = getSceneData();
	scene->lightCount[0] = (float32)gvk.pointLightCount;
	scene->lightCount[1] = gvk.pointLightGlowStrength;
	if(gvk.pointLightCount == 0 && !gvk.modernWater)
		return;
	const float32 *m = gvk.worldToPlay;
	const uint32 count = gvk.pointLightCount;
	const float32 worldScale = gvk.worldScale > 0.0001f ? gvk.worldScale : 1.0f;
	for(uint32 i = 0; i < count; i++){
		const PointLight &light = gvk.pointLights[i];
		const float32 x = light.position[0];
		const float32 y = light.position[1];
		const float32 z = light.position[2];
		scene->lightPosRad[i][0] = m[0]*x + m[4]*y + m[8]*z + m[12];
		scene->lightPosRad[i][1] = m[1]*x + m[5]*y + m[9]*z + m[13];
		scene->lightPosRad[i][2] = m[2]*x + m[6]*y + m[10]*z + m[14];
		scene->lightPosRad[i][3] = light.radius/(gvk.worldScale > 0.0001f ? gvk.worldScale : 1.0f);
		scene->lightColour[i][0] = light.colour[0];
		scene->lightColour[i][1] = light.colour[1];
		scene->lightColour[i][2] = light.colour[2];
		scene->lightColour[i][3] = 0.0f;
		const float32 dx = light.direction[0];
		const float32 dy = light.direction[1];
		const float32 dz = light.direction[2];
		scene->lightDir[i][0] = worldScale*(m[0]*dx + m[4]*dy + m[8]*dz);
		scene->lightDir[i][1] = worldScale*(m[1]*dx + m[5]*dy + m[9]*dz);
		scene->lightDir[i][2] = worldScale*(m[2]*dx + m[6]*dy + m[10]*dz);
		scene->lightDir[i][3] = light.spot ? 1.0f : 0.0f;
	}
	scene->lightCount[0] = (float32)count;
	scene->lightCount[1] = gvk.pointLightGlowStrength;

	// The water glint's sun rides the same fold as everything else.
	const float32 sx = gvk.waterSunDirWorld[0];
	const float32 sy = gvk.waterSunDirWorld[1];
	const float32 sz = gvk.waterSunDirWorld[2];
	scene->waterSun[0] = worldScale*(m[0]*sx + m[4]*sy + m[8]*sz);
	scene->waterSun[1] = worldScale*(m[1]*sx + m[5]*sy + m[9]*sz);
	scene->waterSun[2] = worldScale*(m[2]*sx + m[6]*sy + m[10]*sz);
	uint32 sunPacked =
		(uint32)(gvk.waterSunColour[0] < 0.0f ? 0.0f :
		         gvk.waterSunColour[0] > 1.0f ? 255.0f :
		         gvk.waterSunColour[0]*255.0f) |
		(uint32)(gvk.waterSunColour[1] < 0.0f ? 0.0f :
		         gvk.waterSunColour[1] > 1.0f ? 255.0f :
		         gvk.waterSunColour[1]*255.0f) << 8 |
		(uint32)(gvk.waterSunColour[2] < 0.0f ? 0.0f :
		         gvk.waterSunColour[2] > 1.0f ? 255.0f :
		         gvk.waterSunColour[2]*255.0f) << 16 | 0xFF000000u;
	memcpy(&scene->waterSun[3], &sunPacked, sizeof(sunPacked));
}

// A reflection probe lives in THIS frame's play space, but the image it is
// looked up in was rendered in LAST frame's -- and the fold between them
// moves with every camera step. Left uncomposed, the reflection swims by one
// frame of camera motion and snaps back, a wobble the headset makes very
// visible. So build the whole round trip once on the CPU: current play ->
// world -> submitted frame's play -> its clip.
void
refreshSceneReprojection(void)
{
	if(!sceneReflectionsActive() && !gvk.modernWater)
		return;
	SceneData *scene = getSceneData();

	// The fold has a uniform scale. Its inverse is the transposed basis
	// multiplied by worldScale squared: m already includes 1/worldScale.
	const float32 *m = gvk.worldToPlay;
	const float32 worldScale = gvk.worldScale > 0.0001f ? gvk.worldScale : 1.0f;
	const float32 inverseBasisScale = worldScale*worldScale;
	float32 playToWorld[16];
	playToWorld[0] = m[0]*inverseBasisScale; playToWorld[4] = m[1]*inverseBasisScale; playToWorld[8]  = m[2]*inverseBasisScale;
	playToWorld[1] = m[4]*inverseBasisScale; playToWorld[5] = m[5]*inverseBasisScale; playToWorld[9]  = m[6]*inverseBasisScale;
	playToWorld[2] = m[8]*inverseBasisScale; playToWorld[6] = m[9]*inverseBasisScale; playToWorld[10] = m[10]*inverseBasisScale;
	playToWorld[3] = 0.0f; playToWorld[7] = 0.0f; playToWorld[11] = 0.0f;
	playToWorld[12] = -inverseBasisScale*(m[0]*m[12] + m[1]*m[13] + m[2]*m[14]);
	playToWorld[13] = -inverseBasisScale*(m[4]*m[12] + m[5]*m[13] + m[6]*m[14]);
	playToWorld[14] = -inverseBasisScale*(m[8]*m[12] + m[9]*m[13] + m[10]*m[14]);
	playToWorld[15] = 1.0f;
	// The water ripple field reads it to anchor waves in the world.
	memcpy(scene->playToWorld, playToWorld, sizeof(playToWorld));

	// Before the first frame is submitted there is no captured fold; using
	// the current one degrades to a plain previous-view projection.
	const float32 *submitted = gvk.worldToPlaySubmitted[15] != 0.0f ?
		gvk.worldToPlaySubmitted : gvk.worldToPlay;
	float32 foldDelta[16];
	multiplyMatrix(foldDelta, submitted, playToWorld);

	for(int32 eye = 0; eye < 2; eye++){
		float32 composed[16];
		multiplyMatrix(composed,
		               gvk.reflectionPrevValid ?
		               gvk.reflectionPrevViewProj[eye] :
		               gvk.stereoViewProjectionUnjittered[eye],
		               foldDelta);
		memcpy(scene->reflectionReproject[eye], composed,
		       sizeof(composed));
	}
}

void
setIm2DTransform(const float32 transform[16], float32 planeDistance,
                 const float32 eye[3])
{
	SceneData *scene = getSceneData();
	memcpy(scene->im2dTransform, transform, sizeof(float32)*16);
	memcpy(gvk.im2dTransformActive, transform, sizeof(gvk.im2dTransformActive));
	if(!gvk.im2dSpriteOverride)
		memcpy(scene->im2dSpriteTransform, transform, sizeof(float32)*16);
	scene->im2dParams[0] = planeDistance;
	scene->im2dParams[1] = eye[0];
	scene->im2dParams[2] = eye[1];
	scene->im2dParams[3] = eye[2];
}

void
setIm2DSpriteTransform(const float32 transform[16])
{
	// Wrist panels swap the HUD plane mid-frame and restore it through
	// setIm2DTransform; the override keeps that from dragging the sprite
	// plane along with it.
	gvk.im2dSpriteOverride = 1;
	memcpy(getSceneData()->im2dSpriteTransform, transform,
	       sizeof(float32)*16);
}

void
setHeadPose(const float32 position[3], float32 yaw, const float32 quat[4])
{
	if(gvk.worldScale <= 0.0001f)
		gvk.worldScale = 1.0f;
	if(!gvk.trackingComfortValid){
		gvk.headTranslation = 1.0f;
		gvk.heightOffsetMetres = 0.0f;
		gvk.seatedTracking = 0;
	}
	float32 calibrated[3] = { position[0], position[1], position[2] };
	if(!gvk.seatedTracking && !gvk.standingHeadHeightValid){
		gvk.standingHeadHeight = position[1];
		gvk.standingHeadHeightValid = 1;
	}
	float32 heightShift = gvk.heightOffsetMetres;
	if(gvk.seatedTracking && gvk.standingHeadHeightValid)
		heightShift += gvk.standingHeadHeight-position[1];
	calibrated[1] += heightShift;
	for(int axis = 0; axis < 3; axis++)
		gvk.headPoseOffset[axis] = calibrated[axis]-position[axis];
	if(gvk.firstPersonActive){
		for(int axis = 0; axis < 3; axis++){
			const float32 scaled = gvk.fpLatchedHeadPos[axis] +
				(calibrated[axis]-gvk.fpLatchedHeadPos[axis])*gvk.headTranslation;
			gvk.headPosition[axis] = scaled;
			gvk.headPoseCorrection[axis] = scaled-calibrated[axis];
		}
	}else{
		memcpy(gvk.headPosition, calibrated, sizeof(gvk.headPosition));
		memset(gvk.headPoseCorrection, 0, sizeof(gvk.headPoseCorrection));
	}
	gvk.headYaw = yaw;
	memcpy(gvk.headQuat, quat, sizeof(gvk.headQuat));

	if(!gvk.firstPersonActive){
		// Outside first person the latch simply follows the head, so first
		// person always starts from a current neutral pose no matter how the
		// anchor updates and the pose updates interleave.
		memcpy(gvk.fpLatchedHeadPos, gvk.headPosition,
		       sizeof(gvk.fpLatchedHeadPos));
		return;
	}
	// A system recenter (long Meta press), a Guardian rebuild or tracking
	// loss moves the reference space under the game; the neutral pose would
	// then stay wrong for the rest of the session. Room-scale movement never
	// covers three metres between two frames, so re-anchor on such a jump
	// instead of dragging the world along with it.
	const float32 dx = gvk.headPosition[0] - gvk.fpLatchedHeadPos[0];
	const float32 dy = gvk.headPosition[1] - gvk.fpLatchedHeadPos[1];
	const float32 dz = gvk.headPosition[2] - gvk.fpLatchedHeadPos[2];
	if(dx*dx + dy*dy + dz*dz > 9.0f)
		memcpy(gvk.fpLatchedHeadPos, gvk.headPosition,
		       sizeof(gvk.fpLatchedHeadPos));
}

void
setWorldScale(float32 unitsPerMetre)
{
	if(isfinite(unitsPerMetre) && unitsPerMetre >= 0.25f && unitsPerMetre <= 4.0f)
		gvk.worldScale = unitsPerMetre;
}

void
setTrackingComfort(float32 headTranslation, float32 heightOffsetMetres,
                   bool32 seated)
{
	if(isfinite(headTranslation))
		gvk.headTranslation = fmaxf(0.0f, fminf(headTranslation, 2.0f));
	if(isfinite(heightOffsetMetres))
		gvk.heightOffsetMetres = fmaxf(-1.0f, fminf(heightOffsetMetres, 1.0f));
	gvk.seatedTracking = seated != 0;
	gvk.trackingComfortValid = 1;
}

bool32
getFirstPersonViewFrame(float32 rwRight[3], float32 rwUp[3],
                        float32 rwAt[3], float32 position[3])
{
	if(!gvk.firstPersonActive)
		return 0;

	// Head axes in play space from the quaternion.
	const float32 x = gvk.headQuat[0], y = gvk.headQuat[1];
	const float32 z = gvk.headQuat[2], w = gvk.headQuat[3];
	const float32 hr[3] = { 1-2*(y*y+z*z), 2*(x*y+z*w),   2*(x*z-y*w) };
	const float32 hu[3] = { 2*(x*y-z*w),   1-2*(x*x+z*z), 2*(y*z+x*w) };
	const float32 hf[3] = { -2*(x*z+y*w),  -2*(y*z-x*w),  -(1-2*(x*x+y*y)) };

	// Play axes in world space, the same basis updateWorldToPlay builds.
	float32 px[3], py[3], pz[3];
	getFirstPersonPlayBasis(px, py, pz);

	// world = px*play.x + py*play.y + pz*play.z for each head axis.
	for(int i = 0; i < 3; i++){
		// RenderWare camera convention: the right column points LEFT.
		rwRight[i] = -(px[i]*hr[0] + py[i]*hr[1] + pz[i]*hr[2]);
		rwUp[i]    =   px[i]*hu[0] + py[i]*hu[1] + pz[i]*hu[2];
		rwAt[i]    =   px[i]*hf[0] + py[i]*hf[1] + pz[i]*hf[2];
	}
	// Where the eye actually is: the game anchor displaced by the physical
	// offset from the latched head. Callers aim, trace and place tracked
	// geometry from here, so it has to agree with the rendered viewpoint.
	const float32 relative[3] = {
		gvk.headPosition[0] - gvk.fpLatchedHeadPos[0],
		gvk.headPosition[1] - gvk.fpLatchedHeadPos[1],
		gvk.headPosition[2] - gvk.fpLatchedHeadPos[2] };
	for(int i = 0; i < 3; i++)
		position[i] = gvk.fpHeadWorld[i] + gvk.worldScale*
			(px[i]*relative[0] + py[i]*relative[1] + pz[i]*relative[2]);
	return 1;
}

void
setFirstPersonEyePoses(const float32 positions[2][3],
                       const float32 orientations[2][4])
{
	if(positions == nil || orientations == nil){
		gvk.eyePosesValid = 0;
		return;
	}
	for(int eye = 0; eye < 2; eye++)
		for(int axis = 0; axis < 3; axis++)
			gvk.eyePosition[eye][axis] = positions[eye][axis] +
				gvk.headPoseCorrection[axis];
	memcpy(gvk.eyeQuat, orientations, sizeof(gvk.eyeQuat));
	gvk.eyePosesValid = 1;
}

void
clearFirstPersonEyePoses(void)
{
	gvk.eyePosesValid = 0;
}

bool32
getFirstPersonEyeViewFrame(uint32 eye, float32 rwRight[3],
                           float32 rwUp[3], float32 rwAt[3],
                           float32 position[3])
{
	if(!gvk.firstPersonActive || !gvk.eyePosesValid || eye >= 2 ||
	   rwRight == nil || rwUp == nil || rwAt == nil || position == nil)
		return 0;
	float32 worldRight[3];
	if(!playPoseToFirstPersonWorld(gvk.eyePosition[eye], gvk.eyeQuat[eye],
	     worldRight, rwUp, rwAt, position))
		return 0;
	// RenderWare camera convention points the matrix Right column leftward.
	for(int32 axis = 0; axis < 3; axis++)
		rwRight[axis] = -worldRight[axis];
	return 1;
}

bool32
playPoseToFirstPersonWorld(const float32 playPosition[3],
                           const float32 playQuaternion[4],
                           float32 worldRight[3], float32 worldUp[3],
                           float32 worldForward[3], float32 worldPosition[3])
{
	if(!gvk.firstPersonActive)
		return 0;

	const float32 x = playQuaternion[0], y = playQuaternion[1];
	const float32 z = playQuaternion[2], w = playQuaternion[3];
	const float32 pr[3] = {
		1-2*(y*y+z*z), 2*(x*y+z*w), 2*(x*z-y*w) };
	const float32 pu[3] = {
		2*(x*y-z*w), 1-2*(x*x+z*z), 2*(y*z+x*w) };
	const float32 pf[3] = {
		-2*(x*z+y*w), -2*(y*z-x*w), -(1-2*(x*x+y*y)) };

	float32 px[3], py[3], pz[3];
	getFirstPersonPlayBasis(px, py, pz);
	// Relative to the LATCHED head, the origin updateWorldToPlay maps the
	// game anchor onto. Measuring from the live head instead would drag eyes,
	// hands and weapons along with every lean, leaving them where the
	// rendered world no longer is.
	const float32 relative[3] = {
		playPosition[0] + gvk.headPoseOffset[0] - gvk.fpLatchedHeadPos[0],
		playPosition[1] + gvk.headPoseOffset[1] - gvk.fpLatchedHeadPos[1],
		playPosition[2] + gvk.headPoseOffset[2] - gvk.fpLatchedHeadPos[2] };
	const float32 worldScale = gvk.worldScale > 0.0001f ? gvk.worldScale : 1.0f;

	for(int i = 0; i < 3; i++){
		worldRight[i] =
			px[i]*pr[0] + py[i]*pr[1] + pz[i]*pr[2];
		worldUp[i] =
			px[i]*pu[0] + py[i]*pu[1] + pz[i]*pu[2];
		worldForward[i] =
			px[i]*pf[0] + py[i]*pf[1] + pz[i]*pf[2];
		worldPosition[i] = gvk.fpHeadWorld[i] + worldScale*
			(px[i]*relative[0] + py[i]*relative[1] + pz[i]*relative[2]);
	}
	return 1;
}

bool32
firstPersonWorldVectorToPlay(const float32 worldVector[3],
                             float32 playVector[3])
{
	if(!gvk.firstPersonActive || !worldVector || !playVector)
		return 0;
	float32 px[3], py[3], pz[3];
	getFirstPersonPlayBasis(px, py, pz);
	playVector[0] = worldVector[0]*px[0]+worldVector[1]*px[1]+worldVector[2]*px[2];
	playVector[1] = worldVector[0]*py[0]+worldVector[1]*py[1]+worldVector[2]*py[2];
	playVector[2] = worldVector[0]*pz[0]+worldVector[1]*pz[1]+worldVector[2]*pz[2];
	return 1;
}

bool32
firstPersonWorldPositionToPlay(const float32 worldPosition[3],
                               float32 playPosition[3])
{
	if(!gvk.firstPersonActive || !worldPosition || !playPosition)
		return 0;
	float32 px[3], py[3], pz[3];
	getFirstPersonPlayBasis(px, py, pz);
	// Undo playPoseToFirstPersonWorld: it measures from the LATCHED head in
	// play space and lands on the anchor in the world, so the return trip
	// starts at the anchor and ends back on that same latched origin.
	const float32 relative[3] = {
		worldPosition[0] - gvk.fpHeadWorld[0],
		worldPosition[1] - gvk.fpHeadWorld[1],
		worldPosition[2] - gvk.fpHeadWorld[2] };
	const float32 inverseScale = 1.0f/(gvk.worldScale > 0.0001f ? gvk.worldScale : 1.0f);
	playPosition[0] = gvk.fpLatchedHeadPos[0] - gvk.headPoseOffset[0] + inverseScale*
		(relative[0]*px[0]+relative[1]*px[1]+relative[2]*px[2]);
	playPosition[1] = gvk.fpLatchedHeadPos[1] - gvk.headPoseOffset[1] + inverseScale*
		(relative[0]*py[0]+relative[1]*py[1]+relative[2]*py[2]);
	playPosition[2] = gvk.fpLatchedHeadPos[2] - gvk.headPoseOffset[2] + inverseScale*
		(relative[0]*pz[0]+relative[1]*pz[1]+relative[2]*pz[2]);
	return 1;
}

static bool32
sphereIntersectsClipSidePlanes(const float32 planes[5][4],
                               const float32 centre[3], float32 radius)
{
	for(int32 i = 0; i < 5; i++){
		const float32 *plane = planes[i];
		const float32 signedValue =
			plane[0]*centre[0] + plane[1]*centre[1] +
			plane[2]*centre[2] + plane[3];
		if(!isfinite(signedValue))
			return 1;
		if(signedValue < -radius)
			return 0;
	}
	return 1;
}

bool32
isFirstPersonWorldSphereVisibleInStereo(const float32 worldCentre[3],
                                        float32 radius,
                                        float32 angularMarginTangent)
{
	if(!gvk.firstPersonActive || !gvk.stereoCullPlanesValid ||
	   worldCentre == nil)
		return 1;
	if(!isfinite(worldCentre[0]) || !isfinite(worldCentre[1]) ||
	   !isfinite(worldCentre[2]) || !isfinite(radius) ||
	   !isfinite(angularMarginTangent))
		return 1;

	// worldToPlay is the exact rigid transform applied to every world draw in
	// beginUpdate. Testing in that same play space avoids reconstructing eye
	// poses or approximating their asymmetric FOV on the game side.
	const float32 *m = gvk.worldToPlay;
	float32 playCentre[3];
	for(int32 row = 0; row < 3; row++)
		playCentre[row] =
			m[row]*worldCentre[0] + m[4+row]*worldCentre[1] +
			m[8+row]*worldCentre[2] + m[12+row];
	if(!isfinite(playCentre[0]) || !isfinite(playCentre[1]) ||
	   !isfinite(playCentre[2]))
		return 1;

	const float32 dx = worldCentre[0] - gvk.fpHeadWorld[0];
	const float32 dy = worldCentre[1] - gvk.fpHeadWorld[1];
	const float32 dz = worldCentre[2] - gvk.fpHeadWorld[2];
	const float32 distance = sqrtf(dx*dx + dy*dy + dz*dz);
	if(!isfinite(distance))
		return 1;
	const float32 clampedMarginTangent =
		fmaxf(0.0f, fminf(angularMarginTangent, 0.17632698f));
	const float32 inverseScale = 1.0f/(gvk.worldScale > 0.0001f ? gvk.worldScale : 1.0f);
	const float32 safeRadius = fmaxf(0.0f, radius)*inverseScale + 0.25f +
		distance*inverseScale*clampedMarginTangent;

	return sphereIntersectsClipSidePlanes(
		gvk.stereoCullPlanes[0], playCentre, safeRadius) ||
		sphereIntersectsClipSidePlanes(
			gvk.stereoCullPlanes[1], playCentre, safeRadius);
}

bool32
isFirstPersonWorldBoxVisibleInStereo(const float32 worldCorners[8][3],
                                     float32 angularMarginTangent)
{
	if(!gvk.firstPersonActive || !gvk.stereoCullPlanesValid ||
	   worldCorners == nil || !isfinite(angularMarginTangent))
		return 1;
	const float32 clampedMarginTangent =
		fmaxf(0.0f, fminf(angularMarginTangent, 0.17632698f));
	float32 playCorners[8][3];
	float32 farthestDistance = 0.0f;
	const float32 *m = gvk.worldToPlay;
	for(int32 corner = 0; corner < 8; corner++){
		const float32 *world = worldCorners[corner];
		if(!isfinite(world[0]) || !isfinite(world[1]) ||
		   !isfinite(world[2]))
			return 1;
		for(int32 row = 0; row < 3; row++)
			playCorners[corner][row] =
				m[row]*world[0] + m[4+row]*world[1] +
				m[8+row]*world[2] + m[12+row];
		const float32 dx = world[0]-gvk.fpHeadWorld[0];
		const float32 dy = world[1]-gvk.fpHeadWorld[1];
		const float32 dz = world[2]-gvk.fpHeadWorld[2];
		const float32 distance = sqrtf(dx*dx+dy*dy+dz*dz);
		if(!isfinite(distance))
			return 1;
		farthestDistance = fmaxf(farthestDistance, distance);
	}
	const float32 inverseScale = 1.0f/(gvk.worldScale > 0.0001f ? gvk.worldScale : 1.0f);
	const float32 safeMargin = 0.25f+
		farthestDistance*inverseScale*clampedMarginTangent;
	for(int32 eye = 0; eye < 2; eye++){
		bool32 eyeVisible = 1;
		for(int32 planeIndex = 0; planeIndex < 5; planeIndex++){
			const float32 *plane =
				gvk.stereoCullPlanes[eye][planeIndex];
			bool32 allOutside = 1;
			for(int32 corner = 0; corner < 8; corner++){
				const float32 signedValue =
					plane[0]*playCorners[corner][0] +
					plane[1]*playCorners[corner][1] +
					plane[2]*playCorners[corner][2] + plane[3];
				if(!isfinite(signedValue))
					return 1;
				if(signedValue >= -safeMargin){
					allOutside = 0;
					break;
				}
			}
			if(allOutside){
				eyeVisible = 0;
				break;
			}
		}
		if(eyeVisible)
			return 1;
	}
	return 0;
}

void
setFirstPersonAnchor(const float32 headWorld[3], float32 headingYaw,
                     bool32 followHeading, bool32 active)
{
	if(gvk.firstPersonActive != active)
	{
		gvk.temporalHistoryValid = 0;
		resetAtomicMotionHistory();
	}
	if(active && (!gvk.firstPersonActive ||
	              gvk.fpFollowHeading != followHeading ||
	              gvk.fpUseFullBasis)){
		// (Re)latch so the player's current physical facing looks along the
		// heading: view yaw = anchorYaw + headYaw. Re-latching on a mode
		// change makes entering a vehicle start looking down its nose.
		gvk.fpLatchedHeadYaw = gvk.headYaw;
		gvk.fpAnchorYaw = headingYaw - gvk.headYaw;
		// The position latches with the yaw: standing where you stand
		// becomes the neutral pose the world is built around.
		memcpy(gvk.fpLatchedHeadPos, gvk.headPosition,
		       sizeof(gvk.fpLatchedHeadPos));
	}
	gvk.firstPersonActive = active;
	gvk.fpFollowHeading = followHeading;
	gvk.fpUseFullBasis = 0;
	if(active){
		memcpy(gvk.fpHeadWorld, headWorld, sizeof(gvk.fpHeadWorld));
		// Cockpit: the anchor turns with whatever is being ridden, keeping
		// the physical-facing offset taken at entry.
		if(followHeading)
			gvk.fpAnchorYaw = headingYaw - gvk.fpLatchedHeadYaw;
	}
}

void
setFirstPersonAnchorBasis(const float32 headWorld[3],
                          const float32 right[3],
                          const float32 up[3],
                          const float32 forward[3],
                          float32 headingYaw, bool32 active)
{
	if(gvk.firstPersonActive != active)
	{
		gvk.temporalHistoryValid = 0;
		resetAtomicMotionHistory();
	}
	if(active && (!gvk.firstPersonActive ||
	              !gvk.fpFollowHeading ||
	              !gvk.fpUseFullBasis)){
		gvk.fpLatchedHeadYaw = gvk.headYaw;
		memcpy(gvk.fpLatchedHeadPos, gvk.headPosition,
		       sizeof(gvk.fpLatchedHeadPos));
	}
	gvk.firstPersonActive = active;
	gvk.fpFollowHeading = active;
	gvk.fpUseFullBasis = active;
	if(!active)
		return;

	memcpy(gvk.fpHeadWorld, headWorld, sizeof(gvk.fpHeadWorld));
	gvk.fpAnchorYaw = headingYaw-gvk.fpLatchedHeadYaw;

	// Rotate the vehicle frame by the inverse of the physical yaw that was
	// present when cockpit mode activated. This is the full-basis equivalent
	// of fpAnchorYaw = headingYaw - fpLatchedHeadYaw.
	const float32 c = cosf(gvk.fpLatchedHeadYaw);
	const float32 s = sinf(gvk.fpLatchedHeadYaw);
	for(int32 i = 0; i < 3; i++){
		gvk.fpPlayX[i] = right[i]*c-forward[i]*s;
		gvk.fpPlayY[i] = up[i];
		gvk.fpPlayZ[i] = -(forward[i]*c+right[i]*s);
	}
}

bool32
getFirstPersonViewYaw(float32 *yawOut)
{
	if(!gvk.firstPersonActive)
		return 0;
	*yawOut = gvk.fpAnchorYaw + gvk.headYaw;
	return 1;
}

bool32
getFirstPersonLocalHeadYaw(float32 *yawOut)
{
	if(!gvk.firstPersonActive || yawOut == nil)
		return 0;
	float32 yaw = gvk.headYaw-gvk.fpLatchedHeadYaw;
	const float32 pi = 3.14159265358979323846f;
	const float32 twoPi = 2.0f*pi;
	while(yaw >= pi) yaw -= twoPi;
	while(yaw < -pi) yaw += twoPi;
	*yawOut = yaw;
	return 1;
}

void
setFogParams(float32 start, float32 end)
{
	gvk.fogStart = start;
	gvk.fogEnd = end;
}

void
setClearColour(uint8 red, uint8 green, uint8 blue)
{
	gvk.clearColour[0] = red / 255.0f;
	gvk.clearColour[1] = green / 255.0f;
	gvk.clearColour[2] = blue / 255.0f;
}

void
setPostFx(uint32 mode, uint32 red, uint32 green, uint32 blue,
          float32 intensity)
{
	if(red > 255) red = 255;
	if(green > 255) green = 255;
	if(blue > 255) blue = 255;
	if(intensity < 0.0f) intensity = 0.0f;

	const float32 scale = intensity / 255.0f;
	gvk.postFxConstants.blurColour[0] = red * scale;
	gvk.postFxConstants.blurColour[1] = green * scale;
	gvk.postFxConstants.blurColour[2] = blue * scale;
	// POSTFX_NORMAL uses the same fixed blend alpha on the PC renderer.
	gvk.postFxConstants.blurColour[3] = 30.0f / 255.0f;
	// 0 = pass-through, 1 = Vice City colour treatment, 2 = an opaque
	// transition mask used while the cutscene world/camera is being rebuilt.
	gvk.postFxConstants.mode[0] = mode <= 2 ? mode : 0u;
	gvk.postFxConstants.mode[2] = gvk.sceneWidth;
	gvk.postFxConstants.mode[3] = gvk.sceneHeight;
	gvk.postFxConstants.upscale[0] = gvk.width;
	gvk.postFxConstants.upscale[1] = gvk.height;
	gvk.postFxConstants.upscale[2] = gvk.sgsrMode;
	gvk.postFxConstants.upscale[3] =
		(uint32)(gvk.renderScaleEffectivePercent+0.5f);
}

void
setFxaaEnabled(bool32 enabled)
{
	gvk.postFxConstants.mode[1] = enabled ? 1u : 0u;
}

static bool32 gPs2AlphaTestEnabled = 1;

void
setPs2AlphaTestEnabled(bool32 enabled)
{
	gPs2AlphaTestEnabled = enabled;
}

bool32
getPs2AlphaTestEnabled(void)
{
	return gPs2AlphaTestEnabled;
}

static bool32 gRenderDiagnostics;
static uint32 gMaskedMipBias;

void
setMaskedMipBias(uint32 halfLevels)
{
	gMaskedMipBias = halfLevels > 3 ? 3 : halfLevels;
}

uint32
getMaskedMipBias(void)
{
	return gMaskedMipBias;
}

void
setRenderDiagnostics(bool32 enabled)
{
	gRenderDiagnostics = enabled;
}

bool32
renderDiagnosticsEnabled(void)
{
	return gRenderDiagnostics;
}

void
setSpatialAaMode(uint32 mode)
{
	const uint32 accepted = mode ? 1u : 0u;
	if(gvk.postFxConstants.mode[1] != accepted)
		VKLOG("spatial AA mode %u -> %u", gvk.postFxConstants.mode[1],
		      accepted);
	gvk.postFxConstants.mode[1] = accepted;
}

bool32
getSgsrStatus(uint32 *mode, uint32 *sceneWidth, uint32 *sceneHeight,
              uint32 *outputWidth, uint32 *outputHeight)
{
	if(!gvk.initialised)
		return 0;
	if(mode) *mode = gvk.sgsrMode;
	if(sceneWidth) *sceneWidth = gvk.sceneWidth;
	if(sceneHeight) *sceneHeight = gvk.sceneHeight;
	if(outputWidth) *outputWidth = gvk.width;
	if(outputHeight) *outputHeight = gvk.height;
	return 1;
}

uint32
getSceneSampleCount(void)
{
	return gvk.sceneSamples == VK_SAMPLE_COUNT_4_BIT ? 4u :
	       gvk.sceneSamples == VK_SAMPLE_COUNT_2_BIT ? 2u : 1u;
}

bool32
isInitialized(void) { return gvk.initialised; }

const char *
getAdapterName(void)
{
	return gvk.initialised ? gvk.deviceProperties.deviceName : "";
}

bool32
waitForGpu(void)
{
	if(!gvk.initialised)
		return 0;
	return vkDeviceWaitIdle(gvk.device) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Device interface
// ---------------------------------------------------------------------------

static void
beginUpdate(Camera *cam)
{
	// The per-eye projection comes from OpenXR, but where the viewer stands in
	// Vice City comes from the game camera, exactly as it does on the desktop
	// build -- there the VR layer writes the eye pose into the RwCamera's frame
	// and librw derives the view matrix from it. Multiview needs both eyes at
	// once, so the composition happens here instead.
	updateWorldToPlay(cam);
	// The dynamic lights follow the fold. Mid-recording the rewrite is safe --
	// the scene buffer is host visible and the GPU reads it at submit, so the
	// last camera of the frame wins, exactly as it does for the fold itself.
	// Outside a frame the GPU may be reading the mapped block right now, and
	// there is nothing to light anyway.
	if(gvk.inFrame && worldEffectsActive()){
		refreshScenePointLights();
		refreshSceneReprojection();
		uploadSceneData();
	}
}

static void
endUpdate(Camera *cam)
{
	(void)cam;
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	(void)cam;
	(void)col;
	(void)mode;
	// The render pass clears both attachments on load, so an explicit clear is
	// only needed for mid-pass clears, which Vice City does not issue.
}

static void
showRaster(Raster *raster, uint32 flags)
{
	// Presentation belongs to OpenXR: the composited image is handed back
	// through xrReleaseSwapchainImage, not presented by the renderer.
	(void)raster;
	(void)flags;
}

static bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	(void)raster;
	(void)x;
	(void)y;
	return 0;
}

static void
setRenderState(int32 state, void *value)
{
	// TEXTURERASTER carries a pointer, not a packed value, so it is handled
	// before the truncation below.
	if(state == TEXTURERASTER){
		gstate.texture = (Raster*)value;
		return;
	}

	const uint32 v = (uint32)(uintptr)value;
	switch(state){
	case VERTEXALPHA:      gstate.vertexAlphaEnabled = v; break;
	case SRCBLEND:         gstate.srcBlend = v; break;
	case DESTBLEND:        gstate.dstBlend = v; break;
	case ZTESTENABLE:      gstate.zTestEnabled = v; break;
	case ZWRITEENABLE:     gstate.zWriteEnabled = v; break;
	case CULLMODE:         gstate.cullMode = v; break;
	case ALPHATESTFUNC:    gstate.alphaTestFunction = v; break;
	case ALPHATESTREF:     gstate.alphaTestRef = v; break;
	case GSALPHATEST:      gstate.gsAlphaTest = v; break;
	case GSALPHATESTREF:   gstate.gsAlphaTestRef = v; break;
	case FOGENABLE:        gstate.fogEnabled = v; break;
	case FOGCOLOR: {
		RGBA c;
		memcpy(&c, &value, sizeof(c));
		gstate.fogColor = c;
		break;
	}
	case TEXTUREADDRESS:   gstate.textureAddressU = gstate.textureAddressV = v; break;
	case TEXTUREADDRESSU:  gstate.textureAddressU = v; break;
	case TEXTUREADDRESSV:  gstate.textureAddressV = v; break;
	case TEXTUREFILTER:    gstate.textureFilter = v; break;
	default: break;
	}
}

static void *
getRenderState(int32 state)
{
	if(state == TEXTURERASTER)
		return gstate.texture;

	uint32 v = 0;
	switch(state){
	case VERTEXALPHA:      v = gstate.vertexAlphaEnabled; break;
	case SRCBLEND:         v = gstate.srcBlend; break;
	case DESTBLEND:        v = gstate.dstBlend; break;
	case ZTESTENABLE:      v = gstate.zTestEnabled; break;
	case ZWRITEENABLE:     v = gstate.zWriteEnabled; break;
	case CULLMODE:         v = gstate.cullMode; break;
	case ALPHATESTFUNC:    v = gstate.alphaTestFunction; break;
	case ALPHATESTREF:     v = gstate.alphaTestRef; break;
	case GSALPHATEST:      v = gstate.gsAlphaTest; break;
	case GSALPHATESTREF:   v = gstate.gsAlphaTestRef; break;
	case FOGENABLE:        v = gstate.fogEnabled; break;
	case FOGCOLOR:
		memcpy(&v, &gstate.fogColor, sizeof(v));
		break;
	case TEXTUREADDRESS:
		v = gstate.textureAddressU == gstate.textureAddressV ?
		    gstate.textureAddressU : 0;
		break;
	case TEXTUREADDRESSU:  v = gstate.textureAddressU; break;
	case TEXTUREADDRESSV:  v = gstate.textureAddressV; break;
	case TEXTUREFILTER:    v = gstate.textureFilter; break;
	default: break;
	}
	return (void*)(uintptr)v;
}

static void
resetRenderState(void)
{
	memset(&gstate, 0, sizeof(gstate));
	gstate.zTestEnabled = 1;
	gstate.zWriteEnabled = 1;
	gstate.srcBlend = BLENDSRCALPHA;
	gstate.dstBlend = BLENDINVSRCALPHA;
	gstate.cullMode = CULLBACK;
	gstate.alphaTestFunction = ALPHAGREATEREQUAL;
	gstate.alphaTestRef = 128;
	gstate.gsAlphaTest = 0;
	gstate.gsAlphaTestRef = 128;
	gstate.textureFilter = Texture::LINEAR;
	gstate.textureAddressU = Texture::WRAP;
	gstate.textureAddressV = Texture::WRAP;
}

static int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	switch(req){
	case DEVICEOPEN: {
		gLastDeviceOpenRenderTargetFailure = 0;
		EngineOpenParams *params = (EngineOpenParams*)arg;
		if(params == nil || params->device == VK_NULL_HANDLE){
			VKERR("DEVICEOPEN without an OpenXR-created Vulkan device");
			return 0;
		}
		gvk.instance = params->instance;
		gvk.physicalDevice = params->physicalDevice;
		gvk.device = params->device;
		gvk.fragmentStoresAndAtomicsEnabled =
			params->fragmentStoresAndAtomicsEnabled;
		gvk.queue = params->queue;
		gvk.queueFamilyIndex = params->queueFamilyIndex;
		gvk.width = params->width;
		gvk.height = params->height;
		gvk.renderScaleEffectivePercent =
			params->renderScaleEffectivePercent >= 50.0f &&
			params->renderScaleEffectivePercent <= 400.0f ?
			params->renderScaleEffectivePercent : 100.0f;
		gvk.sceneWidth = params->sceneWidth ? params->sceneWidth : gvk.width;
		gvk.sceneHeight = params->sceneHeight ? params->sceneHeight : gvk.height;
		gvk.sceneWidth = gvk.sceneWidth > gvk.width ? gvk.width : gvk.sceneWidth;
		gvk.sceneHeight = gvk.sceneHeight > gvk.height ? gvk.height : gvk.sceneHeight;
		gvk.sgsrMode = params->sgsrMode < SGSR_MODE_COUNT ?
			params->sgsrMode : SGSR_OFF;
		// Valid even before the first gameplay SetPostFx call. Startup/logo
		// frames can reach the resolve path before timecycle colour is updated.
		gvk.postFxConstants.mode[2] = gvk.sceneWidth;
		gvk.postFxConstants.mode[3] = gvk.sceneHeight;
		gvk.postFxConstants.upscale[0] = gvk.width;
		gvk.postFxConstants.upscale[1] = gvk.height;
		gvk.postFxConstants.upscale[2] = gvk.sgsrMode;
		gvk.postFxConstants.upscale[3] =
			(uint32)(gvk.renderScaleEffectivePercent+0.5f);
		gvk.viewCount = params->viewCount ? params->viewCount : 1;
		gvk.colourFormat = params->colourFormat;

		vkGetPhysicalDeviceProperties(gvk.physicalDevice, &gvk.deviceProperties);
		vkGetPhysicalDeviceMemoryProperties(gvk.physicalDevice, &gvk.memoryProperties);
		vkGetPhysicalDeviceFeatures(gvk.physicalDevice, &gvk.deviceFeatures);
		gvk.supportsBC = gvk.deviceFeatures.textureCompressionBC ? 1 : 0;
		const VkSampleCountFlags framebufferSamples =
			gvk.deviceProperties.limits.framebufferColorSampleCounts &
			gvk.deviceProperties.limits.framebufferDepthSampleCounts;
		gvk.sceneSamples = VK_SAMPLE_COUNT_1_BIT;
		if(gvk.sgsrMode == SGSR_OFF){
			if(params->sceneSampleCount >= 4 &&
			   (framebufferSamples & VK_SAMPLE_COUNT_4_BIT))
				gvk.sceneSamples = VK_SAMPLE_COUNT_4_BIT;
			else if(params->sceneSampleCount >= 2 &&
			        (framebufferSamples & VK_SAMPLE_COUNT_2_BIT))
				gvk.sceneSamples = VK_SAMPLE_COUNT_2_BIT;
		}
		// Camera motion is reconstructed directly from depth at full precision.
		// Dynamic object roots occupy a small NDC range, so x8 encoded RG8 SNORM
		// halves attachment memory and bandwidth. Fall back on unusual hardware.
		gvk.motionFormat = VK_FORMAT_R8G8_SNORM;
		VkFormatProperties motionProperties = {};
		vkGetPhysicalDeviceFormatProperties(gvk.physicalDevice,
			gvk.motionFormat, &motionProperties);
		const VkFormatFeatureFlags motionRequired =
			VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
			VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
			VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		if((motionProperties.optimalTilingFeatures & motionRequired) !=
		   motionRequired)
			gvk.motionFormat = VK_FORMAT_R16G16_SFLOAT;

		VKLOG("adopting device %s, output=%ux%u scene=%ux%u x%u views, "
		      "SGSR2-foundation=%u MSAA=%ux motion-format=%d BC=%d", gvk.deviceProperties.deviceName,
		      gvk.width, gvk.height, gvk.sceneWidth, gvk.sceneHeight,
		      gvk.viewCount, gvk.sgsrMode,
		      gvk.sceneSamples == VK_SAMPLE_COUNT_4_BIT ? 4u :
		      gvk.sceneSamples == VK_SAMPLE_COUNT_2_BIT ? 2u : 1u,
		      (int)gvk.motionFormat,
		      (int)gvk.supportsBC);

		VkCommandPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = gvk.queueFamilyIndex;
		if(vkCreateCommandPool(gvk.device, &poolInfo, nil, &gvk.commandPool) != VK_SUCCESS)
			return 0;

		VkCommandBufferAllocateInfo commandInfo = {};
		commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandInfo.commandPool = gvk.commandPool;
		commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandInfo.commandBufferCount = NUM_FRAME_CONTEXTS;
		VkCommandBuffer commandBuffers[NUM_FRAME_CONTEXTS] = {};
		if(vkAllocateCommandBuffers(gvk.device, &commandInfo,
		                            commandBuffers) != VK_SUCCESS)
			return 0;
		for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
			gvk.frames[i].commandBuffer = commandBuffers[i];

		VkFenceCreateInfo fenceInfo = {};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
			if(vkCreateFence(gvk.device, &fenceInfo, nil,
			                 &gvk.frames[i].fence) != VK_SUCCESS)
				return 0;

		// Timestamp queries are optional.  Check the actual queue family rather
		// than assuming that every Vulkan graphics queue exposes timestamp bits.
		uint32 queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(
			gvk.physicalDevice, &queueFamilyCount, nil);
		if(gvk.queueFamilyIndex < queueFamilyCount){
			VkQueueFamilyProperties *queueFamilies =
				new VkQueueFamilyProperties[queueFamilyCount];
			vkGetPhysicalDeviceQueueFamilyProperties(
				gvk.physicalDevice, &queueFamilyCount, queueFamilies);
			gvk.frameTimestampValidBits =
				queueFamilies[gvk.queueFamilyIndex].timestampValidBits;
			delete[] queueFamilies;
		}
		if(gvk.deviceProperties.limits.timestampComputeAndGraphics &&
		   gvk.frameTimestampValidBits != 0){
			VkQueryPoolCreateInfo queryInfo = {};
			queryInfo.sType =
				VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
			queryInfo.queryCount = 2;
			bool32 allQueryPoolsCreated = 1;
			for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
				if(vkCreateQueryPool(gvk.device, &queryInfo, nil,
				                     &gvk.frames[i].timestampQueryPool) !=
				   VK_SUCCESS){
					allQueryPoolsCreated = 0;
					break;
				}
			if(allQueryPoolsCreated)
				VKLOG("GPU frame timestamps enabled (%u valid bits, "
				      "%.3f ns/tick)",
				      gvk.frameTimestampValidBits,
				      gvk.deviceProperties.limits.timestampPeriod);
			else{
				for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
					if(gvk.frames[i].timestampQueryPool)
						vkDestroyQueryPool(
							gvk.device,
							gvk.frames[i].timestampQueryPool, nil);
					gvk.frames[i].timestampQueryPool = VK_NULL_HANDLE;
				}
				VKLOG("GPU frame timestamps unavailable: query pool "
				      "creation failed");
			}
		}else
			VKLOG("GPU frame timestamps unavailable on queue family %u",
			      gvk.queueFamilyIndex);

		gvk.sceneReflectionDivisor = params->sceneReflectionDivisor;
		chooseSceneReflectionSize();
		for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
			if(!createDepthBuffer(gvk.frames[i])){
				gLastDeviceOpenRenderTargetFailure = 1;
				return 0;
			}
			if(!createSceneColour(gvk.frames[i])){
				gLastDeviceOpenRenderTargetFailure = 1;
				return 0;
			}
			if(!createSceneReflection(gvk.frames[i])){
				gLastDeviceOpenRenderTargetFailure = 1;
				return 0;
			}
			if(!createSceneMsaaColour(gvk.frames[i])){
				gLastDeviceOpenRenderTargetFailure = 1;
				return 0;
			}
			if(!createResolvedHistory(gvk.frames[i])){
				gLastDeviceOpenRenderTargetFailure = 1;
				return 0;
			}
			if(!createSgsr2ConvertImage(gvk.frames[i])){
				gLastDeviceOpenRenderTargetFailure = 1;
				return 0;
			}
			if(!createMotionImage(gvk.frames[i])){
				gLastDeviceOpenRenderTargetFailure = 1;
				return 0;
			}
		}
		if(!createRenderPass())
			return 0;
		for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
			if(!createSceneFramebuffer(gvk.frames[i])){
				gLastDeviceOpenRenderTargetFailure = 1;
				return 0;
			}
			if(!createMotionFramebuffer(gvk.frames[i])){
				gLastDeviceOpenRenderTargetFailure = 1;
				return 0;
			}
			if(!createSgsr2ConvertFramebuffer(gvk.frames[i])){
				gLastDeviceOpenRenderTargetFailure = 1;
				return 0;
			}
		}
		if(!createMotionResources()){
			gLastDeviceOpenRenderTargetFailure = 1;
			return 0;
		}
		if(!createSgsr2ConvertResources()){
			gLastDeviceOpenRenderTargetFailure = 1;
			return 0;
		}
		if(!createPostFxResources())
			return 0;

		resetRenderState();
		// The pipeline cache builds against gvk.renderPass, so it can only be
		// created once the render pass exists.
		if(!stateInit())
			return 0;
		gvk.activeFrame = 0;
		gvk.nextFrame = 0;
		gvk.lastSubmittedFrame = 0;
		gvk.hasSubmittedFrame = 0;
		gvk.temporalHistoryValid = 0;
		gvk.motionFrameSerial = 0;
		resetAtomicMotionHistory();
		gvk.frameCommands = gvk.frames[0].commandBuffer;
		setStateFrame(0);
		gvk.initialised = 1;
		VKLOG("%u Vulkan frame contexts enabled", NUM_FRAME_CONTEXTS);
		return 1;
	}

	case DEVICECLOSE:
		stopGeneratedMips();
		if(gvk.device != VK_NULL_HANDLE){
			vkDeviceWaitIdle(gvk.device);
			resetAtomicMotionHistory();
			for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++)
				flushRetired(i);
			stateShutdown();
			for(uint32 frameIndex = 0;
			    frameIndex < NUM_FRAME_CONTEXTS; frameIndex++){
				FrameContext &frame = gvk.frames[frameIndex];
				if(frame.sceneFramebuffer)
					vkDestroyFramebuffer(
						gvk.device, frame.sceneFramebuffer, nil);
				for(uint32 i = 0; i < frame.numFramebuffers; i++)
					if(frame.framebuffers[i])
						vkDestroyFramebuffer(
							gvk.device, frame.framebuffers[i], nil);
				if(frame.motionFramebuffer)
					vkDestroyFramebuffer(
						gvk.device, frame.motionFramebuffer, nil);
				if(frame.sgsrConvertFramebuffer)
					vkDestroyFramebuffer(
						gvk.device, frame.sgsrConvertFramebuffer, nil);
			}
			if(gvk.sgsrConvertPipeline)
				vkDestroyPipeline(gvk.device, gvk.sgsrConvertPipeline, nil);
			if(gvk.sgsrConvertPipelineLayout)
				vkDestroyPipelineLayout(gvk.device,
				                        gvk.sgsrConvertPipelineLayout, nil);
			if(gvk.sgsrConvertDescriptorPool)
				vkDestroyDescriptorPool(gvk.device,
				                        gvk.sgsrConvertDescriptorPool, nil);
			if(gvk.sgsrConvertDescriptorLayout)
				vkDestroyDescriptorSetLayout(gvk.device,
				                              gvk.sgsrConvertDescriptorLayout, nil);
			if(gvk.motionPipeline) vkDestroyPipeline(gvk.device, gvk.motionPipeline, nil);
			if(gvk.motionPipelineLayout) vkDestroyPipelineLayout(gvk.device, gvk.motionPipelineLayout, nil);
			if(gvk.motionDescriptorPool) vkDestroyDescriptorPool(gvk.device, gvk.motionDescriptorPool, nil);
			if(gvk.motionDescriptorLayout) vkDestroyDescriptorSetLayout(gvk.device, gvk.motionDescriptorLayout, nil);
			if(gvk.motionDepthSampler) vkDestroySampler(gvk.device, gvk.motionDepthSampler, nil);
			if(gvk.postFxPipeline) vkDestroyPipeline(gvk.device, gvk.postFxPipeline, nil);
			if(gvk.postFxPipelineLayout) vkDestroyPipelineLayout(gvk.device, gvk.postFxPipelineLayout, nil);
			if(gvk.postFxDescriptorPool) vkDestroyDescriptorPool(gvk.device, gvk.postFxDescriptorPool, nil);
			if(gvk.postFxDescriptorLayout) vkDestroyDescriptorSetLayout(gvk.device, gvk.postFxDescriptorLayout, nil);
			if(gvk.postFxSampler) vkDestroySampler(gvk.device, gvk.postFxSampler, nil);
			for(int32 panel = 0; panel < WRIST_PANEL_COUNT; panel++){
				destroyWristPanelTarget(gWristPanels[panel]);
				if(gWristPanels[panel].raster){
					gWristPanels[panel].raster->destroy();
					gWristPanels[panel].raster = nil;
				}
			}
			if(gvk.postFxRenderPass) vkDestroyRenderPass(gvk.device, gvk.postFxRenderPass, nil);
			if(gvk.motionRenderPass) vkDestroyRenderPass(gvk.device, gvk.motionRenderPass, nil);
			if(gvk.sgsrConvertRenderPass)
				vkDestroyRenderPass(gvk.device, gvk.sgsrConvertRenderPass, nil);
			if(gvk.renderPass) vkDestroyRenderPass(gvk.device, gvk.renderPass, nil);
			for(uint32 i = 0; i < NUM_FRAME_CONTEXTS; i++){
				FrameContext &frame = gvk.frames[i];
				if(frame.sceneColourView)
					vkDestroyImageView(gvk.device,
					                   frame.sceneColourView, nil);
				if(frame.sceneColourImage)
					vkDestroyImage(gvk.device,
					               frame.sceneColourImage, nil);
				if(frame.sceneColourMemory)
					vkFreeMemory(gvk.device,
						             frame.sceneColourMemory, nil);
				if(frame.sceneReflectionView)
					vkDestroyImageView(gvk.device,
					                   frame.sceneReflectionView, nil);
				if(frame.sceneReflectionImage)
					vkDestroyImage(gvk.device,
					               frame.sceneReflectionImage, nil);
				if(frame.sceneReflectionMemory)
					vkFreeMemory(gvk.device,
						             frame.sceneReflectionMemory, nil);
				if(frame.sceneMsaaView)
					vkDestroyImageView(gvk.device,
					                   frame.sceneMsaaView, nil);
				if(frame.sceneMsaaImage)
					vkDestroyImage(gvk.device,
					               frame.sceneMsaaImage, nil);
				if(frame.sceneMsaaMemory)
					vkFreeMemory(gvk.device,
					             frame.sceneMsaaMemory, nil);
				if(frame.resolvedHistoryView)
					vkDestroyImageView(gvk.device,
					                   frame.resolvedHistoryView, nil);
				if(frame.resolvedHistoryImage)
					vkDestroyImage(gvk.device,
					               frame.resolvedHistoryImage, nil);
				if(frame.resolvedHistoryMemory)
					vkFreeMemory(gvk.device,
					             frame.resolvedHistoryMemory, nil);
				if(frame.motionMapped && frame.motionBufferMemory)
					vkUnmapMemory(gvk.device, frame.motionBufferMemory);
				if(frame.motionBuffer)
					vkDestroyBuffer(gvk.device, frame.motionBuffer, nil);
				if(frame.motionBufferMemory)
					vkFreeMemory(gvk.device, frame.motionBufferMemory, nil);
				if(frame.motionView)
					vkDestroyImageView(gvk.device, frame.motionView, nil);
				if(frame.motionImage)
					vkDestroyImage(gvk.device, frame.motionImage, nil);
				if(frame.motionMemory)
					vkFreeMemory(gvk.device, frame.motionMemory, nil);
				if(frame.sgsrConvertView)
					vkDestroyImageView(gvk.device,
					                   frame.sgsrConvertView, nil);
				if(frame.sgsrConvertImage)
					vkDestroyImage(gvk.device,
					               frame.sgsrConvertImage, nil);
				if(frame.sgsrConvertMemory)
					vkFreeMemory(gvk.device,
					             frame.sgsrConvertMemory, nil);
				if(frame.depthView)
					vkDestroyImageView(gvk.device, frame.depthView, nil);
				if(frame.depthImage)
					vkDestroyImage(gvk.device, frame.depthImage, nil);
				if(frame.depthMemory)
					vkFreeMemory(gvk.device, frame.depthMemory, nil);
				if(frame.timestampQueryPool)
					vkDestroyQueryPool(gvk.device,
					                   frame.timestampQueryPool, nil);
				if(frame.fence)
					vkDestroyFence(gvk.device, frame.fence, nil);
			}
			if(gvk.commandPool) vkDestroyCommandPool(gvk.device, gvk.commandPool, nil);
		}
		// The device itself belongs to OpenXR and is destroyed with the
		// session, never here.
		memset(&gvk, 0, sizeof(gvk));
		return 1;

	case DEVICEINIT:
	case DEVICETERM:
	case DEVICEFINALIZE:
		return 1;

	case DEVICEGETNUMSUBSYSTEMS:
	case DEVICEGETNUMVIDEOMODES:
		return 1;
	case DEVICEGETCURRENTSUBSYSTEM:
	case DEVICEGETCURRENTVIDEOMODE:
		return 0;
	case DEVICESETSUBSYSTEM:
	case DEVICESETVIDEOMODE:
		return 1;
	case DEVICEGETSUBSSYSTEMINFO: {
		SubSystemInfo *info = (SubSystemInfo*)arg;
		if(info != nil && n == 0){
			strncpy(info->name, gvk.deviceProperties.deviceName,
			        sizeof(info->name)-1);
			info->name[sizeof(info->name)-1] = '\0';
			return 1;
		}
		return 0;
	}
	case DEVICEGETVIDEOMODEINFO: {
		VideoMode *mode = (VideoMode*)arg;
		// The headset owns the resolution; report the OpenXR target so any
		// caller inspecting the mode sees the real per-eye size.
		if(mode != nil && n == 0){
			mode->width = (int32)gvk.width;
			mode->height = (int32)gvk.height;
			mode->depth = 32;
			mode->flags = VIDEOMODEEXCLUSIVE;
			return 1;
		}
		return 0;
	}
	default:
		return 0;
	}
}

bool32
didLastDeviceOpenFailForRenderTarget(void)
{
	return gLastDeviceOpenRenderTargetFailure;
}

// ---------------------------------------------------------------------------
// Plugin registration
// ---------------------------------------------------------------------------

static void*
driverOpen(void *object, int32, int32)
{
	Driver *driver = engine->driver[PLATFORM_VULKAN];
	driver->defaultPipeline = makeDefaultPipeline();
	driver->rasterNativeOffset = nativeRasterOffset;
	driver->rasterCreate = rasterCreate;
	driver->rasterLock = rasterLock;
	driver->rasterUnlock = rasterUnlock;
	driver->rasterLockPalette = null::rasterLockPalette;
	driver->rasterUnlockPalette = null::rasterUnlockPalette;
	driver->rasterNumLevels = rasterNumLevels;
	driver->imageFindRasterFormat = imageFindRasterFormat;
	driver->rasterFromImage = rasterFromImage;
	driver->rasterToImage = rasterToImage;
	return object;
}

static void*
driverClose(void *object, int32, int32)
{
	return object;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_VULKAN, 0, PLATFORM_VULKAN,
	                       driverOpen, driverClose);
	registerNativeRaster();
}

#else

// Built without RW_VULKAN: keep the symbols so the platform can be selected at
// configure time without conditionalising every caller.
void registerPlatformPlugins(void) {}

#endif

Device renderdevice = {
	0.0f, 1.0f,
#ifdef RW_VULKAN
	vulkan::beginUpdate,
	vulkan::endUpdate,
	vulkan::clearCamera,
	vulkan::showRaster,
	vulkan::rasterRenderFast,
	vulkan::setRenderState,
	vulkan::getRenderState,
	vulkan::im2DRenderLine,
	vulkan::im2DRenderTriangle,
	vulkan::im2DRenderPrimitive,
	vulkan::im2DRenderIndexedPrimitive,
	vulkan::im3DTransform,
	vulkan::im3DRenderPrimitive,
	vulkan::im3DRenderIndexedPrimitive,
	vulkan::im3DEnd,
	vulkan::deviceSystem,
#else
	null::beginUpdate,
	null::endUpdate,
	null::clearCamera,
	null::showRaster,
	null::rasterRenderFast,
	null::setRenderState,
	null::getRenderState,
	null::im2DRenderLine,
	null::im2DRenderTriangle,
	null::im2DRenderPrimitive,
	null::im2DRenderIndexedPrimitive,
	null::im3DTransform,
	null::im3DRenderPrimitive,
	null::im3DRenderIndexedPrimitive,
	null::im3DEnd,
	null::deviceSystem,
#endif
};

}
}
