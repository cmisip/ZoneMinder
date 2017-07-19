//
// ZoneMinder Ffmpeg Camera Class Implementation, $Date: 2009-01-16 12:18:50 +0000 (Fri, 16 Jan 2009) $, $Revision: 2713 $
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
// 

#include "zm.h"

#if HAVE_LIBAVFORMAT

#include "zm_ffmpeg_camera.h"

extern "C" {
#include "libavutil/time.h"
}
#ifndef AV_ERROR_MAX_STRING_SIZE
#define AV_ERROR_MAX_STRING_SIZE 64
#endif

#ifdef SOLARIS
#include <sys/errno.h>  // for ESRCH
#include <signal.h>
#include <pthread.h>
#endif

FfmpegCamera::FfmpegCamera( int p_id, const std::string &p_path, const std::string &p_method, const std::string &p_options, int p_width, int p_height, int p_colours, int p_brightness, int p_contrast, int p_hue, int p_colour, bool p_capture, bool p_record_audio,  h264_codec ictype, Monitor::Function icfunction ) :
  Camera( p_id, FFMPEG_SRC, p_width, p_height, p_colours, ZM_SUBPIX_ORDER_DEFAULT_FOR_COLOUR(p_colours), p_brightness, p_contrast, p_hue, p_colour, p_capture, p_record_audio ),
  mPath( p_path ),
  mMethod( p_method ),
  mOptions( p_options ),
  ctype( ictype),
  cfunction(icfunction)
{
  if ( capture ) {
    Initialise();
  }

  mFormatContext = NULL;
  mVideoStreamId = -1;
  mAudioStreamId = -1;
  mVideoCodecContext = NULL;
  mAudioCodecContext = NULL;
  mVideoCodec = NULL;
  mAudioCodec = NULL;
  mRawFrame = NULL;
  mFrame = NULL;
  frameCount = 0;
  startTime=0;
  mIsOpening = false;
  mCanCapture = false;
  mOpenStart = 0;
  mReopenThread = 0;
  videoStore = NULL;
  video_last_pts = 0;

#if HAVE_LIBSWSCALE  
  mConvertContext = NULL;
#endif
  /* Has to be located inside the constructor so other components such as zma will receive correct colours and subpixel order */
  if(colours == ZM_COLOUR_RGB32) {
    subpixelorder = ZM_SUBPIX_ORDER_RGBA;
    imagePixFormat = AV_PIX_FMT_RGBA;
  } else if(colours == ZM_COLOUR_RGB24) {
    subpixelorder = ZM_SUBPIX_ORDER_RGB;
    imagePixFormat = AV_PIX_FMT_RGB24;
  } else if(colours == ZM_COLOUR_GRAY8) {
    subpixelorder = ZM_SUBPIX_ORDER_NONE;
    imagePixFormat = AV_PIX_FMT_GRAY8;
  } else {
    Panic("Unexpected colours: %d",colours);
  }

}

FfmpegCamera::~FfmpegCamera() {

  if ( videoStore ) {
    delete videoStore;
  }
  CloseFfmpeg();

  if ( capture ) {
    Terminate();
  }
}

void FfmpegCamera::Initialise() {
  if ( logDebugging() )
    av_log_set_level( AV_LOG_DEBUG ); 
  else
    av_log_set_level( AV_LOG_QUIET ); 

  av_register_all();
  avformat_network_init();
}

void FfmpegCamera::Terminate() {
}

int FfmpegCamera::PrimeCapture() {
  mVideoStreamId = -1;
  mAudioStreamId = -1;
  Info( "Priming capture from %s", mPath.c_str() );

  if (OpenFfmpeg() != 0){
    ReopenFfmpeg();
  }
  return 0;
}

int FfmpegCamera::PreCapture()
{
  // Nothing to do here
  return( 0 );
}

