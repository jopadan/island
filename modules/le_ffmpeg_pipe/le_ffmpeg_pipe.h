#ifndef GUARD_le_ffmpeg_pipe_H
#define GUARD_le_ffmpeg_pipe_H

#include "le_core.h"

// The decoder interface is rarely used direcly. You are probably better off using
// `le_resource_manager`.
//
// The decoder interface is declared in:
//
// #include "shared/interfaces/le_image_decoder_interface.h"
//
// The encoder interface is declared in:
//
// #include "shared/interfaces/le_image_encoder_interface.h"

struct le_image_encoder_interface_t;

struct le_ffmpeg_pipe_encoder_parameters_t {
	// parameters are empty for now - we might want to allow
	// choosing the number of channels, and the compression ratio here
	// in the future.
	char const* command_line; // non-owning
};

// clang-format off
struct le_ffmpeg_pipe_api {

	le_image_encoder_interface_t * le_ffmpeg_pipe_encoder_i = nullptr; // abstract image encoder interface

};
// clang-format on

LE_MODULE( le_ffmpeg_pipe );
LE_MODULE_LOAD_DEFAULT( le_ffmpeg_pipe );

#ifdef __cplusplus

namespace le_ffmpeg_pipe {
	static const auto &api = le_ffmpeg_pipe_api_i;
} // namespace

#endif // __cplusplus
#endif
