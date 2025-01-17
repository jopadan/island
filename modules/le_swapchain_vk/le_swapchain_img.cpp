#include "le_swapchain_vk.h"

#include "le_backend_vk.h"
#include "le_backend_types_internal.h"

#include "private/le_swapchain_vk/le_swapchain_vk_common.inl"
#include "private/le_swapchain_vk/vk_to_string_helpers.inl"

#include "shared/interfaces/le_image_encoder_interface.h" // generic encoder interface - to use it, you must set encoder api, and image encoder parameter object via swapchain creation parameters

#include <cassert>
#include "util/vk_mem_alloc/vk_mem_alloc.h"
#include "le_log.h"

#include <string>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <ctime>

#include "le_swapchain_img.h"

static constexpr auto LOGGER_LABEL = "le_swapchain_img";

struct TransferFrame {
	VkImage           image                = nullptr; // Owned. Handle to image
	VkBuffer          buffer               = nullptr; // Owned. Handle to buffer
	VmaAllocation     imageAllocation      = nullptr; // Owned. Handle to image allocation
	VmaAllocation     bufferAllocation     = nullptr; // Owned. Handle to buffer allocation
	VmaAllocationInfo imageAllocationInfo  = {};
	VmaAllocationInfo bufferAllocationInfo = {};
	VkFence           frameFence           = nullptr;
	VkCommandBuffer   cmdPresent           = nullptr; // copies from image to buffer
	VkCommandBuffer   cmdAcquire           = nullptr; // transfers image back to correct layout
	bool              in_flight            = false;   // whether this frame is currently rendering on the queue
};

struct img_data_o {
	le_swapchain_img_settings_t   mSettings;
	uint32_t                      swapchain_images_count;             // Number of images in swapchain
	uint32_t                      current_image_idx;                  // current image index
	uint32_t                      produced_images_count;              // total number of produced images
	uint32_t                      vk_queue_family_index;              // queue family index for the queue which this swapchain will use
	VkExtent3D                    mSwapchainExtent;                   //
	VkSurfaceFormatKHR            surface_format;                     //
	uint32_t                      reserved__;                         // RESERVED for packing this struct
	VkDevice                      device;                             // Owned by backend
	VkPhysicalDevice              physicalDevice;                     // Owned by backend
	VkCommandPool                 vkCommandPool;                      // Command pool from wich we allocate present and acquire command buffers
	le_backend_o*                 backend = nullptr;                  // Not owned. Backend owns swapchain.
	std::vector<TransferFrame>    transferFrames;                     //
	std::string                   image_filename_template  = "";      // template for image file name
	std::string                   first_image_filename     = "";      // template for image file name
	size_t                        frame_number_offset      = 0;       // optional -- added to running frame number - useful to match renderer frame numbers to image names
	le_image_encoder_interface_t* image_encoder_i          = nullptr; // optional, non-owing: generic encoder api
	le_image_encoder_o*           image_encoder            = nullptr;
	void*                         image_encoder_parameters = nullptr; // optional, owning - cloned via `image_encoder_i.clone_image_encoder_parameters_object()`
	BackendQueueInfo*             queue_info = nullptr;               // Non-owning. Present-enabled queue, initially null, set at create
};

// ----------------------------------------------------------------------

