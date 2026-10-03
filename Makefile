SRCDIR ?= /opt/fpp/src
include ${SRCDIR}/makefiles/common/setup.mk
include $(SRCDIR)/makefiles/platform/*.mk

all: libfpp-plugin-listen-sync.$(SHLIB_EXT)
debug: all

OBJECTS_fpp_listen_sync_so += src/ListenSyncPlugin.o
LIBS_fpp_listen_sync_so += -L${SRCDIR} -lfpp -ljsoncpp
CXXFLAGS_src/ListenSyncPlugin.o += -I${SRCDIR}

%.o: %.cpp Makefile
	$(CCACHE) $(CC) $(CFLAGS) $(CXXFLAGS) $(CXXFLAGS_$@) -c $< -o $@

libfpp-plugin-listen-sync.$(SHLIB_EXT): $(OBJECTS_fpp_listen_sync_so) ${SRCDIR}/libfpp.$(SHLIB_EXT)
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_listen_sync_so) $(LIBS_fpp_listen_sync_so) $(LDFLAGS) -o $@

clean:
	rm -f libfpp-plugin-listen-sync.$(SHLIB_EXT) $(OBJECTS_fpp_listen_sync_so)
