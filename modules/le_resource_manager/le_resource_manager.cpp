#include "le_resource_manager.h"
#include "le_core.h"
#include "le_renderer.hpp"
#include "le_pixels.h"
#include "le_file_watcher.h"
#include "le_log.h"

#include <string>
#include <vector>
#include <assert.h>

#include <unordered_map>

// only used to print debug messages:
#include "private/le_renderer/le_resource_handle_t.inl"

#include "shared/interfaces/le_image_decoder_interface.h"

static le::Log& logger() {
	// Enforce lazy initialization for logger oblect
	static auto logger = le::Log( "resource_manager" );
	return logger;
};

struct le_image_decoder_format_o {
	le::Format format;
};

// ----------------------------------------------------------------------
// TODO:
// * add a method to safely remove resources from the manager
// * add a method to update resources from within the manager
// ----------------------------------------------------------------------

struct le_resource_manager_o {

	le_file_watcher_o* file_watcher = nullptr;

	struct image_data_layer_t {
		le_image_decoder_o*                 image_decoder;
		le_image_decoder_interface_t const* decoder_i;

		std::string                    path;
		bool                           was_uploaded     = false;
		bool                           extents_inferred = false;
		le_resource_info_t::ImageInfo* image_info; // non-owning
		int                            watch_id = -1;
		uint32_t                       width;
		uint32_t                       height;
	};

	struct resource_item_t {
		le_image_resource_handle        image_handle;
		le_resource_info_t              image_info;
		std::vector<image_data_layer_t> image_layers; // must have at least one element
	};

	std::unordered_map<std::string, le_image_decoder_interface_t*> available_decoder_interfaces; // map from hash of lowercase file extension (`exr`, `png`, ... ) to an image decoder inferface that can deal with this extension

	std::unordered_map<le_resource_handle, resource_item_t> resources;
};

// ----------------------------------------------------------------------

static bool setupTransferPass( le_renderpass_o* pRp, void* user_data ) {
	le::RenderPass rp{ pRp };
	auto           manager = static_cast<le_resource_manager_o const*>( user_data );

	if ( manager->resources.empty() ) {
		return false;
	}

	// --------| invariant: some elements need upload

	bool needsTransfer = false;

	for ( auto const& [ k, r ] : manager->resources ) {

		bool uses_resource = false;

		for ( auto const& layer : r.image_layers ) {
			if ( layer.was_uploaded == false ) {
				uses_resource = true;
				break;
			}
		}

		if ( uses_resource ) {
			rp.useImageResource( r.image_handle, le::AccessFlagBits2::eTransferWrite );
			needsTransfer = true;
		}
	}

	return needsTransfer;
}

// ----------------------------------------------------------------------

