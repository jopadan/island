#include "le_core.h"
#include "le_hash_util.h" // fixme-we shouldn't do that.

#include "le_renderer.h"

#include "le_backend_vk.h"
#include "le_swapchain_vk.h"
#include "le_swapchain_khr.h"
#include "le_log.h"
#include "le_debug_print_text.h"

#include <iostream>
#include <iomanip>
#include <vector>
#include "assert.h"
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <cstring> // for memcpy
#include <bitset>

#include "private/le_renderer/le_resource_handle_t.inl"
#include "private/le_renderer/le_rendergraph.h"

#include "le_tracy.h"

const uint64_t LE_RENDERPASS_MARKER_EXTERNAL = hash_64_fnv1a_const( "rp-external" );

using NanoTime = std::chrono::time_point<std::chrono::high_resolution_clock>;

#include "le_jobs.h"

#ifndef LE_MT
#	define LE_MT 0
#endif

// ----------------------------------------------------------------------
// ffdecl.
static le_swapchain_handle renderer_add_swapchain( le_renderer_o* self, le_swapchain_settings_t const* settings );
// ----------------------------------------------------------------------

struct FrameData {

	enum class State : int64_t {
		eFailedClear    = -4,
		eFailedDispatch = -3,
		eFailedAcquire  = -2,
		eInitial        = -1,
		eCleared        = 0,
		eAcquired,
		eRecorded,
		eProcessed,
		eDispatched,
	};

	State state = State::eInitial;

	le_rendergraph_o* rendergraph = nullptr;

	size_t frameNumber = size_t( ~0 );
};

struct le_texture_handle_t {
	std::string debug_name;
};

struct le_texture_handle_store_t {
	std::unordered_multimap<std::string, le_texture_handle_t> texture_handles;
	std::mutex                                                mtx;
};

struct le_resource_handle_store_t {
	std::unordered_multimap<le_resource_handle_data_t, le_resource_handle_t, le_resource_handle_data_hash> resource_handles;
	std::mutex                                                                                             mtx;
};

static le_texture_handle_store_t* get_texture_handle_library( bool erase = false ) {

	static le_texture_handle_store_t* texture_handle_library = nullptr;

	if ( erase ) {
		delete texture_handle_library;
		void** texture_handle_library_ptr = le_core_produce_dictionary_entry( hash_64_fnv1a_const( "texture_handle_library" ) );
		*texture_handle_library_ptr       = nullptr; // null pointer stored in global store
		texture_handle_library            = nullptr; // null pointer stored local store
		return nullptr;                              // return nullptr
	}

	if ( texture_handle_library ) {
		return texture_handle_library;
	}

	// ----------| Invariant: not yet in local store
	void** texture_handle_library_ptr = le_core_produce_dictionary_entry( hash_64_fnv1a_const( "texture_handle_library" ) );

	if ( *texture_handle_library_ptr ) {
		// Found in global store
		texture_handle_library = static_cast<le_texture_handle_store_t*>( *texture_handle_library_ptr );
	} else {
		// Not yet available in global store - create & make available
		texture_handle_library      = new le_texture_handle_store_t{};
		*texture_handle_library_ptr = texture_handle_library;
	}

	return texture_handle_library;
}

static le_resource_handle_store_t* get_resource_handle_library( bool erase = false ) {
	static le_resource_handle_store_t* resource_handle_library = nullptr;

	if ( erase ) {
		delete resource_handle_library;
		void** resource_handle_library_ptr = le_core_produce_dictionary_entry( hash_64_fnv1a_const( "resource_handle_library" ) );
		*resource_handle_library_ptr       = nullptr; // null pointer stored in global store
		resource_handle_library            = nullptr; // null pointer stored in local store
		return nullptr;                               // return nullptr
	}

	if ( resource_handle_library ) {
		return resource_handle_library;
	}

	// ----------| Invariant: not yet in local store
	void** resource_handle_library_ptr = le_core_produce_dictionary_entry( hash_64_fnv1a_const( "resource_handle_library" ) );

	if ( *resource_handle_library_ptr ) {
		// Found in global store
		resource_handle_library = static_cast<le_resource_handle_store_t*>( *resource_handle_library_ptr );
	} else {
		// Not yet available in global store - create & make available.
		resource_handle_library      = new le_resource_handle_store_t();
		*resource_handle_library_ptr = resource_handle_library;
	}

	return resource_handle_library;
}

// ----------------------------------------------------------------------

struct le_renderer_o {
	// uint64_t      swapchainDirty = false;
	le_backend_o* backend = nullptr; // Owned, created in setup

