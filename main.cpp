#include "pal_api_loader/ApiRegistry.hpp"

#include "test_app/test_app.h"
#include "pal_window/pal_window.h"
#include "le_backend_vk/le_backend_vk.h"
#include "le_swapchain_vk/le_swapchain_vk.h"
#include "le_renderer/le_renderer.h"
#include "le_shader_compiler/le_shader_compiler.h"
#include "le_gltf_loader/le_gltf_loader.h"

// ----------------------------------------------------------------------

int main( int argc, char const *argv[] ) {

#ifdef PLUGINS_DYNAMIC
	Registry::addApiDynamic<pal_window_api>( true );
	Registry::addApiDynamic<le_backend_vk_api>( true );
	Registry::addApiDynamic<le_swapchain_vk_api>( true );
	Registry::addApiDynamic<le_renderer_api>( true );
	Registry::addApiDynamic<le_shader_compiler_api>( true );
	Registry::addApiDynamic<le_gltf_loader_api>( true );
	Registry::addApiDynamic<test_app_api>( true );
#else
	Registry::addApiStatic<pal_window_api>();
	Registry::addApiStatic<le_backend_vk_api>();
	Registry::addApiStatic<le_swapchain_vk_api>();
	Registry::addApiStatic<le_renderer_api>();
	Registry::addApiStatic<le_shader_compiler_api>();
	Registry::addApiStatic<le_gltf_loader_api>();
	Registry::addApiStatic<test_app_api>();
#endif

	TestApp::initialize();

	{
		// We instantiate TestApp in its own scope - so that
		// it will be destroyed before TestApp::terminate
		// is called.

		TestApp testApp{};

		for ( ;; ) {
			Registry::pollForDynamicReload();

			auto result = testApp.update();

			if ( !result ) {
				break;
			}
		}
	}

	// Must only be called once last TestApp is destroyed
	TestApp::terminate();

	return 0;
}
