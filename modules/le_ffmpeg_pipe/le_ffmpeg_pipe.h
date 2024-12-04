#ifndef GUARD_le_ffmpeg_pipe_H
#define GUARD_le_ffmpeg_pipe_H

#include "le_core.h"

/* Write raw images to an external (ffmpeg) video encoder via a linux pipe.
 * 
 * As this implements an image encoder interface, you may 
 * use it in-place wherever an image encoder is expected.
 *
 * This is particularly useful with screen recorders. You can 
 * record to a video file as follows:

le_ffmpeg_pipe_encoder_parameters_t encoder_params{
    // .command_line = "ffmpeg -r 60 -f rawvideo -pix_fmt %s -s %dx%d -i - -filter_complex \"[0:v] fps=30,split [a][b];[a] palettegen [p];[b][p] paletteuse\" %s",
    .command_line = "ffmpeg -r 60 -f rawvideo -pix_fmt %s -s %dx%d -i - -threads 0 -vcodec h264_nvenc -preset llhq -rc:v vbr_minqp -qmin:v 19 -qmax:v 21 -b:v 2500k -maxrate:v 5000k -r 30 -profile:v high %s",
};

le_swapchain_img_settings_t settings_ffmpeg_pipe{
    .width_hint  = 0, // 0 means to take the width of the renderer's first swapchain
    .height_hint = 0, // 0 means to take the height of the renderer's first swapchain

    .format_hint              = le::Format::eR8G8B8A8Unorm,
    .image_encoder_i          = le_ffmpeg_pipe::api->le_ffmpeg_pipe_encoder_i,
    .image_encoder_parameters = &encoder_params,
    .image_filename_template  = "./capture/recording_%04d.mp4",
};

le_screenshot::le_screenshot_i.record( self->screen_recorder, rendergraph, self->swapchain_image, &self->should_take_screenshots, &settings_ffmpeg_pipe );

 * Note that the filename must match the video codec parameters.
 *
 * Note that you can fine-tune the command line - as long as it 
 * creates a pipe this encoder will happily write to it.
 *
 * In case you don't specify a command line, the default command 
 * line is chosen.
 *
 */


// The encoder interface is declared in:
//
// #include "shared/interfaces/le_image_encoder_interface.h"

struct le_image_encoder_interface_t;

struct le_ffmpeg_pipe_encoder_parameters_t {
	char const* command_line; // non-owning
};

struct le_ffmpeg_pipe_api {
	le_image_encoder_interface_t * le_ffmpeg_pipe_encoder_i = nullptr; // abstract image encoder interface
};

LE_MODULE( le_ffmpeg_pipe );
LE_MODULE_LOAD_DEFAULT( le_ffmpeg_pipe );

#ifdef __cplusplus

namespace le_ffmpeg_pipe {
	static const auto &api = le_ffmpeg_pipe_api_i;
} // namespace

#endif // __cplusplus
#endif
