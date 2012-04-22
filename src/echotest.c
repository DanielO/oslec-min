#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
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
#include "fifo.h"
#include "portaudio.h"
#include "samplerate.h"
#include "sndfile.h"

/* Defines */
#define SAMPLE_RATE	(8000)
#define IN_FRAMES	(128)
#define SRC_MIN_FRAMES	(8)
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

/* Prototypes */
typedef struct {
    SRC_STATE			*src;

    pthread_mutex_t		mtx;		/* Mutex for frobbing queues */
    
    /* Incoming samples after decompression
     * Written with result of  recvfrom + codec2_decode
     * Read by sample rate converter
     */
    struct fifo			*incoming;
    int				incoverflow;
    
    /* Samples after rate conversion
     * Written by sample rate converter
     * Read by PA callback
     */
    struct fifo			*incrate;
    int				underrun;

    /* Outgoing samples
     * Written by PA callback
     * Read by codec2_encode + sendto
     */
    struct fifo			*outgoing;

    int				overrun;

    echo_can_state_t 		*echocan;	/* Echo canceller state */
    void			*codec2;	/* Codec2 state */
    
} PaCtx;

/* Declarations */
static int	done = 0;

/* Prototypes */
void		runstream(PaCtx *ctx, int netfd, struct sockaddr *send_addr, socklen_t addrlen);
void		freectx(PaCtx *ctx);

static void
sigstop(int sig, siginfo_t *si, void *context) {
    done = 1;
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
    int				avail, amt;
    
    ctx = (PaCtx *)userData;
    out = (int16_t *)outputBuffer;
    in = (int16_t *)inputBuffer;

    //debug("called");
    
    pthread_mutex_lock(&ctx->mtx);
    
    amt = framesPerBuffer * sizeof(out[0]);
    
    /* Copy out samples to be played */
    if ((avail = fifo_get(ctx->incrate, (uint8_t *)out, amt)) < amt) {
	/* Zero out samples there are no data for */
	bzero(out + (avail / sizeof(out[0])), amt - avail);
	ctx->underrun += (amt - avail) / sizeof(out[0]);
    }
    
    /* Copy in samples to be recorded */
    if ((avail = fifo_put(ctx->outgoing, (uint8_t *)in, amt)) < amt) {
	/* Zero out samples there are no data for */
	bzero(in + (avail / sizeof(out[0])), amt - avail);
	ctx->overrun += (amt - avail) / sizeof(out[0]);
    }

#if 0
    /* Run the echo canceller */
    for (int ofs = 0; ofs < framesPerBuffer; ofs++)
	out[ofs] = echo_can_update(ctx->echocan, in[ofs], out[ofs]);
#endif
    pthread_mutex_unlock(&ctx->mtx);

    return paContinue;
}

void
usage(char *argv0) {
    fprintf(stderr,
	    "%s [-b ip] [-h] [-p port] destip destport\n"
	    "\tip\tIP to bind to (default INADDR_ANY)\n"
	    "\tport\tPort to bind to (default 4000)\n"
	    "\tdestip\tIP to send to\n"
	    "\tdestport\tPort to send to\n", argv0);
    exit(1);
}