static void swapchain_img_reset( le_swapchain_o* base, le_swapchain_img_settings_t const* settings_ ) {
	static auto logger = LeLog( LOGGER_LABEL );

	auto self = static_cast<img_data_o* const>( base->data );

	if ( settings_ ) {
		self->mSettings        = *settings_;
		self->mSwapchainExtent = {
		    .width  = self->mSettings.width_hint,
		    .height = self->mSettings.height_hint,
		    .depth  = 1,
		};
		self->swapchain_images_count = self->mSettings.base.imagecount_hint;

		// If there exists an image encoder parameter object that we own,
		// and that we created with an earlier version of the image encoder interface
		// we must first destroy it, using that version of the image encoder interface.

		if ( self->image_encoder_i && self->image_encoder_parameters ) {
			self->image_encoder_i->destroy_image_encoder_parameters_object( self->image_encoder_parameters );
		}

		if ( self->image_encoder_i && self->image_encoder ) {
			self->image_encoder_i->destroy_image_encoder( self->image_encoder );
			self->image_encoder = nullptr;
		}

		// Update the image encoder interface
		self->image_encoder_i = self->mSettings.image_encoder_i;

		// Clone image encoder parameters using the given interface
		if ( self->image_encoder_i ) {

			// filename should not matter just now; this will get updated when writing the file
			self->image_encoder = self->image_encoder_i->create_image_encoder( self->image_filename_template.c_str(), self->mSwapchainExtent.width, self->mSwapchainExtent.height );

			if ( self->mSettings.image_encoder_parameters ) {
				self->image_encoder_parameters =
				    self->image_encoder_i->clone_image_encoder_parameters_object(
				        self->mSettings.image_encoder_parameters );
			}
		}
	}

	assert( self->mSettings.base.type == le_swapchain_settings_t::Type::LE_IMG_SWAPCHAIN );

	VkResult imgAllocationResult = VK_ERROR_UNKNOWN;
	VkResult bufAllocationResult = VK_ERROR_UNKNOWN;

	uint32_t const numFrames = self->swapchain_images_count;

	self->transferFrames.resize( numFrames, {} );

	for ( auto& frame : self->transferFrames ) {

		{
			// Allocate space for an image which can hold a render surface

			VkImageCreateInfo imageCreateInfo{
			    .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			    .pNext                 = nullptr, // optional
			    .flags                 = 0,       // optional
			    .imageType             = VK_IMAGE_TYPE_2D,
			    .format                = self->surface_format.format,
			    .extent                = self->mSwapchainExtent,
			    .mipLevels             = 1,
			    .arrayLayers           = 1,
			    .samples               = VK_SAMPLE_COUNT_1_BIT,
			    .tiling                = VK_IMAGE_TILING_OPTIMAL,
			    .usage                 = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			    .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
			    .queueFamilyIndexCount = 1, // optional
			    .pQueueFamilyIndices   = &self->vk_queue_family_index,
			    .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
			};

			VmaAllocationCreateInfo allocationCreateInfo{};
			allocationCreateInfo.flags          = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
			allocationCreateInfo.usage          = VMA_MEMORY_USAGE_GPU_ONLY;
			allocationCreateInfo.preferredFlags = 0;

			using namespace le_backend_vk;
			imgAllocationResult = VkResult(
			    private_backend_vk_i.allocate_image(
			        self->backend, &imageCreateInfo,
			        &allocationCreateInfo,
			        &frame.image,
			        &frame.imageAllocation,
			        &frame.imageAllocationInfo ) );
			assert( imgAllocationResult == VK_SUCCESS );
		}

		uint64_t image_buffer_size_in_bytes = 0;
		{
			// Note that imageAllocationInfo.size might not be what you expect if image
			// has non-linear (that is: optimal) tiling.
			//
			// Optimally tiled images may occupy less (or perhaps even more) than an
			// equivalent RAW image with given extent and pixel type.

			image_buffer_size_in_bytes = frame.imageAllocationInfo.size;

			le_num_type num_type     = {};
			uint32_t    num_channels = 0;
			if ( le_format_infer_channels_and_num_type( le::Format( self->surface_format.format ), &num_channels, &num_type ) ) {
				uint32_t num_bytes_per_channel = 1 << ( std::underlying_type_t<le_num_type>( num_type ) & 0b11 );
				image_buffer_size_in_bytes     = num_bytes_per_channel * num_channels * self->mSwapchainExtent.width * self->mSwapchainExtent.height * self->mSwapchainExtent.depth;
			} else {
				logger.error( "Could not infer channels and num type for given format: %x", self->surface_format.format );
			}
		}
		{
			// Allocate space for a buffer in which to read back the image data.
			//
			// now we need a buffer which is host visible and coherent, which we can use to read out our data.
			// there needs to be one buffer per image;
			using namespace le_backend_vk;

			VkBufferCreateInfo bufferCreateInfo{
			    .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			    .pNext                 = nullptr, // optional
			    .flags                 = 0,       // optional
			    .size                  = image_buffer_size_in_bytes,
			    .usage                 = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			    .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
			    .queueFamilyIndexCount = 1, // optional
			    .pQueueFamilyIndices   = &self->vk_queue_family_index,
			};

			VmaAllocationCreateInfo allocationCreateInfo{};
			allocationCreateInfo.flags          = VMA_ALLOCATION_CREATE_MAPPED_BIT;
			allocationCreateInfo.usage          = VMA_MEMORY_USAGE_CPU_ONLY;
			allocationCreateInfo.preferredFlags = 0;

			bufAllocationResult = VkResult(
			    private_backend_vk_i.allocate_buffer(
			        self->backend,
			        &bufferCreateInfo,
			        &allocationCreateInfo,
			        &frame.buffer,
			        &frame.bufferAllocation,
			        &frame.bufferAllocationInfo //
			        ) );
			assert( bufAllocationResult == VK_SUCCESS );
		}

		{
			VkFenceCreateInfo info{
			    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			    .pNext = nullptr, // optional
			    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
			};

			vkCreateFence( self->device, &info, nullptr, &frame.frameFence );
		}
	}

	// Allocate command buffers for each frame.
	// Each frame needs one command buffer

	VkCommandBufferAllocateInfo allocateInfo{
	    .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .pNext              = nullptr, // optional
	    .commandPool        = self->vkCommandPool,
	    .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = numFrames * 2,
	};

	{
		std::vector<VkCommandBuffer> cmdBuffers( numFrames * 2 );
		vkAllocateCommandBuffers( self->device, &allocateInfo, cmdBuffers.data() );

		// set up commands in frames.

		for ( size_t i = 0; i != numFrames; ++i ) {
			self->transferFrames[ i ].cmdAcquire = cmdBuffers[ i * 2 ];
			self->transferFrames[ i ].cmdPresent = cmdBuffers[ i * 2 + 1 ];
		}
	}

	// Add commands to command buffers for all frames.

	for ( auto& frame : self->transferFrames ) {
		{
			// copy == transfer image to buffer memory
			VkCommandBuffer& cmdPresent = frame.cmdPresent;
			{
				VkCommandBufferBeginInfo info = {
				    .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				    .pNext            = nullptr, // optional
				    .flags            = 0,       // optional
				    .pInheritanceInfo = 0,       // optional
				};

				vkBeginCommandBuffer( cmdPresent, &info );
			}

			{

				VkImageMemoryBarrier2 img_barrier{
				    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				    .pNext               = nullptr,                              // optional
				    .srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,  // wait for nothing
				    .srcAccessMask       = 0,                                    // flush nothing
				    .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, // block on any transfer stage
				    .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,        // make memory visible to transfer read (after layout transition)
				    .oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,      // transition from present_src
				    .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, // to transfer_src optimal
				    .srcQueueFamilyIndex = self->vk_queue_family_index,
				    .dstQueueFamilyIndex = self->vk_queue_family_index,
				    .image               = frame.image,
				    .subresourceRange    = {
				           .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				           .baseMipLevel   = 0,
				           .levelCount     = 1,
				           .baseArrayLayer = 0,
				           .layerCount     = 1,
                    },
				};

				VkDependencyInfo info{
				    .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				    .pNext                    = nullptr, // optional
				    .dependencyFlags          = 0,       // optional
				    .memoryBarrierCount       = 0,       // optional
				    .pMemoryBarriers          = 0,
				    .bufferMemoryBarrierCount = 0, // optional
				    .pBufferMemoryBarriers    = 0,
				    .imageMemoryBarrierCount  = 1, // optional
				    .pImageMemoryBarriers     = &img_barrier,
				};

				vkCmdPipelineBarrier2( cmdPresent, &info );
			}

			VkBufferImageCopy imgCopy{
			    .bufferOffset      = 0,
			    .bufferRowLength   = self->mSwapchainExtent.width,
			    .bufferImageHeight = self->mSwapchainExtent.height,
			    .imageSubresource  = {
			         .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			         .mipLevel       = 0,
			         .baseArrayLayer = 0,
			         .layerCount     = 1,
                },
			    .imageOffset = {},
			    .imageExtent = self->mSwapchainExtent,
			};
			;

			// TODO: here we must wait for the buffer read event to have been signalled - because that means that the buffer
			// is available for writing again. Perhaps we should use a buffer which is not persistently mapped, if that's faster.

			// Image must be transferred to a buffer - we can then read from this buffer.
			vkCmdCopyImageToBuffer( cmdPresent, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame.buffer, 1, &imgCopy );
			vkEndCommandBuffer( cmdPresent );
		}
		{
			// Move ownership of image back from transfer -> graphics
			// Change image layout back to colorattachment

			VkCommandBuffer& cmdAcquire = frame.cmdAcquire;
			{
				VkCommandBufferBeginInfo info = {
				    .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				    .pNext            = nullptr, // optional
				    .flags            = 0,       // optional
				    .pInheritanceInfo = 0,       // optional
				};

				vkBeginCommandBuffer( cmdAcquire, &info );
			}

			{

				VkImageMemoryBarrier2 img_read_to_acquire_barrier = {
				    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				    .pNext               = nullptr,                                         // optional
				    .srcStageMask        = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,               //
				    .srcAccessMask       = 0,                                               //
				    .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, // block on color attachment output
				    .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,          // make image memory visible to attachment write (after layout transition)
				    .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,                       // transition from undefined to
				    .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,        // attachment optimal
				    .srcQueueFamilyIndex = self->vk_queue_family_index,
				    .dstQueueFamilyIndex = self->vk_queue_family_index,
				    .image               = frame.image,
				    .subresourceRange    = {
				           .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				           .baseMipLevel   = 0,
				           .levelCount     = 1,
				           .baseArrayLayer = 0,
				           .layerCount     = 1,
                    },
				};

				VkDependencyInfo info = {
				    .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				    .pNext                    = nullptr, // optional
				    .dependencyFlags          = 0,       // optional
				    .memoryBarrierCount       = 0,       // optional
				    .pMemoryBarriers          = 0,
				    .bufferMemoryBarrierCount = 0, // optional
				    .pBufferMemoryBarriers    = 0,
				    .imageMemoryBarrierCount  = 1, // optional
				    .pImageMemoryBarriers     = &img_read_to_acquire_barrier,
				};

				vkCmdPipelineBarrier2( cmdAcquire, &info );
			}

			vkEndCommandBuffer( cmdAcquire );
		}
	}
}

