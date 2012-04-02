#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <unistd.h>

#include "codec2.h"
#define SPAN_DECLARE(x)	x
#include "echo.h"
#include "portaudio.h"
#include "sndfile.h"
#include "samplerate.h"

/* Defines */
#define SAMPLE_RATE	(8000)
#define IN_FRAMES	(128)
#define NUM_BUFS	(8)
#define ECHO_LEN	(128)
#define ADAPT_MODE	(ECHO_CAN_USE_ADAPTION | ECHO_CAN_USE_NLP | ECHO_CAN_USE_CNG)
#define MIN(x, y)	((x) > (y) ? y : x)
#define MAX(x, y)	((x) < (y) ? y : x)

#define CODEC2_BYTES_PER_FRAME ((CODEC2_BITS_PER_FRAME + 7) / 8)

#if 1
#define debug(fmt, ...) \
        do { fprintf(stderr, "%s:%d:%s(): " fmt "\n", __FILE__, 	\
		     __LINE__, __func__, ## __VA_ARGS__); } 		\
while (0)
#else
#define debug(...)
#endif

/* Decls */
struct audiobuf_t {
    STAILQ_ENTRY(audiobuf_t) link;

    int			valid;
    
    int16_t		samples[0];
};

STAILQ_HEAD(audioqueue_t, audiobuf_t);

typedef struct {
    SF_INFO			sfinfo;
    SNDFILE			*sndfile;
    SRC_STATE			*src;
    int				done;

    pthread_mutex_t		mtx;		/* Mutex for frobbing queues */
    

    struct audioqueue_t		play;		/* Outgoing samples */
    struct audiobuf_t		*playcur;	/* Current buffer being played */
    int				playofs;	/* Offset into current buffer */
    struct audioqueue_t		playfree;

    int				underrun;
    
    struct audioqueue_t		rec;		/* Incoming samples */
    struct audiobuf_t		*reccur;	/* Current buffer being played */
    struct audioqueue_t		recfree;
    int				overrun;

    echo_can_state_t 		*echocan;	/* Echo canceller state */
    void			*codec2;	/* Codec2 state */
    
} PaCtx;

/* Prototypes */
void		runstream(PaCtx *ctx, int netfd, struct sockaddr *send_addr, socklen_t addrlen);
void		freectx(PaCtx *ctx);

int
countqueue(struct audioqueue_t *q) {
    int			count;
    struct audiobuf_t	*v, *tmp;
    
    count = 0;
    STAILQ_FOREACH_SAFE(v, q, link, tmp) {
	count++;
	assert(count < 2 * NUM_BUFS);
    }

    return count;
}

void
logbufcnt(PaCtx *ctx, char *prefix) {
    debug("%sRec: %d, Rec Free: %d, Play: %d, Play Free: %d",
	  prefix == NULL ? "" : prefix,
	  countqueue(&ctx->rec), countqueue(&ctx->recfree),
	  countqueue(&ctx->play), countqueue(&ctx->playfree));
}

/* This routine will be called by the PortAudio engine when audio is needed.
** It may called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int
patestCallback(const void *inputBuffer, void *outputBuffer,
	       unsigned long framesPerBuffer,
	       const PaStreamCallbackTimeInfo* timeInfo,
	       PaStreamCallbackFlags statusFlags,
	       void *userData) {
    PaCtx			*ctx;
    int16_t			*in, *out;
    int				avail, ofs;
    
    ctx = (PaCtx *)userData;
    out = (int16_t *)outputBuffer;
    in = (int16_t *)inputBuffer;

    debug("called");
    logbufcnt(ctx, "pa ");
    
    if (ctx->done) {
	debug("done, exiting early");
	return paComplete;
    }
    
    pthread_mutex_lock(&ctx->mtx);
    
    /* Run the echo canceller */
#if 0
    for (ofs = 0; ofs < framesPerBuffer; ofs++)
	out[ofs] = echo_can_update(ctx->echocan, in[ofs], out[ofs]);
#endif

    /* Copy out samples to be played */
    for (ofs = 0; ofs < framesPerBuffer; ) {
	/* Need a new buffer? */
	if (ctx->playcur == NULL) {
	    /* Queue is empty, note underrun and drop out */
	    if ((ctx->playcur = STAILQ_FIRST(&ctx->play)) == NULL) {
		debug("Play: Underrun");
		ctx->underrun += framesPerBuffer - ofs;
		break;
	    }

	    STAILQ_REMOVE_HEAD(&ctx->play, link);
	    debug("Play: Popping buffer of %d samples", ctx->playcur->valid);
	    
	    ctx->playofs = 0;
	}
	
	/* Find the amount of data we can copy */
	avail = MIN(framesPerBuffer - ofs, ctx->playcur->valid - ctx->playofs);
	debug("Copying %d samples to out", avail);
	if (avail == 0)
	    abort();
	
	bcopy(ctx->playcur->samples + ctx->playofs, out, avail * sizeof(out[0]));
	ctx->playofs += avail;
	ofs += avail;
	
	if (ctx->playofs == ctx->playcur->valid) {
	    debug("Play: Pushing buffer to free list");
	    ctx->playcur->valid = 0;
	    STAILQ_INSERT_TAIL(&ctx->playfree, ctx->playcur, link);
	    ctx->playcur = NULL;
	    ctx->playofs = 0;
	}
    }
    
    /* Copy in samples to be recorded */
    for (ofs = 0; ofs < framesPerBuffer; ) {
	/* Need a new buffer? */
	if (ctx->reccur == NULL) {
	    debug("Rec: Grabbing free buffer");

	    /* No free packets, bail out */
	    if ((ctx->reccur = STAILQ_FIRST(&ctx->recfree)) == NULL) {
		debug("Rec: overrun");
		ctx->overrun += framesPerBuffer - ofs;
		break;
	    }

	    STAILQ_REMOVE_HEAD(&ctx->recfree, link);
	    ctx->reccur->valid = 0;
	}
	
	/* Find the amount of data we can copy */
	avail = MIN(framesPerBuffer - ofs, IN_FRAMES - ctx->reccur->valid);

	assert(avail > 0);

	debug("Copying %d samples from in", avail);
	
	bcopy(in, ctx->reccur->samples + ctx->reccur->valid, avail * sizeof(ctx->reccur->samples[0]));
	ctx->reccur->valid += avail;
	ofs += avail;
	
	if (ctx->reccur->valid == IN_FRAMES) {
	    debug("Rec: Pushing buffer to rec list");
	    STAILQ_INSERT_TAIL(&ctx->rec, ctx->reccur, link);
	    ctx->reccur = NULL;
	}
    }
    debug("PA callback done for %lu frames", framesPerBuffer);
    
    pthread_mutex_unlock(&ctx->mtx);

    return paContinue;
}

/* Set stop flag on completion */
void
streamdone(void *userData) {
    PaCtx *ctx __attribute__((unused)) = (PaCtx *)userData;

}

/*******************************************************************/

int
main(int argc, char **argv) {
    PaStream		*stream;
    PaError		err, err2;
    PaCtx		ctx;
    int			srcerr, i, netfd;
    struct audiobuf_t	*tmpbuf;
    struct sockaddr_in	my_addr, send_addr;
    socklen_t		addrlen;
    
    ctx.sndfile = NULL;
    ctx.src = NULL;
    ctx.echocan = NULL;
    ctx.codec2 = NULL;
    
    err = err2 = 0;
    
    if (argc != 2) {
	fprintf(stderr, "Bad usage\n");
	exit(1);
    }
    
    /* Construct address to send to */
    bzero(&send_addr, sizeof(send_addr));
    send_addr.sin_family = AF_INET;
    send_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    send_addr.sin_port = htons(4001);
    i = inet_pton(AF_INET, "127.0.0.1", &send_addr.sin_addr);
    if (i == 0) {
	fprintf(stderr, "Unable to parse destination address\n");
	exit(1);
    } else if (i == -1) {
	fprintf(stderr, "Unable to parse destination address: %s\n", strerror(errno));
	exit(1);
    }
    addrlen = sizeof(send_addr);
    
    /* Open/bind socket */
    if ((netfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
	fprintf(stderr, "Unable to create socket: %s\n", strerror(errno));
	exit(1);
    }
    
    bzero(&my_addr, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    my_addr.sin_port = htons(4000);

    if (bind(netfd, (struct sockaddr *)&my_addr, sizeof(my_addr)) < 0) {
	fprintf(stderr, "Unable to bind socket: %s\n", strerror(errno));
	exit(1);
    }

    /* Load file */
    if ((ctx.sndfile = sf_open(argv[1], SFM_READ, &ctx.sfinfo)) == NULL) {
	fprintf(stderr, "Unable to open \"%s\": %s\n", argv[1], sf_strerror(ctx.sndfile));
	exit(1);
    }

    /* Init queues */
    STAILQ_INIT(&ctx.play);
    STAILQ_INIT(&ctx.playfree);
    STAILQ_INIT(&ctx.rec);
    STAILQ_INIT(&ctx.recfree);

    /* Init mutex */
    pthread_mutex_init(&ctx.mtx, NULL);
    
    /* Init counters, etc */
    ctx.playofs = ctx.underrun = ctx.overrun = ctx.done = 0;
    ctx.playcur = ctx.reccur = NULL;

    /* Allocate buffers */
    for (i = 0; i < NUM_BUFS * 2; i++) {
	if ((tmpbuf = malloc(sizeof(struct audiobuf_t) + IN_FRAMES * sizeof(tmpbuf->samples[0]))) == NULL) {
	    fprintf(stderr, "Unable to allocate memory for sample buffers\n");
	    err2 = 1;
	    goto error;
	}

	if (i < NUM_BUFS)
	    STAILQ_INSERT_TAIL(&ctx.playfree, tmpbuf, link);
	else
	    STAILQ_INSERT_TAIL(&ctx.recfree, tmpbuf, link);
    }
    
    /* Init sample rate converter */
    if ((ctx.src = src_new(SRC_SINC_BEST_QUALITY, ctx.sfinfo.channels, &srcerr)) == NULL) {
	fprintf(stderr, "Unable to init sample rate converter: %d\n", srcerr);
	err2 = 1;
	goto error;
    }

    /* Init echo canceller */
    if ((ctx.echocan = echo_can_init(ECHO_LEN, ADAPT_MODE)) == NULL) {
	fprintf(stderr, "Unable to init echo canceller\n");
	err2 = 1;
	goto error;
    }

    /* Init codec2 */
    if ((ctx.codec2 = codec2_create()) == NULL) {
	fprintf(stderr, "Unable to init codec2\n");
	err2 = 1;
	goto error;
    }
    
    /* Initialize Port Audio library */
    if ((err = Pa_Initialize()) != paNoError)
	goto error;
     
    /* Open an audio I/O stream. */
    if ((err = Pa_OpenDefaultStream(&stream,
				    1,          /* input channels */
				    1,          /* output channels */
				    paInt16,
				    SAMPLE_RATE,
				    IN_FRAMES, /* frames per buffer */
				    patestCallback,
				    &ctx)) != paNoError)
	goto error;
 
    /* Set callback to notify us when the stream is done */
    if ((err = Pa_SetStreamFinishedCallback(stream, streamdone)) != paNoError)
	goto error;
    
    /* Start stream */
    if ((err = Pa_StartStream(stream)) != paNoError)
	goto error;

    runstream(&ctx, netfd, (struct sockaddr *)&send_addr, addrlen);
 
    /* Close down stream, PA, etc */
    if ((err = Pa_StopStream(stream)) != paNoError)
	goto error;

    if ((err = Pa_CloseStream(stream)) != paNoError)
	goto error;

  error:
    Pa_Terminate();
    
    /* Free things */
    freectx(&ctx);
    if (netfd != -1)
	close(netfd);
    
    if (err != paNoError)
	fprintf(stderr, "Port audio error: %s\n", Pa_GetErrorText(err));

    return MAX(err, err2);
}

void
runstream(PaCtx *ctx, int netfd, struct sockaddr *send_addr, socklen_t addrlen) {
    int			i, amt, err, out_frames, residue, codecinres, codecinamt, done, avail;
    SRC_DATA		srcdata;
    int16_t		*sndinbuf, *sndoutbuf, *tmp;
    float		*srcinbuf, *srcoutbuf;
    double		ratio;
    struct audiobuf_t	*tmpbuf, *codecinbuf;
    int16_t		codecinbuf2[CODEC2_SAMPLES_PER_FRAME];
    unsigned char	codecbits[CODEC2_BYTES_PER_FRAME];
    
    ratio = (double)SAMPLE_RATE / (double)ctx->sfinfo.samplerate;
    out_frames = MAX(16, IN_FRAMES * ratio);
    debug("In rate %d, out rate %d, ratio %.3f, in frames %d, out frames %d",
	  SAMPLE_RATE, ctx->sfinfo.samplerate, ratio, IN_FRAMES, out_frames);
    
    sndinbuf = sndoutbuf = NULL;
    srcinbuf = srcoutbuf = NULL;
    
    /* Allocate memory for reading from the file (all channels) */
    if ((sndinbuf = malloc(IN_FRAMES * ctx->sfinfo.channels * sizeof(sndinbuf[0]))) == NULL) {
	fprintf(stderr, "Unable to allocate memory for sndinbuf\n");
	goto out;
    }

    /* Allocate memory for float version of above (1 channel only) */
    if ((srcinbuf = malloc(IN_FRAMES * sizeof(srcinbuf[0]))) == NULL) {
	fprintf(stderr, "Unable to allocate memory for srcinbuf\n");
	goto out;
    }
    
    /* Allocate memory for rate conversion output buffers */
    if ((sndoutbuf = malloc(out_frames * sizeof(sndoutbuf[0]))) == NULL) {
	fprintf(stderr, "Unable to allocate memory for sndoutbuf\n");
	goto out;
    }

    if ((srcoutbuf = malloc(out_frames * sizeof(srcoutbuf[0]))) == NULL) {
	fprintf(stderr, "Unable to allocate memory for srcoutbuf\n");
	goto out;
    }
    
    codecinbuf = NULL;	/* Buffer we are currently compressing */
    tmpbuf = NULL;	/* Buffer we are currently filling */
    residue = 0;	/* Residue of samples read from file */
    codecinres = 0;	/* Residue of samples in codecbuf being fed into the compressor */
    codecinamt = 0;	/* Number of samples in codecbuf2 */

    while (!ctx->done) {
	logbufcnt(ctx, "top ");
	Pa_Sleep(0);

	/* Need a buffer? */
	if (tmpbuf == NULL) {
	    debug("Grabbing free play buffer");
	    
	    while (1) {
		pthread_mutex_lock(&ctx->mtx);
		tmpbuf = STAILQ_FIRST(&ctx->playfree);
		pthread_mutex_unlock(&ctx->mtx);
		
		if (tmpbuf != NULL) {
		    STAILQ_REMOVE_HEAD(&ctx->playfree, link);
		    logbufcnt(ctx, "pop ");
		    break;
		}
		
		debug("Sleeping..");
		Pa_Sleep(100);
	    }
	    tmpbuf->valid = 0; /* Number of valid samples in this buffer */
	}
	
	assert(IN_FRAMES - tmpbuf->valid - residue > 0);

	/* Read chunk of data from the file
	 * Ask for enough to fill what's left of our current buffer at most */
	if (IN_FRAMES - residue > 0) {
	    amt = sf_readf_short(ctx->sndfile, sndinbuf, IN_FRAMES - residue);
	    debug("Read %d frames, asked for %d (%d)", amt, IN_FRAMES - residue,
		   residue);
	
	    /* Convert int16_t buffer to floats, pluck out the first channel */
	    tmp = sndinbuf;
	    for (i = 0; i < amt; i++) {
		srcinbuf[i + residue] = (float)*tmp / 32768.0;
		tmp += ctx->sfinfo.channels;
	    }
	}
	
	/* Fill in conversion info */
	srcdata.data_in = srcinbuf;
	srcdata.input_frames = amt + residue;
	srcdata.data_out = srcoutbuf;
	srcdata.output_frames = out_frames - tmpbuf->valid;
	srcdata.src_ratio = ratio;
	
	/* Tell SRC we're done */
	if (amt < IN_FRAMES) {
	    srcdata.end_of_input = 1;
	    ctx->done = 1;
	} else
	    srcdata.end_of_input = 0;

	/* Run conversion */
	if ((err = src_process(ctx->src, &srcdata)) != 0) {
	    fprintf(stderr, "Failed to convert audio: %s\n", src_strerror(err));
	    goto out;
	}

	debug("Conversion consumed %lu/%lu frames and generated %lu/%lu frames, EOI %d",
	      srcdata.input_frames_used, srcdata.input_frames,
	      srcdata.output_frames_gen, srcdata.output_frames, srcdata.end_of_input);
	
	assert(tmpbuf->valid + srcdata.output_frames <= out_frames);
	
	/* Convert floats back to int16_t */
	src_float_to_short_array(srcoutbuf, tmpbuf->samples + tmpbuf->valid, srcdata.output_frames_gen);

	tmpbuf->valid += srcdata.output_frames_gen;
	residue = srcdata.input_frames - srcdata.input_frames_used;

	/* Pull down residual samples */
	bcopy(srcinbuf + srcdata.input_frames_used, srcinbuf, residue * sizeof(srcinbuf[0]));
	
	assert(tmpbuf->valid <= IN_FRAMES);
	
	if (tmpbuf->valid == IN_FRAMES) {
	    debug("Pushing buffer for playback");

	    pthread_mutex_lock(&ctx->mtx);
	    STAILQ_INSERT_TAIL(&ctx->play, tmpbuf, link);	
	    pthread_mutex_unlock(&ctx->mtx);
	    tmpbuf = NULL;
	}

	/* Recording */
	pthread_mutex_lock(&ctx->mtx);
	done = 0;
	while (!done) {
	    /* Fill up a buffer to encode */
	    for (codecinamt = 0; codecinamt < CODEC2_SAMPLES_PER_FRAME; ) {
		/* Grab a new buffer to encode */
		if (codecinbuf == NULL) {
		    if ((codecinbuf = STAILQ_FIRST(&ctx->rec)) == NULL) {
			done = 1;
			break;
		    }
		    
		    codecinres = 0;
		    STAILQ_REMOVE_HEAD(&ctx->rec, link);
		}
		avail = MIN(codecinbuf->valid - codecinres, CODEC2_SAMPLES_PER_FRAME - i);
		assert(avail > 0);

		/* Copy data */
		bcopy(codecinbuf->samples + codecinres, codecinbuf2 + codecinamt, avail * sizeof(codecinbuf->samples[0]));
		codecinres += avail;
		codecinamt += avail;
		
		/* Done with buffer? */
		if (codecinres == codecinbuf->valid) {
		    STAILQ_INSERT_TAIL(&ctx->recfree, codecinbuf, link);
		    codecinbuf = NULL;
		}
	    }

	    /* Filled the buffer? Encode */
	    if (codecinamt == CODEC2_SAMPLES_PER_FRAME) {
		codec2_encode(ctx->codec2, codecbits, codecinbuf2);
		if (sendto(netfd, codecbits, CODEC2_BYTES_PER_FRAME, 0, send_addr, addrlen) != 0)
		    fprintf(stderr, "Unable to send packet: %s\n", strerror(errno));
	    }
	}
	
	pthread_mutex_unlock(&ctx->mtx);
    }
    
    /* Left over buffers? Move to free list so it will be freed */
    pthread_mutex_lock(&ctx->mtx);
    if (tmpbuf != NULL)
	STAILQ_INSERT_TAIL(&ctx->playfree, tmpbuf, link);

    if (codecinbuf != NULL)
	STAILQ_INSERT_TAIL(&ctx->playfree, codecinbuf, link);
    pthread_mutex_unlock(&ctx->mtx);
    
    printf("Play underruns: %d\n", ctx->underrun);
    printf("Record overruns: %d\n", ctx->overrun);
    
  out:
    if (sndinbuf != NULL)
	free(sndinbuf);
    if (sndoutbuf != NULL)
	free(sndoutbuf);
    
    if (srcinbuf != NULL)
	free(srcinbuf);
    if (srcoutbuf != NULL)
	free(srcoutbuf);
}

void
freequeue(struct audioqueue_t *q) {
    struct audiobuf_t	*v, *tmp;
    
    STAILQ_FOREACH_SAFE(v, q, link, tmp) {
	free(v);
    }
}

void
freectx(PaCtx *ctx) {
    /* Destroy mutex */
    pthread_mutex_destroy(&ctx->mtx);
    
    /* Close soundfile handle */
    if (ctx->sndfile != NULL)
	sf_close(ctx->sndfile);

    /* Free SRC resources */
    if (ctx->src != NULL)
	src_delete(ctx->src);

    /* Free echo caneller */
    if (ctx->echocan != NULL)
	echo_can_free(ctx->echocan);

    /* Free codec2 */
    if (ctx->codec2 != NULL)
	codec2_destroy(ctx->codec2);
    
    /* Free sample buffers */
    freequeue(&ctx->play);
    freequeue(&ctx->playfree);
    freequeue(&ctx->rec);
    freequeue(&ctx->recfree);
}