int
main(int argc, char **argv) {
    PaStream		*stream;
    PaError		err, err2;
    PaCtx		ctx;
    int			srcerr, i, netfd, bindport, destport, ch;
    struct sockaddr_in	my_addr, send_addr;
    socklen_t		addrlen;
    char		*bindaddr, *argv0;
    
    bindport = 4000;
    bindaddr = NULL;
    ctx.src = NULL;
    ctx.echocan = NULL;
    ctx.codec2 = NULL;
    ctx.incoming = ctx.incrate = ctx.outgoing = NULL;
    ctx.incoverflow = ctx.underrun = ctx.overrun = 0;
    
    err = err2 = 0;
    argv0 = argv[0];
    
    while ((ch = getopt(argc, argv, "b:hp:")) != -1) {
	switch (ch) {
	    case 'b':
		bindaddr = optarg;
		break;

	    case 'p':
		bindport = atoi(optarg);
		if (bindport <= 0 || bindport > 65535) {
		    fprintf(stderr, "bindport out of range, must be 1-65535\n");
		    exit(1);
		}
		break;
		

	    case'h':
	    case'?':
	    default:
		usage(argv0);
		break;
	}
    }

    argc -= optind;
    argv += optind;
	
    if (argc != 2)
	usage(argv0);

    destport = atoi(argv[1]);
    if (destport <= 0 || destport > 65535) {
	fprintf(stderr, "destport out of range, must be 1-65535\n");
	exit(1);
    }
    
    /* Construct address to send to */
    bzero(&send_addr, sizeof(send_addr));
    send_addr.sin_family = AF_INET;
    send_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    send_addr.sin_port = htons(destport);
    i = inet_pton(AF_INET, argv[0], &send_addr.sin_addr);
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
    
    if (fcntl(netfd, F_SETFL, O_NONBLOCK) == -1) {
	fprintf(stderr, "Unable to set non-blocking on socket: %s\n", strerror(errno));
	exit(1);
    }

    /* Bind local address/port */
    bzero(&my_addr, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    if (bindaddr == NULL)
	my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else {
	i = inet_pton(AF_INET, bindaddr, &my_addr.sin_addr);
	if (i == 0) {
	    fprintf(stderr, "Unable to parse bind address\n");
	    exit(1);
	} else if (i == -1) {
	    fprintf(stderr, "Unable to parse bind address: %s\n", strerror(errno));
	    exit(1);
	}
    }
    my_addr.sin_port = htons(bindport);

    if (bind(netfd, (struct sockaddr *)&my_addr, sizeof(my_addr)) < 0) {
	fprintf(stderr, "Unable to bind socket: %s\n", strerror(errno));
	exit(1);
    }

    /* Init mutex */
    if (pthread_mutex_init(&ctx.mtx, NULL) != 0){
	fprintf(stderr, "Unable to init mutex: %s\n", strerror(errno));
	err2 = 1;
	goto error;
    }

    /* Allocate FIFOs */
    i = IN_FRAMES * 10 * sizeof(int16_t);
    printf("Allocating %d byte FIFOs\n", i);
    
    if ((ctx.incoming = fifo_alloc(i)) == NULL) {
	fprintf(stderr, "Unable to allocate incoming FIFO\n");
	err2 = 1;
	goto error;
    }
    if ((ctx.incrate = fifo_alloc(i)) == NULL) {
	fprintf(stderr, "Unable to allocate incoming SRC FIFO\n");
	err2 = 1;
	goto error;
    }
    if ((ctx.outgoing = fifo_alloc(i)) == NULL) {
	fprintf(stderr, "Unable to allocate outgoing FIFO\n");
	err2 = 1;
	goto error;
    }
    

    /* Init sample rate converter */
    if ((ctx.src = src_new(SRC_SINC_BEST_QUALITY, 1, &srcerr)) == NULL) {
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
 
    /* Start stream */
    if ((err = Pa_StartStream(stream)) != paNoError)
	goto error;

    runstream(&ctx, netfd, (struct sockaddr *)&send_addr, addrlen);
 
    /* Close down stream, PA, etc */
/* XXX: hangs in pthread_join on Ubuntu 10.04 */
#ifndef linux
    if ((err = Pa_StopStream(stream)) != paNoError)
	goto error;
#endif
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
    int			i, amt, err, out_frames, avail, didplay, didrec;
    SRC_DATA		srcdata;
    int16_t		*sndinbuf, *sndoutbuf;
    float		*srcinbuf, *srcoutbuf;
    double		ratio;
    int16_t		codecinbuf[CODEC2_SAMPLES_PER_FRAME];
    unsigned char	codecbits[CODEC2_BYTES_PER_FRAME];
    struct sockaddr_in	recv_addr;
    socklen_t		recv_addrlen;
    ssize_t		recvlen;
    struct sigaction	sa;
    
    ratio = (double)SAMPLE_RATE / (double)SAMPLE_RATE;
    out_frames = MAX(16, IN_FRAMES * ratio);
    debug("In rate %d, out rate %d, ratio %.3f, in frames %d, out frames %d",
	  SAMPLE_RATE, SAMPLE_RATE, ratio, IN_FRAMES, out_frames);
    
    sndinbuf = sndoutbuf = NULL;
    srcinbuf = srcoutbuf = NULL;
    
    /* Allocate memory for SRC conversion buffer */
    if ((sndinbuf = malloc(IN_FRAMES * sizeof(sndinbuf[0]))) == NULL) {
	fprintf(stderr, "Unable to allocate memory for sndinbuf\n");
	goto out;
    }

    /* Allocate memory for float version of above */
    if ((srcinbuf = malloc(IN_FRAMES * sizeof(srcinbuf[0]))) == NULL) {
	fprintf(stderr, "Unable to allocate memory for srcinbuf\n");
	goto out;
    }
    
    /* Allocate memory for rate conversion output buffer */
    if ((sndoutbuf = malloc(out_frames * sizeof(sndoutbuf[0]))) == NULL) {
	fprintf(stderr, "Unable to allocate memory for sndoutbuf\n");
	goto out;
    }

    /* Allocate memory for float version of above */
    if ((srcoutbuf = malloc(out_frames * sizeof(srcoutbuf[0]))) == NULL) {
	fprintf(stderr, "Unable to allocate memory for srcoutbuf\n");
	goto out;
    }
    
    sa.sa_sigaction = sigstop;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGHUP, &sa, NULL) ||
	sigaction(SIGINT, &sa, NULL) ||
	sigaction(SIGTERM, &sa, NULL)) {
	fprintf(stderr, "Couldn't set signal handler, %s", strerror(errno));
	goto out;
    }

    done = 0;
    
    while (!done) {
	didplay = didrec = 0;
    
	/* Check for new packets if we have room 
	 * XXX: should be smarter but this helps testing with ttcp
	 */
	pthread_mutex_lock(&ctx->mtx);
	
	if (fifo_space(ctx->incoming) >= CODEC2_SAMPLES_PER_FRAME * sizeof(sndinbuf[0])) {
	    if ((recvlen = recvfrom(netfd, codecbits, CODEC2_BYTES_PER_FRAME, 0, (struct sockaddr *)&recv_addr, &recv_addrlen)) == -1) {
		if (errno != EAGAIN)
		    debug("Error reading from socket: %s", strerror(errno));
	    } else {
		debug("Received %ld bytes", (long)recvlen);
               
		/* Unpack received data */
		codec2_decode(ctx->codec2, sndinbuf, codecbits);

		/* Put it into the FIFO */
		amt = CODEC2_SAMPLES_PER_FRAME * sizeof(sndinbuf[0]);
		if ((i = fifo_put(ctx->incoming, (uint8_t *)sndinbuf, amt)) != amt)
		    ctx->incoverflow += amt - i;
		didplay = 1;
	    }
	}
	
	pthread_mutex_unlock(&ctx->mtx);

	/* Feed the sample rate converter */
	pthread_mutex_lock(&ctx->mtx);
	if ((avail = fifo_avail(ctx->incoming)) > SRC_MIN_FRAMES) {
	    amt = MIN(avail, IN_FRAMES * sizeof(sndinbuf[0]));
	    assert(fifo_get(ctx->incoming, (uint8_t *)sndinbuf, amt) == amt);
	
	    /* Convert int16_t buffer to floats */
	    src_short_to_float_array(sndinbuf, srcinbuf, amt / sizeof(sndinbuf[0]));
	
	    /* Fill in conversion info */
	    srcdata.data_in = srcinbuf;
	    srcdata.input_frames = amt / sizeof(sndinbuf[0]);
	    srcdata.data_out = srcoutbuf;
	    srcdata.output_frames = out_frames;
	    srcdata.src_ratio = ratio;
	    srcdata.end_of_input = 0;

	    /* Run conversion */
	    if ((err = src_process(ctx->src, &srcdata)) != 0) {
		fprintf(stderr, "Failed to convert audio: %s\n", src_strerror(err));
		goto out;
	    }

#if 0
	    debug("Conversion consumed %lu/%lu frames and generated %lu/%lu frames, EOI %d",
		  srcdata.input_frames_used, srcdata.input_frames,
		  srcdata.output_frames_gen, srcdata.output_frames, srcdata.end_of_input);
#endif	
	
	    /* Convert floats back to int16_t */
	    src_float_to_short_array(srcoutbuf, sndoutbuf, srcdata.output_frames_gen);

	    /* Unget the unused samples back into the FIFO */
	    fifo_unget(ctx->incoming, (uint8_t *)(sndinbuf + srcdata.input_frames_used), srcdata.input_frames - srcdata.input_frames_used);
	
	    /* Push rate converted samples to FIFO */
	    fifo_put(ctx->incrate, (uint8_t *)sndoutbuf, srcdata.output_frames_gen * sizeof(sndoutbuf[0]));
	    didplay = 1;
	}
    
	pthread_mutex_unlock(&ctx->mtx);

	/* Recording */
	pthread_mutex_lock(&ctx->mtx);

	/* Enough to compress and send a packet? */
	amt = CODEC2_SAMPLES_PER_FRAME * sizeof(codecinbuf[0]);
	if ((avail = fifo_avail(ctx->outgoing)) >= amt) {
	    assert((i = fifo_get(ctx->outgoing, (uint8_t *)codecinbuf, amt)) == amt);

	    /* Compress & send */
	    codec2_encode(ctx->codec2, codecbits, codecinbuf);
	    if ((i = sendto(netfd, codecbits, CODEC2_BYTES_PER_FRAME, 0, send_addr, addrlen)) != 0) {
		if (i < 0)
		    fprintf(stderr, "Unable to send packet: %s\n", strerror(errno));
		else if (i != CODEC2_BYTES_PER_FRAME)
		    fprintf(stderr, "Short packet sent, expected %d, sent %d\n", CODEC2_BYTES_PER_FRAME, i);
	    } else
		printf("Sent packet\n");
	    didrec = 1;
	}
	pthread_mutex_unlock(&ctx->mtx);

	if (didrec == 0 && didplay == 0)
	    Pa_Sleep(20);
    }
    
    printf("\nIncoming overflows: %d\n", ctx->incoverflow);
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
freectx(PaCtx *ctx) {
    /* Destroy mutex */
    pthread_mutex_destroy(&ctx->mtx);
    
    /* Free SRC resources */
    if (ctx->src != NULL)
	src_delete(ctx->src);

    /* Free echo caneller */
    if (ctx->echocan != NULL)
	echo_can_free(ctx->echocan);

    /* Free codec2 */
    if (ctx->codec2 != NULL)
	codec2_destroy(ctx->codec2);

    /* Free FIFOs */
    if (ctx->incoming != NULL)
	fifo_free(ctx->incoming);
    if (ctx->incrate != NULL)
	fifo_free(ctx->incrate);
    if (ctx->outgoing != NULL)
	fifo_free(ctx->outgoing);
    
}