	std::vector<FrameData>           frames;
	size_t                           backendDataFramesCount = 0;
	size_t                           currentFrameNumber     = 0;  // ever increasing number of current frame
	le_renderer_settings_t           settings               = {}; // initial settings for the renderer - these will be used on setup()
	le_swapchain_windowed_settings_t default_windowed_swapchain_setting;
};

static void renderer_clear_frame( le_renderer_o* self, size_t frameIndex ); // ffdecl

// ----------------------------------------------------------------------

static le_renderer_o* renderer_create() {
	auto obj = new le_renderer_o();

	if ( LE_MT > 0 ) {
		le_jobs::initialize( LE_MT );
	}

	using namespace le_backend_vk;
	obj->backend = vk_backend_i.create();

	return obj;
}

// ----------------------------------------------------------------------

// creates a new handle if no name was given, or given name was not found in list of current handles.
static le_texture_handle renderer_produce_texture_handle( char const* maybe_name ) {

	// lock handle library for reading/writing
	static le_texture_handle_store_t* texture_handle_library = get_texture_handle_library();
	std::scoped_lock                  lock( texture_handle_library->mtx );

	le_texture_handle handle;

	if ( maybe_name ) {
		// if a string was given, search for multimap and see if we can find something.
		auto it = texture_handle_library->texture_handles.find( maybe_name );
		if ( it == texture_handle_library->texture_handles.end() ) {
			// not found, insert a new element
			handle = &texture_handle_library->texture_handles.emplace( maybe_name, le_texture_handle_t( { maybe_name } ) )->second;
		} else {
			// found, return a pointer to the found element
			handle = &it->second;
		}
	} else {
		// no name given: handle is set to address of newly inserted element
		// As this is a multimap, there can be any number of textures with the same
		// key "unnamed" in the map.
		handle = &texture_handle_library->texture_handles.emplace( "unnamed", le_texture_handle_t{} )->second;
	}

	// handle is a pointer to the element in the container, and as such it is
	// guaranteed to stay valid, even through rehashes of the texture_handles
	// container, because that's a guarantee that maps give us in c++, until
	// the element gets erased.

	return handle;
}

// ----------------------------------------------------------------------

static char const* texture_handle_get_name( le_texture_handle texture ) {
	if ( texture && !texture->debug_name.empty() ) {
		return texture->debug_name.c_str();
	} else {
		return nullptr;
	}
}

// creates a new resource if no name was given, or given name was not found in list of current handles.
le_resource_handle renderer_produce_resource_handle(
    char const*           maybe_name,
    LeResourceType const& resource_type,
    uint8_t               num_samples      = 0,
    uint8_t               flags            = 0,
    uint16_t              index            = 0,
    le_resource_handle    reference_handle = nullptr ) {

	static le_resource_handle_store_t* resource_handle_library = get_resource_handle_library();
	// lock handle library for reading/writing
	std::scoped_lock lock( resource_handle_library->mtx );

	le_resource_handle handle;

	le_resource_handle_data_t* p_data = new le_resource_handle_data_t{};
	p_data->flags                     = flags;
	p_data->num_samples               = num_samples;
	p_data->reference_handle          = reference_handle;
	p_data->type                      = resource_type;
	p_data->index                     = index;

	if ( maybe_name && maybe_name[ 0 ] != '\0' ) {
		memcpy( p_data->debug_name, maybe_name, sizeof( p_data->debug_name ) );
		// if a string was given, search for multimap and see if we can find something.
		auto it = resource_handle_library->resource_handles.find( *p_data );
		if ( it == resource_handle_library->resource_handles.end() ) {
			// not found, insert a new element
			handle = &resource_handle_library->resource_handles.emplace( *p_data, le_resource_handle_t{ p_data } )->second;
		} else {
			// found, return a pointer to the found element
			handle = &it->second;
			delete ( p_data );
		}
	} else {
		// no name given: handle is set to address of newly inserted element
		// As this is a multimap, there can be any number of textures with the same
		// key "unnamed" in the map.
		handle = &resource_handle_library->resource_handles.emplace( *p_data, le_resource_handle_t{ p_data } )->second;
		// we tag the element with a debug name that contains the handle so that
		// the debug name is unique.
		sprintf( handle->data->debug_name, "[%p]", handle );
	}

	// handle is a pointer to the element in the container, and as such it is
	// guaranteed to stay valid, even through rehashes of the resource_handle_library
	// container, because that's a guarantee that maps give us in c++, until
	// the element gets erased.

	return handle;
}

// ----------------------------------------------------------------------

static le_image_resource_handle renderer_produce_img_resource_handle( char const* maybe_name, uint8_t num_samples,
                                                                      le_image_resource_handle reference_handle, uint8_t flags ) {
	return static_cast<le_image_resource_handle>(
	    renderer_produce_resource_handle( maybe_name, LeResourceType::eImage, num_samples, flags, 0,
	                                      static_cast<le_resource_handle>( reference_handle ) ) );
}