// ----------------------------------------------------------------------

static le_swapchain_o* swapchain_img_create( le_backend_o* backend, const le_swapchain_settings_t* settings_ ) {
	static auto logger = LeLog( LOGGER_LABEL );
	auto        base   = new le_swapchain_o( le_swapchain_vk::api->swapchain_img_i );
	base->data         = new img_data_o{};
	auto self          = static_cast<img_data_o*>( base->data );

	assert( settings_->type == le_swapchain_settings_t::LE_IMG_SWAPCHAIN );

	le_swapchain_img_settings_t const* settings = reinterpret_cast<le_swapchain_img_settings_t const*>( settings_ );

	self->mSettings                 = *settings;
	self->backend                   = backend;
	self->surface_format.format     = VkFormat( settings->format_hint );
	self->surface_format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	self->current_image_idx         = uint32_t( ~0 );
	self->image_filename_template   = settings->image_filename_template ? std::string( settings->image_filename_template ) : "img_%08d.raw";
	self->frame_number_offset       = settings->frame_number_offset;

	{

		using namespace le_backend_vk;
		self->device         = private_backend_vk_i.get_vk_device( backend );
		self->physicalDevice = private_backend_vk_i.get_vk_physical_device( backend );
		self->queue_info     = private_backend_vk_i.get_default_graphics_queue_info( backend );
	}

	{
		// Create a command pool, so that we may create command buffers from it.

		VkCommandPoolCreateInfo createInfo{
		    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		    .pNext            = nullptr,                                         // optional
		    .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, // optional
		    .queueFamilyIndex = self->vk_queue_family_index,
		};

		vkCreateCommandPool( self->device, &createInfo, nullptr, &self->vkCommandPool );
	}

	swapchain_img_reset( base, settings );

	logger.info( "Created Swapchain: %p: Image Swapchain", base );

	return base;
}