static void execTransferPass( le_command_buffer_encoder_o* pEncoder, void* user_data ) {
	le::TransferEncoder encoder{ pEncoder };
	auto                manager = static_cast<le_resource_manager_o*>( user_data );

	// we will probably end up with a number of write operations which will all target the same image resource,
	// but will write into different aspects of memory associated with the resource.

	// First figure out whether we must write to an image at all

	using namespace le_pixels;

	for ( auto& [ k, r ] : manager->resources ) {

		uint32_t const num_layers     = uint32_t( r.image_layers.size() );
		uint32_t const image_width    = r.image_info.image.extent.width;
		uint32_t const image_height   = r.image_info.image.extent.height;
		uint32_t const image_depth    = r.image_info.image.extent.depth;
		uint32_t const num_mip_levels = r.image_info.image.mipLevels;

		uint32_t layer_index = 0;
		for ( auto& layer : r.image_layers ) {

			// We can fill in the correct handling for mutiple mip levels later.
			// for now, assert that there is exatcly one mip level.

			if ( layer.was_uploaded ) {
				layer_index++;
				continue;
			}

			// --------| invariant: layer was not yet uploaded.

			for ( uint32_t mip_level = 0; mip_level != 1; mip_level++ ) {

				assert( mip_level == 0 && "mip level greater than 0 not implemented" );
				uint32_t width  = layer.extents_inferred ? ( layer.width >> mip_level ) : ( image_width >> mip_level );
				uint32_t height = layer.extents_inferred ? ( layer.height >> mip_level ) : ( image_height >> mip_level );
				uint32_t depth  = image_depth;

				le_write_to_image_settings_t write_info =
				    le::WriteToImageSettingsBuilder()
				        .setDstMiplevel( mip_level )
				        .setNumMiplevels( num_mip_levels )
				        .setArrayLayer( layer_index ) // faces are indexed: +x, -x, +y, -y, +z, -z
				        .setImageH( height )
				        .setImageW( width )
				        .setImageD( depth )
				        .build();

				le_image_decoder_format_o decoder_format;

				uint32_t    w, h, num_channels;
				le_num_type channel_data_type;

				assert( layer.image_decoder );
				layer.decoder_i->get_image_data_description( layer.image_decoder, &decoder_format, &w, &h );

				bool result = le_format_infer_channels_and_num_type( decoder_format.format, &num_channels, &channel_data_type );

				if ( false == result ) {
					logger().error( "Could not infer format info from format: %s", le::to_str( decoder_format.format ) );
					continue;
				}

				uint32_t bytes_per_pixel = num_channels * size_of( channel_data_type ); // See definition of le_num_type
				size_t   num_bytes       = bytes_per_pixel * w * h;

				// We map memory via the encoder - if all goes well the encoder gives us a pointer
				// into which we can read pixels into.
				void* bytes = nullptr;

				encoder.mapImageMemory( r.image_handle, write_info, num_bytes, &bytes );
				assert( bytes != nullptr && "Image memory mapping must have been successful." );

				bool read_success = layer.decoder_i->read_pixels( layer.image_decoder, ( uint8_t* )bytes, num_bytes );
				assert( read_success && "Pixels were not read successfully." );
			}

			layer.was_uploaded = true;
			layer_index++;
		}
	}
}

// ----------------------------------------------------------------------

static void update_image_array_layer( le_resource_manager_o::image_data_layer_t& layer_data ) {

	// we must find out the pixels type from image info format
	uint32_t num_channels = 0;

	le_image_decoder_o* new_decoder = layer_data.decoder_i->create_image_decoder( layer_data.path.c_str() );

	if ( layer_data.image_decoder && new_decoder ) {
		layer_data.decoder_i->destroy_image_decoder( layer_data.image_decoder );
	}

	if ( new_decoder ) {
		layer_data.image_decoder = new_decoder;
	} else {
		return;
	}

	le_image_decoder_format_o detected_format  = { le::Format::eUndefined };
	le_image_decoder_format_o requested_format = { layer_data.image_info->format };

	uint32_t w, h;

	layer_data.decoder_i->get_image_data_description( layer_data.image_decoder, &detected_format, &w, &h );

	// If Format is not any of the formats that we know, we
	// adjust the format so that it fits.
	//

	switch ( requested_format.format ) {
	case ( le::Format::eR8G8B8A8Unorm ): // deliberate fall-through
	case ( le::Format::eR8G8B8Unorm ):
	case ( le::Format::eR8G8Unorm ):
	case ( le::Format::eR8Unorm ):
	case ( le::Format::eR32G32B32A32Sfloat ): // deliberate fall-through
	case ( le::Format::eR32G32B32Sfloat ):
	case ( le::Format::eR32G32Sfloat ):
	case ( le::Format::eR32Sfloat ):
	case ( le::Format::eR16G16B16A16Sfloat ): // deliberate fall-through
	case ( le::Format::eR16G16B16Sfloat ):
	case ( le::Format::eR16G16Sfloat ):
	case ( le::Format::eR16Sfloat ):
	case ( le::Format::eR16G16B16A16Unorm ): // deliberate fall-through
	case ( le::Format::eR16G16B16Unorm ):
	case ( le::Format::eR16G16Unorm ):
	case ( le::Format::eR16Unorm ):
		break;

	default:
		requested_format.format = le::Format::eR8G8B8A8Unorm;
		break;
	}

	if ( detected_format.format != requested_format.format ) {
		logger().info( "File '%s' -> adjusting imported image format from %s to: %s",
		               layer_data.path.c_str(),
		               le::to_str( detected_format.format ),
		               le::to_str( requested_format.format ) );
	}

	layer_data.decoder_i->set_requested_format( layer_data.image_decoder, &requested_format );

	if ( layer_data.extents_inferred ) {
		layer_data.image_info->extent.depth  = 1;
		layer_data.image_info->extent.width  = w;
		layer_data.image_info->extent.height = h;
	}

	layer_data.width              = w;
	layer_data.height             = h;
	layer_data.image_info->format = requested_format.format;

	layer_data.image_info->usage |= ( le::ImageUsageFlagBits::eTransferDst | le::ImageUsageFlagBits::eSampled | le::ImageUsageFlagBits::eStorage );

	layer_data.was_uploaded = false;
}

