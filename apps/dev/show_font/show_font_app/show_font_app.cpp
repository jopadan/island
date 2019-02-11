#include "show_font_app.h"

#include "pal_window/pal_window.h"
#include "le_renderer/le_renderer.h"

#include "le_camera/le_camera.h"
#include "le_pipeline_builder/le_pipeline_builder.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE // vulkan clip space is from 0 to 1
#define GLM_FORCE_RIGHT_HANDED      // glTF uses right handed coordinate system, and we're following its lead.
#include "glm.hpp"
#include "gtc/matrix_transform.hpp"

#include "le_font/le_font.h"
#include "le_tessellator/le_tessellator.h"

#include <iostream>
#include <memory>
#include <sstream>
#include <vector>
#include <complex>

#include "./3rdparty/kissfft/kiss_fft.h"

struct show_font_app_o {
	pal::Window  window;
	le::Renderer renderer;
	uint64_t     frame_counter = 0;

	le_glyph_shape_o *glyph_shape = nullptr;
	le_font_o *       font;

	std::vector<glm::vec3> blobVertices;

	LeCamera camera;
};

// ----------------------------------------------------------------------

static void initialize() {
	pal::Window::init();
};

// ----------------------------------------------------------------------

static void terminate() {
	pal::Window::terminate();
};

static void reset_camera( show_font_app_o *self ); // ffdecl.

// ----------------------------------------------------------------------

static show_font_app_o *show_font_app_create() {
	auto app = new ( show_font_app_o );

	pal::Window::Settings settings;
	settings
	    .setWidth( 1024 )
	    .setHeight( 1024 )
	    .setTitle( "Island // ShowFontApp" );

	// create a new window
	app->window.setup( settings );

	app->renderer.setup( le::RendererInfoBuilder( app->window ).build() );

	// Set up the camera
	reset_camera( app );

	using namespace le_font;
	app->font = le_font_i.create();

	return app;
}

// ----------------------------------------------------------------------

static void reset_camera( show_font_app_o *self ) {
	le::Extent2D extents{};
	self->renderer.getSwapchainExtent( &extents.width, &extents.height );
	self->camera.setViewport( {0, 0, float( extents.width ), float( extents.height ), 0.f, 1.f} );
	self->camera.setFovRadians( glm::radians( 60.f ) ); // glm::radians converts degrees to radians
	glm::mat4 camMatrix = glm::lookAt( glm::vec3{0, 0, self->camera.getUnitDistance()}, glm::vec3{0}, glm::vec3{0, 1, 0} );
	self->camera.setViewMatrixGlm( camMatrix );
}

// ----------------------------------------------------------------------

typedef bool ( *renderpass_setup )( le_renderpass_o *pRp, void *user_data );

static bool pass_main_setup( le_renderpass_o *pRp, void *user_data ) {
	auto rp  = le::RenderPass{pRp};
	auto app = static_cast<show_font_app_o *>( user_data );

	// Attachment resource info may be further specialised using ImageInfoBuilder().
	// Attachment clear color, load and store op may be set via le_image_attachment_info_t.

	rp
	    .addColorAttachment( app->renderer.getSwapchainResource() ) // color attachment
	    .setIsRoot( true );

	return true;
}

// ----------------------------------------------------------------------

