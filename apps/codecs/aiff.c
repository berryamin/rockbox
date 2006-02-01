/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (c) 2005 Jvo Studer
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "codeclib.h"
#include "inttypes.h"

CODEC_HEADER

struct codec_api* rb;

/* This codec supports AIFF files with the following formats:
 * - PCM, 8 and 16 bits, mono or stereo
 */

enum
{
    AIFF_FORMAT_PCM = 0x0001,   /* AIFF PCM Format (big endian) */
    IEEE_FORMAT_FLOAT = 0x0003, /* IEEE Float */
    AIFF_FORMAT_ALAW = 0x0004,  /* AIFC ALaw compressed */
    AIFF_FORMAT_ULAW = 0x0005   /* AIFC uLaw compressed */
};

/* Maximum number of bytes to process in one iteration */
/* for 44.1kHz stereo 16bits, this represents 0.023s ~= 1/50s */
#define AIF_CHUNK_SIZE (1024*2)

#ifdef USE_IRAM
extern char iramcopy[];
extern char iramstart[];
extern char iramend[];
extern char iedata[];
extern char iend[];
#endif

static int16_t int16_samples[AIF_CHUNK_SIZE] IBSS_ATTR;


/* this is the codec entry point */
enum codec_status codec_start(struct codec_api* api)
{
  struct codec_api* ci;
  uint32_t numbytes, bytesdone;
  uint16_t num_channels = 0;
  uint32_t num_sample_frames = 0;
  uint16_t sample_size = 0;
  uint32_t sample_rate = 0;
  uint32_t i;
  size_t n, aifbufsize;
  int endofstream;
  unsigned char* buf;
  uint16_t* aifbuf;
  long chunksize;
  uint32_t offset2snd = 0;
  uint16_t block_size  = 0;
  uint32_t avgbytespersec = 0;
  off_t firstblockposn;     /* position of the first block in file */
  int shortorlong = 1;      /* do we output shorts (1) or longs (2)? */
  int32_t * const int32_samples = (int32_t*)int16_samples;

  /* Generic codec initialisation */
  rb = api;
  ci = api;

#ifdef USE_IRAM 
  ci->memcpy(iramstart, iramcopy, iramend-iramstart);
  ci->memset(iedata, 0, iend - iedata);
#endif

  ci->configure(CODEC_SET_FILEBUF_WATERMARK, (int *)(1024*512));
  ci->configure(CODEC_SET_FILEBUF_CHUNKSIZE, (int *)(1024*256));

  ci->configure(DSP_DITHER, (bool *)false);
  
  next_track:

  if (codec_init(api)) {
      i = CODEC_ERROR;
      goto exit;
  }

  while (!*ci->taginfo_ready)
      ci->yield();
    
  /* assume the AIFF header is less than 1024 bytes */
  buf=ci->request_buffer((long *)&n,1024);
  if (n<44) {
      i = CODEC_ERROR;
      goto exit;
  }
  if ((memcmp(buf,"FORM",4)!=0) || (memcmp(&buf[8],"AIFF",4)!=0)) {
      i = CODEC_ERROR;
      goto exit;
  }

  buf += 12;
  n -= 12;
  numbytes = 0;

  /* read until 'SSND' chunk, which typically is last */
  while(numbytes == 0 && n >= 8) {
      /* chunkSize */
      i = ((buf[4]<<24)|(buf[5]<<16)|(buf[6]<<8)|buf[7]);
      if (memcmp(buf,"COMM",4)==0) {
          if (i != 18) {
              DEBUGF("CODEC_ERROR: 'COMM' chunk size=%lu != 18\n",i);
              i = CODEC_ERROR;
              goto exit;
          }
          /* num_channels */
          num_channels = ((buf[8]<<8)|buf[9]);
          /* num_sample_frames */
          num_sample_frames = ((buf[10]<<24)|(buf[11]<<16)|(buf[12]<<8)|buf[13]);
          /* sample_size */
          sample_size  = ((buf[14]<<8)|buf[15]);
          /* sample_rate (don't use last 4 bytes, only integer fs) */
          if (buf[16] != 0x40) {
              DEBUGF("CODEC_ERROR: wierd sampling rate (no @)\n",i);
              i = CODEC_ERROR;
              goto exit;
          }
          sample_rate = ((buf[18]<<24)|(buf[19]<<16)|(buf[20]<<8)|buf[21])+1;
          sample_rate = sample_rate >> (16+14-buf[17]);
          /* calc average bytes per second */
          avgbytespersec = sample_rate*num_channels*sample_size/8;
      }
      else if (memcmp(buf,"SSND",4)==0) {
          if (sample_size == 0) {
              DEBUGF("CODEC_ERROR: unsupported chunk order\n");
              i = CODEC_ERROR;
              goto exit;
          }
          /* offset2snd */
          offset2snd = ((buf[8]<<8)|buf[9]);
          /* block_size */
          block_size = ((buf[10]<<8)|buf[11]);
          if (block_size == 0)
              block_size = num_channels*sample_size;
          numbytes = i-8-offset2snd;
          i = 8+offset2snd; /* advance to the beginning of data */
      }
      else {
          DEBUGF("unsupported AIFF chunk: '%c%c%c%c', size=%lu\n",
                 buf[0], buf[1], buf[2], buf[3], i);
      }

      if (i & 0x01) /* odd chunk sizes must be padded */
          i++;
      buf += i+8;
      if (n < (i+8)) {
          DEBUGF("CODEC_ERROR: AIFF header size > 1024\n");
          i = CODEC_ERROR;
          goto exit;
      }
      n -= i+8;
  } /* while 'SSND' */