// ----------------------------------------------------------------------

static void le_resource_manager_file_watcher_callback( char const* path, void* user_data ) {
	// we must update the image array layer in question
	auto layer = static_cast<le_resource_manager_o::image_data_layer_t*>( user_data );
	logger().info( "Reloading file: %s", path );
	update_image_array_layer( *layer );
}

// ----------------------------------------------------------------------

static le_resource_manager_o* le_resource_manager_create() {
	auto self = new le_resource_manager_o{};

	// Register default file types with resource manager -
	// the decoder for these files is provided by le_pixels
	// which is a dependency of le_resource_manager.

	// Feel free to add (and override) decoders at runtime -
	// you can do so by invoking
	//
	// `le_resource_manager_update_decoder_interface_for_filetype`
	// (see definition of this function further below)
	//
	// Image decoders may be provided by any module that implements
	// the abstract `le_image_decoder_interface_t` interface.

	auto& interface = le_resource_manager_api_i->le_resource_manager_i;

	interface.set_decoder_interface_for_filetype( self, "png", le_pixels_api_i->le_pixels_image_decoder_i );
	interface.set_decoder_interface_for_filetype( self, "tga", le_pixels_api_i->le_pixels_image_decoder_i );
	interface.set_decoder_interface_for_filetype( self, "jpg", le_pixels_api_i->le_pixels_image_decoder_i );
	interface.set_decoder_interface_for_filetype( self, "jpeg", le_pixels_api_i->le_pixels_image_decoder_i );

	return self;
}

// ----------------------------------------------------------------------

static void le_resource_manager_destroy( le_resource_manager_o* self ) {

	using namespace le_pixels;

	if ( self->file_watcher ) {
		le_file_watcher::le_file_watcher_i.destroy( self->file_watcher );
	}

	for ( auto& [ k, r ] : self->resources ) {
		for ( auto& l : r.image_layers ) {
			if ( l.image_decoder ) {
				l.decoder_i->destroy_image_decoder( l.image_decoder );
				l.image_decoder = nullptr;
			}
		}
	}
	delete ( self );
}

// ----------------------------------------------------------------------

static void le_resource_manager_update( le_resource_manager_o* self, le_rendergraph_o* rg ) {
	using namespace le_renderer;

	// Poll for any files that might have changed on-disk -
	// this will trigger callbacks for any files which have changed.
	if ( self->file_watcher ) {
		le_file_watcher::le_file_watcher_i.poll_notifications( self->file_watcher );
	}

	for ( auto& [ k, r ] : self->resources ) {
		rendergraph_i.declare_resource( rg, k, r.image_info );
	}

	auto renderPassTransfer =
	    le::RenderPass( "xfer_le_resource_manager", le::QueueFlagBits::eTransfer )
	        .setSetupCallback( self, setupTransferPass )  // decide whether to go forward
	        .setExecuteCallback( self, execTransferPass ) //
	    ;

	rendergraph_i.add_renderpass( rg, renderPassTransfer );
}

