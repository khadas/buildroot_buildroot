/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-amladec
 * @see_also: lame
 *
 * AML audio decoder.
 *
 * <refsect2>
 * <title>Example pipelines</title>
 * |[
 * gst-launch filesrc location=001.mp3 !mp3parse !amladec !amlasink
 * ]| Decode the mp3 file and play
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include "gstamladec.h"
#include  "gstamlaudioheader.h"
#include  "gstamlsysctl.h"
#include  "../../common/include/codec.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h> 
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <ctype.h>

enum
{
    PROP_0,
    PROP_SILENT,
    PROP_SYNC,
    PROP_ASYNC,
    PROP_MUTE
};

GST_DEBUG_CATEGORY_STATIC (gst_amladec_debug);
#define GST_CAT_DEFAULT gst_amladec_debug

static GstStaticPadTemplate amladec_src_template_factory =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw-int")
    );
static GstStaticPadTemplate amladec_sink_template_factory =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/mpeg; audio/x-ac3; audio/x-adpcm; audio/x-flac;audio/x-wma;audio/x-vorbis;audio/x-mulaw;audio/x-raw-int")
    );
static void gst_amladec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_amladec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean gst_amladec_src_event (GstPad * pad, GstEvent * event);
static gboolean gst_amladec_set_caps (GstPad   *pad, GstCaps * caps);
static gboolean gst_amladec_start (GstAmlAdec *amladec);
static gboolean gst_amladec_stop (GstAmlAdec *amladec);
static gboolean gst_amladec_sink_event (GstPad * pad, GstEvent * event);
static GstFlowReturn gst_amladec_chain (GstPad * pad, GstBuffer * buffer);
static GstFlowReturn gst_amladec_render (GstAmlAdec *amladec, GstBuffer *buffer);
static GstStateChangeReturn gst_amladec_change_state (GstElement * element,
    GstStateChange transition);

GST_BOILERPLATE (GstAmlAdec, gst_amladec, GstElement, GST_TYPE_ELEMENT);

static codec_para_t a_codec_para;
static codec_para_t *apcodec;

//#define AML_DEBUG g_print
#define  AML_DEBUG(...)   GST_INFO_OBJECT(amladec,__VA_ARGS__) 

static void gst_amladec_base_init (gpointer g_class)
{
    GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
    
    gst_element_class_add_static_pad_template (element_class,
          &amladec_sink_template_factory);
    gst_element_class_add_static_pad_template (element_class,
          &amladec_src_template_factory);
    gst_element_class_set_details_simple (element_class, "amladec decoder",
          "Codec/Decoder/Audio",
          "send audio es to aml audiodsp", "AML <aml@amlogic.com>");
}

static void gst_amladec_class_init (GstAmlAdecClass * klass)
{
    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;
  
    gobject_class = (GObjectClass *) klass;
    gstelement_class = (GstElementClass *) klass;
  
    parent_class = g_type_class_peek_parent (klass);
  
    gobject_class->set_property = gst_amladec_set_property;
    gobject_class->get_property = gst_amladec_get_property;
    gstelement_class->change_state = gst_amladec_change_state;
  
    /* init properties */
    g_object_class_install_property (gobject_class, PROP_SILENT, g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
            FALSE, G_PARAM_READWRITE)); 
    g_object_class_install_property (gobject_class, PROP_MUTE, g_param_spec_boolean ("mute", "Mute", "mute audio or not ?",
            FALSE, G_PARAM_READWRITE));  

}

static void gst_amladec_init (GstAmlAdec * amladec, GstAmlAdecClass * klass)
{
    GstPadTemplate *template;
   
     /* create the sink and src pads */
     template = gst_static_pad_template_get (&amladec_sink_template_factory);
     amladec->sinkpad = gst_pad_new_from_template (template, "sink");
     gst_object_unref (template);
     gst_element_add_pad (GST_ELEMENT (amladec), amladec->sinkpad);
     gst_pad_set_chain_function (amladec->sinkpad, GST_DEBUG_FUNCPTR (gst_amladec_chain));
     gst_pad_set_event_function (amladec->sinkpad,GST_DEBUG_FUNCPTR (gst_amladec_sink_event));
     gst_pad_set_setcaps_function (amladec->sinkpad,GST_DEBUG_FUNCPTR(gst_amladec_set_caps));
   
     template = gst_static_pad_template_get (&amladec_src_template_factory);
     amladec->srcpad = gst_pad_new_from_template (template, "src");
     gst_object_unref (template);
     gst_element_add_pad (GST_ELEMENT (amladec), amladec->srcpad);
     gst_pad_set_event_function (amladec->srcpad,GST_DEBUG_FUNCPTR (gst_amladec_src_event));     
     gst_pad_use_fixed_caps (amladec->srcpad);

}