// ----------------------------------------------------------------------

static le_buffer_resource_handle renderer_produce_buf_resource_handle( char const* maybe_name, uint8_t flags, uint16_t index ) {
	return static_cast<le_buffer_resource_handle>( renderer_produce_resource_handle( maybe_name, LeResourceType::eBuffer, 0, flags, index ) );
}

// ----------------------------------------------------------------------

static le_tlas_resource_handle renderer_produce_tlas_resource_handle( char const* maybe_name ) {
	return static_cast<le_tlas_resource_handle>( renderer_produce_resource_handle( maybe_name, LeResourceType::eRtxTlas ) );
}

// ----------------------------------------------------------------------

static le_blas_resource_handle renderer_produce_blas_resource_handle( char const* maybe_name ) {
	return static_cast<le_blas_resource_handle>( renderer_produce_resource_handle( maybe_name, LeResourceType::eRtxBlas ) );
}
// ----------------------------------------------------------------------

static void renderer_destroy( le_renderer_o* self ) {

	using namespace le_renderer; // for rendergraph_i

	const auto& lastIndex = self->currentFrameNumber;

	for ( size_t i = 0; i != self->frames.size(); ++i ) {
		auto index = ( lastIndex + i ) % self->frames.size();
		renderer_clear_frame( self, index );
		// -- FIXME: delete graph builders which we added in create.
		// This is not elegant.
		rendergraph_i.destroy( self->frames[ index ].rendergraph );
	}

	self->frames.clear();

	// Delete texture handle library
	get_texture_handle_library( false );

	{
		le_resource_handle_store_t* resource_handle_library = get_resource_handle_library();
		if ( resource_handle_library ) {
			// we must deallocate manually allocated data for resource handles
			for ( auto& e : resource_handle_library->resource_handles ) {
				delete ( e.second.data );
			}
			// Delete static pointer to resource handle library
			get_resource_handle_library( true );
		}
	}

	if ( self->backend ) {
		// Destroy the backend, as it is owned by the renderer
		using namespace le_backend_vk;
		vk_backend_i.destroy( self->backend );
		self->backend = nullptr;
	}

	// Destroy any swapchain settings objects that were created by cloning
	// settings in setup()
	//
	// Note that we don't increment s in the for loop main clause, but in
	// the body of the loop, because we must fetch the next pointer before
	// we destroy the current link in the linked list.
	for ( auto s = self->settings.swapchain_settings; s != nullptr; ) {
		le_swapchain_settings_t* next_setting = s->p_next;
		le_swapchain_vk_api_i->swapchain_i.settings_destroy( s );
		s = next_setting;
	}

#if ( LE_MT > 0 )
	le_jobs::terminate();
#endif

	delete self;
}

// ----------------------------------------------------------------------

static le_rtx_blas_info_handle renderer_create_rtx_blas_info_handle( le_renderer_o* self, le_rtx_geometry_t* geometries, uint32_t geometries_count, le::BuildAccelerationStructureFlagsKHR const* flags ) {
	using namespace le_backend_vk;
	return vk_backend_i.create_rtx_blas_info( self->backend, geometries, geometries_count, flags );
}

// ----------------------------------------------------------------------

static le_rtx_tlas_info_handle renderer_create_rtx_tlas_info_handle( le_renderer_o* self, uint32_t instances_count, le::BuildAccelerationStructureFlagsKHR const* flags ) {
	using namespace le_backend_vk;
	return vk_backend_i.create_rtx_tlas_info( self->backend, instances_count, flags );
}

// ----------------------------------------------------------------------

static le_backend_o* renderer_get_backend( le_renderer_o* self ) {
	return self->backend;
}

// ----------------------------------------------------------------------

static le_pipeline_manager_o* renderer_get_pipeline_manager( le_renderer_o* self ) {
	using namespace le_backend_vk;
	return vk_backend_i.get_pipeline_cache( self->backend );
}

// ----------------------------------------------------------------------

// Only returns true if capablilites for **all** swapchains given in settings
// were successfully requested.
static bool renderer_request_swapchain_capabilities( le_renderer_o* self, le_swapchain_settings_t const* settings ) {

	// Request extensions from the backend - this must only be called
	// before or while renderer-setup() is called for the first time.

	using namespace le_swapchain_vk;
	bool result = true;

	for ( auto s = settings; s != nullptr; s = s->p_next ) {
		// swapchain_i.get_min_max_image_count( settings );
		result &= swapchain_i.request_backend_capabilities( s );
	}

	return result;
}

// ----------------------------------------------------------------------