int FfmpegCamera::Capture( Image &image ) {
  if (!mCanCapture){
    return -1;
  }

  // If the reopen thread has a value, but mCanCapture != 0, then we have just reopened the connection to the ffmpeg device, and we can clean up the thread.
  if (mReopenThread != 0) {
    void *retval = 0;
    int ret;

    ret = pthread_join(mReopenThread, &retval);
    if (ret != 0){
      Error("Could not join reopen thread.");
    }

    Info( "Successfully reopened stream." );
    mReopenThread = 0;
  }

  int frameComplete = false;
  while ( !frameComplete ) {
    int ret;
    int avResult = av_read_frame( mFormatContext, &packet );
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    if ( avResult < 0 ) {
      av_strerror(avResult, errbuf, AV_ERROR_MAX_STRING_SIZE);
      if (
          // Check if EOF.
          (avResult == AVERROR_EOF || (mFormatContext->pb && mFormatContext->pb->eof_reached)) ||
          // Check for Connection failure.
          (avResult == -110)
         ) {
        Info( "av_read_frame returned \"%s\". Reopening stream.", errbuf );
        ReopenFfmpeg();
      }

      Error( "Unable to read packet from stream %d: error %d \"%s\".", packet.stream_index, avResult, errbuf );
      return( -1 );
    }
    Debug( 5, "Got packet from stream %d dts (%d) pts(%d)", packet.stream_index, packet.pts, packet.dts );
    // What about audio stream? Maybe someday we could do sound detection...
    if ( packet.stream_index == mVideoStreamId ) {
   
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
      ret = avcodec_send_packet( mVideoCodecContext, &packet );
      if ( ret < 0 ) {
        av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
        Error( "Unable to send packet at frame %d: %s, continuing", frameCount, errbuf );
        zm_av_packet_unref( &packet );
        continue;
      }
      ret = avcodec_receive_frame( mVideoCodecContext, mRawFrame );
      if ( ret < 0 ) {
        av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
        Error( "Unable to send packet at frame %d: %s, continuing", frameCount, errbuf );
        zm_av_packet_unref( &packet );
        continue;
      }
      frameComplete = 1;
 
# else
      ret = zm_avcodec_decode_video( mVideoCodecContext, mRawFrame, &frameComplete, &packet );
      if ( ret < 0 ) {
        av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
        Error( "Unable to decode frame at frame %d: %s, continuing", frameCount, errbuf );
        zm_av_packet_unref( &packet );
        continue;
      }
#endif

      Debug( 4, "Decoded video packet at frame %d", frameCount );



      if ( frameComplete ) {
        Debug( 4, "Got frame %d", frameCount );
        
        uint8_t* directbuffer;

        /* Request a writeable buffer of the target image */
        directbuffer = image.WriteBuffer(width, height, colours, subpixelorder);
        if(directbuffer == NULL) {
          Error("Failed requesting writeable buffer for the captured image.");
          return (-1);
        }
        

        
 
      uint8_t* mvect_buffer=NULL;     
if (!( cfunction == Monitor::MVDECT )) {     
    //Info("Camera Function is not MVDECT.. Will not capture motion vectors.");
    goto end;
}

        
      {   
        mvect_buffer=image.VectBuffer();   
        if (mvect_buffer ==  NULL ){
                Error("Failed requesting vector buffer for the captured image.");
                return (-1); 
        } else
                memset(mvect_buffer,0,image.mv_size);
        
        
        
      }

     
        
if (!ctype) { //motion vectors from software h264 decoding
            
         
            AVFrameSideData *sd=NULL;

            sd = av_frame_get_side_data(mRawFrame, AV_FRAME_DATA_MOTION_VECTORS);
            uint16_t vector_ceiling=((((mRawFrame->width * mRawFrame->height)/16)*(double)20)/100);  //FIXMEC, just 20% of the maximum number of 4x4 blocks that will fit
        
            if (sd) {
                   
                   uint8_t offset=sizeof(uint16_t)*2;
                   const AVMotionVector *mvs = (const AVMotionVector *)sd->data;
                   uint16_t vec_count=0;
                   for (unsigned int i = 0; i < sd->size / sizeof(*mvs); i++) {
                        const AVMotionVector *mv = &mvs[i];
                      
                        motion_vector mvt;
                        
                        int x_disp = mv->src_x - mv->dst_x;
                        int y_disp = mv->src_y - mv->dst_y;
                        
                        
                        if ((abs(x_disp) + abs(y_disp)) < 1)
                            continue;
                        
                        for (uint16_t i=0 ; i< mv->w/4; i++) {
                           for (uint16_t j=0 ; j< mv->h/4; j++) {
                                mvt.xcoord=mv->dst_x+i*4;
                                mvt.ycoord=mv->dst_y+j*4;
                                
                                memcpy(mvect_buffer+offset,&mvt,sizeof(motion_vector));
                                offset+=sizeof(motion_vector);
                                vec_count++;
                           }
                       }
                      
                        if (vec_count > vector_ceiling) {  
                            memset(mvect_buffer,0,image.mv_size);
                            vec_count=0;
                            break;
                        }    
                        
                    }
                       memcpy(mvect_buffer,&vec_count, 2); //size on first byte
                       uint16_t vec_type = 1;
                         
                       memcpy(mvect_buffer+2,&vec_type, 2);   //type of vector at 3rd byte
                    //Info("FFMPEG SW VEC_COUNT %d, ceiling %d", vec_count, vector_ceiling);
               
            } 
        }    
        
#ifdef __arm__        
 if (ctype) { //motion vectors from hardware h264 encoding on the RPI only, the size of macroblocks are 16x16 pixels and there are a fixed number covering the entire frame.
                MMAL_BUFFER_HEADER_T *buffer;
                
                //send free buffer to encoder
                if ((buffer = mmal_queue_get(pool_out->queue)) != NULL) {
                   if (mmal_port_send_buffer(encoder->output[0], buffer) != MMAL_SUCCESS) {
                      Warning("failed to send buffer to encoder for frame %d\n", frameCount);
                      goto end;
                   }
                } 
                
                //send buffer with yuv420 data to encoder
                if ((buffer = mmal_queue_get(pool_in->queue)) != NULL)  {
                    
                  int bufsize=av_image_get_buffer_size(AV_PIX_FMT_YUV420P, mRawFrame->width, mRawFrame->height, 1);  
                    
                  mmal_buffer_header_mem_lock(buffer);
                   
                  av_image_copy_to_buffer(buffer->data, bufsize, (const uint8_t **)mRawFrame->data, mRawFrame->linesize,
                                 AV_PIX_FMT_YUV420P, mRawFrame->width, mRawFrame->height, 1);
                  buffer->length=bufsize;
                  //buffer->offset = 0; buffer->pts = buffer->dts = MMAL_TIME_UNKNOWN;
                  mmal_buffer_header_mem_unlock(buffer);
                  
           
                  if (mmal_port_send_buffer(encoder->input[0], buffer) != MMAL_SUCCESS) {
                       Warning("failed to send YUV420 buffer for frame %d\n", frameCount);
                       goto end;
                  }
                } 
                
                
                while ((buffer = mmal_queue_get(context.queue)) != NULL) {
                        
                      if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO) {
                          
                        uint8_t offset=sizeof(uint16_t)*2;
                        uint16_t vector_ceiling=(((mRawFrame->width * mRawFrame->height)/256)*(double)20)/100;
                        uint16_t vec_count=0;  
                        
                        mmal_buffer_header_mem_lock(buffer);
                        uint16_t size=buffer->length/4;
                        
                        const mmal_motion_vector *mvi = (const mmal_motion_vector *)buffer->data;
                        for (int i=0;i < size ; i++) {
                            motion_vector mvt;
                            const mmal_motion_vector *mv = &mvi[i]; 
                            
                            if ((abs(mv->x_vector) + abs(mv->y_vector)) < 1)
                               continue;
                          
                            mvt.xcoord = (i*16) % (mVideoCodecContext->width + 16);
                            mvt.ycoord = ((i*16)/(mVideoCodecContext->width+16))*16;
                            vec_count++;
                            
                            memcpy(mvect_buffer+offset,&mvt,sizeof(motion_vector));
                            offset+=sizeof(motion_vector);
                            
                            if (vec_count > vector_ceiling) {  
                              memset(mvect_buffer,0,image.mv_size);
                              vec_count=0;
                            break;
                        }    
                            
                         mmal_buffer_header_mem_unlock(buffer);    
                            
                         } 
                         memcpy(mvect_buffer,&vec_count, 2);  //size at first byte
                         uint16_t vec_type = 0;
                         
                         memcpy(mvect_buffer+2,&vec_type, 2);   //type of vector at 3rd byte
                         //Info("FFMPEG HW VEC_COUNT %d, ceiling %d", vec_count, vector_ceiling);
                        
                      } 
                    
                    
                    mmal_buffer_header_release(buffer);
                    
                  
                    if ((buffer = mmal_queue_get(pool_out->queue)) != NULL) {
                           if (mmal_port_send_buffer(encoder->output[0], buffer) != MMAL_SUCCESS) {
                              goto end;
                           } 
                    }         
                    
                    
                    
                    
                }
                
                

                
                
        } //if ctype
        
//  end:    
        
#endif
        
end:      
        

        

        
#if LIBAVUTIL_VERSION_CHECK(54, 6, 0, 6, 0)
        av_image_fill_arrays(mFrame->data, mFrame->linesize,
            directbuffer, imagePixFormat, width, height, 1);
#else
        avpicture_fill( (AVPicture *)mFrame, directbuffer,
            imagePixFormat, width, height);
#endif

#if HAVE_LIBSWSCALE
        if(mConvertContext == NULL) {
          mConvertContext = sws_getContext(mVideoCodecContext->width,
                                           mVideoCodecContext->height,
                                           mVideoCodecContext->pix_fmt,
                                           width, height, imagePixFormat,
                                           SWS_BICUBIC, NULL, NULL, NULL);

          if(mConvertContext == NULL)
            Fatal( "Unable to create conversion context for %s", mPath.c_str() );
        }

        if (sws_scale(mConvertContext, mRawFrame->data, mRawFrame->linesize, 0, mVideoCodecContext->height, mFrame->data, mFrame->linesize) < 0)
          Fatal("Unable to convert raw format %u to target format %u at frame %d", mVideoCodecContext->pix_fmt, imagePixFormat, frameCount);
#else // HAVE_LIBSWSCALE
        Fatal( "You must compile ffmpeg with the --enable-swscale option to use ffmpeg cameras" );
#endif // HAVE_LIBSWSCALE

        frameCount++;
      } // end if frameComplete
      
      
    } else {
      Debug( 4, "Different stream_index %d", packet.stream_index );
    } // end if packet.stream_index == mVideoStreamId
    zm_av_packet_unref( &packet );
  } // end while ! frameComplete
  return (0);
} // FfmpegCamera::Capture

