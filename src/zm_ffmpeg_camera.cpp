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

  uint8_t* directbuffer=NULL;
  uint8_t* mvect_buffer=NULL;  
  
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

   

    Debug( 4, "Got packet from stream %d packet pts (%d) dts(%d)", 
        packet.stream_index, packet.pts, packet.dts
        );


    if ( packet.stream_index == mVideoStreamId ) {
     
      Debug(4, "about to decode video" );
      
    if (!ctype) {   
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
      
      
   }  else { //if ctype
    //the mmal decoder builds mRawFrame here with an I420 buffer	
      frameComplete=mmal_decode(&packet);
      Debug( 4, "Decoded video packet at frame %d", frameCount );
      
   }  //if cytpe   

  } //if packet index is video

  if ( packet.stream_index == mVideoStreamId ) {
	  //Mvect buffer to be requested for both ctype and !ctype
        if (cfunction == Monitor::MVDECT) {
           mvect_buffer=image.VectBuffer();   
           if (mvect_buffer ==  NULL ){
                Error("Failed requesting vector buffer for the captured image.");
                return (-1); 
           } 
        } 
	  
	  
  }	
  
  if ( packet.stream_index == mVideoStreamId ) {      
//FRAMECOMPLETE 1 -> got a decoded packet and in case of mmal, 
                   //mmal_decode has filled MRawFrame with I420 buffer at this point

      if ( frameComplete ) {
        //Debug( 4, "Got frame %d", frameCount );

        /* Request a writeable buffer of the target image */
        directbuffer = image.WriteBuffer(width, height, colours, subpixelorder);
                   
        if ( directbuffer == NULL ) {
          Error("Failed requesting writeable buffer for the captured image.");
          zm_av_packet_unref( &packet );
          return (-1);
        }
        
        
        
        
        //mmal will use mFrame to store the rgb buffer
        if  (ctype) {
           
#if LIBAVUTIL_VERSION_CHECK(54, 6, 0, 6, 0)
            av_image_fill_arrays(mFrame->data, mFrame->linesize,
                     directbuffer, imagePixFormat, width, height, 1);
#else
            avpicture_fill( (AVPicture *)mFrame, directbuffer,
                     imagePixFormat, width, height);
#endif
        
            frameComplete=mmal_resize(&directbuffer); 
           
        } else  if (((ctype) && (cfunction == Monitor::MODECT)) || (!ctype)) {
	      //This is the software scaler. Directbuffer is packaged into mFrame and processed by swscale to convert to appropriate format and size
          //Need SWScale when Source Type is FFmpeg with Function as Mvdect or Modect
          //or Source type is FFmpeghw and Function is Modect  
          //This is only used if source is FFmpeg or source is FFmpeghw and Function is Modect. 
			
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
		frameCount++; //MODECT mode ends here
	    }	
        
         
      }  //if framecomplete 1 



//FRAMECOMPLETE 2 -> mmal_resize has taken the buffer from mRawFrame, resized it and put it in mFrame        
        
      if (frameComplete) {
         if ((ctype) && (cfunction == Monitor::MVDECT)) {
		         
		        frameComplete=mmal_encode(&mvect_buffer);
		        
		}  //if cfunction
	  } //if frameComplete 2
	  
	  
	  
//FRAMECOMPLETE 3 -> mmal_encode successfully received an output buffer. 	  
	
		
	  if ( frameComplete ) {
		    if  ((ctype) && (cfunction == Monitor::MVDECT)) {
				
				Visualize_Buffer(&directbuffer);	    
				
				//Create the JPEG buffer for this frame if frame is alarmed, using downscaled resolution
                uint8_t* jpegbuffer=NULL;
                jpegbuffer=image.JPEGBuffer(width, height);
                if (jpegbuffer ==  NULL ){
                   Error("Failed requesting jpeg buffer for the captured image.");
                   return (-1); 
                }
			

			    //Option 1. use the image EncodeJpeg function which requires argument jpeg_size
			    if (jmode == libjpeg) {
		            int *jpeg_size=(int *)jpegbuffer; 
				    image.EncodeJpeg(jpegbuffer+4, jpeg_size );
			    } else 
			    //Option 2. use hardware mmal.
			    if (jmode == mmal)
     		        mmal_jpeg(&jpegbuffer);


        
          } //if cfunction
        
          
        frameCount++;
      }  // end if frameComplete
      
      
      
    } else if ( packet.stream_index == mAudioStreamId ) { //FIXME best way to copy all other streams
		
	 //Nothing to do
      
    } else {
#if LIBAVUTIL_VERSION_CHECK(56, 23, 0, 23, 0)
      Debug( 3, "Some other stream index %d, %s", packet.stream_index, av_get_media_type_string( mFormatContext->streams[packet.stream_index]->codecpar->codec_type) );
#else
      Debug( 3, "Some other stream index %d", packet.stream_index );
#endif
    }
      
      // the packet contents are ref counted... when queuing, we allocate another packet and reference it with that one, so we should always need to unref here, which should not affect the queued version.
      zm_av_packet_unref( &packet );
  } // end while ! frameComplete
  
  
  
  //Info("Framecount is %d", frameCount);
  return (frameCount);
} // FfmpegCamera::Capture

int FfmpegCamera::PostCapture() {
  // Nothing to do here
  return( 0 );
}


#ifdef __arm__
void FfmpegCamera::input_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
   CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;
   mmal_buffer_header_release(buffer);
}

void FfmpegCamera::output_callbackr(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
   CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;
   mmal_queue_put(ctx->rqueue, buffer);
}

void FfmpegCamera::output_callbacke(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
   CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;
   mmal_queue_put(ctx->equeue, buffer);
}

void FfmpegCamera::output_callbackd(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
   CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;
   mmal_queue_put(ctx->dqueue, buffer);
}

void FfmpegCamera::output_callbackj(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
   CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;
   mmal_queue_put(ctx->jqueue, buffer);
}



/** Callback from the control port.
 * Component is sending us an event. */
void FfmpegCamera::control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   struct CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;

   switch (buffer->cmd)
   {
   case MMAL_EVENT_EOS:
      /* Only sink component generate EOS events */
      Warning("PORT: %s MMAL EOS\n", port->name);
      break;
   case MMAL_EVENT_ERROR:
      /* Something went wrong. Signal this to the application */
      ctx->status = *(MMAL_STATUS_T *)buffer->data;
      Warning("ERROR: PORT: %s Type: %d\n", port->name, ctx->status);
      break;
   default:
      break;
   }

   /* Done with the event, recycle it */
   mmal_buffer_header_release(buffer);

}



void FfmpegCamera::pixel_write(RGB24 *rgb_ptr, int b_index, pattern r_pattern, RGB24 rgb) {
	    if (b_index <0)
	      return;
        
       /* switch (r_pattern) {
		  case nw:
		      DEFAULT=NW;
		      break;
		  case w:
		      DEFAULT=WEST;
		      break;
		  case sw:
		      DEFAULT=SW;
		      break;
		  case s:
		      DEFAULT=SOUTH;
		      break;
		  case se:
		      DEFAULT=SE;
		      break;
		  case e:
		      DEFAULT=EAST;
		      break;
		  case ne:
		      DEFAULT=NE;
		      break;
		  case n:
		      DEFAULT=NORTH;
		      break;
		  case center:
		      return;    
		 }
		*/              
		int c_index=0;                                	
		for ( int i=0; i<25 ; i++) {
		   if ((P_ARRAY+r_pattern)->bpattern & (0x80000000 >> i)) {
			  c_index=b_index + *(cpixel+i);
			  (rgb_ptr+c_index)->R=rgb.R;
              (rgb_ptr+c_index)->G=rgb.G;
              (rgb_ptr+c_index)->B=rgb.B;   
		   }	   
		   	
		}		
			
          	
	
}	

int FfmpegCamera::mmal_decode(AVPacket *pkt) {   
	MMAL_BUFFER_HEADER_T *buffer;
	int got_frame=false;
	
	if ((buffer = mmal_queue_get(pool_ind->queue)) != NULL) {  
         
         memcpy(buffer->data,pkt->data,pkt->size);
         buffer->length=pkt->size;
         
         buffer->flags|=MMAL_BUFFER_HEADER_FLAG_FRAME_START;
         buffer->flags|=MMAL_BUFFER_HEADER_FLAG_FRAME_END;
         
         buffer->pts = pkt->pts == AV_NOPTS_VALUE ? MMAL_TIME_UNKNOWN : pkt->pts;
         buffer->dts = pkt->dts == AV_NOPTS_VALUE ? MMAL_TIME_UNKNOWN : pkt->dts;
         
         buffer->alloc_size = decoder->input[0]->buffer_size;
            
         if (mmal_port_send_buffer(decoder->input[0], buffer) != MMAL_SUCCESS) {
                 Warning("failed to send H264 buffer to decoder for frame %d\n", frameCount);
                  
         }
                 
         
      }

      
      //while ((buffer = mmal_queue_get(context.dqueue)) != NULL)
      //while ((buffer = mmal_queue_timedwait(context.dqueue, 50)) != NULL) {
      if ((buffer = mmal_queue_get(context.dqueue)) == NULL)
         buffer = mmal_queue_timedwait(context.dqueue, 50);
      if (buffer) {
         //save it as AVFrame holding an I420 buffer with original video source resolution
         av_image_fill_arrays(mRawFrame->data, mRawFrame->linesize, buffer->data, AV_PIX_FMT_YUV420P, mRawFrame->width, mRawFrame->height, 1);
         got_frame=true;
         
         mmal_buffer_header_release(buffer);
      }

      //if ((buffer = mmal_queue_get(pool_outd->queue)) != NULL) {
      while ((buffer = mmal_queue_get(pool_outd->queue)) != NULL){
		 if (mmal_port_send_buffer(decoder->output[0], buffer) != MMAL_SUCCESS) {
			   Warning("failed to send empty buffer to decoder output for frame %d\n", frameCount);
         } 
      }
      
     return (got_frame);    
}	



