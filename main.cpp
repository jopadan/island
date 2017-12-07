#include "pal_api_loader/ApiRegistry.hpp"
#include "pal_window/pal_window.h"
#include "pal_backend_vk/pal_backend_vk.h"
// ----------------------------------------------------------------------

int main( int argc, char const *argv[] ) {

#ifdef PLUGIN_PAL_WINDOW_STATIC
	Registry::addApiStatic<pal_window_api>();
#else
	Registry::addApiDynamic<pal_window_api>( true );
#endif

#ifdef PLUGIN_PAL_BACKEND_VK_STATIC
	Registry::addApiStatic<pal_backend_vk_api>( );
#else
	Registry::addApiDynamic<pal_backend_vk_api>( true );
#endif

	{
		pal::Backend mBackend;
		pal::Window::init();

		pal::Window window{};

		for ( ; window.shouldClose() == false; ) {

			Registry::pollForDynamicReload();

			pal::Window::pollEvents();
			window.update();
			window.draw();
		}

		pal::Window::terminate();
	}

	return 0;
}