int FfmpegCamera::PostCapture() {
  // Nothing to do here
  return( 0 );
}


#ifdef __arm__
void FfmpegCamera::input_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
   //CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;
   mmal_buffer_header_release(buffer);
}

void FfmpegCamera::output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
   CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;
   mmal_queue_put(ctx->queue, buffer);
}


int FfmpegCamera::OpenMmal(AVCodecContext *mVideoCodecContext){  
   

   // Create the encoder component.
   if ( mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder)  != MMAL_SUCCESS) {
      Fatal("failed to create mmal encoder");
   }   

   /* Set format of video encoder input port */
   MMAL_ES_FORMAT_T *format_in = encoder->input[0]->format;
   format_in->type = MMAL_ES_TYPE_VIDEO;
   format_in->encoding = MMAL_ENCODING_I420;
   format_in->es->video.width = mVideoCodecContext->width;
   format_in->es->video.height = mVideoCodecContext->height;
   format_in->es->video.frame_rate.num = 30;
   format_in->es->video.frame_rate.den = 1;
   format_in->es->video.par.num = 1;
   format_in->es->video.par.den = 1;
   format_in->es->video.crop.width = mVideoCodecContext->width;
   format_in->es->video.crop.height = mVideoCodecContext->height;
 

   
   if ( mmal_port_format_commit(encoder->input[0]) != MMAL_SUCCESS ) {
      Fatal("failed to commit mmal encoder input format");
   }   

   MMAL_ES_FORMAT_T *format_out = encoder->output[0]->format;
   format_out->type = MMAL_ES_TYPE_VIDEO;
   format_out->encoding = MMAL_ENCODING_H264;
   format_out->es->video.width = mVideoCodecContext->width;
   format_out->es->video.height = mVideoCodecContext->height;
   format_out->es->video.frame_rate.num = 30;
   format_out->es->video.frame_rate.den = 1;
   format_out->es->video.par.num = 0; 
   format_out->es->video.par.den = 1;
   
   
   if ( mmal_port_format_commit(encoder->output[0]) != MMAL_SUCCESS ) {
     Fatal("failed to commit output format");
   }
   
   if (mmal_port_parameter_set_boolean(encoder->output[0], MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS, 1) != MMAL_SUCCESS) {
      Fatal("failed to request inline motion vectors from mmal encoder");
   }   

   /* Display the input port format */
   Info("INPUT FORMAT \n");
   Info("%s\n", encoder->input[0]->name);
   Info(" type: %i, fourcc: %4.4s\n", format_in->type, (char *)&format_in->encoding);
   Info(" bitrate: %i, framed: %i\n", format_in->bitrate,
           !!(format_in->flags & MMAL_ES_FORMAT_FLAG_FRAMED));
   Info(" extra data: %i, %p\n", format_in->extradata_size, format_in->extradata);
   Info(" width: %i, height: %i, (%i,%i,%i,%i)\n",
           format_in->es->video.width, format_in->es->video.height,
           format_in->es->video.crop.x, format_in->es->video.crop.y,
           format_in->es->video.crop.width, format_in->es->video.crop.height);

   /* Display the output port format */
   Info("OUTPUT FORMAT \n");
   Info("%s\n", encoder->output[0]->name);
   Info(" type: %i, fourcc: %4.4s\n", format_out->type, (char *)&format_out->encoding);
   Info(" bitrate: %i, framed: %i\n", format_out->bitrate,
           !!(format_out->flags & MMAL_ES_FORMAT_FLAG_FRAMED));
   Info(" extra data: %i, %p\n", format_out->extradata_size, format_out->extradata);
   Info(" width: %i, height: %i, (%i,%i,%i,%i)\n",
           format_out->es->video.width, format_out->es->video.height,
           format_out->es->video.crop.x, format_out->es->video.crop.y,
           format_out->es->video.crop.width, format_out->es->video.crop.height);


   /* The format of both ports is now set so we can get their buffer requirements and create
    * our buffer headers. We use the buffer pool API to create these. */
   encoder->input[0]->buffer_num = encoder->input[0]->buffer_num_min;
   encoder->input[0]->buffer_size = encoder->input[0]->buffer_size_min;
   encoder->output[0]->buffer_num = encoder->output[0]->buffer_num_min;
   encoder->output[0]->buffer_size = encoder->output[0]->buffer_size_min;
   pool_in = mmal_pool_create(encoder->input[0]->buffer_num,
                              encoder->input[0]->buffer_size);
   pool_out = mmal_pool_create(encoder->output[0]->buffer_num,
                               encoder->output[0]->buffer_size);

   /* Create a queue to store our decoded video frames. The callback we will get when
    * a frame has been decoded will put the frame into this queue. */
   context.queue = mmal_queue_create();

   /* Store a reference to our context in each port (will be used during callbacks) */
   encoder->input[0]->userdata = (MMAL_PORT_USERDATA_T *)&context;
   encoder->output[0]->userdata = (MMAL_PORT_USERDATA_T *)&context;
   
   // Enable all the input port and the output port.
   if ( mmal_port_enable(encoder->input[0], input_callback) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal input port");
   }  
   
   if ( mmal_port_enable(encoder->output[0], output_callback) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal output port");
   }
   
   /* Component won't start processing data until it is enabled. */
   if ( mmal_component_enable(encoder) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal encoder component");
   }  

   return 0;

}