// ----------------------------------------------------------------------

le_image_decoder_interface_t* le_resource_manager_get_decoder_interface_for_file( le_resource_manager_o* self, std::string const& path ) {

	std::size_t last_dot_pos = path.find_last_of( '.' );
	std::string file_extension;
	file_extension.reserve( 4 );

	if ( last_dot_pos != std::string::npos ) {

		// lowercase the file extension string so that we have it
		// in the canonical form which we may look up in our map
		size_t sz = path.size();
		for ( size_t i = last_dot_pos + 1; i != sz; i++ ) {
			file_extension.push_back( std::tolower( path[ i ] ) );
		}

		// Try to find the file extension in our map - if found,
		// return the image decoder interface that was associated
		// with this file extension.
		//
		auto it = self->available_decoder_interfaces.find( file_extension );
		if ( it != self->available_decoder_interfaces.end() ) {
			return it->second;
		} else {
			return nullptr;
		}
	}
	// could not find a valid decoder interface for this file extension
	return nullptr;
}

// ----------------------------------------------------------------------
// NOTE: You must provide an array of paths in image_paths, and the
// array's size must match `image_info.image.arrayLayers`
// Most meta-data about the image file is loaded via image_info
static void le_resource_manager_add_item( le_resource_manager_o*          self,
                                          le_image_resource_handle const* image_handle,
                                          le_resource_info_t const*       image_info,
                                          char const**                    image_paths,
                                          bool                            should_watch ) {

    auto [ it, was_emplaced ] = self->resources.emplace( *image_handle, le_resource_manager_o::resource_item_t{} );

	if ( was_emplaced ) {
		auto& item = it->second;

		item.image_handle = *image_handle;
		item.image_info   = *image_info;
		item.image_layers.reserve( image_info->image.arrayLayers );

		bool extents_inferred = false;
		if ( item.image_info.image.extent.width == 0 ||
		     item.image_info.image.extent.height == 0 ||
		     item.image_info.image.extent.depth == 0 ) {
			extents_inferred = true;
		}

		// item.image_info.image.format = le::Format::eUndefined;

		{
			for ( int i = 0; i != image_info->image.arrayLayers; i++ ) {
				auto l             = le_resource_manager_o::image_data_layer_t{};
				l.path             = image_paths[ i ];
				l.was_uploaded     = false;
				l.image_info       = &item.image_info.image;
				l.extents_inferred = extents_inferred;

				// Pick image decoder api based on abstract image decoder that was potentially added at runtime,
				// Depending on the file's extension.
				l.decoder_i = le_resource_manager_get_decoder_interface_for_file( self, l.path );

				if ( l.decoder_i == nullptr ) {
					logger().warn( "Could not find image decoder for image layer sourced from file: '%s', skipping.", l.path.c_str() );
					continue;
				}

				update_image_array_layer( l );

				if ( item.image_info.image.format == le::Format::eUndefined ) {
					item.image_info.image.format = l.image_info->format;
				}

				if ( item.image_info.image.format != l.image_info->format ) {
					logger().error( "Layers must have consistent image format." );
					continue;
				}

				item.image_layers.emplace_back( l );
			}
		}

		if ( should_watch ) {
			if ( nullptr == self->file_watcher ) {
				self->file_watcher = le_file_watcher::le_file_watcher_i.create();
			}
			for ( auto& l : item.image_layers ) {
				le_file_watcher_watch_settings watch_settings{};
				watch_settings.filePath           = l.path.c_str();
				watch_settings.callback_fun       = le_core_forward_callback( le_resource_manager::api->le_resource_manager_private_i.file_watcher_callback );
				watch_settings.callback_user_data = &l;
				l.watch_id                        = le_file_watcher::le_file_watcher_i.add_watch( self->file_watcher, &watch_settings );
			}
		}

		assert( item.image_info.image.extent.width != 0 &&
		        item.image_info.image.extent.height != 0 &&
		        item.image_info.image.extent.depth != 0 &&
		        "Image extents for resource are not valid." );
	} else {
		logger().error( "Resource '%s' was added more than once.", ( *image_handle )->data->debug_name );
	}
}