int FfmpegCamera::mmal_encode(uint8_t **mv_buffer) {  //uses mFrame (downscaled frame) data 
	MMAL_BUFFER_HEADER_T *buffer;
	int got_result=false;
	
               
	if ((buffer = mmal_queue_get(pool_ine->queue)) != NULL) {  
         
         av_image_copy_to_buffer(buffer->data, bufsize_r, (const uint8_t **)mFrame->data, mFrame->linesize,
                                 encoderPixFormat, mFrame->width, mFrame->height, 1);
         buffer->length=bufsize_r;
         
         buffer->pts = buffer->dts = MMAL_TIME_UNKNOWN;
         
         buffer->alloc_size = encoder->input[0]->buffer_size;
          
         if (mmal_port_send_buffer(encoder->input[0], buffer) != MMAL_SUCCESS) {
                 Warning("failed to send I420 buffer to encoder for frame %d\n", frameCount);
                  
         }   
         
         
      }

      
      //while ((buffer = mmal_queue_get(context.equeue)) != NULL) {
      //while ((buffer = mmal_queue_timedwait(context.equeue, 100)) != NULL) {
      if ((buffer = mmal_queue_get(context.equeue)) == NULL)
         buffer = mmal_queue_timedwait(context.equeue, 50);
      if (buffer) {   
            
         if(buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO) {
			      got_result=true;  //succeeded wether we receive a video buffer or vector buffer  
			      
			      uint32_t m_offset=0; 
			      
                  for (int i=0; i < czones_n ; i++) {
                        mmal_motion_vector *mvarray=(mmal_motion_vector *)buffer->data;
                        czones[i]->motion_detected=false;
						
						
				        Blocks *cur_block=0;  //this block
				        int bcount=0;  //block counter
						
						//ALARMED PIXELS VARIABLES
						uint32_t alarm_pixels=0;
						
						uint32_t wcount=0;  //counts to 32 
						uint32_t registers=0;
                        uint32_t mask=0;
                        uint32_t res=0;
                        
                        uint32_t c=0;
                        uint32_t vec_count=0;
						
						uint32_t offset=0;
                        uint8_t* zone_vector_mask=czones[i]->zone_vector_mask;
                        
						//FILTERED PIXELS VARIABLES
						int min_filtered=czones[i]->GetMinFilteredPixels();
						int max_filtered=czones[i]->GetMaxFilteredPixels();
						uint32_t filter_pixels=0;
						
						
	                    int vcount=0;  //container counter
	                    int ccount=0;  //column counter
	                    int rcount=0;  //row counter
	
	                    Blocks *prev_block=0; //block preceeding 
	                    Blocks *up_block=0;   //block above
	                    Blocks *block_row_up=NULL;
	                    
	                    int blob_count=0;
						
	                    memcpy(&mask, zone_vector_mask+offset, sizeof(mask));
                        for (int j=0; j<rows; j++) {
		                   for (int k=0; k<columns; k++) {
							  
							  cur_block=(Block+bcount);
							  cur_block->status=0;
							  cur_block->index=bcount;
							  
		                      if ((abs(mvarray[bcount].x_vector) + abs(mvarray[bcount].y_vector)) > min_vector_distance) { 
								 cur_block->status=1;
							     registers = registers | (0x80000000 >> wcount);
							        
							        
								    //-----------direction-start
							     if (display_vectors) {
						        	uint8_t block_direction=0;
							   
							        //[nw][w][sw][s][se][e][ne][n]
							        //[0] [1][2] [3][4] [5][6] [7]
							 
							 
							        //[n][ne][e][se][s][sw][w][nw]
							        //[7][6] [5][4] [3][2] [1][0] 
								    
								                                
 /*     N      */                 if (mvarray[bcount].x_vector >0) { //to e
                                      if (mvarray[bcount].y_vector >0) { //to se
 /* W       E  */                         block_direction |= 1<<4;
                                    } else if (mvarray[bcount].y_vector <0) { //to ne
 /*     S      */                         block_direction |= 1<<6;
                                    } else { //e
                                          block_direction |= 1<<5;
							        }
                                  } else if (mvarray[bcount].x_vector <0) {  //to w 
								     if (mvarray[bcount].y_vector >0) { //to sw
                                         block_direction |= 1<<2; 
                                     } else if (mvarray[bcount].y_vector <0) { //to nw
                                         block_direction |= 1<<0;
                                     } else { //w
                                         block_direction |= 1<<1;
							      }
								     
                                  } else { //dx=0
								    if (mvarray[bcount].y_vector >0) { //to s
                                         block_direction |= 1<<3; 
                                    } else if (mvarray[bcount].y_vector <0) { //to n
                                         block_direction |= 1<<7;
                                    } else { //dy=0
                                
							        }  
							      }
							
							     //each Block save to the zones direction buffer on the jth position
							     memcpy(direction[i]+bcount,&block_direction,sizeof(block_direction));
								    
							     } //if display_vectors
							        
							        
							  } //mvarray	
							  
							  
							  wcount++;   	   
		                         
		                      //-------------COUNT VECTORS AND FILL RESULTS MASK-----------------------------------------
							  if ( wcount == 32) { //last batch of less than 32 bits will not be saved
							   
                                 memcpy(&mask, zone_vector_mask+offset, sizeof(mask));  
                                 res= registers & mask;
                                 memcpy(result[i]+offset,&res,sizeof(res));
                                 c =  ((res & 0xfff) * 0x1001001001001ULL & 0x84210842108421ULL) % 0x1f;
                                 c += (((res & 0xfff000) >> 12) * 0x1001001001001ULL & 0x84210842108421ULL) % 0x1f;
                                 c += ((res >> 24) * 0x1001001001001ULL & 0x84210842108421ULL) % 0x1f;
                                 vec_count+=c;
						       
                                 wcount=0;
                                 offset+=4;
                                 registers=0;

                              } 
                              //++++++++++++++COUNT VECTORS AND FILL RESULTS MASK+++++++++++++++++++++++++++++++++++++++++
							  
								 
							  //FILTERED PIXELS START---------------------------------------------------------------------	
							  if (czones[i]->GetCheckMethod() == 2) {	
								    
								if (cur_block->status) {  //If THIS is ON  
								    
			                        if (rcount == 0) {         //if this is first row, no connection to TOP possible, so stuff into CURRENT bin
			                            cur_block->vect=&v_arr[vcount];
			                            v_arr[vcount].push_back(cur_block);
			                        } else { 
				                         if ((ccount >0) && (ccount < columns)) {
				                             prev_block=(Block+bcount-1);
				      
				                             if (prev_block->status) {
						                        cur_block->vect=prev_block->vect;
					                            prev_block->vect->push_back(cur_block); //add THIS to the left bin if connected to the LEFT
					     
						                        up_block = (block_row_up+ccount); 
						                        if (up_block->status) {  //add TOP bin contents to the left bin too if connected to the TOP
							                        if (prev_block->vect != up_block->vect) {
							                            //prev_block->vect->reserve( up_block->vect->size() + prev_block->vect->size() );   
                                                        prev_block->vect->insert( prev_block->vect->end(), up_block->vect->begin(), up_block->vect->end() );
                                                        up_block->vect->clear();
						                            }
            						            }        	  
					  
					                         }	//if prev_block is ON
					                         else { 
						                        up_block = (block_row_up+ccount); 
						                        if (up_block->status) {    //if not connected to the LEFT add THIS to the TOP bin if connected to the TOP
							                       cur_block->vect=up_block->vect;
					                               up_block->vect->push_back(cur_block);
							  
						                        } else {             //if not connected to the LEFT and not connected to the TOP, put in a CURRENT bin
						                           cur_block->vect=&v_arr[vcount];
			                                       v_arr[vcount].push_back(cur_block);
						                        }
				                             }	 //if prev_block is OFF	    
			                             } else {//if column 0 
				                             cur_block->vect=&v_arr[vcount];
				                             v_arr[vcount].push_back(cur_block);
				     
			                             }	  
		                            }  //if rcount>0 closing bracket 
		   
	                             }   else {  //if THIS is OFF
			  
			                         if (v_arr[vcount].size()) {  //start a NEW BIN only if the current bin is not empty
			                            if (vcount < 50){
			                               vcount++;
			                            } else { 
				                           Info("Blobber out of vectors");	     
			                               break;
			                            } 
			                         }    
	                             }
	                             
		   
	                             
	                             ccount++;	
		                         
		                         //-----------------------------------------------------
	                             if (ccount==columns) {
			                         block_row_up=(Block+(ccount*rcount)); //set the row pointer to the proper position in the Block* array  
			                         ccount=0;
			                         rcount++;
			                         if (v_arr[vcount].size()) {
		                                 vcount++; //when moving from row to row, there needs to be a new container
		                             }
	                             }
	                             
	                             
	                            } //FILTERED PIXELS END---------------------------------------------------------------------
	                            
	                            
	                            
	                            //Block counter iterator
	                            bcount++;
                           } //for k
                         }  //for j
                         
                         
                         //----------RESULTS------------------------------------------------------
                         
                         //--------------------Level 1 Scoring
                         if (czones[i]->GetCheckMethod() == 1) {
                               alarm_pixels = vec_count<<(8+score_shift_multiplier);
					           memcpy((*mv_buffer)+m_offset ,&alarm_pixels, 4 );
					           //czones[i]->motion_detected=true; //FIXME, only turn on if there are sufficient alarm_pixels 
					           //if (alarm_pixels)	
							       //Info("Alarm pixels score %d", alarm_pixels); 	    
                         } 
                         
                         //-----------------_--Level 2 scoring
                         else if (czones[i]->GetCheckMethod() == 2) {
                         //figure the score by adding the total blocks in all bins 
                         
                               if (vcount > 50)  //avoid segfault when blobber is out of vectors
                                   vcount=50;
                               for (int m=0; m < vcount ; m++) {
							          if ((v_arr[m].size() > min_filtered) && (v_arr[m].size() < max_filtered)) {
							              blob_count+=v_arr[m].size();
							              for (int n=0; n< v_arr[m].size() ; n++){
								              v_arr[m][n]->status=2;	
							              }	 
							          }
								     
	     				              v_arr[m].clear();    
	                           }	 
	                     
                         
                         
                               filter_pixels=blob_count<<(8+score_shift_multiplier);
						       memcpy((*mv_buffer)+m_offset ,&filter_pixels, 4 );
					           //czones[i]->motion_detected=true; //FIXME, only set if filter_pixels above threshold
						       //if (filter_pixels)	
						           //Info("Filter pixels score %d", filter_pixels);
					     } 
					     
                         
                         m_offset+=4;
                        
                     } //czones_n
	     } //buffer_flags_codec_sideinfo
	     
	     
         
         mmal_buffer_header_release(buffer);
      }

      //if ((buffer = mmal_queue_get(pool_out->queue)) != NULL) {
      while ((buffer = mmal_queue_get(pool_oute->queue)) != NULL) {
                   if (mmal_port_send_buffer(encoder->output[0], buffer) != MMAL_SUCCESS) {
                      Warning("failed to send buffer to encoder output for frame %d\n", frameCount);
                   }
		  
      }
      
      return (got_result);
}	