static void renderer_setup( le_renderer_o* self, le_renderer_settings_t const* settings_ ) {
	static auto logger = LeLog( "le_renderer" );

	// We store swapchain settings with the renderer so that we can pass
	// backend a permanent pointer to it.

	if ( settings_ ) {
		self->settings = *settings_;

		// Make a local copy of swapchain_settings linked list by cloning
		// settings objects (we clone because swapchain_settings may be
		// derived, and clone() will copy the correct content depending
		// on swapchain_setting type.)
		//
		// Note that this means that we must destroy all settings that we
		// have created when we tear down the renderer.
		{
			// make a copy of settings linked list
			le_swapchain_settings_t* list_start = nullptr;
			le_swapchain_settings_t* list_end   = nullptr;
			for ( auto s = settings_->swapchain_settings; s != nullptr; s = s->p_next ) {
				if ( list_start == nullptr ) {
					list_start = le_swapchain_vk_api_i->swapchain_i.settings_clone( s );
					list_end   = list_start;
				} else {
					list_end->p_next = le_swapchain_vk_api_i->swapchain_i.settings_clone( s );
					list_end         = list_end->p_next;
				}
			}
			self->settings.swapchain_settings = list_start;
		}
	} else {
		self->settings = {};
	}

	{
		// Before we can initialise the backend, we must query for any required
		// capabilities and extensions that come implied via swapchains:

		// TODO: we must make sure that swapchains have requested capabilities
		// at this point in time.
		bool has_correct_backend_capablities = renderer_request_swapchain_capabilities( self, self->settings.swapchain_settings );

		if ( !has_correct_backend_capablities ) {
			logger.error( "Could not get all requested backend capabilies." );
		}

#if ( LE_MT > 0 )
		le_backend_vk::settings_i.set_concurrency_count( LE_MT );
#endif

		// We can now initialize the backend so that it hopefully conforms to
		// any requirements and capabilities that have been requested so far...
		//
		le_backend_vk::vk_backend_i.initialise( self->backend );

		// We setup our backend so that the backend's allocator becomes
		// available in case that any swapchain implementation needs to allocate resources
		// via the backend. (This is currently the case with the image swapchain, which
		// will use the backend to allocate its target images)
		le_backend_vk::vk_backend_i.setup( self->backend );

		// Now that we have backend device and instance, we can use this to
		// create surfaces for swapchains for example.
		//
		// The first added swapchain will try to set the number of data frames
		//  - via the global backend_settings singleton -
		// so that the number of data frames is less or equal to the number of
		// available images in the swapchain.
		//
		// swapchain settings are a linked list - which means that you must have set these up before,
		for ( le_swapchain_settings_t* s = self->settings.swapchain_settings; s != nullptr; s = s->p_next ) {
			renderer_add_swapchain( self, s );
		}
	}

	self->backendDataFramesCount = le_backend_vk::vk_backend_i.get_data_frames_count( self->backend );

	using namespace le_renderer; // for rendergraph_i
	self->frames.reserve( self->backendDataFramesCount );

	for ( size_t i = 0; i != self->backendDataFramesCount; ++i ) {
		auto frameData        = FrameData();
		frameData.rendergraph = rendergraph_i.create();
		self->frames.push_back( std::move( frameData ) );
	}

	self->currentFrameNumber = 0;
}

// ----------------------------------------------------------------------
// Applies window to first found window swapchain in renderer->settings
// if no window swapchain is found, a new one is added.
static void renderer_setup_with_window( le_renderer_o* self, le_window_o* window ) {
	static auto logger = LeLog( "le_renderer" );

	le_swapchain_settings_t* last_settings            = nullptr;
	bool                     found_matching_swapchain = false;
	if ( self->settings.swapchain_settings ) {

		// If swapchain settings exist, we set the window to the first window
		// swapchain that has not yet a setting for window.

		for ( auto s = self->settings.swapchain_settings; s != nullptr; s = s->p_next ) {
			last_settings = s;
			if ( s->type == le_swapchain_settings_t::LE_KHR_SWAPCHAIN ) {

				auto sw = reinterpret_cast<le_swapchain_windowed_settings_t*>( s );
				if ( nullptr == sw->window ) {
					logger.info( "Applied window pointer to first window swapchain found in"
					             "renderer settings." );

					sw->window               = window;
					found_matching_swapchain = true;
				}
				break;
			}
		}

		// if no window swapchain was found, add one.

		if ( false == found_matching_swapchain ) {
			self->default_windowed_swapchain_setting.window = window;
			last_settings->p_next                           = &self->default_windowed_swapchain_setting.base;
			logger.info( "Inserted window swapchain into renderer settings." );
		}

	} else {
		// swapchain settings do not exist, we add a default window swapchain object

		self->default_windowed_swapchain_setting.window = window;
		self->settings.swapchain_settings               = &self->default_windowed_swapchain_setting.base;
	}

	renderer_setup( self, &self->settings );
}

