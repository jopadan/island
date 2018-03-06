#ifndef GUARD_LE_RENDERER_H
#define GUARD_LE_RENDERER_H

#include <stdint.h>
#include "pal_api_loader/ApiRegistry.hpp"

// depends on le_backend_vk. le_backend_vk must be loaded before this class is used.

#ifdef __cplusplus
extern "C" {
#endif

void register_le_renderer_api( void *api );

namespace vk {
    enum class Format; // forward declaration
	enum class AttachmentStoreOp;
	enum class AttachmentLoadOp;
}

namespace le {
    struct Viewport;
}

struct le_renderer_o;
struct le_render_module_o;
struct le_renderpass_o;
struct le_graph_builder_o;
struct le_command_buffer_encoder_o;
struct le_backend_o;


struct le_renderer_api {

	static constexpr auto id       = "le_renderer";
	static constexpr auto pRegFun  = register_le_renderer_api;

	struct renderer_interface_t {
		le_renderer_o* ( *create  ) (le_backend_o* backend);
		void           ( *destroy ) (le_renderer_o* obj);
		void           ( *setup   ) (le_renderer_o* obj);
		void           ( *update  ) (le_renderer_o* obj, le_render_module_o* module);
	};

	enum AccessFlagBits : uint8_t {
		eRead      = 0x01,
		eWrite     = 0x02,
		eReadWrite = eRead | eWrite,
	};

	struct image_attachment_info_o {
		uint64_t              id           = 0; // hash name given to this attachment, based on name string
		uint64_t              source_id    = 0; // hash name of writer/creator
		uint8_t               access_flags = 0; // read, write or readwrite
		vk::Format            format;
		vk::AttachmentLoadOp  loadOp;
		vk::AttachmentStoreOp storeOp;

		struct SyncState {
			    uint64_t idxInitial : 16;                 // info for renderpass load/clear op
				uint64_t idxFinal : 16;                   // info for last_subpass_to_external_dependency
				uint64_t reserved : 32;
		} syncState = {0,0,0};

		void ( *onClear )( void *clear_data ) = nullptr;
		char debugName[ 32 ];
	};

	typedef bool(*pfn_renderpass_setup_t)(le_renderpass_o* obj);
	typedef void(*pfn_renderpass_render_t)(le_command_buffer_encoder_o* encoder, void* user_data);

	struct renderpass_interface_t {
		le_renderpass_o* ( *create                ) (const char* renderpass_name);
		void             ( *destroy               ) (le_renderpass_o* obj);
		void             ( *set_setup_fun         ) (le_renderpass_o* obj, pfn_renderpass_setup_t setup_fun );
		void             ( *add_image_attachment  ) (le_renderpass_o* obj, const char*, image_attachment_info_o* info);
		void             ( *set_render_callback   ) (le_renderpass_o* obj, pfn_renderpass_render_t render_fun, void* user_data );
	};

	struct rendermodule_interface_t {
		le_render_module_o* ( *create)         ( );
		void                ( *destroy)        ( le_render_module_o* obj );
		void                ( *add_renderpass) ( le_render_module_o* obj, le_renderpass_o* rp );
		void                ( *build_graph)    ( le_render_module_o* obj, le_graph_builder_o* gb );
		void                ( *execute_graph ) ( le_render_module_o* obj, le_graph_builder_o* gb );
	};

	// graph builder builds a graph for a module
	struct graph_builder_interface_t {
		le_graph_builder_o* ( *create        ) ( );
		void                ( *destroy       ) ( le_graph_builder_o* obj, const le_renderer_api& api );
		void                ( *reset         ) ( le_graph_builder_o* obj, const le_renderer_api& api );
		void                ( *add_renderpass) ( le_graph_builder_o* obj, le_renderpass_o* rp );
		void                ( *build_graph   ) ( le_graph_builder_o* obj );
		void                ( *execute_graph ) ( le_graph_builder_o* obj, const le_renderer_api& api );
		void                ( *get_passes    ) ( le_graph_builder_o* obj, le_renderpass_o** pPasses, size_t* pNumPasses);
		void                ( *get_encoded_data_for_pass) (le_graph_builder_o* obj, size_t passIndex, void** data, size_t *numBytes, size_t* numCommands);
	};

	struct command_buffer_encoder_interface_t {
		le_command_buffer_encoder_o * ( *create         ) ( );
		void                          ( *destroy        ) ( le_command_buffer_encoder_o *obj );

		void                          ( *get_encoded_data )(le_command_buffer_encoder_o* self, void** data, size_t* numBytes, size_t* numCommands);

		void                          ( *set_line_width ) ( le_command_buffer_encoder_o* self, float line_width_);
		void                          ( *draw           ) ( le_command_buffer_encoder_o* self, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
		void                          ( *set_viewport   ) ( le_command_buffer_encoder_o* self, uint32_t firstViewport, const uint32_t viewportCount, const le::Viewport* pViewports);
	};

	renderpass_interface_t             le_renderpass_i;
	rendermodule_interface_t           le_render_module_i;
	graph_builder_interface_t          le_graph_builder_i;
	renderer_interface_t               le_renderer_i;
	command_buffer_encoder_interface_t le_command_buffer_encoder_i;
};

#ifdef __cplusplus
} // extern "C"


namespace le {

using ImageAttachmentInfo = le_renderer_api::image_attachment_info_o;
using AccessFlagBits      = le_renderer_api::AccessFlagBits;

class Renderer {
	const le_renderer_api &                      rendererApiI = *Registry::getApi<le_renderer_api>();
	const le_renderer_api::renderer_interface_t &rendererI    = rendererApiI.le_renderer_i;

	le_renderer_o *self;

  public:
	Renderer( le_backend_o *backend)
	    : self( rendererI.create( backend ) ) {
	}

	~Renderer() {
		rendererI.destroy( self );
	}

	void setup() {
		rendererI.setup( self );
	}

	void update( le_render_module_o *module ) {
		rendererI.update( self, module );
	}
};

class RenderPass {
	const le_renderer_api &                        rendererApiI = *Registry::getApi<le_renderer_api>();
	const le_renderer_api::renderpass_interface_t &renderpassI  = rendererApiI.le_renderpass_i;

	le_renderpass_o *self;

  public:
	RenderPass( const char *name_ )
	    : self( renderpassI.create( name_ ) ) {
	}

	~RenderPass() {
		renderpassI.destroy( self );
	}

	operator auto() {
		return self;
	}

	void setSetupCallback( le_renderer_api::pfn_renderpass_setup_t fun ) {
		renderpassI.set_setup_fun( self, fun );
	}

	void setRenderCallback(le_renderer_api::pfn_renderpass_render_t fun, void* user_data_ = nullptr){
		renderpassI.set_render_callback( self, fun, user_data_ );
	}

};

// ----------------------------------------------------------------------

class RenderPassRef {
	// non-owning version of RenderPass, but with more public methods
	const le_renderer_api &                        rendererApiI = *Registry::getApi<le_renderer_api>();
	const le_renderer_api::renderpass_interface_t &renderpassI  = rendererApiI.le_renderpass_i;

	le_renderpass_o *self = nullptr;

  public:
	RenderPassRef()  = delete;
	~RenderPassRef() = default;

	RenderPassRef( le_renderpass_o *self_ )
	    : self( self_ ) {
	}

	operator auto() {
		return self;
	}

	RenderPassRef &addImageAttachment( const char *name_, le_renderer_api::image_attachment_info_o *info ) {
		renderpassI.add_image_attachment( self, name_, info );
		return *this;
	}
};

// ----------------------------------------------------------------------

class GraphBuilder : NoCopy, NoMove {
	const le_renderer_api &                           rendererApiI  = *Registry::getApi<le_renderer_api>();
	const le_renderer_api::graph_builder_interface_t &graphbuilderI = rendererApiI.le_graph_builder_i;

	le_graph_builder_o *self;
	bool                is_reference = false;

  public:
	GraphBuilder()
	    : self( graphbuilderI.create() ) {
	}

	GraphBuilder( le_graph_builder_o *self_ )
	    : self( self_ )
	    , is_reference( true ) {
	}

	~GraphBuilder() {
		if ( !is_reference ) {
			graphbuilderI.destroy( self, rendererApiI);
		}
	}

	operator auto() {
		return self;
	}

	void reset() {
		graphbuilderI.reset( self, rendererApiI );
	}

	void addRenderpass( le_renderpass_o *rp ) {
		graphbuilderI.add_renderpass( self, rp );
	}

	void buildGraph() {
		graphbuilderI.build_graph( self );
	}

	void executeGraph() {
		graphbuilderI.execute_graph( self, rendererApiI );
	}

	void getPasses(le_renderpass_o**pPasses, size_t* numPasses){
		graphbuilderI.get_passes(self, pPasses,numPasses);
	}


};

// ----------------------------------------------------------------------

class RenderModule {
	const le_renderer_api &                          rendererApiI  = *Registry::getApi<le_renderer_api>();
	const le_renderer_api::rendermodule_interface_t &rendermoduleI = rendererApiI.le_render_module_i;

	le_render_module_o *self;
	bool                is_reference = false;

  public:
	RenderModule(  )
	    : self( rendermoduleI.create( ) ) {
	}

	RenderModule( le_render_module_o *self_ )
	    : self( self_ )
	    , is_reference( true ) {
	}

	~RenderModule() {
		if ( !is_reference ) {
			rendermoduleI.destroy( self );
		}
	}

	operator auto() {
		return self;
	}

	void addRenderPass( le_renderpass_o *renderpass ) {
		rendermoduleI.add_renderpass( self, renderpass );
	}

	void buildGraph( le_graph_builder_o *gb_ ) {
		rendermoduleI.build_graph( self, gb_ );
	}

};

// ----------------------------------------------------------------------

class CommandBufferEncoder: NoCopy, NoMove {
	const le_renderer_api &                                    rendererApiI = *Registry::getApi<le_renderer_api>();
	const le_renderer_api::command_buffer_encoder_interface_t &cbEncoderI   = rendererApiI.le_command_buffer_encoder_i;

	le_command_buffer_encoder_o *self;
	bool                         is_reference = false;

  public:
	CommandBufferEncoder()
	    : self( cbEncoderI.create() ) {
	}

	CommandBufferEncoder( le_command_buffer_encoder_o *self_ )
	    : self( self_ )
	    , is_reference( true ) {
	}

	~CommandBufferEncoder() {
		if ( !is_reference ) {
			cbEncoderI.destroy( self );
		}
	}

	operator auto() {
		return self;
	}
	
	void setLineWidth(float lineWidth){
		cbEncoderI.set_line_width(self, lineWidth);
	}

	void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance){
		cbEncoderI.draw( self, vertexCount, instanceCount, firstVertex, firstInstance );
	}

	void setViewport(uint32_t firstViewport, const uint32_t viewportCount, const le::Viewport* pViewports){
		cbEncoderI.set_viewport(self,firstViewport, viewportCount, pViewports);
	}
};

} // namespace le
#endif // __cplusplus
#endif // GUARD_LE_RENDERER_H