int  FfmpegCamera::mmal_resize(uint8_t** dbuffer) {   //uses mRawFrame data, builds mFrame
	int got_resized=false;
	MMAL_BUFFER_HEADER_T *buffer;
	if ((buffer = mmal_queue_get(pool_inr->queue)) != NULL) { 
		 
         av_image_copy_to_buffer(buffer->data, bufsize_d, (const uint8_t **)mRawFrame->data, mRawFrame->linesize,
                                 AV_PIX_FMT_YUV420P, mRawFrame->width, mRawFrame->height, 1);
         buffer->length=bufsize_d;
         
         buffer->pts = buffer->dts = MMAL_TIME_UNKNOWN;
         //buffer->flags=packet->flags;
         
         buffer->alloc_size = resizer->input[0]->buffer_size;
            
         if (mmal_port_send_buffer(resizer->input[0], buffer) != MMAL_SUCCESS) {
                 Warning("failed to send I420 buffer to resizer for frame %d\n", frameCount);
                  
         }      
         
      }
      
      //while ((buffer = mmal_queue_get(context.rqueue)) != NULL){
      //while ((buffer = mmal_queue_timedwait(context.rqueue, 100)) != NULL) {
      if ((buffer = mmal_queue_get(context.rqueue)) == NULL)
         buffer = mmal_queue_timedwait(context.rqueue, 50);
      if (buffer) {
         got_resized=true;
         memcpy((*dbuffer),buffer->data,width*height*colours);
         //save it as AVFrame holding a buffer with original video source resolution
         av_image_fill_arrays(mFrame->data, mFrame->linesize, *dbuffer, encoderPixFormat, mFrame->width, mFrame->height, 1);
         
         mmal_buffer_header_release(buffer);
      }

     
      //if ((buffer = mmal_queue_get(pool_outr->queue)) != NULL) {
      while ((buffer = mmal_queue_get(pool_outr->queue)) != NULL) {
                   if (mmal_port_send_buffer(resizer->output[0], buffer) != MMAL_SUCCESS) {
                      Warning("failed to send buffer to resizer output for frame %d\n", frameCount);
                   }
		  
      }
     return (got_resized);    
}	


int  FfmpegCamera::mmal_jpeg(uint8_t** jbuffer) {   //uses mFrame data
	int got_jpeg=false;
	MMAL_BUFFER_HEADER_T *buffer;
	if ((buffer = mmal_queue_get(pool_inj->queue)) != NULL) { 
		 
         av_image_copy_to_buffer(buffer->data, bufsize_r, (const uint8_t **)mFrame->data, mFrame->linesize,
                                 encoderPixFormat, mFrame->width, mFrame->height, 1);
         buffer->length=bufsize_r;
         
         buffer->pts = buffer->dts = MMAL_TIME_UNKNOWN;
         //buffer->flags=packet->flags;
         buffer->flags|=MMAL_BUFFER_HEADER_FLAG_FRAME_END;
         
         buffer->alloc_size = jcoder->input[0]->buffer_size;
            
         if (mmal_port_send_buffer(jcoder->input[0], buffer) != MMAL_SUCCESS) {
                 Warning("failed to send RGB buffer to jpeg encoder for frame %d\n", frameCount);
                  
         }
         
      }
      
      //while ((buffer = mmal_queue_get(context.jqueue)) != NULL) {
      //while ((buffer = mmal_queue_timedwait(context.jqueue, 100)) != NULL) {
      if ((buffer = mmal_queue_get(context.jqueue)) == NULL)
         buffer = mmal_queue_timedwait(context.jqueue, 50);
      if (buffer) {
		 got_jpeg=true; 
         if (buffer->length < jpeg_limit) {
           memcpy((*jbuffer),&buffer->length,4);
           memcpy((*jbuffer)+4,buffer->data,buffer->length);
         } else {
		   Info("JPEG buffer is too small at %d, while actual jpeg size is %d for quality %d", jpeg_limit, buffer->length, config.jpeg_file_quality);
	       int zero_size=0;
           memcpy((*jbuffer),&zero_size,4);
	     }
         mmal_buffer_header_release(buffer);
      }

     
      //if ((buffer = mmal_queue_get(pool_outr->queue)) != NULL) {
      while ((buffer = mmal_queue_get(pool_outj->queue)) != NULL) {
                   if (mmal_port_send_buffer(jcoder->output[0], buffer) != MMAL_SUCCESS) {
                      Warning("failed to send buffer to jpeg encoder output for frame %d\n", frameCount);
                   }
		  
      }
     return (got_jpeg);    
}	




void FfmpegCamera::display_format(MMAL_PORT_T **port, MMAL_ES_FORMAT_T **iformat){
 	/* Display the port format */
   Info("---------------------------------------------------\n");
   Info("PORT %s\n", (*port)->name);
   Info(" type: %i, fourcc: %4.4s\n", (*iformat)->type, (char *)&(*iformat)->encoding);
   Info(" bitrate: %i, framed: %i\n", (*iformat)->bitrate,
           !!((*iformat)->flags & MMAL_ES_FORMAT_FLAG_FRAMED));
   Info(" extra data: %i, %p\n", (*iformat)->extradata_size, (*iformat)->extradata);
   Info(" width: %i, height: %i, (%i,%i,%i,%i)\n",
           (*iformat)->es->video.width, (*iformat)->es->video.height,
           (*iformat)->es->video.crop.x, (*iformat)->es->video.crop.y,
           (*iformat)->es->video.crop.width, (*iformat)->es->video.crop.height);
   Info("PORT %s BUFFER SIZE %d NUM %d\n", (*port)->name, (*port)->buffer_size,(*port)->buffer_num);
   Info("---------------------------------------------------\n");        
       
}	