// ----------------------------------------------------------------------

static le_renderer_settings_t const* renderer_get_settings( le_renderer_o* self ) {
	return &self->settings;
}

// ----------------------------------------------------------------------

static size_t renderer_get_current_frame_number( le_renderer_o* self ) {
	return self->currentFrameNumber;
}

// ----------------------------------------------------------------------

static void renderer_clear_frame( le_renderer_o* self, size_t frameIndex ) {

	auto& frame = self->frames[ frameIndex ];

	using namespace le_backend_vk; // for vk_bakend_i
	using namespace le_renderer;   // for rendergraph_i

	if ( frame.state == FrameData::State::eCleared ) {
		return;
	}

	// ----------| invariant: frame was not yet cleared

	// + ensure frame fence has been reached
	if ( frame.state == FrameData::State::eDispatched ||
	     frame.state == FrameData::State::eFailedDispatch ||
	     frame.state == FrameData::State::eFailedClear ) {

		while ( false == vk_backend_i.poll_frame_fence( self->backend, frameIndex ) ) {
			// Note: this call may block until the fence has been reached.
#if ( LE_MT > 0 )
			le_jobs::yield();
#endif
		}

		bool result = vk_backend_i.clear_frame( self->backend, frameIndex );

		if ( result != true ) {
			frame.state = FrameData::State::eFailedClear;
			return;
		}
	}

	rendergraph_i.reset( frame.rendergraph );

	//	std::cout << "CLEAR FRAME " << frameIndex << std::endl
	//	          << std::flush;

	frame.state = FrameData::State::eCleared;
}

// ----------------------------------------------------------------------

static void renderer_record_frame( le_renderer_o* self, size_t frameIndex, le_rendergraph_o* graph_, size_t frameNumber ) {
	static auto logger = LeLog( "le_renderer" );

	ZoneScoped;

	// High-level
	// - resolve rendergraph: which render passes do contribute?
	// - consolidate resources, synchronisation for resources
	// - For each render pass, call renderpass' render method, build intermediary command lists

	auto& frame       = self->frames[ frameIndex ];
	frame.frameNumber = frameNumber;

	if ( frame.state != FrameData::State::eCleared && frame.state != FrameData::State::eInitial ) {
		return;
	}

	// ---------| invariant: Frame was previously acquired successfully.

	// - build up dependencies for graph, create table of unique resources for graph

	// setup passes calls `setup` callback on all passes - this initalises virtual resources,
	// and stores their descriptors (information needed to allocate physical resources)
	//
	using namespace le_renderer; // for rendergraph_i, rendergraph_i
	le_renderer::api->le_rendergraph_private_i.setup_passes( graph_, frame.rendergraph );

	// Find out which renderpasses contribute, only add contributing render passes to
	// rendergraph
	le_renderer::api->le_rendergraph_private_i.build( frame.rendergraph, frameNumber );

	if ( le::DebugPrint::needsDraw() ) {

		// If there are debug messages to print to screen, we must draw them onto the last
		// renderpass.
		//
		// This assumes that the last renderpass is the renderpass that goes to the screen.
		// If there is no last renderpass, then we must warn about this.
		//
		if ( !frame.rendergraph->passes.empty() ) {
			le::DebugPrint::drawAllMessages( frame.rendergraph->passes.back() );
		} else {
			logger.warn( "le::DebugPrint has messages, but no way to print them. Discarding messages." );
			le::DebugPrint::drawAllMessages( nullptr );
		}
	}

	// Register any clear callbacks for the current frame so that any object with frame lifetime
	// can be cleaned up on frame clear:
	if ( !frame.rendergraph->on_frame_clear_callbacks.empty() ) {
		le_backend_vk::private_backend_vk_i.frame_add_on_clear_callbacks(
		    self->backend, frameIndex,
		    frame.rendergraph->on_frame_clear_callbacks.data(),
		    frame.rendergraph->on_frame_clear_callbacks.size() );
	}

	// declare any resources that come from swapchains
	le_backend_vk::vk_backend_i.acquire_swapchain_resources( self->backend, frameIndex );

	// Execute callbacks into main application for each render pass,
	// build command lists per render pass in intermediate, api-agnostic representation
	//
	le_renderer::api->le_rendergraph_private_i.execute( frame.rendergraph, frameIndex, self->backend );

	frame.state = FrameData::State::eRecorded;
}

// ----------------------------------------------------------------------