static gboolean gst_amladec_src_event (GstPad * pad, GstEvent * event)
{
    gboolean res = TRUE;
    GstAmlAdec *amladec;  
    amladec = GST_AMLADEC (GST_PAD_PARENT (pad));
  
    switch (GST_EVENT_TYPE (event)) {
        case GST_EVENT_SEEK:
          /* the all-formats seek logic, ref the event, we need it later */
            gst_event_ref (event);
            res = gst_pad_push_event (amladec->sinkpad, event); 
            gst_event_unref (event);
            break;
        default:
            res = gst_pad_push_event (amladec->sinkpad, event);
            break;
    }
  
    return res;
}


static void gst_amladec_set_property (GObject * object, guint prop_id,
	const GValue * value, GParamSpec * pspec)
{
    GstAmlAdec *amladec;
  
    amladec = GST_AMLADEC (object);
  
    switch (prop_id) {
        case PROP_SILENT:
            amladec->silent = g_value_get_boolean (value);
            break;
        case PROP_MUTE:
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void gst_amladec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
    GstAmlAdec *amladec;
  
    amladec = GST_AMLADEC (object);
        switch (prop_id) {
            case PROP_SILENT:
                g_value_set_boolean (value,amladec->silent);
                break;
            case PROP_MUTE:

                break;
            default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
      }
}
  
static void wait_for_render_end (void)
{
    unsigned rp_move_count = 40,count=0;
    unsigned last_rp = 0;
    struct buf_status abuf;
    int ret=1;	
    do {
        if(count>2000)//avoid infinite loop
	          break;	
        ret = codec_get_abuf_state (apcodec, &abuf);
        if (ret != 0) {
            g_print("codec_get_abuf_state error: %x\n", -ret);
            break;
        }
        if(last_rp != abuf.read_pointer){
            last_rp = abuf.read_pointer;
            rp_move_count = 40;
        }else
           rp_move_count--;        
        usleep(1000*30);
        count++;	
    } while (abuf.data_len > 0x100 && rp_move_count > 0);

}

static gboolean
gst_amladec_sink_event (GstPad * pad, GstEvent * event)
{
    GstAmlAdec *amladec = GST_AMLADEC (GST_PAD_PARENT (pad));
    gboolean result;  
    GST_DEBUG ("handling %s event", GST_EVENT_TYPE_NAME (event));
  
    switch (GST_EVENT_TYPE (event)) {
        case GST_EVENT_NEWSEGMENT:{
            GstFormat format;
            gboolean update;
            gdouble rate, applied_rate;
            gint64 start, stop, pos;
      
            gst_event_parse_new_segment_full (event, &update, &rate, &applied_rate,
                &format, &start, &stop, &pos);
      
            if (format == GST_FORMAT_TIME) {      
                result = gst_pad_push_event (amladec->srcpad, event);  
                gst_segment_set_newsegment_full (&amladec->segment, update, rate,
                    applied_rate, GST_FORMAT_TIME, start, stop, pos);
            } else {
                GST_DEBUG ("dropping newsegment event in format %s",
                gst_format_get_name (format));
                gst_event_unref (event);        
                result = TRUE;
            }
            break;
        }
        case GST_EVENT_EOS:
            g_print ("GST_EVENT_EOS\n"); 
             if (amladec->codec_init_ok)
            {	
                wait_for_render_end();
                amladec->is_eos=TRUE;
            }
            result = gst_pad_push_event(amladec->srcpad, event);
            break;
        case GST_EVENT_FLUSH_STOP:{
            int ret = -1;
            if(amladec->codec_init_ok){
                if (amladec->is_paused == TRUE) {
                    ret=codec_resume (apcodec);
                    if (ret != 0) {
                        g_print("[%s:%d]resume failed!ret=%d\n", __FUNCTION__, __LINE__, ret);
                    }else
                    amladec->is_paused = FALSE;
                }	
                ret = codec_reset (apcodec);
                if (ret < 0) {
                    g_print("reset acodec failed, res= %x\n", ret);
                    return FALSE;
                }       
            }
            result = gst_pad_push_event (amladec->srcpad,event);
            break;
        }
        case GST_EVENT_FLUSH_START:
            result = gst_pad_push_event (amladec->srcpad, event);
            break;
        default:
            result = gst_pad_push_event (amladec->srcpad, event);
            break;
    }
    return result;
}

static gboolean gst_set_astream_info (GstAmlAdec *amladec, GstCaps * caps)
{
    GstStructure *structure;
    const char *name;
    int ret = CODEC_ERROR_NONE;
    int mpegversion; 
    GValue *extra_data_buf = NULL; 
	
    structure = gst_caps_get_structure (caps, 0);
    name = gst_structure_get_name (structure);  

    if (strcmp(name, "audio/mpeg") == 0) {
        gst_structure_get_int (structure, "mpegversion", &mpegversion); 
        AML_DEBUG("mpegversion=%d\n",mpegversion);      	   
        if (mpegversion==1/*&&layer==3*/) { //mp3
            apcodec->audio_type = AFORMAT_MPEG;
            AML_DEBUG("mp3 info set ok\n");
        }else if (mpegversion==4||mpegversion==2) {
            apcodec->audio_type = AFORMAT_AAC;
		        if (gst_structure_has_field (structure, "codec_data")) {	
                extra_data_buf = (GValue *) gst_structure_get_value (structure, "codec_data");
                if (NULL != extra_data_buf) {
                    guint8 *hdrextdata;
                    gint i;
                    amladec->codec_data = gst_value_get_buffer (extra_data_buf);	 
       		 AML_DEBUG("AAC SET CAPS check for codec data \n");    
                    amladec->codec_data_len = GST_BUFFER_SIZE (amladec->codec_data);
                    AML_DEBUG("\n>>aac decoder: AAC Codec specific data length is %d\n",amladec->codec_data_len);
                    AML_DEBUG("aac codec data is \n");
                    hdrextdata = GST_BUFFER_DATA (amladec->codec_data);
                    for(i=0;i<amladec->codec_data_len;i++)
                        AML_DEBUG("%x ",hdrextdata[i]);
                    AML_DEBUG("\n");
                    extract_adts_header_info (amladec);
                    hdrextdata = GST_BUFFER_DATA(amladec->codec_data);
                    for(i=0;i<GST_BUFFER_SIZE(amladec->codec_data);i++)
                        AML_DEBUG("%x ",hdrextdata[i]);
                    AML_DEBUG("\n");					
                }
	          }		   
        }		
    }else if (strcmp(name, "audio/x-ac3") == 0) {   	   
        apcodec->audio_type = AFORMAT_AC3;       	 	
    }else if (strcmp(name, "audio/x-adpcm") == 0) {   	   
        apcodec->audio_type = AFORMAT_ADPCM;       	 	
    }else if (strcmp(name, "audio/x-flac") == 0) {   	   
        apcodec->audio_type = AFORMAT_FLAC;       	 	
    }else if (strcmp(name, "audio/x-wma") == 0) {   	   
        apcodec->audio_type = AFORMAT_WMA;       	 	
    }else if (strcmp(name, "audio/x-vorbis") == 0) {   	   
        apcodec->audio_type = AFORMAT_VORBIS;       	 	
    }else if (strcmp(name, "audio/x-mulaw") == 0) {   	   
        apcodec->audio_type = AFORMAT_MULAW;       	 	
    }else if (strcmp(name, "audio/x-raw-int") == 0) {
        gint endianness,depth;
        gboolean getsigned;
        gst_structure_get_int (structure, "endianness", &endianness);
        gst_structure_get_int (structure, "depth", &depth);
        gst_structure_get_int (structure, "rate", &amladec->sample_rate);
        gst_structure_get_int (structure, "channels", &amladec->channels);
        gst_structure_get_boolean (structure, "signed", &getsigned);
        g_print("depth=%d,endianness=%d\n",depth,endianness);
	      if (endianness==1234&&depth==16&&getsigned==true){	
            apcodec->audio_type = AFORMAT_PCM_S16LE;
            amladec->codec_id = CODEC_ID_PCM_S16LE;
        }			
    }else {
        g_print("unsupport audio format name=%s\n",name);
        return FALSE;
   }
	
    if (apcodec&&apcodec->stream_type == STREAM_TYPE_ES_AUDIO){
        if (IS_AUIDO_NEED_EXT_INFO (apcodec->audio_type))
            audioinfo_need_set (apcodec,amladec); 
        ret = codec_init (apcodec);
        if (ret != CODEC_ERROR_NONE){
            g_print("codec init failed, ret=-0x%x", -ret);
            return -1;
        }
        amladec->codec_init_ok = 1;
        set_tsync_enable(1);		
    } 
	return TRUE;
}
/* this function handles the link with other elements */
static gboolean gst_amladec_set_caps (GstPad * pad, GstCaps * caps)
{
    GstAmlAdec *amladec;  
    GstPad *otherpad;

    amladec = GST_AMLADEC (gst_pad_get_parent (pad));
    otherpad = (pad == amladec->srcpad) ? amladec->sinkpad : amladec->srcpad;
	
    if(caps)	
        gst_set_astream_info (amladec, caps ); 
	
    gst_object_unref (amladec);	
    return gst_pad_set_caps (otherpad, caps);
}

static GstFlowReturn gst_amladec_render (GstAmlAdec *amladec, GstBuffer * buf)
{ 
    guint8 *data;
    guint size;
    gint written;
    GstClockTime timestamp,pts;

    struct buf_status abuf;
    int ret=1;
    GstCaps * caps = NULL;

    if (!amladec->codec_init_ok){
        caps = GST_BUFFER_CAPS (buf);
        if (caps)		
            gst_set_astream_info (amladec, caps );
    }
	
    if (apcodec&&amladec->codec_init_ok)
    {
        ret = codec_get_abuf_state (apcodec, &abuf);
        if (ret == 0){
            if (abuf.data_len*10 > abuf.size*8){  
                usleep(1000*40);
                //return GST_FLOW_OK;
            }
        }
        timestamp = GST_BUFFER_TIMESTAMP (buf);    
        pts=timestamp*9LL/100000LL+1L;
        if (!amladec->is_headerfeed&&amladec->codec_data_len){
        audiopre_header_feeding (apcodec,amladec,&buf);
        }		
		
        data = GST_BUFFER_DATA (buf);
        size = GST_BUFFER_SIZE (buf);	
        if (timestamp!= GST_CLOCK_TIME_NONE){
            GST_DEBUG_OBJECT (amladec,"pts=%x\n",(unsigned long)pts);
            GST_DEBUG_OBJECT (amladec, "PTS to (%" G_GUINT64_FORMAT ") time: %"
            GST_TIME_FORMAT , pts, GST_TIME_ARGS (timestamp)); 
			
            if (codec_checkin_pts(apcodec,(unsigned long)pts)!=0)
                g_print("pts checkin flied maybe lose sync\n");        	
        }
    	
        again:
    
        GST_DEBUG_OBJECT (amladec, "writing %d bytes to stream buffer r\n", size);
        written=codec_write (apcodec, data, size);
    
        /* check for errors */
        if (G_UNLIKELY (written < 0)) {
          /* try to write again on non-fatal errors */
            if (errno == EAGAIN || errno == EINTR)
                goto again;
            /* else go to our error handler */
            goto write_error;
        }
        /* all is fine when we get here */
        size -= written;
        data += written;
        GST_DEBUG_OBJECT (amladec, "wrote %d bytes, %d left", written, size);
        /* short write, select and try to write the remainder */
        if (G_UNLIKELY (size > 0))
            goto again;   
      
        return GST_FLOW_OK;
    
        write_error:
        {
            switch (errno) {
                case ENOSPC:
                    GST_ELEMENT_ERROR (amladec, RESOURCE, NO_SPACE_LEFT, (NULL), (NULL));
                    break;
                default:{
                   GST_ELEMENT_ERROR (amladec, RESOURCE, WRITE, (NULL),("Error while writing to file  %s",g_strerror (errno)));
                }
            }
            return GST_FLOW_ERROR;
        }
    } else {
        g_print("we will do nothing in render as audio decoder not ready yet\n");

    }	
    return GST_FLOW_OK;
}
/* chain function
 * this function does the actual processing
 */
static GstFlowReturn gst_amladec_chain (GstPad * pad, GstBuffer * buf)
{
    GstAmlAdec *amladec;
    GstStructure *structure;
    amladec = GST_AMLADEC (GST_OBJECT_PARENT (pad));

    if (amladec->silent == FALSE){  	
        gst_amladec_render (amladec, buf);	
    }	

    /* dummy asink, so we return here to avoid cap negociation with sink at present*/
    return GST_FLOW_OK;//gst_pad_push (amladec->srcpad, buf);
}

static gboolean gst_amladec_start (GstAmlAdec *amladec)
{ 
    apcodec = &a_codec_para;
    memset(apcodec, 0, sizeof(codec_para_t ));
    apcodec->audio_pid = 0;
    apcodec->has_audio = 1;
    apcodec->has_video = 0;
    apcodec->audio_channels =0;
    apcodec->audio_samplerate = 0;
    apcodec->noblock = 0;
    apcodec->audio_info.channels = 0;
    apcodec->audio_info.sample_rate = 0;
    apcodec->audio_info.valid = 0;
    apcodec->stream_type = STREAM_TYPE_ES_AUDIO;
    amladec->codec_init_ok = 0;
    amladec->sample_rate = 0;
    amladec->channels = 0;
    amladec->codec_id = 0;
    amladec->bitrate = 0;
    amladec->block_align = 0;
    amladec->is_headerfeed = FALSE;
    amladec->is_paused = FALSE;
    amladec->is_eos = FALSE;
    amladec->codec_init_ok = 0;
    return TRUE;
}

static gboolean gst_amladec_stop (GstAmlAdec *amladec)
{ 
    gint ret = -1;
    if (amladec->codec_init_ok){
        if (amladec->is_paused == TRUE) {
            ret=codec_resume (apcodec);
            if (ret != 0) {
                g_print("[%s:%d]resume failed!ret=%d\n", __FUNCTION__, __LINE__, ret);
            }else
            amladec->is_paused = FALSE;
        }	
        codec_close (apcodec);
        amladec->codec_init_ok=0;
    }		
    return TRUE;
}

static GstStateChangeReturn gst_amladec_change_state (GstElement * element, GstStateChange transition)
{
    GstAmlAdec *amladec;
    GstStateChangeReturn result;
    gint ret=-1;
    amladec = GST_AMLADEC (element);
  
    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            gst_amladec_start (amladec); 
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            break;
        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:	  	
            if (amladec->is_paused == TRUE && amladec->codec_init_ok) {
                ret=codec_resume (apcodec);
                if (ret != 0) {
                      g_print("[%s:%d]resume failed!ret=%d\n", __FUNCTION__, __LINE__, ret);
                }else
                amladec->is_paused = FALSE;
              }
            break;
        default:
            break;
      }
  
    result = parent_class->change_state (element, transition);
  
    switch (transition) {
        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            if (!amladec->is_eos && amladec->codec_init_ok)
            {
                ret = codec_pause (apcodec);
                if (ret != 0) {
                    g_print("[%s:%d]pause failed!ret=%d\n", __FUNCTION__, __LINE__, ret);
                }else
                    amladec->is_paused = TRUE;
            }	
            break;
        case GST_STATE_CHANGE_PAUSED_TO_READY:			
            break;
        case GST_STATE_CHANGE_READY_TO_NULL: 
            gst_amladec_stop (amladec);
            break;
        default:
            break;
    }
    return result;
}

static gboolean plugin_init (GstPlugin * plugin)
{
    GST_DEBUG_CATEGORY_INIT (gst_amladec_debug, "amladec", 0, "amladec decoding");  
    return gst_element_register (plugin, "amladec", GST_RANK_PRIMARY,gst_amladec_get_type ());
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    "amladec",
    "amladec decoding",
    plugin_init,
    VERSION,
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/");