int FfmpegCamera::OpenMmalDecoder(AVCodecContext *mVideoCodecContext){  
   

   // Create the decoder component.
   if ( mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, &decoder)  != MMAL_SUCCESS) {
      Fatal("failed to create mmal decoder");
   }   
   
   // CONTROL PORT SETTINGS
   decoder->control->userdata = (MMAL_PORT_USERDATA_T *)&context;
   if ( mmal_port_enable(decoder->control, control_callback) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal decoder control port");
   }  
   
   
   /* Get statistics on the input port */
   MMAL_PARAMETER_CORE_STATISTICS_T stats = {{0}};
   stats.hdr.id = MMAL_PARAMETER_CORE_STATISTICS;
   stats.hdr.size = sizeof(MMAL_PARAMETER_CORE_STATISTICS_T);
   if (mmal_port_parameter_get(decoder->input[0], &stats.hdr) != MMAL_SUCCESS) {
     Info("failed to get decoder port statistics");
   }
   else {
     Info("Decoder stats: %i, %i", stats.stats.buffer_count, stats.stats.max_delay);
   }
   
   
   // Set the zero-copy parameter on the input port 
   MMAL_PARAMETER_BOOLEAN_T zc = {{MMAL_PARAMETER_ZERO_COPY, sizeof(zc)}, MMAL_TRUE};
   if (mmal_port_parameter_set(decoder->input[0], &zc.hdr) != MMAL_SUCCESS)
     Info("Failed to set zero copy on decoder input");

   // Set the zero-copy parameter on the output port 
   if (mmal_port_parameter_set_boolean(decoder->output[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE) != MMAL_SUCCESS)
     Info("Failed to set zero copy on decoder output");
    
   
   /* Set format of video decoder input port */
   MMAL_ES_FORMAT_T *format_in = decoder->input[0]->format;
   format_in->type = MMAL_ES_TYPE_VIDEO;
   format_in->encoding = MMAL_ENCODING_H264;
   
   format_in->es->video.width = VCOS_ALIGN_UP(mVideoCodecContext->width, 32);
   format_in->es->video.height = VCOS_ALIGN_UP(mVideoCodecContext->height,16);
   format_in->es->video.crop.width = mVideoCodecContext->width;
   format_in->es->video.crop.height = mVideoCodecContext->height;
   
   format_in->es->video.frame_rate.num = 24000;
   format_in->es->video.frame_rate.den = 1001;
   format_in->es->video.par.num = mVideoCodecContext->sample_aspect_ratio.num;
   format_in->es->video.par.den = mVideoCodecContext->sample_aspect_ratio.den;
   format_in->flags = MMAL_ES_FORMAT_FLAG_FRAMED;
 

   
   if ( mmal_port_format_commit(decoder->input[0]) != MMAL_SUCCESS ) {
      Fatal("failed to commit mmal decoder input format");
   }   

   MMAL_ES_FORMAT_T *format_out = decoder->output[0]->format;
   format_out->type = MMAL_ES_TYPE_VIDEO;
   format_out->encoding = MMAL_ENCODING_I420;
  
   
   //ALLOCATE Extradata, copying from avcodec context
   if (mmal_format_extradata_alloc(format_in, mVideoCodecContext->extradata_size) != MMAL_SUCCESS)
     Fatal("failed to allocate extradata ");
     
   Info("Decoder extradata size %d\n", mVideoCodecContext->extradata_size);
   format_in->extradata_size = mVideoCodecContext->extradata_size;
   if (format_in->extradata_size)
      memcpy(format_in->extradata, mVideoCodecContext->extradata, mVideoCodecContext->extradata_size);
   
   
   if ( mmal_port_format_commit(decoder->output[0]) != MMAL_SUCCESS ) {
     Fatal("failed to commit decoder output format");
   }
   


   /* The format of both ports is now set so we can get their buffer requirements and create
    * our buffer headers. We use the buffer pool API to create these. */
   //decoder->input[0]->buffer_num = decoder->input[0]->buffer_num_recommended;
   decoder->input[0]->buffer_num = 1;
   decoder->input[0]->buffer_size = 512*1024;
   decoder->output[0]->buffer_num = decoder->output[0]->buffer_num_recommended;
   decoder->output[0]->buffer_size = decoder->output[0]->buffer_size_recommended;
 
   pool_ind = mmal_port_pool_create(decoder->input[0],decoder->input[0]->buffer_num,
                              decoder->input[0]->buffer_size);
   pool_outd = mmal_port_pool_create(decoder->output[0],decoder->output[0]->buffer_num,
                               decoder->output[0]->buffer_size);
   /*                            
   pool_ind = mmal_pool_create(decoder->input[0]->buffer_num,
                              decoder->input[0]->buffer_size);
   pool_outd = mmal_pool_create(decoder->output[0]->buffer_num,
                               decoder->output[0]->buffer_size);
   */                             
                               
   /* Display the input port format */
   display_format(&decoder->input[0],&format_in);
   
   display_format(&decoder->output[0],&format_out);
                               

   /* Create a queue to store our decoded video frames. The callback we will get when
    * a frame has been decoded will put the frame into this queue. */
   context.dqueue = mmal_queue_create();

   /* Store a reference to our context in each port (will be used during callbacks) */
   decoder->input[0]->userdata = (MMAL_PORT_USERDATA_T *)&context;
   decoder->output[0]->userdata = (MMAL_PORT_USERDATA_T *)&context;
   
   // Enable all the input port and the output port.
   if ( mmal_port_enable(decoder->input[0], input_callback) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal decoder input port");
   }  
   
   if ( mmal_port_enable(decoder->output[0], output_callbackd) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal decoder output port");
   }
   
   /* Component won't start processing data until it is enabled. */
   if ( mmal_component_enable(decoder) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal decoder component");
   }  

   return 0;

}


int FfmpegCamera::OpenMmalEncoder(AVCodecContext *mVideoCodecContext){  
   

   // Create the encoder component.
   if ( mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder)  != MMAL_SUCCESS) {
      Fatal("failed to create mmal encoder");
   }   
   
   // CONTROL PORT SETTINGS
   encoder->control->userdata = (MMAL_PORT_USERDATA_T *)&context;
   if ( mmal_port_enable(encoder->control, control_callback) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal encoder control port");
   }  
   
   /* Get statistics on the input port */
   MMAL_PARAMETER_CORE_STATISTICS_T stats = {{0}};
   stats.hdr.id = MMAL_PARAMETER_CORE_STATISTICS;
   stats.hdr.size = sizeof(MMAL_PARAMETER_CORE_STATISTICS_T);
   if (mmal_port_parameter_get(encoder->input[0], &stats.hdr) != MMAL_SUCCESS) {
     Info("failed to get encoder port statistics");
   }
   else {
     Info("Encoder stats: %i, %i", stats.stats.buffer_count, stats.stats.max_delay);
   }
   
   // Set the zero-copy parameter on the input port 
   MMAL_PARAMETER_BOOLEAN_T zc = {{MMAL_PARAMETER_ZERO_COPY, sizeof(zc)}, MMAL_TRUE};
   if (mmal_port_parameter_set(encoder->input[0], &zc.hdr) != MMAL_SUCCESS)
     Info("Failed to set zero copy on encoder input");

   // Set the zero-copy parameter on the output port 
   if (mmal_port_parameter_set_boolean(encoder->output[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE) != MMAL_SUCCESS)
     Info("Failed to set zero copy on encoder output");
    
    
   /* Set format of video encoder input port */
   MMAL_ES_FORMAT_T *format_in = encoder->input[0]->format;
   format_in->type = MMAL_ES_TYPE_VIDEO;
   
   if ( colours == ZM_COLOUR_RGB32 ) {
       format_in->encoding = MMAL_ENCODING_RGBA;
   } else if ( colours == ZM_COLOUR_RGB24 ) {
       format_in->encoding = MMAL_ENCODING_RGB24;
   } else if(colours == ZM_COLOUR_GRAY8) { 
       format_in->encoding = MMAL_ENCODING_I420;
   }
   
   
   format_in->es->video.width = VCOS_ALIGN_UP(width, 32);
   format_in->es->video.height = VCOS_ALIGN_UP(height,16);
   format_in->es->video.crop.width = width;
   format_in->es->video.crop.height = height;
   
   format_in->es->video.frame_rate.num = 24000;
   format_in->es->video.frame_rate.den = 1001;
   format_in->es->video.par.num = mVideoCodecContext->sample_aspect_ratio.num;
   format_in->es->video.par.den = mVideoCodecContext->sample_aspect_ratio.den;
   format_in->flags = MMAL_ES_FORMAT_FLAG_FRAMED;
 

   
   if ( mmal_port_format_commit(encoder->input[0]) != MMAL_SUCCESS ) {
      Fatal("failed to commit mmal encoder input format");
   }   

   MMAL_ES_FORMAT_T *format_out = encoder->output[0]->format;
   format_out->type = MMAL_ES_TYPE_VIDEO;
   format_out->encoding = MMAL_ENCODING_H264;
   
   format_out->es->video.width = VCOS_ALIGN_UP(width, 32);
   format_out->es->video.height = VCOS_ALIGN_UP(height,16);
   format_out->es->video.crop.width = width;
   format_out->es->video.crop.height = height;
   
   
   if ( mmal_port_format_commit(encoder->output[0]) != MMAL_SUCCESS ) {
     Fatal("failed to commit encoder output format");
   }
   
   if (mmal_port_parameter_set_boolean(encoder->output[0], MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS, 1) != MMAL_SUCCESS) {
      Fatal("failed to request inline motion vectors from mmal encoder");
   }   

   /* Display the input port format */
   display_format(&encoder->input[0],&format_in);
   
   display_format(&encoder->output[0],&format_out);
   

   /* The format of both ports is now set so we can get their buffer requirements and create
    * our buffer headers. We use the buffer pool API to create these. */
   encoder->input[0]->buffer_num = encoder->input[0]->buffer_num_min;
   encoder->input[0]->buffer_size = encoder->input[0]->buffer_size_min;
   encoder->output[0]->buffer_num = encoder->output[0]->buffer_num_min;
   encoder->output[0]->buffer_size = encoder->output[0]->buffer_size_min;
   
   pool_ine = mmal_port_pool_create(encoder->input[0],encoder->input[0]->buffer_num,
                              encoder->input[0]->buffer_size);
   pool_oute = mmal_port_pool_create(encoder->output[0],encoder->output[0]->buffer_num,
                               encoder->output[0]->buffer_size);
   /*                           
   pool_ine = mmal_pool_create(encoder->input[0]->buffer_num,
                              encoder->input[0]->buffer_size);
   pool_oute = mmal_pool_create(encoder->output[0]->buffer_num,
                               encoder->output[0]->buffer_size);                            
   */
   /* Create a queue to store our decoded video frames. The callback we will get when
    * a frame has been decoded will put the frame into this queue. */
   context.equeue = mmal_queue_create();

   /* Store a reference to our context in each port (will be used during callbacks) */
   encoder->input[0]->userdata = (MMAL_PORT_USERDATA_T *)&context;
   encoder->output[0]->userdata = (MMAL_PORT_USERDATA_T *)&context;
   
   // Enable all the input port and the output port.
   if ( mmal_port_enable(encoder->input[0], input_callback) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal encoder input port");
   }  
   
   if ( mmal_port_enable(encoder->output[0], output_callbacke) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal encoder output port");
   }
   
   /* Component won't start processing data until it is enabled. */
   if ( mmal_component_enable(encoder) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal encoder component");
   }  
   
   return 0;

}