// ----------------------------------------------------------------------

static bool le_resource_manager_remove_item( le_resource_manager_o* self, le_image_resource_handle const* resource_handle ) {

	// TODO
	// SAFETY: As soon as the le_command_buffer recording phase has completed,
	// we can dispose of any items from self->resources that were referenced
	// up to this point.
	//
	// This means that we should protect any resources that are currently being
	// uploaded by keeping them until the recording step has been completed.
	//
	// Until this is implemented, we should not consider this method safe.

	auto it = self->resources.find( *resource_handle );

	if ( it == self->resources.end() ) {
		logger().warn( "Could not remove resource. Resource '%s' not found.", ( *resource_handle )->data->debug_name );
		return false;
	}

	// ----------| Invariant: Resource was found

	auto [ k, r ] = *it;

	for ( auto& l : r.image_layers ) {

		if ( l.watch_id != -1 ) {
			le_file_watcher::le_file_watcher_i.remove_watch( self->file_watcher, l.watch_id );
			l.watch_id = -1;
		}
		if ( l.image_decoder ) {
			l.decoder_i->destroy_image_decoder( l.image_decoder );
			l.image_decoder = nullptr;
		}
	}

	self->resources.erase( it );

	return true;
}
// ----------------------------------------------------------------------

static void le_resource_manager_set_decoder_interface_for_filetype( le_resource_manager_o* self, const char* file_extension, le_image_decoder_interface_t* decoder_interface ) {

	// First we must lowercase the file extension
	std::string file_ext;
	for ( char const* c = file_extension; *c != 0; c++ ) {
		if ( ( *c ) == '.' ) {
			logger().warn( "Do not include dot ('.') in file extension when updating image decoder interface: '%s'",
			               file_extension );
			continue;
		}
		file_ext.push_back( std::tolower( *c ) );
	}

	if ( file_ext.size() == 0 ) {
		logger().warn( "Could not register file extension: '%s'", file_extension );
		return;
	}

	// find out if we're replacing an existing interface
	auto it                          = self->available_decoder_interfaces.find( file_ext );
	bool interface_did_already_exist = ( it != self->available_decoder_interfaces.end() );

	// replace or update the interface
	self->available_decoder_interfaces[ file_ext ] = decoder_interface;

	if ( interface_did_already_exist ) {
		logger().info( "Updated    interface for file extension: '%s'", file_ext.c_str() );
	} else {
		logger().info( "Registered interface for file extension: '%s'", file_ext.c_str() );
	}
};

// ----------------------------------------------------------------------

LE_MODULE_REGISTER_IMPL( le_resource_manager, api ) {
	auto& le_resource_manager_i         = static_cast<le_resource_manager_api*>( api )->le_resource_manager_i;
	auto& le_resource_manager_private_i = static_cast<le_resource_manager_api*>( api )->le_resource_manager_private_i;

	le_resource_manager_i.create      = le_resource_manager_create;
	le_resource_manager_i.destroy     = le_resource_manager_destroy;
	le_resource_manager_i.update      = le_resource_manager_update;
	le_resource_manager_i.add_item    = le_resource_manager_add_item;
	le_resource_manager_i.remove_item = le_resource_manager_remove_item;

	le_resource_manager_i.set_decoder_interface_for_filetype = le_resource_manager_set_decoder_interface_for_filetype;

	le_resource_manager_private_i.file_watcher_callback = le_resource_manager_file_watcher_callback;
}