// ----------------------------------------------------------------------

static le_swapchain_o* swapchain_img_create_from_old_swapchain( le_swapchain_o* old_swapchain, uint32_t width, uint32_t height ) {
	// not implemented
	assert( false && "Not implemented." );
	return nullptr;
}

static void write_image( img_data_o* self, const TransferFrame& frame, uint32_t frame_index ); // ffdecl

// ----------------------------------------------------------------------

static void swapchain_img_destroy( le_swapchain_o* base ) {

	static auto logger = LeLog( LOGGER_LABEL );
	auto        self   = static_cast<img_data_o* const>( base->data );

	using namespace le_backend_vk;

	{ // We flush any images that are still in-flight
		//
		if ( self->produced_images_count < self->transferFrames.size() ) {
			self->produced_images_count = self->transferFrames.size();
		}

		for ( uint32_t offset_idx = 0; offset_idx != self->transferFrames.size(); offset_idx++ ) {

			uint32_t imageIndex = ( self->current_image_idx + offset_idx + 1 ) % self->swapchain_images_count;

			if ( self->transferFrames[ imageIndex ].in_flight == false ) {
				continue;
			}

			// -- Wait for current in-flight frame to be completed on device.
			//
			// We must do this since we're not allowed to delete any vulkan resources
			// which are currently used by the device. Awaiting the fence guarantees
			// that no resources are in-flight at this point.
			auto fenceWaitResult = vkWaitForFences( self->device, 1, &self->transferFrames[ imageIndex ].frameFence, VK_TRUE, 100'000'000 );

			self->transferFrames[ imageIndex ].in_flight = false;

			if ( fenceWaitResult == VK_SUCCESS ) {
				write_image( self, self->transferFrames[ imageIndex ], self->produced_images_count - self->transferFrames.size() );
				++self->produced_images_count;
			} else {
				logger.warn( "Could not flush in-flight image swapchain frame: wait for fence took too long." );
			}
		}
	}

	for ( auto& f : self->transferFrames ) {

		// Destroy image allocation for this frame.
		private_backend_vk_i.destroy_image( self->backend, f.image, f.imageAllocation );
		// Destroy buffer allocation for this frame.
		private_backend_vk_i.destroy_buffer( self->backend, f.buffer, f.bufferAllocation );

		if ( f.frameFence ) {
			vkDestroyFence( self->device, f.frameFence, nullptr );
			f.frameFence = nullptr;
		}
	}

	// Clear TransferFrame

	self->transferFrames.clear();

	if ( self->vkCommandPool ) {

		// Destroying the command pool implicitly frees all command buffers
		// which were allocated from it, so we don't have to free command
		// buffers explicitly.

		vkDestroyCommandPool( self->device, self->vkCommandPool, nullptr );
		self->vkCommandPool = nullptr;
	}

	{
		// Delete image encoder settings object - as this was cloned when received
		if ( self->image_encoder_i ) {
			self->image_encoder_i->destroy_image_encoder_parameters_object(
			    self->image_encoder_parameters );
			self->image_encoder_parameters = nullptr;
		}
		// delete image encoder
		if ( self->image_encoder_i && self->image_encoder ) {
			self->image_encoder_i->destroy_image_encoder( self->image_encoder );
			self->image_encoder = nullptr;
		}
	}

	delete self; // delete object's data
	delete base; // delete object

	logger.info( "Deleted Swapchain: %p: Image Swapchain", base );
}