  if (num_channels == 0) {
      DEBUGF("CODEC_ERROR: 'COMM' chunk not found or 0-channels file\n");
      i = CODEC_ERROR;
      goto exit;
  }
  if (numbytes == 0) {
      DEBUGF("CODEC_ERROR: 'SSND' chunk not found or has zero length\n");
      i = CODEC_ERROR;
      goto exit;
  }
  if (sample_size > 24) {
      DEBUGF("CODEC_ERROR: PCM with more than 24 bits per sample "
             "is unsupported\n");
      i = CODEC_ERROR;
      goto exit;
  }

  ci->configure(CODEC_DSP_ENABLE, (bool *)true);
  ci->configure(DSP_SET_FREQUENCY, (long *)(ci->id3->frequency));

  if (sample_size <= 16) {
      ci->configure(DSP_SET_SAMPLE_DEPTH, (int *)(16));
  } else {
      shortorlong = 2;
      ci->configure(DSP_DITHER, (bool *)false);
      ci->configure(DSP_SET_SAMPLE_DEPTH, (long *) (32));
      ci->configure(DSP_SET_CLIP_MAX, (long *) (2147483647));
      ci->configure(DSP_SET_CLIP_MIN, (long *) (-2147483647-1));
  }

  if (num_channels == 2) {
      ci->configure(DSP_SET_STEREO_MODE, (int *)STEREO_INTERLEAVED);
  } else if (num_channels == 1) {
      ci->configure(DSP_SET_STEREO_MODE, (int *)STEREO_MONO);
  } else {
      DEBUGF("CODEC_ERROR: more than 2 channels unsupported\n");
      i = CODEC_ERROR;
      goto exit;
  }

  firstblockposn = (1024-n);
  ci->advance_buffer(firstblockposn);

  /* The main decoder loop */

  bytesdone=0;
  ci->set_elapsed(0);
  endofstream=0;
  /* chunksize is computed so that one chunk is about 1/50s.
   * this make 4096 for 44.1kHz 16bits stereo.
   * It also has to be a multiple of blockalign */
  chunksize = (1 + avgbytespersec / (50*block_size)) * block_size;
  /* check that the output buffer is big enough (convert to samplespersec,
     then round to the block_size multiple below) */
  if (((uint64_t)chunksize*ci->id3->frequency*num_channels*shortorlong)
      / (uint64_t)avgbytespersec >= AIF_CHUNK_SIZE) {
      chunksize = ((uint64_t)AIF_CHUNK_SIZE * avgbytespersec
                   / ((uint64_t)ci->id3->frequency * num_channels * shortorlong 
                      * block_size)) * block_size;
  }

  while (!endofstream) {
      uint8_t *aifbuf8;

      ci->yield();
    if (ci->stop_codec || ci->reload_codec) {
        break;
    }

    if (ci->seek_time) {
        uint32_t newpos;

        /* use avgbytespersec to round to the closest blockalign multiple,
           add firstblockposn. 64-bit casts to avoid overflows. */
        newpos = (((uint64_t)avgbytespersec * (ci->seek_time - 1))
                  / (1000LL*block_size)) * block_size;
        if (newpos > numbytes)
            break;
        if (ci->seek_buffer(firstblockposn + newpos)) {
            bytesdone = newpos;
        }
        ci->seek_complete();
    }
    aifbuf=ci->request_buffer((long *)&n,chunksize);
    aifbuf8 = (uint8_t*)aifbuf;

    if (n==0)
        break; /* End of stream */

    if (bytesdone + n > numbytes) {
        n = numbytes - bytesdone;
        endofstream = 1;
    }

    aifbufsize = sizeof(int16_samples);

    if (sample_size > 24) {
        for (i=0;i<n;i+=4) {
            int32_samples[i/4]=(int32_t)((aifbuf8[i]<<24)|
                          (aifbuf8[i+1]<<16)|(aifbuf8[i+2]<<8)|aifbuf8[i+3]);
        }
        aifbufsize = n;
    } else if (sample_size > 16) {
        for (i=0;i<n;i+=3) {
            int32_samples[i/3]=(int32_t)((aifbuf8[i]<<24)|
                       (aifbuf8[i+1]<<16)|(aifbuf8[i+2]<<8));
        }
        aifbufsize = n*4/3;
    } else if (sample_size > 8) {
        /* copy data. */
        for (i=0;i<n;i+=2) {
            int16_samples[i/2]=(int16_t)((aifbuf8[i]<<8)|aifbuf8[i+1]);
        }
        aifbufsize = n;
    } else {
        for (i=0;i<n;i++) {
            int16_samples[i] = (aifbuf8[i]<<8) - 0x8000;
        }
        aifbufsize = n*2;
    }

    while (!ci->pcmbuf_insert((char*)int16_samples, aifbufsize)) {
        ci->yield();
    }

    ci->advance_buffer(n);
    bytesdone += n;
    if (bytesdone >= numbytes) {
        endofstream=1;
    }

    ci->set_elapsed(bytesdone*1000LL/avgbytespersec);
  }

  if (ci->request_next_track())
      goto next_track;

  i = CODEC_OK;
exit:
  return i;
}
