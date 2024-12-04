#pragma once

/*

  Forward declaration for the abstract interface that any image decoders
  must implement.

  That way, clients of image decoders can stick to this abstract interface
  when using any client decoders.

*/

#include <cstddef>
#include <stdint.h>

struct le_image_decoder_o;

struct le_image_decoder_format_o; // struct wrapper around le::Format

// ----------------------------------------------------------------------
// clang-format off

struct le_image_decoder_interface_t {

	static constexpr uint64_t API_VERSION = 0ull << 48 | 0ull << 32 | 1ull << 16 | 0ull << 0;

	le_image_decoder_o* ( *create_image_decoder )( char const* file_name );

	void ( *destroy_image_decoder      )( le_image_decoder_o* image_decoder_o );

	// load image data from file, and read it into a pre-allocated byte array at p_pixels
	bool ( *read_pixels                )( le_image_decoder_o* image_decoder_o, uint8_t* p_pixels, size_t pixels_byte_count );

	// get image data description (this does should not read full image, only just the image metadata)
	void ( *get_image_data_description )( le_image_decoder_o* image_decoder_o, le_image_decoder_format_o* p_format, uint32_t* w, uint32_t* h );

	// set the format into which to read pixels
	void ( *set_requested_format       )( le_image_decoder_o* image_decoder_o, le_image_decoder_format_o const* format );
};

// clang-format on