// ----------------------------------------------------------------------

// no-op for now
static void swapchain_img_release( le_swapchain_o* base ) {
	static auto logger = LeLog( LOGGER_LABEL );
	auto        self   = static_cast<img_data_o* const>( base->data );
}

// ----------------------------------------------------------------------
struct le_image_encoder_format_o {
	le::Format format;
};

// ----------------------------------------------------------------------

static void write_image( img_data_o* self, const TransferFrame& frame, uint32_t frame_index ) {
	static auto logger = LeLog( LOGGER_LABEL );
	if ( self->image_encoder_i ) {
		char filename[ 1024 ];
		sprintf( filename, self->image_filename_template.c_str(), frame_index + self->frame_number_offset );

		if ( self->first_image_filename.empty() ) {
			self->first_image_filename = filename;
		}

		logger.debug( "Start  Encoding Image: %s +[%08d]", self->first_image_filename.c_str(), frame_index );

		self->image_encoder_i->update_filename( self->image_encoder, filename );

		if ( self->image_encoder_parameters ) {
			self->image_encoder_i->set_encode_parameters( self->image_encoder, self->image_encoder_parameters );
		}

		le_image_encoder_format_o format_wrapper{ le::Format( self->surface_format.format ) };

		self->image_encoder_i->write_pixels(
		    self->image_encoder, ( uint8_t* )frame.bufferAllocationInfo.pMappedData,
		    frame.bufferAllocationInfo.size,
		    &format_wrapper );

		logger.info( "Finish Encoding Image: %s +[%08d]", self->first_image_filename.c_str(), frame_index );

	} else {
		char file_name[ 1024 ];
		sprintf( file_name, self->image_filename_template.c_str(), frame_index + self->frame_number_offset );
		std::ofstream myfile( file_name, std::ios::out | std::ios::binary );
		myfile.write( ( char* )frame.bufferAllocationInfo.pMappedData,
					  self->mSwapchainExtent.width * self->mSwapchainExtent.height * 4 );
		myfile.close();
		logger.info( "Wrote Image: %s", file_name );
	}
}