static void pass_main_exec( le_command_buffer_encoder_o *encoder_, void *user_data ) {
	auto        app = static_cast<show_font_app_o *>( user_data );
	le::Encoder encoder{encoder_};

	auto extents = encoder.getRenderpassExtent();

	le::Viewport viewports[ 1 ] = {
	    {0.f, 0.f, float( extents.width ), float( extents.height ), 0.f, 1.f},
	};

	app->camera.setViewport( viewports[ 0 ] );

	// Data as it is laid out in the shader ubo
	struct MatrixStackUbo_t {
		glm::mat4 model;
		glm::mat4 view;
		glm::mat4 projection;
	};

	{
		using namespace le_font;

		if ( app->glyph_shape ) {
			le_glyph_shape_i.destroy( app->glyph_shape );
		}

		// app->glyph_shape = le_font_i.get_shape_for_glyph( app->font, 0xae, nullptr ); // draw registration mark '(r)' glyph

		app->glyph_shape = le_font_i.get_shape_for_glyph( app->font, 's', nullptr );

		//		app->glyph_shape = le_font_i.get_shape_for_glyph( app->font, 'i', nullptr );

		//		app->glyph_shape = le_font_i.get_shape_for_glyph( app->font, 'B', nullptr );
	}

	// Draw main scene

	static auto shaderVert = app->renderer.createShaderModule( "./resources/shaders/default.vert", le::ShaderStage::eVertex );
	static auto shaderFrag = app->renderer.createShaderModule( "./resources/shaders/default.frag", le::ShaderStage::eFragment );

	MatrixStackUbo_t mvp;

	mvp.model      = glm::mat4( 1.f ); // identity matrix
	mvp.model      = glm::scale( mvp.model, glm::vec3( 1.0 ) );
	mvp.model      = glm::translate( mvp.model, glm::vec3( -200, 200, 0 ) );
	mvp.view       = app->camera.getViewMatrixGlm();
	mvp.projection = app->camera.getProjectionMatrixGlm();

	if ( false ) {
		// draw body

		static auto pipelineFontFill =
		    LeGraphicsPipelineBuilder( encoder.getPipelineManager() )
		        .addShaderStage( shaderVert )
		        .addShaderStage( shaderFrag )
		        .withInputAssemblyState()
		        .setToplogy( le::PrimitiveTopology::eTriangleList )
		        .end()
		        .withRasterizationState()
		        .setPolygonMode( le::PolygonMode::eLine )
		        .end()
		        .withDepthStencilState()
		        .setDepthTestEnable( false ) // disable depth testing
		        .end()
		        .build();

		using namespace le_font;
		using namespace le_tessellator;

		size_t numContours = le_glyph_shape_i.get_num_contours( app->glyph_shape );

		auto tess = le_tessellator_i.create();

		// First, we gather all contours and feed them to the tessellator.

		for ( size_t i = 0; i != numContours; i++ ) {
			size_t     numV = 0;
			glm::vec2 *vv   = le_glyph_shape_i.get_vertices_for_shape_contour( app->glyph_shape, i, &numV );

			// add polyline to tessellator
			le_tessellator_i.add_polyline( tess, vv, numV );
		}

		// Once the tessellator has all contours, we may triangulate.

		le_tessellator_i.tessellate( tess );

		// we must translate vertices to vec3
		std::vector<glm::vec3> vertices;
		std::vector<glm::vec4> colors;

		{
			size_t           numVertices = 0;
			glm::vec2 const *pVertices   = nullptr;
			le_tessellator_i.get_vertices( tess, &pVertices, &numVertices );

			vertices.reserve( numVertices );
			colors.reserve( numVertices );

			for ( size_t i = 0; i != numVertices; ++i ) {
				// add vertices and colors to  outline
				// note that we flip the y-axis
				vertices.emplace_back( pVertices[ i ].x,
				                       -pVertices[ i ].y,
				                       0 );
				colors.emplace_back( 1, 1, 1, 1 );
			}
		}

		// we should now have a list of indices which reference our vertices in tessellator.

		uint16_t const *pIndices   = nullptr;
		size_t          numIndices = 0;
		le_tessellator_i.get_indices( tess, &pIndices, &numIndices );

		encoder
		    .bindGraphicsPipeline( pipelineFontFill )
		    .setArgumentData( LE_ARGUMENT_NAME( "MatrixStack" ), &mvp, sizeof( MatrixStackUbo_t ) ) //

		    .setVertexData( vertices.data(), sizeof( glm::vec3 ) * vertices.size(), 0 )
		    .setVertexData( colors.data(), sizeof( glm::vec4 ) * colors.size(), 1 )
		    .setIndexData( pIndices, sizeof( le_tessellator_api::le_tessellator_interface_t::IndexType ) * numIndices )
		    .drawIndexed( uint32_t( numIndices ) ) //
		    ;

		// free the tessellator object

		le_tessellator_i.destroy( tess );
	}

	static auto pipelineFontOutline =
	    LeGraphicsPipelineBuilder( encoder.getPipelineManager() )
	        .addShaderStage( shaderVert )
	        .addShaderStage( shaderFrag )
	        .withInputAssemblyState()
	        .setToplogy( le::PrimitiveTopology::eLineStrip )
	        .end()
	        .withDepthStencilState()
	        .setDepthTestEnable( false ) // disable depth testing
	        .end()
	        .build();
	if ( true ) {

		// draw outline

		encoder
		    .setLineWidth( 1.f )
		    .bindGraphicsPipeline( pipelineFontOutline )
		    .setArgumentData( LE_ARGUMENT_NAME( "MatrixStack" ), &mvp, sizeof( MatrixStackUbo_t ) ) //
		    ;

		using namespace le_font;

		size_t numContours = le_glyph_shape_i.get_num_contours( app->glyph_shape );

		for ( size_t i = 0; i != numContours; i++ ) {
			size_t                 numV = 0;
			glm::vec2 *            vv   = le_glyph_shape_i.get_vertices_for_shape_contour( app->glyph_shape, i, &numV );
			std::vector<glm::vec3> vertices;
			std::vector<glm::vec4> colors;

			vertices.reserve( numV );
			colors.reserve( numV );

			for ( auto p = vv; p != vv + numV; p++ ) {
				vertices.emplace_back( p->x, -p->y, 0 ); // note that we flip the y-axis.
				colors.emplace_back( 1, 0, 0, 1 );       // use red for outline
			}

			encoder
			    .setVertexData( vertices.data(), sizeof( glm::vec3 ) * numV, 0 )
			    .setVertexData( colors.data(), sizeof( glm::vec4 ) * numV, 1 )
			    .draw( numV );
		}
	}

	if ( true ) {
		app->blobVertices.clear();

		static int at = 0;
		at            = ( ++at ) % 360;

		using namespace le_font;
		size_t     numV;
		glm::vec2 *vv     = le_glyph_shape_i.get_vertices_for_shape_contour( app->glyph_shape, 0, &numV );
		auto const vv_end = vv + numV;

		std::vector<kiss_fft_cpx> fft_in;

		fft_in.reserve( numV );

		for ( auto p = vv; p != vv_end; p++ ) {
			fft_in.push_back( {p->x, p->y} ); // note that we flip the y-axis.
		}

		//		fft_in = {
		//		    {-50, 50},
		//		    //		    {-50, 0},
		//		    {-50, -50},
		//		    //		    {0 + 500 * float( at ) / 360.f, -50},
		//		    {50, -50},
		//		    //		    {50, 0},
		//		    {50 + 500 * float( at ) / 360.f, 50},
		//		    //		    {0 + 500 * float( at ) / 360.f, 50},
		//		};
		std::vector<kiss_fft_cpx> fft_out;

		{

			// use kisfft to calculate fast fourier transform

			kiss_fft_cfg kiss = kiss_fft_alloc( fft_in.size(), 0, nullptr, nullptr );

			fft_out.resize( fft_in.size() );

			kiss_fft( kiss, fft_in.data(), fft_out.data() );

			kiss_fft_free( kiss );
		}

		// Alias terms to fft_out
		auto &terms = fft_out;

		app->blobVertices.clear();
		constexpr size_t numVertices   = 300;
		constexpr auto   ImaginaryUnit = std::complex<float>( 0, 1 ); // constant I, the imaginary number
		app->blobVertices.reserve( numVertices );

		size_t numPoints = terms.size();

		for ( size_t i = 0; i < numVertices; i++ ) {

			// evaluate for t
			float t = float( i ) / float( numVertices - 1 );

			glm::vec2 sum{0};

			for ( int n = 0; n != numPoints; ++n ) {

				int freq = ( n % 2 ) * ( ( n + 1 ) / 2 ) + // only applies to odd  numbers
				           ( ( n + 1 ) % 2 ) * ( -n / 2 )  // only applies to even numbers
				    ;

				auto const idx = size_t( int( numPoints ) + freq ) % numPoints;

				auto const &term          = terms[ idx ];
				float       dft_amplitude = sqrtf( term.i * term.i + term.r * term.r ) / ( numPoints );
				float       dft_phase     = atan2( term.i, term.r );

				float angle = glm::two_pi<float>() * freq * t + dft_phase;

				sum += dft_amplitude * glm::vec2{cos( angle ), -sin( angle )};
			}

			app->blobVertices.emplace_back( sum.x, sum.y, 0 );
		}

		// let's see if we have enough colors in the colors buffer
		std::vector<glm::vec4> colors;
		colors.reserve( numVertices );

		for ( size_t i = 0; i != app->blobVertices.size(); i++ ) {
			colors.emplace_back( 0, 1, 1, 1 ); // use blue for outline
		}

		//		mvp.model = glm::mat4( 1.f ); // reset model matrix

		encoder
		    .setLineWidth( 1.f )
		    .bindGraphicsPipeline( pipelineFontOutline )
		    .setArgumentData( LE_ARGUMENT_NAME( "MatrixStack" ), &mvp, sizeof( MatrixStackUbo_t ) ) //
		    .setVertexData( app->blobVertices.data(), sizeof( glm::vec3 ) * numVertices, 0 )
		    .setVertexData( colors.data(), sizeof( glm::vec4 ) * numVertices, 1 )
		    .draw( numVertices );
	}
}

