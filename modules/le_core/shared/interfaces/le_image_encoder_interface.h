#pragma once

/*

  Forward declaration for the abstract interface that any image encoders
  must implement.

  That way, clients of image encoders can stick to this abstract interface
  when using any client encoders.

  //

  For image encoders there are a lot of format-specific settings that need
  to be set - as in how should the image be encoded, the encoding quality,
  the number of channels to use for encoding etc.

  We need to provide the encoder with a method to exchange these settings
  with whoever uses this api.


  Maybe we should pass image file encoding parameters as structured data
  (json/jsmn!?) so that we can keep the interface versatile?

*/

#include <cstddef>
#include <stdint.h>

struct le_image_encoder_o;

struct le_image_encoder_format_o;     // struct wrapper around le::Format
// struct le_image_encoder_parameters_o; // parameters struct

// ----------------------------------------------------------------------
// clang-format off

struct le_image_encoder_interface_t {

	static constexpr uint64_t API_VERSION = 0ull << 48 | 0ull << 32 | 2ull << 16 | 1ull << 0;

	// ------------------------------

	void *              ( *clone_image_encoder_parameters_object   )( void const * obj );
	void                ( *destroy_image_encoder_parameters_object )( void * obj );

	// ------------------------------

	le_image_encoder_o* ( *create_image_encoder  )( char const* filename, uint32_t width, uint32_t height );
	
	void 				( *destroy_image_encoder )( le_image_encoder_o* image_encoder_o );
	void 				( *set_encode_parameters )( le_image_encoder_o* image_encoder_o, void* params );

	// use this to encode another file with the same encoder
	void 				( *update_filename       )( le_image_encoder_o* image_encoder_o, char const* filename);


	uint64_t   			( *get_encoder_version   )( le_image_encoder_o* encoder );

	bool 				( *write_pixels          )( le_image_encoder_o* image_encoder_o, uint8_t const * p_pixel_data, size_t pixel_data_byte_count, le_image_encoder_format_o* pixel_data_format );

};

// clang-format on