// ----------------------------------------------------------------------

static bool swapchain_img_acquire_next_image( le_swapchain_o* base, VkSemaphore semaphorePresentComplete, uint32_t* imageIndex ) {
	static auto logger = LeLog( LOGGER_LABEL );

	auto self = static_cast<img_data_o* const>( base->data );
	// This method will return the next avaliable vk image index for this swapchain, possibly
	// before this image is available for writing. Image will be ready for writing when
	// semaphorePresentComplete is signalled.

	// acquire next image, signal semaphore
	*imageIndex = ( self->current_image_idx + 1 ) % self->swapchain_images_count;

	auto fenceWaitResult = vkWaitForFences( self->device, 1, &self->transferFrames[ *imageIndex ].frameFence, VK_TRUE, 100'000'000 );

	if ( fenceWaitResult != VK_SUCCESS ) {
		assert( false ); // waiting for fence took too long.
	}

	self->transferFrames[ *imageIndex ].in_flight = false;
	vkResetFences( self->device, 1, &self->transferFrames[ *imageIndex ].frameFence );

	self->current_image_idx = *imageIndex;

	auto const& frame = self->transferFrames[ *imageIndex ];

	// We only want to write out images which have been rendered into
	// depending on how deep your image swapchain is, you will have to
	// wait for n steps for a frame to have passed from record to
	// submit to render, for it to produce some pixels.
	//
	// The images will originally be black, until they have completed
	// the round-trip to the renderer. We know they have completed the
	// round-trip as soon as self->produced_images_count == self->transferFrames.size().
	//
	if ( self->produced_images_count >= self->transferFrames.size() ) {
		write_image( self, frame, self->produced_images_count - self->transferFrames.size() );
	}
	++self->produced_images_count;

	// ----------| invariant: CPU read-access to memory held by this frame is now complete.
	//  			          since write_image is a synchronous operation

	// this frame is now ready to be re-used
	self->transferFrames[ *imageIndex ].in_flight = true;

	// The number of array elements must correspond to the number of wait semaphores, as each
	// mask specifies what the semaphore is waiting for.
	// std::array<VkPipelineStageFlags, 1> wait_dst_stage_mask = { VkPipelineStageFlagBits::eTransfer };

	VkSubmitInfo submitInfo{
		.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext                = nullptr, // optional
		.waitSemaphoreCount   = 0,       // optional
		.pWaitSemaphores      = 0,
		.pWaitDstStageMask    = 0,
		.commandBufferCount   = 1, // optional
		.pCommandBuffers      = &self->transferFrames[ *imageIndex ].cmdAcquire,
		.signalSemaphoreCount = 1, // optional
		.pSignalSemaphores    = &semaphorePresentComplete,
	};

	{
		// Submitting directly via the queue is not very elegant, as the queue must be exernally synchronised, and
		// by submitting to the queue here we are living on the edge if we ever wanted
		// to have more than one thread producing frames.
		//
		// We should be fine - just make sure that acquire_next_frame (this method) gets called after all
		// frame producers have submitted their payloads to the queue.

		if ( self->queue_info ) {
			auto result = vkQueueSubmit( self->queue_info->queue, 1, &submitInfo, nullptr );
			assert( result == VK_SUCCESS );
		} else {
			le::Log( LOGGER_LABEL ).error( "queue was not set when acquiring image for image swapchain." );
		}
	}

	return true;
}

// ----------------------------------------------------------------------

static bool swapchain_img_present( le_swapchain_o* base, VkQueue queue, VkSemaphore renderCompleteSemaphore_, uint32_t* pImageIndex ) {

	auto self = static_cast<img_data_o* const>( base->data );

	VkPipelineStageFlags wait_dst_stage_mask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

	VkSubmitInfo submitInfo{
		.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext                = nullptr, // optional
		.waitSemaphoreCount   = 1,
		.pWaitSemaphores      = &renderCompleteSemaphore_, // tells us that the image has been written
		.pWaitDstStageMask    = &wait_dst_stage_mask,
		.commandBufferCount   = 1,
		.pCommandBuffers      = &self->transferFrames[ *pImageIndex ].cmdPresent, // copies image to buffer
		.signalSemaphoreCount = 0,                                                // optional
		.pSignalSemaphores    = 0,
	};

	vkQueueSubmit( queue, 1, &submitInfo, self->transferFrames[ *pImageIndex ].frameFence );
	return true;
};

