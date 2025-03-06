bin_PROGRAMS = rtsp-proxy

rtsp_proxy_SOURCES = rtsp-proxy.c
rtsp_proxy_CFLAGS = $(FFMPEG_CFLAGS)
rtsp_proxy_LDADD = $(FFMPEG_LIBS) $(INIPARSER_LIBS) $(NNG_LIBS) -lpthread
