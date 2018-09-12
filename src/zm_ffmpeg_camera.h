//
// ZoneMinder Ffmpeg Class Interface, $Date: 2008-07-25 10:33:23 +0100 (Fri, 25 Jul 2008) $, $Revision: 2611 $
// Copyright (C) 2001-2008 Philip Coombes
// 
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//#define __arm__
#ifndef ZM_FFMPEG_CAMERA_H
#define ZM_FFMPEG_CAMERA_H

#include "zm_camera.h"

#include "zm_buffer.h"
#include "zm_ffmpeg.h"
#include "zm_videostore.h"
#include "zm_packetqueue.h"

#include <bitset> 

#ifdef __arm__
extern "C" {
#include "interface/mmal/mmal.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_util.h"
}
#endif

//
// Class representing 'ffmpeg' cameras, i.e. those which are
// accessed using ffmpeg multimedia framework
//
class FfmpegCamera : public Camera {
  protected:
    std::string         mPath;
    std::string         mMethod;
    std::string         mOptions;

    int frameCount; 
    int bufsize_d=0;
    int bufsize_r=0;   

#if HAVE_LIBAVFORMAT
    AVFormatContext     *mFormatContext;
    int                 mVideoStreamId;
    int                 mAudioStreamId;
    AVCodecContext      *mVideoCodecContext;
    AVCodecContext      *mAudioCodecContext;
    AVCodec             *mVideoCodec;
    AVCodec             *mAudioCodec;
    AVFrame             *mRawFrame; 

    AVFrame             *mFrame;
    _AVPIXELFORMAT      imagePixFormat;
#ifdef __arm__    
    _AVPIXELFORMAT      encoderPixFormat;
#endif

    // Need to keep track of these because apparently the stream can start with values for pts/dts and then subsequent packets start at zero.
    int64_t audio_last_pts;
    int64_t audio_last_dts;
    int64_t video_last_pts;
    int64_t video_last_dts;

    // Used to store the incoming packet, it will get copied when queued. 
    // We only ever need one at a time, so instead of constantly allocating
    // and freeing this structure, we will just make it a member of the object.
    AVPacket packet;       

    int OpenFfmpeg();
    int ReopenFfmpeg();
    int CloseFfmpeg();
    static int FfmpegInterruptCallback(void *ctx);
    static void* ReopenFfmpegThreadCallback(void *ctx);
    bool mIsOpening;
    bool mCanCapture;
    int mOpenStart;
    pthread_t mReopenThread;
#endif // HAVE_LIBAVFORMAT

    VideoStore          *videoStore;
    char                oldDirectory[4096];
    unsigned int        old_event_id;
    zm_packetqueue      packetqueue;

#if HAVE_LIBSWSCALE
    struct SwsContext   *mConvertContext;
#endif
    int64_t             startTime;

  public:
      
#ifdef __arm__

    MMAL_COMPONENT_T *encoder, *decoder, *resizer, *jcoder;
    MMAL_POOL_T *pool_ind, *pool_outd, 
                *pool_ine, *pool_oute, 
                *pool_inr, *pool_outr,
                *pool_inj, *pool_outj;


    struct CONTEXT_T {
    MMAL_QUEUE_T *rqueue,*equeue,*dqueue,*jqueue;
    MMAL_STATUS_T status;
    } context;
    
    

    struct mmal_motion_vector {
     char x_vector;
     char y_vector; 
     short sad;
    };
#endif
    enum h264_codec {
        software,
        hardware
    };
    h264_codec ctype; 
    
    
    
    Monitor::Function cfunction;
    
    
    //typedef Image::motion_vector motion_vector;
  
  
  
    FfmpegCamera( int p_id, const std::string &path, const std::string &p_method, const std::string &p_options, int p_width, int p_height, int p_colours, int p_brightness, int p_contrast, int p_hue, int p_colour, bool p_capture, bool p_record_audio, h264_codec ictype, Monitor::Function cfunction );
    ~FfmpegCamera();

    const std::string &Path() const { return( mPath ); }
    const std::string &Options() const { return( mOptions ); } 
    const std::string &Method() const { return( mMethod ); }

    void Initialise();
    void Terminate();

    int PrimeCapture();
    int PreCapture();
    int Capture( Image &image );
    int CaptureAndRecord( Image &image, timeval recording, char* event_directory );
    int PostCapture();
    
#ifdef __arm__
    static void input_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);

    static void output_callbackr(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
    static void output_callbacke(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
    static void output_callbackd(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
    static void output_callbackj(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
    
    static void control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer);
    
    static void display_format(MMAL_PORT_T **port, MMAL_ES_FORMAT_T **iformat);
    
    int mmal_decode(AVPacket *packet);
    int mmal_encode(uint8_t **mv_buffer);
    int mmal_resize(uint8_t **dbuffer);
    int mmal_jpeg(uint8_t** jbuffer);
    
    int OpenMmalDecoder(AVCodecContext *mVideoCodecContext);
    int OpenMmalEncoder(AVCodecContext *mVideoCodecContext);
    int OpenMmalResizer(AVCodecContext *mVideoCodecContext);
    int OpenMmalJPEG(AVCodecContext *mVideoCodecContext);

    int CloseMmal();
    
    Zone **czones;
    int czones_n=0;
    //int j_encode_count=0;
    int jpeg_limit=0;
    
    uint8_t *result[10];
    int numblocks=0;
    
    
    
#endif
};

#endif // ZM_FFMPEG_CAMERA_H