// ----------------------------------------------------------------------

static VkImage swapchain_img_get_image( le_swapchain_o* base, uint32_t index ) {

	auto self = static_cast<img_data_o* const>( base->data );

#ifndef NDEBUG
	assert( index < self->transferFrames.size() );
#endif
	return self->transferFrames[ index ].image;
}

// ----------------------------------------------------------------------

static VkSurfaceFormatKHR* swapchain_img_get_surface_format( le_swapchain_o* base ) {
	auto self = static_cast<img_data_o* const>( base->data );
	return &reinterpret_cast<VkSurfaceFormatKHR&>( self->surface_format );
}

// ----------------------------------------------------------------------

static uint32_t swapchain_img_get_image_width( le_swapchain_o* base ) {
	auto self = static_cast<img_data_o* const>( base->data );
	return self->mSwapchainExtent.width;
}

// ----------------------------------------------------------------------

static uint32_t swapchain_img_get_image_height( le_swapchain_o* base ) {

	auto self = static_cast<img_data_o* const>( base->data );
	return self->mSwapchainExtent.height;
}

// ----------------------------------------------------------------------

static size_t swapchain_img_get_swapchain_images_count( le_swapchain_o* base ) {
	auto self = static_cast<img_data_o* const>( base->data );
	return self->swapchain_images_count;
}

// ----------------------------------------------------------------------

static bool swapchain_request_backend_capabilities( const le_swapchain_settings_t* ) {
	using namespace le_backend_vk;
	// We must activate the swapchain extension otherwise we don't get to transition
	// the image format from VK_IMAGE_LAYOUT_PRESENT_SRC_KHR --- this is not ideal.
	return api->le_backend_settings_i.add_required_device_extension( VK_KHR_SWAPCHAIN_EXTENSION_NAME ) &&
	       api->le_backend_settings_i.add_required_instance_extension( VK_KHR_SURFACE_EXTENSION_NAME );
}
// ----------------------------------------------------------------------

static le_swapchain_settings_t* swapchain_settings_create( le_swapchain_settings_t::Type type ) {
	assert( type == le_swapchain_settings_t::LE_IMG_SWAPCHAIN );
	auto ret = new le_swapchain_img_settings_t{};
	return reinterpret_cast<le_swapchain_settings_t*>( ret );
}

// ----------------------------------------------------------------------

static void swapchain_settings_destroy( le_swapchain_settings_t* settings ) {
	assert( settings->type == le_swapchain_settings_t::LE_IMG_SWAPCHAIN );
	auto obj = reinterpret_cast<le_swapchain_img_settings_t*>( settings );
	delete ( obj );
}

// ----------------------------------------------------------------------

static le_swapchain_settings_t* swapchain_settings_clone( le_swapchain_settings_t const* settings ) {
	assert( settings->type == le_swapchain_settings_t::LE_IMG_SWAPCHAIN );
	auto obj = reinterpret_cast<le_swapchain_img_settings_t const*>( settings );
	auto ret = new le_swapchain_img_settings_t{ *obj };
	return reinterpret_cast<le_swapchain_settings_t*>( ret );
}
// ----------------------------------------------------------------------

void register_le_swapchain_img_api( void* api_ ) {
	auto  api         = static_cast<le_swapchain_vk_api*>( api_ );
	auto& swapchain_i = api->swapchain_img_i;

	swapchain_i.create                    = swapchain_img_create;
	swapchain_i.destroy                   = swapchain_img_destroy;
	swapchain_i.release                   = swapchain_img_release;
	swapchain_i.create_from_old_swapchain = swapchain_img_create_from_old_swapchain;

	swapchain_i.acquire_next_image           = swapchain_img_acquire_next_image;
	swapchain_i.get_image                    = swapchain_img_get_image;
	swapchain_i.get_image_width              = swapchain_img_get_image_width;
	swapchain_i.get_image_height             = swapchain_img_get_image_height;
	swapchain_i.get_surface_format           = swapchain_img_get_surface_format;
	swapchain_i.get_image_count              = swapchain_img_get_swapchain_images_count;
	swapchain_i.present                      = swapchain_img_present;
	swapchain_i.request_backend_capabilities = swapchain_request_backend_capabilities;

	swapchain_i.settings_create  = swapchain_settings_create;
	swapchain_i.settings_clone   = swapchain_settings_clone;
	swapchain_i.settings_destroy = swapchain_settings_destroy;
}
