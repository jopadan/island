
# Summary

Write raw images to an external (ffmpeg) video encoder via a pipe.

> [!Note]
> By default, this module depends on ffmpeg being installed on your system, 
> and available to call directly (which means it must be found in your `$PATH`)

# Linux

    # Ubuntu, Debian
    sudo apt-get install ffmpeg

    # Fedora
    sudo dnf install ffmpeg

# Windows

I recommend `winget`, use the following incantation:

    winget install ffmpeg

You will have to reboot your machine so that ffmpeg is automatically found
in your `$PATH`.

# How To Use This Module

You may use this module as if it was a regular image encoder, as it implements
the image encoder interface. If you want to record multiple frames into a video
you want to write to the encoder once for each frame. Keep the encoder alive
for  the full duration of recording the video.

This is particularly useful with screen recorders. You can  screen record to a
video file like this:

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

 Note that the filename must match the video codec parameters -- this means if
 your encoder parameters suggest an animated GIF, the filename must have a
 `.gif` file ending.

 Note that you can fine-tune the command line - as long as it creates a pipe
 this encoder will happily write to it.

 In case you don't specify a command line, the default command line is chosen.