int FfmpegCamera::CloseMmal(){ 
   if (encoder)
      mmal_component_destroy(encoder);
   if (pool_in)
      mmal_pool_destroy(pool_in);
   if (pool_out)
      mmal_pool_destroy(pool_out);
   if (context.queue)
   mmal_queue_destroy(context.queue);
   
   return 0;
}
#endif

int FfmpegCamera::OpenFfmpeg() {

  Debug ( 2, "OpenFfmpeg called." );

  int ret;

  mOpenStart = time(NULL);
  mIsOpening = true;

  // Open the input, not necessarily a file
#if !LIBAVFORMAT_VERSION_CHECK(53, 2, 0, 4, 0)
  Debug ( 1, "Calling av_open_input_file" );
  if ( av_open_input_file( &mFormatContext, mPath.c_str(), NULL, 0, NULL ) !=0 )
#else
  // Handle options
  AVDictionary *opts = 0;
  ret = av_dict_parse_string(&opts, Options().c_str(), "=", ",", 0);
  if (ret < 0) {
    Warning("Could not parse ffmpeg input options list '%s'\n", Options().c_str());
  }

  // Set transport method as specified by method field, rtpUni is default
  if (Method() == "rtpMulti") {
    ret = av_dict_set(&opts, "rtsp_transport", "udp_multicast", 0);
  } else if (Method() == "rtpRtsp") {
    ret = av_dict_set(&opts, "rtsp_transport", "tcp", 0);
  } else if (Method() == "rtpRtspHttp") {
    ret = av_dict_set(&opts, "rtsp_transport", "http", 0);
  }

  if (ret < 0) {
    Warning("Could not set rtsp_transport method '%s'\n", Method().c_str());
  }

  Debug ( 1, "Calling avformat_open_input" );

  mFormatContext = avformat_alloc_context( );
  mFormatContext->interrupt_callback.callback = FfmpegInterruptCallback;
  mFormatContext->interrupt_callback.opaque = this;

  if ( avformat_open_input( &mFormatContext, mPath.c_str(), NULL, &opts ) !=0 )
#endif
  {
    mIsOpening = false;
    Error( "Unable to open input %s due to: %s", mPath.c_str(), strerror(errno) );
    return -1;
  }

  AVDictionaryEntry *e;
  if ((e = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX)) != NULL) {
    Warning( "Option %s not recognized by ffmpeg", e->key);
  }

  mIsOpening = false;
  Debug ( 1, "Opened input" );

  Info( "Stream open %s", mPath.c_str() );

  //FIXME can speed up initial analysis but need sensible parameters...
  //mFormatContext->probesize = 32;
  //mFormatContext->max_analyze_duration = 32;
  // Locate stream info from avformat_open_input