static const FrameData::State& renderer_acquire_backend_resources( le_renderer_o* self, size_t frameIndex ) {

	ZoneScoped;
	using namespace le_backend_vk; // for vk_bakend_i
	using namespace le_renderer;   // for rendergraph_i

	// ---------| invariant: There are frames to process.

	auto& frame = self->frames[ frameIndex ];

	if ( frame.state != FrameData::State::eRecorded ) {
		return frame.state;
	}

	// ----------| invariant: frame is either initial, or cleared.

	le_renderpass_o** passes          = frame.rendergraph->passes.data();
	size_t            numRenderPasses = frame.rendergraph->passes.size();

	le_resource_handle const* declared_resources       = frame.rendergraph->declared_resources_id.data();
	le_resource_info_t const* declared_resources_infos = frame.rendergraph->declared_resources_info.data();
	size_t                    declared_resources_count = frame.rendergraph->declared_resources_id.size();

	vk_backend_i.acquire_physical_resources(
	    self->backend,
	    frameIndex,
	    passes,
	    numRenderPasses,
	    declared_resources,
	    declared_resources_infos,
	    declared_resources_count );

	{
		// apply root node affinity masks to backend render frame
		// so that the frame can decide how best to dispatch
		le::RootPassesField const* p_affinity_masks   = frame.rendergraph->root_passes_affinity_masks.data();
		uint32_t                   num_affinity_masks = frame.rendergraph->root_passes_affinity_masks.size();

		vk_backend_i.set_frame_queue_submission_keys(
		    self->backend, frameIndex,
		    reinterpret_cast<void const*>( p_affinity_masks ), num_affinity_masks,
		    frame.rendergraph->root_debug_names.data(), frame.rendergraph->root_debug_names.size() );
	}

	frame.state = FrameData::State::eAcquired;

	return frame.state;
}

// ----------------------------------------------------------------------
// translate intermediate draw lists into vk command buffers, and sync primitives
static const FrameData::State& renderer_process_frame( le_renderer_o* self, size_t frameIndex ) {

	ZoneScoped;
	using namespace le_backend_vk; // for vk_bakend_i

	auto& frame = self->frames[ frameIndex ];

	if ( frame.state != FrameData::State::eAcquired ) {
		return frame.state;
	}
	// ---------| invariant: frame was previously recorded successfully

	// translate intermediate draw lists into vk command buffers, and sync primitives
	vk_backend_i.process_frame( self->backend, frameIndex );

	frame.state = FrameData::State::eProcessed;
	return frame.state;
}

// ----------------------------------------------------------------------

static void renderer_dispatch_frame( le_renderer_o* self, size_t frameIndex ) {

	ZoneScoped;
	using namespace le_backend_vk; // for vk_backend_i
	auto& frame = self->frames[ frameIndex ];

	if ( frame.state != FrameData::State::eProcessed ) {
		return;
	}

	// ---------| invariant: frame was successfully processed previously

	vk_backend_i.dispatch_frame( self->backend, frameIndex );

	frame.state = FrameData::State::eDispatched;
}

// ----------------------------------------------------------------------

static le_image_resource_handle renderer_get_swapchain_resource( le_renderer_o* self, le_swapchain_handle swapchain ) {
	ZoneScoped;
	using namespace le_backend_vk;
	return vk_backend_i.get_swapchain_resource( self->backend, swapchain );
}

static le_image_resource_handle renderer_get_swapchain_resource_default( le_renderer_o* self ) {
	ZoneScoped;
	using namespace le_backend_vk;
	return vk_backend_i.get_swapchain_resource_default( self->backend );
}

// ----------------------------------------------------------------------

static bool renderer_get_swapchain_extent( le_renderer_o* self, le_swapchain_handle swapchain, uint32_t* p_width, uint32_t* p_height ) {
	ZoneScoped;
	using namespace le_backend_vk;
	return vk_backend_i.get_swapchain_extent( self->backend, swapchain, p_width, p_height );
}

// ----------------------------------------------------------------------

static le_swapchain_handle renderer_add_swapchain( le_renderer_o* self, le_swapchain_settings_t const* settings ) {
	ZoneScoped;
	using namespace le_backend_vk;
	assert( self->backend && "Backend must exist" );
	return vk_backend_i.add_swapchain( self->backend, settings );
};
// ----------------------------------------------------------------------

static bool renderer_remove_swapchain( le_renderer_o* self, le_swapchain_handle swapchain ) {
	ZoneScoped;
	using namespace le_backend_vk;
	assert( self->backend && "Backend must exist" );
	return vk_backend_i.remove_swapchain( self->backend, swapchain );
};