int FfmpegCamera::OpenMmalResizer(AVCodecContext *mVideoCodecContext){  
   

   // Create the Resizer component.
   if ( mmal_component_create("vc.ril.isp", &resizer)  != MMAL_SUCCESS) { 
      Fatal("failed to create mmal resizer");
   }   
   
   // CONTROL PORT SETTINGS
   resizer->control->userdata = (MMAL_PORT_USERDATA_T *)&context;
   if ( mmal_port_enable(resizer->control, control_callback) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal resizer control port");
   }  
   
   /* Get statistics on the input port */
   MMAL_PARAMETER_CORE_STATISTICS_T stats = {{0}};
   stats.hdr.id = MMAL_PARAMETER_CORE_STATISTICS;
   stats.hdr.size = sizeof(MMAL_PARAMETER_CORE_STATISTICS_T);
   if (mmal_port_parameter_get(resizer->input[0], &stats.hdr) != MMAL_SUCCESS) {
     Info("failed to get resizer port statistics");
   }
   else {
     Info("Resizer stats: %i, %i", stats.stats.buffer_count, stats.stats.max_delay);
   }
   
   // Set the zero-copy parameter on the input port 
   MMAL_PARAMETER_BOOLEAN_T zc = {{MMAL_PARAMETER_ZERO_COPY, sizeof(zc)}, MMAL_TRUE};
   if (mmal_port_parameter_set(resizer->input[0], &zc.hdr) != MMAL_SUCCESS)
     Info("Failed to set zero copy on resizer input");

   // Set the zero-copy parameter on the output port 
   if (mmal_port_parameter_set_boolean(resizer->output[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE) != MMAL_SUCCESS)
     Info("Failed to set zero copy on resizer output");
    
   
   /* Set format of video resizer input port */
   MMAL_ES_FORMAT_T *format_in = resizer->input[0]->format;
   format_in->type = MMAL_ES_TYPE_VIDEO;
   format_in->encoding = MMAL_ENCODING_I420;
   format_in->encoding_variant = MMAL_ENCODING_I420;
   
   format_in->es->video.width = VCOS_ALIGN_UP(mVideoCodecContext->width, 32);
   format_in->es->video.height = VCOS_ALIGN_UP(mVideoCodecContext->height,16);
   format_in->es->video.crop.width = mVideoCodecContext->width;
   format_in->es->video.crop.height = mVideoCodecContext->height;
   
   format_in->es->video.frame_rate.num = 24000;
   format_in->es->video.frame_rate.den = 1001;
   format_in->es->video.par.num = mVideoCodecContext->sample_aspect_ratio.num;
   format_in->es->video.par.den = mVideoCodecContext->sample_aspect_ratio.den;
   format_in->flags = MMAL_ES_FORMAT_FLAG_FRAMED;
   
 

   
   if ( mmal_port_format_commit(resizer->input[0]) != MMAL_SUCCESS ) {
      Fatal("failed to commit mmal resizer input format");
   }   

   
   
   MMAL_ES_FORMAT_T *format_out = resizer->output[0]->format;
   format_out->es->video.width = VCOS_ALIGN_UP(width, 32);
   format_out->es->video.height = VCOS_ALIGN_UP(height,16);
   format_out->es->video.crop.width = width;
   format_out->es->video.crop.height = height;
   
  
   
   if ( colours == ZM_COLOUR_RGB32 ) {
       format_out->encoding = MMAL_ENCODING_RGBA;
       encoderPixFormat=AV_PIX_FMT_RGBA;
   } else if ( colours == ZM_COLOUR_RGB24 ) {
       format_out->encoding = MMAL_ENCODING_RGB24;
       encoderPixFormat=AV_PIX_FMT_RGB24;
   } else if(colours == ZM_COLOUR_GRAY8) { 
       format_out->encoding = MMAL_ENCODING_I420;
       encoderPixFormat=AV_PIX_FMT_GRAY8 ;
   }
   
   
#if LIBAVUTIL_VERSION_CHECK(54, 6, 0, 6, 0)
  bufsize_r = av_image_get_buffer_size(encoderPixFormat , mFrame->width, mFrame->height,1 );
  
#else
  bufsize_r = avpicture_get_size(encoderPixFormat, mFrame->width, mFrame->height );
#endif
   
   if ( mmal_port_format_commit(resizer->output[0]) != MMAL_SUCCESS ) {
     Fatal("failed to commit mmal resizer output format");
   }
   

   /* Display the input port format */
   display_format(&resizer->input[0],&format_in);
   
   display_format(&resizer->output[0],&format_out);
   

   /* The format of both ports is now set so we can get their buffer requirements and create
    * our buffer headers. We use the buffer pool API to create these. */
   resizer->input[0]->buffer_num = resizer->input[0]->buffer_num_min;
   resizer->input[0]->buffer_size = resizer->input[0]->buffer_size_min;
   resizer->output[0]->buffer_num = resizer->output[0]->buffer_num_min;
   resizer->output[0]->buffer_size = resizer->output[0]->buffer_size_min;
   
   
   pool_inr = mmal_port_pool_create(resizer->input[0],resizer->input[0]->buffer_num,
                              resizer->input[0]->buffer_size);
   pool_outr = mmal_port_pool_create(resizer->output[0],resizer->output[0]->buffer_num,
                               resizer->output[0]->buffer_size);
    
   /*
   pool_inr = mmal_pool_create(resizer->input[0]->buffer_num,
                              resizer->input[0]->buffer_size);
   pool_outr = mmal_pool_create(resizer->output[0]->buffer_num,
                               resizer->output[0]->buffer_size);
   */ 
   /* Create a queue to store our decoded video frames. The callback we will get when
    * a frame has been decoded will put the frame into this queue. */
   context.rqueue = mmal_queue_create();

   /* Store a reference to our context in each port (will be used during callbacks) */
   resizer->input[0]->userdata = (MMAL_PORT_USERDATA_T *)&context;
   resizer->output[0]->userdata = (MMAL_PORT_USERDATA_T *)&context;
   
   // Enable all the input port and the output port.
   if ( mmal_port_enable(resizer->input[0], input_callback) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal resizer input port");
   }  
   
   if ( mmal_port_enable(resizer->output[0], output_callbackr) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal resizer output port");
   }
   
   /* Component won't start processing data until it is enabled. */
   if ( mmal_component_enable(resizer) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal resizer component");
   }  

   return 0;

}

int FfmpegCamera::OpenMmalJPEG(AVCodecContext *mVideoCodecContext){  
   

   // Create the jcoder component.
   if ( mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER, &jcoder)  != MMAL_SUCCESS) {
      Fatal("failed to create mmal jpeg encoder");
   }   
   
   // CONTROL PORT SETTINGS
   jcoder->control->userdata = (MMAL_PORT_USERDATA_T *)&context;
   if ( mmal_port_enable(jcoder->control, control_callback) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal jpeg encoder control port");
   }  
   
   /* Get statistics on the input port */
   MMAL_PARAMETER_CORE_STATISTICS_T stats = {{0}};
   stats.hdr.id = MMAL_PARAMETER_CORE_STATISTICS;
   stats.hdr.size = sizeof(MMAL_PARAMETER_CORE_STATISTICS_T);
   if (mmal_port_parameter_get(jcoder->input[0], &stats.hdr) != MMAL_SUCCESS) {
     Info("failed to get jpeg encoder port statistics");
   }
   else {
     Info("JPEG encoder stats: %i, %i", stats.stats.buffer_count, stats.stats.max_delay);
   }
   
   // Set the zero-copy parameter on the input port 
   MMAL_PARAMETER_BOOLEAN_T zc = {{MMAL_PARAMETER_ZERO_COPY, sizeof(zc)}, MMAL_TRUE};
   if (mmal_port_parameter_set(jcoder->input[0], &zc.hdr) != MMAL_SUCCESS)
     Info("Failed to set zero copy on jpeg encoder input");
   // Set the zero-copy parameter on the output port 
   if (mmal_port_parameter_set_boolean(jcoder->output[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE) != MMAL_SUCCESS)
     Info("Failed to set zero copy on jpeg encoder output");
    
    
   /* Set format of jpeg encoder input port */
   MMAL_ES_FORMAT_T *format_in = jcoder->input[0]->format;
   format_in->type = MMAL_ES_TYPE_VIDEO;
   
   if ( colours == ZM_COLOUR_RGB32 ) {
       format_in->encoding = MMAL_ENCODING_RGBA;
   } else if ( colours == ZM_COLOUR_RGB24 ) {
       format_in->encoding = MMAL_ENCODING_RGB24;
   } else if(colours == ZM_COLOUR_GRAY8) { 
       format_in->encoding = MMAL_ENCODING_I420;
   }
   
   
   format_in->es->video.width = VCOS_ALIGN_UP(width, 32);
   format_in->es->video.height = VCOS_ALIGN_UP(height,16);
   format_in->es->video.crop.width = width;
   format_in->es->video.crop.height = height;
   
   format_in->es->video.frame_rate.num = 24000;
   format_in->es->video.frame_rate.den = 1001;
   format_in->es->video.par.num = mVideoCodecContext->sample_aspect_ratio.num;
   format_in->es->video.par.den = mVideoCodecContext->sample_aspect_ratio.den;
   format_in->flags = MMAL_ES_FORMAT_FLAG_FRAMED;
 

   
   if ( mmal_port_format_commit(jcoder->input[0]) != MMAL_SUCCESS ) {
      Fatal("failed to commit mmal jpeg encoder input format");
   }   

   MMAL_ES_FORMAT_T *format_out = jcoder->output[0]->format;
   format_out->type = MMAL_ES_TYPE_VIDEO;
   format_out->encoding = MMAL_ENCODING_JPEG;
   
   format_out->es->video.width = VCOS_ALIGN_UP(width, 32);
   format_out->es->video.height = VCOS_ALIGN_UP(height,16);
   format_out->es->video.crop.width = width;
   format_out->es->video.crop.height = height;
   
   
   if ( mmal_port_format_commit(jcoder->output[0]) != MMAL_SUCCESS ) {
     Fatal("failed to commit jpeg encoder output format");
   }
   
   //FIXME, should get from config
   if (mmal_port_parameter_set_uint32(jcoder->output[0], MMAL_PARAMETER_JPEG_Q_FACTOR, config.jpeg_file_quality) != MMAL_SUCCESS) {
   Fatal("failed to set jpeg quality for mmal jpeg encoder to %d",config.jpeg_file_quality);
   }   

   /* Display the input port format */
   display_format(&jcoder->input[0],&format_in);
   
   display_format(&jcoder->output[0],&format_out);
   

   /* The format of both ports is now set so we can get their buffer requirements and create
    * our buffer headers. We use the buffer pool API to create these. */
   jcoder->input[0]->buffer_num = jcoder->input[0]->buffer_num_min;
   jcoder->input[0]->buffer_size = jcoder->input[0]->buffer_size_min;
   jcoder->output[0]->buffer_num = jcoder->output[0]->buffer_num_min;
   jcoder->output[0]->buffer_size = jcoder->output[0]->buffer_size_min;
   
   pool_inj = mmal_port_pool_create(jcoder->input[0],jcoder->input[0]->buffer_num,
                              jcoder->input[0]->buffer_size);
   pool_outj = mmal_port_pool_create(jcoder->output[0],jcoder->output[0]->buffer_num,
                               jcoder->output[0]->buffer_size);
   /*                           
   pool_inj = mmal_pool_create(jcoder->input[0]->buffer_num,
                              jcoder->input[0]->buffer_size);
   pool_outj = mmal_pool_create(jcoder->output[0]->buffer_num,
                               jcoder->output[0]->buffer_size);                            
   */
   /* Create a queue to store our decoded video frames. The callback we will get when
    * a frame has been decoded will put the frame into this queue. */
   context.jqueue = mmal_queue_create();

   /* Store a reference to our context in each port (will be used during callbacks) */
   jcoder->input[0]->userdata = (MMAL_PORT_USERDATA_T *)&context;
   jcoder->output[0]->userdata = (MMAL_PORT_USERDATA_T *)&context;
   
   // Enable all the input port and the output port.
   if ( mmal_port_enable(jcoder->input[0], input_callback) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal jpeg encoder input port");
   }  
   
   if ( mmal_port_enable(jcoder->output[0], output_callbackj) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal jpeg encoder output port");
   }
   
   /* Component won't start processing data until it is enabled. */
   if ( mmal_component_enable(jcoder) != MMAL_SUCCESS ) {
     Fatal("failed to enable mmal jpeg encoder component");
   }  

   return 0;

}


int FfmpegCamera::CloseMmal(){
if (sigprocmask(SIG_BLOCK, &ctype_sigmask, NULL) == -1)
       Info("Failed to block SIGKILL");
       	
   MMAL_BUFFER_HEADER_T *buffer;
	
   mmal_port_disable(decoder->input[0]);
   mmal_port_disable(decoder->output[0]);
   mmal_port_disable(decoder->control); 
      
   mmal_port_flush(decoder->input[0]);
   mmal_port_flush(decoder->output[0]);
   mmal_port_flush(decoder->control); 
   
   while ((buffer = mmal_queue_get(context.dqueue)))
           mmal_buffer_header_release(buffer);
      
   mmal_port_disable(encoder->input[0]);   
   mmal_port_disable(encoder->output[0]);
   mmal_port_disable(encoder->control); 
      
   mmal_port_flush(encoder->input[0]);
   mmal_port_flush(encoder->output[0]);
   mmal_port_flush(encoder->control); 
   
   while ((buffer = mmal_queue_get(context.equeue)))
           mmal_buffer_header_release(buffer);
   
   mmal_port_disable(resizer->input[0]);
   mmal_port_disable(resizer->output[0]);
   mmal_port_disable(resizer->control); 
   
   mmal_port_flush(resizer->input[0]);
   mmal_port_flush(resizer->output[0]);
   mmal_port_flush(resizer->control); 
   
   while ((buffer = mmal_queue_get(context.rqueue)))
           mmal_buffer_header_release(buffer);
      
   
   mmal_port_disable(jcoder->input[0]);
   mmal_port_disable(jcoder->output[0]);
   mmal_port_disable(jcoder->control); 
   
   mmal_port_flush(jcoder->input[0]);
   mmal_port_flush(jcoder->output[0]);
   mmal_port_flush(jcoder->control); 	
   
   while ((buffer = mmal_queue_get(context.rqueue)))
           mmal_buffer_header_release(buffer);        
	
	
   if (decoder)
      mmal_component_destroy(decoder);
   if (resizer)
      mmal_component_destroy(resizer);
   if (encoder)
      mmal_component_destroy(encoder);
   if (jcoder)
      mmal_component_destroy(jcoder);      
      
      
   if (pool_ind)
      mmal_pool_destroy(pool_ind);
   if (pool_outd)
      mmal_pool_destroy(pool_outd);
   if (pool_inr)
      mmal_pool_destroy(pool_inr);
   if (pool_outr)
      mmal_pool_destroy(pool_outr);
   if (pool_ine)
      mmal_pool_destroy(pool_ine);
   if (pool_oute)
      mmal_pool_destroy(pool_oute);
   if (pool_inj)
      mmal_pool_destroy(pool_inj);
   if (pool_outj)
      mmal_pool_destroy(pool_outj);
      
      
   if (context.equeue)
      mmal_queue_destroy(context.equeue);
   if (context.rqueue)
      mmal_queue_destroy(context.rqueue);
   if (context.dqueue)
      mmal_queue_destroy(context.dqueue);  
   if (context.jqueue)
      mmal_queue_destroy(context.jqueue);  	 
   
   if (sigprocmask(SIG_UNBLOCK, &ctype_sigmask, NULL) == -1)
       Info("Failed to unblock SIGKILL");
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

  //set the option for motionvector export if this is software h264 decoding
   if (!ctype)
        av_dict_set(&optsmv, "flags2", "+export_mvs", 0);

   if (!ctype) {
  // Open the video codec
#if !LIBAVFORMAT_VERSION_CHECK(53, 8, 0, 8, 0)
  Debug ( 1, "Calling avcodec_open" );
  if (avcodec_open(mVideoCodecContext, mVideoCodec) < 0)
#else
    Debug ( 1, "Calling avcodec_open2" );
  if (avcodec_open2(mVideoCodecContext, mVideoCodec, &optsmv) < 0)
#endif
    Fatal( "Unable to open codec for video stream from %s", mPath.c_str() );
}
//AUDIO
  
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
  // Open the audio codec
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
  // Decoder will output mVideoCodecContext->width * mVideoCodecContext->height
  // So need mRawFrame to hold original video resolution.
  mRawFrame->width = VCOS_ALIGN_UP(mVideoCodecContext->width,32);
  mRawFrame->height = VCOS_ALIGN_UP(mVideoCodecContext->height,16); 
  
  //Final output frame used by swscale and mmal_encode
  mFrame = zm_av_frame_alloc(); 
  
  mFrame->width = VCOS_ALIGN_UP(width,32);
  mFrame->height = VCOS_ALIGN_UP(height,16); 
  

  if(mRawFrame == NULL || mFrame == NULL)
    Fatal( "Unable to allocate frame for %s", mPath.c_str() );

  Debug ( 1, "Allocated frames" );

#if LIBAVUTIL_VERSION_CHECK(54, 6, 0, 6, 0)
  int pSize = av_image_get_buffer_size( imagePixFormat, width, height,1 );
  bufsize_d = av_image_get_buffer_size(AV_PIX_FMT_YUV420P , mRawFrame->width, mRawFrame->height,1 );
  
#else
  int pSize = avpicture_get_size( imagePixFormat, width, height );
  bufsize_d = avpicture_get_size(AV_PIX_FMT_YUV420P, mRawFrame->width, mRawFrame->height );
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

#ifdef __arm__ 
  if ( (unsigned int)mVideoCodecContext->width != width || (unsigned int)mVideoCodecContext->height != height ) {
    Info( "Monitor dimensions adusted to %dx%d from camera %dx%d", width, height, mVideoCodecContext->width, mVideoCodecContext->height );
  }
#else
  if ( (unsigned int)mVideoCodecContext->width != width || (unsigned int)mVideoCodecContext->height != height ) {
    Warning( "Monitor dimensions are %dx%d but camera is sending %dx%d", width, height, mVideoCodecContext->width, mVideoCodecContext->height );
  }
#endif  
  
  Info("Width %d, Height %d, Codec->width %d, Codec->height %d",width, height, mVideoCodecContext->width, mVideoCodecContext->height);

  mCanCapture = true;
#ifdef __arm__
  
    
  if (ctype) { 
	  
   if ((sigemptyset(&ctype_sigmask) == -1) || (sigaddset(&ctype_sigmask, SIGKILL) == -1) || (sigaddset(&ctype_sigmask, SIGTERM) == -1)){
     Info("Failed to initialize the capture signal mask");
     return -1;
   }  
	
	if (sigprocmask(SIG_BLOCK, &ctype_sigmask, NULL) == -1)
       Info("Failed to block SIGKILL");
       
	OpenMmalDecoder(mVideoCodecContext);
    OpenMmalEncoder(mVideoCodecContext);
    OpenMmalResizer(mVideoCodecContext);
    OpenMmalJPEG(mVideoCodecContext);
    
    if (sigprocmask(SIG_UNBLOCK, &ctype_sigmask, NULL) == -1)
       Info("Failed to unblock SIGKILL"); 
    
    int j_height=((height+16)/16)*16;
    int j_width=((width+32)/32)*32; 
    jpeg_limit=(j_width*j_height);  //Avoid segfault in case jpeg is bigger than the buffer.
    
    
    columns=(monitor->Width()+16)/16; //41
    rows=(((monitor->Height()+16)/16)*16)/16; //23
  
    numblocks= (columns*rows);  //943
    Info("ZMC with numblocks %d", numblocks);
    
    Block=(Blocks*)zm_mallocaligned(4,sizeof(Blocks)*numblocks);
    
    
    //Create a lookup table for numblocks coordinates and associated rgb index
    //The blocks will have coordinates outside of the frame due to the extra column
    //The rgbindex is only positive if the coordinates are within frame dimensions
    coords=(Coord*)zm_mallocaligned(4,sizeof(Coord)*numblocks);
    for ( int i=0; i< numblocks; i++) {
	   coords[i].X()=(i*16) % (width + 16);
	   coords[i].Y()=((i*16)/(width +16))*16;
	   (Block+i)->coords=coords+i;
	   if ((coords[i].X() < monitor->Width()) && (coords[i].Y() < monitor->Height()))
	      (Block+i)->rgbindex=coords[i].Y()*(width)+coords[i].X();
	   else
	      (Block+i)->rgbindex=-1;
	}	
	
	
	//Setup the bit patterns for displaying motion vector directionality
	P_ARRAY=(bit_pattern*)zm_mallocaligned(4,sizeof(int)*9);
	
	
	                                 
	*(P_ARRAY+0)=bit_pattern(0xE6282000); /*11100  NW
	                                        11000
	                                        10100
	                                        00010
	                                        00000
	                                        00000
	                                        00*/
	
	*(P_ARRAY+1)=bit_pattern(0x223C8200); /*00100  W
	                                        01000
	                                        11110
	                                        01000
	                                        00100
	                                        00000
	                                        00*/
	                                        
	*(P_ARRAY+2)=bit_pattern(0xA98E00);   /*00000  SW
	                                        00010
	                                        10100
	                                        11000
	                                        11100
	                                        00000
	                                        00*/
	                                        
	*(P_ARRAY+3)=bit_pattern(0x12AE200);  /*00000  S
	                                        00100
	                                        10101
	                                        01110
	                                        00100
	                                        00000
	                                        00*/

	*(P_ARRAY+4)=bit_pattern(0x20A3380);  /*00000  SE
	                                        01000
	                                        00101
	                                        00011
	                                        00111
	                                        00000
	                                        00*/
	                                      
	*(P_ARRAY+5)=bit_pattern(0x209E2200); /*00100  E
	                                        00010
	                                        01111
	                                        00010
	                                        00100
	                                        00000
	                                        00*/
	                                        
	*(P_ARRAY+6)=bit_pattern(0x38CA8000); /*00111  NE
	                                        00011
	                                        00101
	                                        01000
	                                        00000
	                                        00000
	                                        00*/
	                                        
	*(P_ARRAY+7)=bit_pattern(0x23AA4000); /*00100  N
	                                        01110
	                                        10101
	                                        00100
	                                        00000
	                                        00000
	                                        00*/
	
	//*(P_ARRAY+8) unused
	
	
	//Calculate relative indexes of center 5x5 pixels of each macroblock
	cpixel=(int*)zm_mallocaligned(4,sizeof(int)*32);
	int topleft=5;
	for (int i=0; i<5; i++) {
	   *(cpixel+i)=5*monitor->Width()+(topleft++);	
	}	
	topleft=5;
	for (int i=5; i<10; i++) {
       *(cpixel+i)=6*monitor->Width()+(topleft++);		
	}	
	topleft=5;
	for (int i=10; i<15; i++) {
       *(cpixel+i)=7*monitor->Width()+(topleft++);		
	}	
	topleft=5;
	for (int i=15; i<20; i++) {
       *(cpixel+i)=8*monitor->Width()+(topleft++);		
	}
	topleft=5;
	for (int i=20; i<25; i++) {
       *(cpixel+i)=9*monitor->Width()+(topleft++);		
	}		
	
	
	
	//Preallocate the vector of Blocks
	for (int n=0; n< 50; n++) {
	  v_arr[n].reserve(50);	
	}	
	
	
    
    //Retrieve the zones info and setup the vector mask and result and direction buffers
    czones_n=monitor->GetZonesNum();
    czones=monitor->GetZones();
    
    for (int i=0; i < monitor->GetZonesNum() ; i++) {
	  czones[i]->SetVectorMask();
	  if (czones[i]->GetCheckMethod() == 1) {
		  Info("Zone Check Method is AlarmedPixels");
	      Info("Zone %d with min alarm pixels %d and max alarm pixels %d", i, czones[i]->GetMinAlarmPixels(), czones[i]->GetMaxAlarmPixels());
	  }    
	  else if (czones[i]->GetCheckMethod() == 2) {
		  Info("Zone Check Method is FilteredPixels");
	      Info("Zone %d with min vector neighbors %d and max vector neighbors %d", i, czones[i]->GetMinFilteredPixels(), czones[i]->GetMaxFilteredPixels());
	  }
	  
	  //Create the results buffer for recording indexes of macroblocks with motion per zone
	  result[i]=(uint8_t*)zm_mallocaligned(4,numblocks/7); //slightly larger than needed
	  memset(result[i],0,numblocks/7);
	  //Create the directions buffer
	  direction[i]=(uint8_t*)zm_mallocaligned(4,numblocks);
	  memset(direction[i],0,numblocks);
	  if(result[i] == NULL)
		     Fatal("Memory allocation for result buffer failed: %s",strerror(errno));
      if(direction[i] == NULL)
		     Fatal("Memory allocation for direction buffer failed: %s",strerror(errno));
    }	
    } //if ctype
    
    //FIXME, temporary /var/www/zm_config.txt parsing for variables that ought to be in the database
    //zm_config.txt
    //display_vectors=1
    //score_shift_multiplier = 2
    //min_vector_distance = 1
    //jpeg_mode = mmal|libjpeg

    std::string line;
    std::string jmodestr;
    std::istringstream sin;
    std::string homedir = getpwuid(getuid())->pw_dir;
    std::string location=homedir+"/zm_config.txt"; //var/www/zm_config.txt
    std::ifstream fin(location.c_str());

    Info("Extra CONFIG Options:");
    while (std::getline(fin, line)) {
    sin.str(line.substr(line.find("=")+1));

    if (line.find("display_vectors") != std::string::npos) {
       sin >> display_vectors;
       Info("display vectors %d",display_vectors);
    } else if (line.find("score_shift_multiplier") != std::string::npos) {
       sin >> score_shift_multiplier;
       Info("score shift multipler %d", score_shift_multiplier);
    } else if (line.find("min_vector_distance") != std::string::npos) {
	   sin >> min_vector_distance;	
	   Info("min vector distance %d", min_vector_distance);
	} else if (line.find("jpeg_mode") != std::string::npos) {
       sin >> jmodestr;
       
       if (jmodestr == "mmal") {
          jmode=mmal;
          Info("Jpeg encoding mode is mmal");
       }   
       else if (jmodestr == "libjpeg" ) {
          jmode=libjpeg;
          Info("Jpeg encoding mode is libjpeg");
       }   

    }	
       sin.clear();
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
  
  if (coords)
     zm_freealigned(coords);
     
  if (Block)
     zm_freealigned(Block); 
     
  if (cpixel)
     zm_freealigned(cpixel);  
     
  if (P_ARRAY)
     zm_freealigned(P_ARRAY);      
     
  
  for (int i=0; i < monitor->GetZonesNum() ; i++) {
	    if (result[i])
           zm_freealigned(result[i]);
        if (direction[i])
           zm_freealigned(direction[i]);
        
  }      

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

int FfmpegCamera::QueueVideoPackets(timeval recording, char* event_file){
    //Info("Queue Video Packet");
        
    //Video recording
    int key_frame = packet.flags & AV_PKT_FLAG_KEY;
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
          int ret=0;
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
        
	
}	

int FfmpegCamera::WriteVideoPacket(){
	//Info("Write Video packet");
    if ( videoStore ) {
        //Write the packet to our video store
        int ret = videoStore->writeVideoFramePacket( &packet );
        if ( ret < 0 ) { //Less than zero and we skipped a frame
          zm_av_packet_unref( &packet );
          return 0;
        }
    }	
}	

int FfmpegCamera::WriteAudioPacket(){
	//Info("Write audio packet");
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
}	

int FfmpegCamera::Visualize_Buffer(uint8_t **dbuffer){
    //Buffer visualization should be here after the resized buffer is created, and before it is converted to jpeg
		        //Turn off for faster performance.  if not visualizing, turn result[i] memcpy off as well in mmal_encode. 
		        if (!display_vectors)
		           return 0;
		        
		        for (int i=0; i < monitor->GetZonesNum() ; i++) {
                     uint32_t offset=0;
                     
                     uint32_t *res=NULL;
                     uint8_t *dir_mask=NULL;
                     
                     pattern r_pattern=center; 
                     
                     int count=0;
                     int index=0;
                     //if (czones[i]->motion_detected) {
						 //int min_filtered=czones[i]->GetMinFilteredPixels();
						 res=(uint32_t*)(result[i]+offset);
						 for (int j=0; j<numblocks; j++) {
						   if (*res) {
							 dir_mask=(uint8_t*)(direction[i]+j);  
							 	 
							 if (*res & (0x80000000 >> count)) {
                                   
                                   //Mark the macroblocks that have motion detected. 
                                   //This is only for debugging and not needed for normal use 
                                   
                                   
                                   //Info("Index at %d, coordinates at %d,%d and RGB index at %d", index, (Block+index)->coords->X(), (Block+index)->coords->Y(), (Block+index)->rgbindex);     
                                   /*if ((Block+index)->rgbindex >=0) {  
									 for (int m=0; m < colour ; m++) {   
                                       *(uint8_t*)(directbuffer+(((Block+index)->rgbindex)+m))=255;
								     }
							       }*/ 
							       
							       //[n][ne][e][se][s][sw][w][nw]
							       //[7][6] [5][4] [3][2] [1][0] 
							       
							       for ( int i=0; i<8 ; i++) {
									  if (*dir_mask & 1<<i) {
										r_pattern=(pattern)i;
										break;  
									  }	     
								   }	   
							       
							       
							       
							       
							       if (colours == 3 )  
                                        RGB=(RGB24*)(*dbuffer); 
                                        
                                   //if (((Block+index)->has_neighbors >= min_filtered) || ((Block+index)->is_neighbors >= min_filtered))
							           //pixel_write(RGB,(Block+index)->rgbindex, r_pattern,RGB24(255,0,0)); //RED for FILTERED
							       //else
							       if ((Block+index)->status==1)
							           pixel_write(RGB,(Block+index)->rgbindex, r_pattern,RGB24(0,255,0)); //GREEN for ALARM
							       else if ((Block+index)->status==2)    
							           pixel_write(RGB,(Block+index)->rgbindex, r_pattern,RGB24(0,0,255)); //BLUE for FILTERED 
							       
							           
							 }
							 
							 count++;
							 index++;
							 if (count==32) {
					            offset+=4;
                                count=0;
								res=(uint32_t*)(result[i]+offset); //Last iteration will read past actual data but the buffer is large enough, just filled with zeroes
								
							 }	
							 
						   } //if (*res)	  
					     }		 	 
                } 
                	
}	

//Function to handle capture and store
int FfmpegCamera::CaptureAndRecord( Image &image, timeval recording, char* event_file ) {

  uint8_t* directbuffer=NULL;
  uint8_t* mvect_buffer=NULL;  
  
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

   

    Debug( 4, "Got packet from stream %d packet pts (%d) dts(%d)", 
        packet.stream_index, packet.pts, packet.dts
        );


    if ( packet.stream_index == mVideoStreamId ) {
     
      Debug(4, "about to decode video" );
      
    if (!ctype) {   
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
      
      //Start queueing video packets if successfully decoded video
      QueueVideoPackets(recording, event_file);
      
   }  else { //if ctype
    //the mmal decoder builds mRawFrame here with an I420 buffer	
      frameComplete=mmal_decode(&packet);
      Debug( 4, "Decoded video packet at frame %d", frameCount );
      
      //Start queueing video packets if successfully decoded video
      QueueVideoPackets(recording, event_file);
   }  //if cytpe   

  } //if packet index is video

  if ( packet.stream_index == mVideoStreamId ) {
	  //Mvect buffer to be requested for both ctype and !ctype
        if (cfunction == Monitor::MVDECT) {
           mvect_buffer=image.VectBuffer();   
           if (mvect_buffer ==  NULL ){
                Error("Failed requesting vector buffer for the captured image.");
                return (-1); 
           } 
        } 
	  
	  
  }	
  
  if ( packet.stream_index == mVideoStreamId ) {      
//FRAMECOMPLETE 1 -> got a decoded packet and in case of mmal, 
                   //mmal_decode has filled MRawFrame with I420 buffer at this point

      if ( frameComplete ) {
        //Debug( 4, "Got frame %d", frameCount );

        /* Request a writeable buffer of the target image */
        directbuffer = image.WriteBuffer(width, height, colours, subpixelorder);
                   
        if ( directbuffer == NULL ) {
          Error("Failed requesting writeable buffer for the captured image.");
          zm_av_packet_unref( &packet );
          return (-1);
        }
        
        
        
        
        //mmal will use mFrame to store the rgb buffer
        if  (ctype) {
           
#if LIBAVUTIL_VERSION_CHECK(54, 6, 0, 6, 0)
            av_image_fill_arrays(mFrame->data, mFrame->linesize,
                     directbuffer, imagePixFormat, width, height, 1);
#else
            avpicture_fill( (AVPicture *)mFrame, directbuffer,
                     imagePixFormat, width, height);
#endif
        
            frameComplete=mmal_resize(&directbuffer); 
           
        } else  if (((ctype) && (cfunction == Monitor::MODECT)) || (!ctype)) {
	      //This is the software scaler. Directbuffer is packaged into mFrame and processed by swscale to convert to appropriate format and size
          //Need SWScale when Source Type is FFmpeg with Function as Mvdect or Modect
          //or Source type is FFmpeghw and Function is Modect  
          //This is only used if source is FFmpeg or source is FFmpeghw and Function is Modect. 
			
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
		frameCount++; //MODECT mode ends here
	    }	
        
         
      }  //if framecomplete 1 



//FRAMECOMPLETE 2 -> mmal_resize has taken the buffer from mRawFrame, resized it and put it in mFrame        
        
      if (frameComplete) {
         if ((ctype) && (cfunction == Monitor::MVDECT)) {
		         
		        frameComplete=mmal_encode(&mvect_buffer);
		        
		}  //if cfunction
	  } //if frameComplete 2
	  
	  
	  
//FRAMECOMPLETE 3 -> mmal_encode successfully received an output buffer. 	  
	
		
	  if ( frameComplete ) {
		    if  ((ctype) && (cfunction == Monitor::MVDECT)) {
				
				Visualize_Buffer(&directbuffer);
				
				
				//Create the JPEG buffer for this frame if frame is alarmed, using downscaled resolution
                uint8_t* jpegbuffer=NULL;
                jpegbuffer=image.JPEGBuffer(width, height);
                if (jpegbuffer ==  NULL ){
                   Error("Failed requesting jpeg buffer for the captured image.");
                   return (-1); 
                }
			

			    if (jmode == libjpeg) {
		            int *jpeg_size=(int *)jpegbuffer; 
				    image.EncodeJpeg(jpegbuffer+4, jpeg_size );
			    } else 
			    //Option 2. use hardware mmal.
			    if (jmode == mmal)
     		        mmal_jpeg(&jpegbuffer);

          } //if cfunction
        
          
        frameCount++;
      }  // end if frameComplete
      
      
      WriteVideoPacket();
      
    } else if ( packet.stream_index == mAudioStreamId ) { //FIXME best way to copy all other streams
		
	  WriteAudioPacket();	
      
    } else {
#if LIBAVUTIL_VERSION_CHECK(56, 23, 0, 23, 0)
      Debug( 3, "Some other stream index %d, %s", packet.stream_index, av_get_media_type_string( mFormatContext->streams[packet.stream_index]->codecpar->codec_type) );
#else
      Debug( 3, "Some other stream index %d", packet.stream_index );
#endif
    }
      
      // the packet contents are ref counted... when queuing, we allocate another packet and reference it with that one, so we should always need to unref here, which should not affect the queued version.
      zm_av_packet_unref( &packet );
  } // end while ! frameComplete
  
  
  
  //Info("Framecount is %d", frameCount);
  return (frameCount);
} // end FfmpegCamera::CaptureAndRecord

#endif // HAVE_LIBAVFORMAT