// ----------------------------------------------------------------------

static bool show_font_app_update( show_font_app_o *self ) {

	// Polls events for all windows
	// Use `self->window.getUIEventQueue()` to fetch events.
	pal::Window::pollEvents();

	if ( self->window.shouldClose() ) {
		return false;
	}

	le::RenderModule mainModule{};
	{

		le::RenderPass renderPassFinal( "root", LE_RENDER_PASS_TYPE_DRAW );

		renderPassFinal
		    .setSetupCallback( self, pass_main_setup )
		    .setExecuteCallback( self, pass_main_exec ) //
		    ;

		mainModule.addRenderPass( renderPassFinal );
	}

	self->renderer.update( mainModule );

	self->frame_counter++;

	return true; // keep app alive
}

// ----------------------------------------------------------------------

static void show_font_app_destroy( show_font_app_o *self ) {

	using namespace le_font;
	le_glyph_shape_i.destroy( self->glyph_shape );
	self->glyph_shape = nullptr;

	le_font_i.destroy( self->font );
	self->font = nullptr;

	delete ( self ); // deletes camera
}

// ----------------------------------------------------------------------

ISL_API_ATTR void register_show_font_app_api( void *api ) {
	auto  show_font_app_api_i = static_cast<show_font_app_api *>( api );
	auto &show_font_app_i     = show_font_app_api_i->show_font_app_i;

	show_font_app_i.initialize = initialize;
	show_font_app_i.terminate  = terminate;

	show_font_app_i.create  = show_font_app_create;
	show_font_app_i.destroy = show_font_app_destroy;
	show_font_app_i.update  = show_font_app_update;
}