static bool renderer_get_swapchains( le_renderer_o* self, size_t* num_swapchains, le_swapchain_handle* p_swapchain_handles ) {
	ZoneScoped;
	using namespace le_backend_vk;
	assert( self->backend && "Backend must exist" );
	return vk_backend_i.get_swapchains( self->backend, num_swapchains, p_swapchain_handles );
}

// ----------------------------------------------------------------------
static bool renderer_resize_swapchain( le_renderer_o* self, le_swapchain_handle swapchain, uint32_t width, uint32_t height ) {
	ZoneScoped;
	using namespace le_backend_vk;
	return vk_backend_i.recreate_swapchain( self->backend, swapchain, width, height );
};

// ----------------------------------------------------------------------

static void renderer_update( le_renderer_o* self, le_rendergraph_o* graph_ ) {
	ZoneScoped;

	static auto logger = LeLog( "le_renderer" );
	using namespace le_backend_vk;

	const auto& index     = self->currentFrameNumber;
	const auto& numFrames = self->frames.size();

	// If necessary, recompile and reload shader modules
	// - this must be complete before the record_frame step

	if ( LE_MT > 0 ) {
		// use task system (experimental)

		le_jobs::counter_t* shader_counter;

		le_jobs::job_t j{
		    []( void* backend ) {
			    vk_backend_i.update_shader_modules( static_cast<le_backend_o*>( backend ) );
		    },
		    self->backend };

		le_jobs::run_jobs( &j, 1, &shader_counter );

		struct frame_params_t {
			le_renderer_o* renderer;
			size_t         frame_index;
		};

		struct record_params_t {
			le_renderer_o*      renderer;
			size_t              frame_index;
			le_rendergraph_o*   rendergraph;
			size_t              current_frame_number;
			le_jobs::counter_t* shader_counter;
		};

		auto record_frame_fun = []( void* param_ ) {
			auto p = static_cast<record_params_t*>( param_ );
			// generate an intermediary, api-agnostic, representation of the frame

			le_jobs::wait_for_counter_and_free( p->shader_counter, 0 );
			renderer_record_frame( p->renderer, p->frame_index, p->rendergraph, p->current_frame_number );
		};

		auto process_frame_fun = []( void* param_ ) {
			auto p = static_cast<frame_params_t*>( param_ );
			// acquire external backend resources such as swapchain
			// and create any temporary resources
			renderer_acquire_backend_resources( p->renderer, p->frame_index );
			// generate api commands for the frame
			renderer_process_frame( p->renderer, p->frame_index );
			// send api commands to GPU queue for processing
			renderer_dispatch_frame( p->renderer, p->frame_index );
		};

		auto clear_frame_fun = []( void* param_ ) {
			auto p = static_cast<frame_params_t*>( param_ );
			renderer_clear_frame( p->renderer, p->frame_index );
		};

		le_jobs::job_t jobs[ 3 ];

		record_params_t record_frame_params;
		record_frame_params.renderer             = self;
		record_frame_params.frame_index          = ( index + 0 ) % numFrames;
		record_frame_params.rendergraph          = graph_;
		record_frame_params.current_frame_number = self->currentFrameNumber;
		record_frame_params.shader_counter       = shader_counter;

		frame_params_t process_frame_params;
		process_frame_params.renderer    = self;
		process_frame_params.frame_index = ( index + 2 ) % numFrames;

		frame_params_t clear_frame_params;
		clear_frame_params.renderer    = self;
		clear_frame_params.frame_index = ( index + 1 ) % numFrames;

		jobs[ 0 ] = { process_frame_fun, &process_frame_params };
		jobs[ 1 ] = { clear_frame_fun, &clear_frame_params };
		jobs[ 2 ] = { record_frame_fun, &record_frame_params };

		le_jobs::counter_t* counter;

		assert( self->backend );

		le_jobs::run_jobs( jobs, 3, &counter );

		// we could theoretically do some more work on the main thread here...

		le_jobs::wait_for_counter_and_free( counter, 0 );

	} else {

		// render on the main thread
		vk_backend_i.update_shader_modules( self->backend );

		{
			// RECORD FRAME
			auto frameIndex = ( index + 0 ) % numFrames;
			// logger.info( "+++ [%5d] RECO", frameIndex );
			renderer_record_frame( self, frameIndex, graph_, self->currentFrameNumber ); // generate an intermediary, api-agnostic, representation of the frame
		}

		{
			// DISPATCH FRAME
			// acquire external backend resources such as swapchain
			// and create any temporary resources
			auto frameIndex = ( index + 2 ) % numFrames;
			// logger.info( "+++ [%5d] DISP", frameIndex );
			renderer_acquire_backend_resources( self, frameIndex ); //
			renderer_process_frame( self, frameIndex );             // generate api commands for the frame
			renderer_dispatch_frame( self, frameIndex );            //
		}

		{
			// CLEAR FRAME
			// wait for frame to come back (important to do this last, as it may block...)
			auto frameIndex = ( index + 1 ) % numFrames;
			// logger.info( "+++ [%5d] CLEA", frameIndex );
			renderer_clear_frame( self, frameIndex );
		}
	}

	// logger.info( "+++ NEXT FRAME\n" );
	++self->currentFrameNumber;
	FrameMark; // We have completed the current frame - this signals it to tracy
}

