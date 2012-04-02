SRCS=		echo.c echotest.c
OBJS=		$(SRCS:.c=.o)
HDRS=		
PROG=		echotest

SPANDSP=	spandsp

CFLAGS=		-g -O2 -O0 -g -Wall -I $(SPANDSP)
LDFLAGS=	 $(EFENCE)
DEFS=		-DPACKAGE_NAME=\"echotest\" -DPACKAGE_TARNAME=\"echotest\" -DPACKAGE_VERSION=\"0.1\" -DPACKAGE_STRING=\"echotest\ 0.1\" -DPACKAGE_BUGREPORT=\"\" -DSTDC_HEADERS=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_STDLIB_H=1 -DHAVE_STRING_H=1 -DHAVE_MEMORY_H=1 -DHAVE_STRINGS_H=1 -DHAVE_INTTYPES_H=1 -DHAVE_STDINT_H=1 -DHAVE_UNISTD_H=1 -DHAVE_MATH_H=1 -DHAVE_FLOAT_H=1 -DHAVE_PTHREAD_H=1 -DHAVE_LIBPTHREAD=1 -DHAVE_LIBM=1 -DHAVE_SINF=1 -DHAVE_COSF=1 -DHAVE_TANF=1 -DHAVE_ASINF=1 -DHAVE_ACOSF=1 -DHAVE_ATANF=1 -DHAVE_ATAN2F=1 -DHAVE_CEILF=1 -DHAVE_FLOORF=1 -DHAVE_POWF=1 -DHAVE_EXPF=1 -DHAVE_LOGF=1 -DHAVE_LOG10F=1
LIBS=		-lm -lpthread 

PORTAUDIO_CFLAGS= -I/opt/local/include   
PORTAUDIO_LIBS=	-framework CoreAudio -framework AudioToolbox -framework AudioUnit -framework Carbon -L/opt/local/lib -lportaudio  

SNDFILE_CFLAGS=	-I/opt/local/include  
SNDFILE_LIBS=	-L/opt/local/lib -lsndfile  

SRC_CFLAGS=	-I/opt/local/include  
SRC_LIBS= 	-L/opt/local/lib -lsamplerate  

CODEC2_CFLAGS= -I/usr/local/include
CODEC2_LIBS= -L/usr/local/lib -lcodec2

CFLAGS+=	$(PORTAUDIO_CFLAGS) $(SNDFILE_CFLAGS) $(SRC_CFLAGS) $(CODEC2_CFLAGS)
LDFLAGS+=	$(PORTAUDIO_LIBS) $(SNDFILE_LIBS) $(SRC_LIBS) $(CODEC2_LIBS)

all:		$(PROG)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(DEFS) -c -o $@ $<

$(PROG):	$(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

clean:
	$(RM) -f $(OBJS) $(PROG)