#if !LIBAVFORMAT_VERSION_CHECK(53, 6, 0, 6, 0)
  Debug ( 1, "Calling av_find_stream_info" );
  if ( av_find_stream_info( mFormatContext ) < 0 )
#else
    Debug ( 1, "Calling avformat_find_stream_info" );
  if ( avformat_find_stream_info( mFormatContext, 0 ) < 0 )
#endif
    Fatal( "Unable to find stream info from %s due to: %s", mPath.c_str(), strerror(errno) );

  startTime = av_gettime();//FIXME here or after find_Stream_info
  Debug ( 1, "Got stream info" );

  // Find first video stream present
  // The one we want Might not be the first
  mVideoStreamId = -1;
  mAudioStreamId = -1;
  for (unsigned int i=0; i < mFormatContext->nb_streams; i++ ) {
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
    if ( mFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ) {
#else
#if (LIBAVCODEC_VERSION_CHECK(52, 64, 0, 64, 0) || LIBAVUTIL_VERSION_CHECK(50, 14, 0, 14, 0))
    if ( mFormatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO ) {
#else
    if ( mFormatContext->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO ) {
#endif
#endif
      if ( mVideoStreamId == -1 ) {
        mVideoStreamId = i;
        // if we break, then we won't find the audio stream
        continue;
      } else {
        Debug(2, "Have another video stream." );
      }
    }
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
    if ( mFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ) {
#else
#if (LIBAVCODEC_VERSION_CHECK(52, 64, 0, 64, 0) || LIBAVUTIL_VERSION_CHECK(50, 14, 0, 14, 0))
    if ( mFormatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO ) {
#else
    if ( mFormatContext->streams[i]->codec->codec_type == CODEC_TYPE_AUDIO ) {
#endif
#endif
      if ( mAudioStreamId == -1 ) {
        mAudioStreamId = i;
      } else {
        Debug(2, "Have another audio stream." );
      }
    }
  } // end foreach stream
  if ( mVideoStreamId == -1 )
    Fatal( "Unable to locate video stream in %s", mPath.c_str() );
  if ( mAudioStreamId == -1 )
    Debug( 3, "Unable to locate audio stream in %s", mPath.c_str() );

  Debug ( 3, "Found video stream at index %d", mVideoStreamId );
  Debug ( 3, "Found audio stream at index %d", mAudioStreamId );

#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
  mVideoCodecContext = avcodec_alloc_context3( NULL );
  avcodec_parameters_to_context( mVideoCodecContext, mFormatContext->streams[mVideoStreamId]->codecpar );
#else
  mVideoCodecContext = mFormatContext->streams[mVideoStreamId]->codec;
#endif
	// STolen from ispy
	//this fixes issues with rtsp streams!! woot.
	//mVideoCodecContext->flags2 |= CODEC_FLAG2_FAST | CODEC_FLAG2_CHUNKS | CODEC_FLAG_LOW_DELAY;  // Enable faster H264 decode.
	mVideoCodecContext->flags2 |= CODEC_FLAG2_FAST | CODEC_FLAG_LOW_DELAY;

  AVDictionary *optsmv = NULL;
        
  // Try and get the codec from the codec context
  // VIDEO
  //software decoder      
  if (!ctype) {       
      if ((mVideoCodec = avcodec_find_decoder(mVideoCodecContext->codec_id)) == NULL) {
          Fatal("Can't find codec for video stream from %s", mPath.c_str());
      } else {
          //Debug(1, "Video Found decoder");
          Info("Found software decoder")
          zm_dump_stream_format(mFormatContext, mVideoStreamId, 0, 0);
      }
  }
    //hardware decoder  
  if (ctype) {       
        if ( (mVideoCodec = avcodec_find_decoder_by_name("h264_mmal")) == NULL ) {
               Fatal("Can't find codec for video stream from %s", mPath.c_str());
        } else {
           //Debug(1, "Video Found decoder");
           Info( "Found HW MMAL decoder");
           zm_dump_stream_format(mFormatContext, mVideoStreamId, 0, 0);
        }  
  
  }    

  //set the option for motionvector export if this is software h264 decoding
    if (!ctype)
        av_dict_set(&optsmv, "flags2", "+export_mvs", 0);

  
#if !LIBAVFORMAT_VERSION_CHECK(53, 8, 0, 8, 0)
  Debug ( 1, "Calling avcodec_open" );
  if (avcodec_open(mVideoCodecContext, mVideoCodec) < 0)
#else
    Debug ( 1, "Calling avcodec_open2" );
  if (avcodec_open2(mVideoCodecContext, mVideoCodec, &optsmv) < 0)
#endif
    Fatal( "Unable to open codec for video stream from %s", mPath.c_str() );

//AUIDO
  
  if ( mAudioStreamId >= 0 ) {
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
    mAudioCodecContext = avcodec_alloc_context3( NULL );
    avcodec_parameters_to_context( mAudioCodecContext, mFormatContext->streams[mAudioStreamId]->codecpar );
#else
    mAudioCodecContext = mFormatContext->streams[mAudioStreamId]->codec;
#endif
    if ((mAudioCodec = avcodec_find_decoder(mAudioCodecContext->codec_id)) == NULL) {
      Debug(1, "Can't find codec for audio stream from %s", mPath.c_str());
    } else {
      Debug(1, "Audio Found decoder");
      zm_dump_stream_format(mFormatContext, mAudioStreamId, 0, 0);
  // Open the codec
#if !LIBAVFORMAT_VERSION_CHECK(53, 8, 0, 8, 0)
  Debug ( 1, "Calling avcodec_open" );
  if (avcodec_open(mAudioCodecContext, mAudioCodec) < 0)
#else
    Debug ( 1, "Calling avcodec_open2" );
  if (avcodec_open2(mAudioCodecContext, mAudioCodec, 0) < 0)
#endif
    Fatal( "Unable to open codec for video stream from %s", mPath.c_str() );
    }
}        
        
  Debug ( 1, "Opened codec" );

  // Allocate space for the native video frame
  mRawFrame = zm_av_frame_alloc();

  // Allocate space for the converted video frame
  mFrame = zm_av_frame_alloc();

  if(mRawFrame == NULL || mFrame == NULL)
    Fatal( "Unable to allocate frame for %s", mPath.c_str() );

  Debug ( 1, "Allocated frames" );

#if LIBAVUTIL_VERSION_CHECK(54, 6, 0, 6, 0)
  int pSize = av_image_get_buffer_size( imagePixFormat, width, height,1 );
#else
  int pSize = avpicture_get_size( imagePixFormat, width, height );
#endif

  if( (unsigned int)pSize != imagesize) {
    Fatal("Image size mismatch. Required: %d Available: %d",pSize,imagesize);
  }

  Debug ( 1, "Validated imagesize" );

#if HAVE_LIBSWSCALE
  Debug ( 1, "Calling sws_isSupportedInput" );
  if (!sws_isSupportedInput(mVideoCodecContext->pix_fmt)) {
    Fatal("swscale does not support the codec format: %c%c%c%c", (mVideoCodecContext->pix_fmt)&0xff, ((mVideoCodecContext->pix_fmt >> 8)&0xff), ((mVideoCodecContext->pix_fmt >> 16)&0xff), ((mVideoCodecContext->pix_fmt >> 24)&0xff));
  }

  if(!sws_isSupportedOutput(imagePixFormat)) {
    Fatal("swscale does not support the target format: %c%c%c%c",(imagePixFormat)&0xff,((imagePixFormat>>8)&0xff),((imagePixFormat>>16)&0xff),((imagePixFormat>>24)&0xff));
  }

  mConvertContext = sws_getContext(mVideoCodecContext->width,
      mVideoCodecContext->height,
      mVideoCodecContext->pix_fmt,
      width, height,
      imagePixFormat, SWS_BICUBIC, NULL,
      NULL, NULL);
  if ( mConvertContext == NULL )
    Fatal( "Unable to create conversion context for %s", mPath.c_str() );
#else // HAVE_LIBSWSCALE
  Fatal( "You must compile ffmpeg with the --enable-swscale option to use ffmpeg cameras" );
#endif // HAVE_LIBSWSCALE

  if ( (unsigned int)mVideoCodecContext->width != width || (unsigned int)mVideoCodecContext->height != height ) {
    Warning( "Monitor dimensions are %dx%d but camera is sending %dx%d", width, height, mVideoCodecContext->width, mVideoCodecContext->height );
  }

  mCanCapture = true;
#ifdef __arm__
  if (ctype) { 
    OpenMmal(mVideoCodecContext);
  }
#endif  

  return 0;
} // int FfmpegCamera::OpenFfmpeg()

int FfmpegCamera::ReopenFfmpeg() {

  Debug(2, "ReopenFfmpeg called.");

  mCanCapture = false;
  if (pthread_create( &mReopenThread, NULL, ReopenFfmpegThreadCallback, (void*) this) != 0){
    // Log a fatal error and exit the process.
    Fatal( "ReopenFfmpeg failed to create worker thread." );
  }

  return 0;
}

int FfmpegCamera::CloseFfmpeg(){

  Debug(2, "CloseFfmpeg called.");
#ifdef __arm__
  if (ctype) {
     CloseMmal();
  }
#endif
  
  mCanCapture = false;

  av_frame_free( &mFrame );
  av_frame_free( &mRawFrame );

#if HAVE_LIBSWSCALE
  if ( mConvertContext ) {
    sws_freeContext( mConvertContext );
    mConvertContext = NULL;
  }
#endif

  if (mVideoCodecContext) {
    avcodec_close(mVideoCodecContext);
    mVideoCodecContext = NULL; // Freed by av_close_input_file
  }
  if (mAudioCodecContext) {
    avcodec_close(mAudioCodecContext);
    mAudioCodecContext = NULL; // Freed by av_close_input_file
  }

  if ( mFormatContext ) {
#if !LIBAVFORMAT_VERSION_CHECK(53, 17, 0, 25, 0)
    av_close_input_file( mFormatContext );
#else
    avformat_close_input( &mFormatContext );
#endif
    mFormatContext = NULL;
  }

  return 0;
}

int FfmpegCamera::FfmpegInterruptCallback(void *ctx) { 
  FfmpegCamera* camera = reinterpret_cast<FfmpegCamera*>(ctx);
  if (camera->mIsOpening){
    int now = time(NULL);
    if ((now - camera->mOpenStart) > config.ffmpeg_open_timeout) {
      Error ( "Open video took more than %d seconds.", config.ffmpeg_open_timeout );
      return 1;
    }
  }

  return 0;
}

void *FfmpegCamera::ReopenFfmpegThreadCallback(void *ctx){
  if (ctx == NULL) return NULL;

  FfmpegCamera* camera = reinterpret_cast<FfmpegCamera*>(ctx);

  while (1){
    // Close current stream.
    camera->CloseFfmpeg();

    // Sleep if necessary to not reconnect too fast.
    int wait = config.ffmpeg_open_timeout - (time(NULL) - camera->mOpenStart);
    wait = wait < 0 ? 0 : wait;
    if (wait > 0){
      Debug( 1, "Sleeping %d seconds before reopening stream.", wait );
      sleep(wait);
    }

    if (camera->OpenFfmpeg() == 0){
      return NULL;
    }
  }
}

//Function to handle capture and store
int FfmpegCamera::CaptureAndRecord( Image &image, timeval recording, char* event_file ) {
  if ( ! mCanCapture ) {
    return -1;
  }
  int ret;
  static char errbuf[AV_ERROR_MAX_STRING_SIZE];
  
  // If the reopen thread has a value, but mCanCapture != 0, then we have just reopened the connection to the ffmpeg device, and we can clean up the thread.
  if ( mReopenThread != 0 ) {
    void *retval = 0;

    ret = pthread_join(mReopenThread, &retval);
    if (ret != 0){
      Error("Could not join reopen thread.");
    }

    Info( "Successfully reopened stream." );
    mReopenThread = 0;
  }

  if ( mVideoCodecContext->codec_id != AV_CODEC_ID_H264 ) {
    Error( "Input stream is not h264.  The stored event file may not be viewable in browser." );
  }

  int frameComplete = false;
  while ( ! frameComplete ) {
    av_init_packet( &packet );

    ret = av_read_frame( mFormatContext, &packet );
    if ( ret < 0 ) {
      av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
      if (
          // Check if EOF.
          (ret == AVERROR_EOF || (mFormatContext->pb && mFormatContext->pb->eof_reached)) ||
          // Check for Connection failure.
          (ret == -110)
         ) {
          Info( "av_read_frame returned \"%s\". Reopening stream.", errbuf);
          ReopenFfmpeg();
      }

      Error( "Unable to read packet from stream %d: error %d \"%s\".", packet.stream_index, ret, errbuf );
      return( -1 );
    }

    int key_frame = packet.flags & AV_PKT_FLAG_KEY;

    Debug( 4, "Got packet from stream %d packet pts (%d) dts(%d), key?(%d)", 
        packet.stream_index, packet.pts, packet.dts, 
        key_frame
        );

    //Video recording
    if ( recording.tv_sec ) {

      uint32_t last_event_id = monitor->GetLastEventId() ;

      if ( last_event_id != monitor->GetVideoWriterEventId() ) {
        Debug(2, "Have change of event.  last_event(%d), our current (%d)", last_event_id, monitor->GetVideoWriterEventId() );

        if ( videoStore ) {
          Info("Re-starting video storage module");

          // I don't know if this is important or not... but I figure we might as well write this last packet out to the store before closing it.
          // Also don't know how much it matters for audio.
          if ( packet.stream_index == mVideoStreamId ) {
            //Write the packet to our video store
            int ret = videoStore->writeVideoFramePacket( &packet );
            if ( ret < 0 ) { //Less than zero and we skipped a frame
              Warning("Error writing last packet to videostore.");
            }
          } // end if video

          delete videoStore;
          videoStore = NULL;

          monitor->SetVideoWriterEventId( 0 );
        } // end if videoStore
      } // end if end of recording

      if ( last_event_id and ! videoStore ) {
        //Instantiate the video storage module

        if (record_audio) {
          if (mAudioStreamId == -1) {
            Debug(3, "Record Audio on but no audio stream found");
            videoStore = new VideoStore((const char *) event_file, "mp4",
                mFormatContext->streams[mVideoStreamId],
                NULL,
                startTime,
                this->getMonitor());

          } else {
            Debug(3, "Video module initiated with audio stream");
            videoStore = new VideoStore((const char *) event_file, "mp4",
                mFormatContext->streams[mVideoStreamId],
                mFormatContext->streams[mAudioStreamId],
                startTime,
                this->getMonitor());
          }
        } else {
          Debug(3, "Record_audio is false so exclude audio stream");
          videoStore = new VideoStore((const char *) event_file, "mp4",
              mFormatContext->streams[mVideoStreamId],
              NULL,
              startTime,
              this->getMonitor());
        } // end if record_audio
        strcpy(oldDirectory, event_file);
        monitor->SetVideoWriterEventId( last_event_id );

        // Need to write out all the frames from the last keyframe?
        // No... need to write out all frames from when the event began. Due to PreEventFrames, this could be more than since the last keyframe.
        unsigned int packet_count = 0;
        ZMPacket *queued_packet;

        // Clear all packets that predate the moment when the recording began
        packetqueue.clear_unwanted_packets( &recording, mVideoStreamId );

        while ( ( queued_packet = packetqueue.popPacket() ) ) {
          AVPacket *avp = queued_packet->av_packet();
            
          packet_count += 1;
          //Write the packet to our video store
          Debug(2, "Writing queued packet stream: %d  KEY %d, remaining (%d)", avp->stream_index, avp->flags & AV_PKT_FLAG_KEY, packetqueue.size() );
          if ( avp->stream_index == mVideoStreamId ) {
            ret = videoStore->writeVideoFramePacket( avp );
          } else if ( avp->stream_index == mAudioStreamId ) {
            ret = videoStore->writeAudioFramePacket( avp );
          } else {
            Warning("Unknown stream id in queued packet (%d)", avp->stream_index );
            ret = -1;
          }
          if ( ret < 0 ) {
            //Less than zero and we skipped a frame
          }
          delete queued_packet;
        } // end while packets in the packetqueue
        Debug(2, "Wrote %d queued packets", packet_count );
      } // end if ! was recording

    } else {
      // Not recording
      if ( videoStore ) {
        Info("Deleting videoStore instance");
        delete videoStore;
        videoStore = NULL;
        monitor->SetVideoWriterEventId( 0 );
      }

      // Buffer video packets, since we are not recording.
      // All audio packets are keyframes, so only if it's a video keyframe
      if ( packet.stream_index == mVideoStreamId ) {
        if ( key_frame ) {
          Debug(3, "Clearing queue");
          packetqueue.clearQueue( monitor->GetPreEventCount(), mVideoStreamId );
        } 
#if 0
// Not sure this is valid.  While a camera will PROBABLY always have an increasing pts... it doesn't have to.
// Also, I think there are integer wrap-around issues.

else if ( packet.pts && video_last_pts > packet.pts ) {
          Warning( "Clearing queue due to out of order pts packet.pts(%d) < video_last_pts(%d)");
          packetqueue.clearQueue();
        }
#endif
      } 
 
      // The following lines should ensure that the queue always begins with a video keyframe
      if ( packet.stream_index == mAudioStreamId ) {
//Debug(2, "Have audio packet, reocrd_audio is (%d) and packetqueue.size is (%d)", record_audio, packetqueue.size() );
        if ( record_audio && packetqueue.size() ) { 
          // if it's audio, and we are doing audio, and there is already something in the queue
          packetqueue.queuePacket( &packet );
        }
      } else if ( packet.stream_index == mVideoStreamId ) {
        if ( key_frame || packetqueue.size() ) // it's a keyframe or we already have something in the queue
          packetqueue.queuePacket( &packet );
      }
    } // end if recording or not

    if ( packet.stream_index == mVideoStreamId ) {
      if ( videoStore ) {
        //Write the packet to our video store
        int ret = videoStore->writeVideoFramePacket( &packet );
        if ( ret < 0 ) { //Less than zero and we skipped a frame
          zm_av_packet_unref( &packet );
          return 0;
        }
      }
      Debug(4, "about to decode video" );
      
#if LIBAVCODEC_VERSION_CHECK(57, 64, 0, 64, 0)
      ret = avcodec_send_packet( mVideoCodecContext, &packet );
      if ( ret < 0 ) {
        av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
        Error( "Unable to send packet at frame %d: %s, continuing", frameCount, errbuf );
        zm_av_packet_unref( &packet );
        continue;
      }
      ret = avcodec_receive_frame( mVideoCodecContext, mRawFrame );
      if ( ret < 0 ) {
        av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
        Debug( 1, "Unable to send packet at frame %d: %s, continuing", frameCount, errbuf );
        zm_av_packet_unref( &packet );
        continue;
      }
      frameComplete = 1;
# else
      ret = zm_avcodec_decode_video( mVideoCodecContext, mRawFrame, &frameComplete, &packet );
      if ( ret < 0 ) {
        av_strerror( ret, errbuf, AV_ERROR_MAX_STRING_SIZE );
        Error( "Unable to decode frame at frame %d: %s, continuing", frameCount, errbuf );
        zm_av_packet_unref( &packet );
        continue;
      }
#endif

      Debug( 4, "Decoded video packet at frame %d", frameCount );

      if ( frameComplete ) {
        Debug( 4, "Got frame %d", frameCount );

        uint8_t* directbuffer;

        /* Request a writeable buffer of the target image */
        directbuffer = image.WriteBuffer(width, height, colours, subpixelorder);
        if ( directbuffer == NULL ) {
          Error("Failed requesting writeable buffer for the captured image.");
          zm_av_packet_unref( &packet );
          return (-1);
        }
#if LIBAVUTIL_VERSION_CHECK(54, 6, 0, 6, 0)
        av_image_fill_arrays(mFrame->data, mFrame->linesize, directbuffer, imagePixFormat, width, height, 1);
#else
        avpicture_fill( (AVPicture *)mFrame, directbuffer, imagePixFormat, width, height);
#endif


        if (sws_scale(mConvertContext, mRawFrame->data, mRawFrame->linesize,
                      0, mVideoCodecContext->height, mFrame->data, mFrame->linesize) < 0) {
          Fatal("Unable to convert raw format %u to target format %u at frame %d",
                mVideoCodecContext->pix_fmt, imagePixFormat, frameCount);
        }

        frameCount++;
      } else {
        Debug( 3, "Not framecomplete after av_read_frame" );
      } // end if frameComplete
    } else if ( packet.stream_index == mAudioStreamId ) { //FIXME best way to copy all other streams
      if ( videoStore ) {
        if ( record_audio ) {
          Debug(3, "Recording audio packet streamindex(%d) packetstreamindex(%d)", mAudioStreamId, packet.stream_index );
          //Write the packet to our video store
          //FIXME no relevance of last key frame
          int ret = videoStore->writeAudioFramePacket( &packet );
          if ( ret < 0 ) {//Less than zero and we skipped a frame
            Warning("Failure to write audio packet.");
            zm_av_packet_unref( &packet );
            return 0;
          }
        } else {
          Debug(4, "Not doing recording of audio packet" );
        }
      } else {
        Debug(4, "Have audio packet, but not recording atm" );
      }
    } else {
#if LIBAVUTIL_VERSION_CHECK(56, 23, 0, 23, 0)
      Debug( 3, "Some other stream index %d, %s", packet.stream_index, av_get_media_type_string( mFormatContext->streams[packet.stream_index]->codecpar->codec_type) );
#else
      Debug( 3, "Some other stream index %d", packet.stream_index );
#endif
    }
    //if ( videoStore ) {
      
      // the packet contents are ref counted... when queuing, we allocate another packet and reference it with that one, so we should always need to unref here, which should not affect the queued version.
      zm_av_packet_unref( &packet );
    //}
  } // end while ! frameComplete
  return (frameCount);
} // end FfmpegCamera::CaptureAndRecord

#endif // HAVE_LIBAVFORMAT