// ----------------------------------------------------------------------

static le_resource_info_t get_default_resource_info_for_image() {
	le_resource_info_t res = {};

	res.type = LeResourceType::eImage;
	{
		auto& img                   = res.image;
		img                         = {};
		img.flags                   = le::ImageCreateFlagBits( 0 );
		img.format                  = le::Format::eUndefined;
		img.arrayLayers             = 1;
		img.extent.width            = 0;
		img.extent.height           = 0;
		img.extent.depth            = 1;
		img.extent_from_pass.width  = 0;
		img.extent_from_pass.height = 0;
		img.extent_from_pass.depth  = 1;
		img.usage                   = le::ImageUsageFlags( le::ImageUsageFlagBits::eSampled );
		img.mipLevels               = 1;
		img.sample_count_log2       = 0; // 0 means 1, as (1 << 0 == 1)
		img.imageType               = le::ImageType::e2D;
		img.tiling                  = le::ImageTiling::eOptimal;
		img.samplesFlags            = 0;
	}

	return res;
}

// ----------------------------------------------------------------------

static le_resource_info_t get_default_resource_info_for_buffer() {
	le_resource_info_t res = {};
	res.type               = LeResourceType::eBuffer;
	res.buffer.size        = 0;
	res.buffer.usage       = le::BufferUsageFlags( le::BufferUsageFlagBits::eTransferDst );
	return res;
}

extern void register_le_rendergraph_api( void* api );            // in le_rendergraph.cpp
extern void register_le_command_buffer_encoder_api( void* api ); // in le_command_buffer_encoder.cpp

// ----------------------------------------------------------------------

LE_MODULE_REGISTER_IMPL( le_renderer, api ) {
	auto  le_renderer_api_i = static_cast<le_renderer_api*>( api );
	auto& le_renderer_i     = le_renderer_api_i->le_renderer_i;

	le_renderer_i.create                         = renderer_create;
	le_renderer_i.destroy                        = renderer_destroy;
	le_renderer_i.setup                          = renderer_setup;
	le_renderer_i.setup_with_window              = renderer_setup_with_window;
	le_renderer_i.update                         = renderer_update;
	le_renderer_i.get_settings                   = renderer_get_settings;
	le_renderer_i.get_current_frame_number       = renderer_get_current_frame_number;
	le_renderer_i.get_swapchain_extent           = renderer_get_swapchain_extent;
	le_renderer_i.get_pipeline_manager           = renderer_get_pipeline_manager;
	le_renderer_i.get_backend                    = renderer_get_backend;
	le_renderer_i.get_swapchain_resource         = renderer_get_swapchain_resource;
	le_renderer_i.get_swapchain_resource_default = renderer_get_swapchain_resource_default;
	le_renderer_i.add_swapchain                  = renderer_add_swapchain;
	le_renderer_i.remove_swapchain               = renderer_remove_swapchain;
	le_renderer_i.resize_swapchain               = renderer_resize_swapchain;
	le_renderer_i.get_swapchains                 = renderer_get_swapchains;
	le_renderer_i.produce_texture_handle         = renderer_produce_texture_handle;
	le_renderer_i.texture_handle_get_name        = texture_handle_get_name;
	le_renderer_i.create_rtx_blas_info           = renderer_create_rtx_blas_info_handle;
	le_renderer_i.create_rtx_tlas_info           = renderer_create_rtx_tlas_info_handle;

	auto& helpers_i = le_renderer_api_i->helpers_i;

	helpers_i.get_default_resource_info_for_buffer = get_default_resource_info_for_buffer;
	helpers_i.get_default_resource_info_for_image  = get_default_resource_info_for_image;
	le_renderer_i.produce_img_resource_handle      = renderer_produce_img_resource_handle;
	le_renderer_i.produce_buf_resource_handle      = renderer_produce_buf_resource_handle;
	le_renderer_i.produce_tlas_resource_handle     = renderer_produce_tlas_resource_handle;
	le_renderer_i.produce_blas_resource_handle     = renderer_produce_blas_resource_handle;

	// register sub-components of this api
	register_le_rendergraph_api( api );

	register_le_command_buffer_encoder_api( api );
	LE_LOAD_TRACING_LIBRARY;
}
