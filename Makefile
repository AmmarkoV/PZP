CC = gcc
CFLAGS = -lzstd -lm
SIMD_FLAGS = -DINTEL_OPTIMIZATIONS -D_GNU_SOURCE  -O3 -mavx2 -march=native -mtune=native  -fPIE -fPIC
RELEASE_FLAGS= -D_GNU_SOURCE  -O3 -march=native -mtune=native  -fPIE -fPIC
DEBUG_FLAGS = -D_GNU_SOURCE -O0 -g3 -fno-omit-frame-pointer -Wstrict-overflow -fPIE -fPIC

SRC = pzp.c
LIB_SRC = pzp_lib.c
OUTDIR = output
PZP = pzp
DPZP = dpzp
SPZP = spzp
LIBPZP = libpzp.so

PREFIX  ?= /usr/local
BINDIR  = $(PREFIX)/bin
LIBDIR  = $(PREFIX)/lib
INCDIR  = $(PREFIX)/include

.PHONY: all clean test install uninstall

all: $(PZP) $(DPZP) $(SPZP) $(LIBPZP)

$(PZP): $(SRC)
	$(CC) $(SRC) $(RELEASE_FLAGS) $(CFLAGS) -o $(PZP)

$(DPZP): $(SRC)
	$(CC) $(SRC) $(DEBUG_FLAGS) $(CFLAGS) -o $(DPZP)

$(SPZP): $(SRC)
	$(CC) $(SRC) $(SIMD_FLAGS) $(CFLAGS) -o $(SPZP)

$(LIBPZP): $(LIB_SRC) pzp.h
	$(CC) -shared -fPIC $(LIB_SRC) $(RELEASE_FLAGS) $(CFLAGS) -o $(LIBPZP)

clean:
	rm -rf $(PZP) $(DPZP) $(SPZP) $(LIBPZP) $(OUTDIR)/*.pzp $(OUTDIR)/*.ppm log*.txt

$(OUTDIR):
	mkdir -p $(OUTDIR)

test: all $(OUTDIR)
	./$(PZP) compress samples/sample.ppm $(OUTDIR)/sample.pzp
	./$(PZP) decompress $(OUTDIR)/sample.pzp $(OUTDIR)/sampleRecode.ppm
	./$(PZP) compress samples/depth16.pnm $(OUTDIR)/depth16.pzp
	./$(PZP) decompress $(OUTDIR)/depth16.pzp $(OUTDIR)/depth16Recode.ppm 
	./$(PZP) compress samples/rgb8.pnm $(OUTDIR)/rgb8.pzp
	./$(PZP) decompress $(OUTDIR)/rgb8.pzp $(OUTDIR)/rgb8Recode.ppm 
	./$(PZP) compress samples/segment.ppm $(OUTDIR)/segment.pzp
	./$(PZP) decompress $(OUTDIR)/segment.pzp $(OUTDIR)/segmentRecode.ppm 


ptest: all $(OUTDIR)
	./$(PZP) pack samples/sample.ppm $(OUTDIR)/sample.pzp
	./$(PZP) decompress $(OUTDIR)/sample.pzp $(OUTDIR)/sampleRecode.ppm
	./$(PZP) pack samples/depth16.pnm $(OUTDIR)/depth16.pzp
	./$(PZP) decompress $(OUTDIR)/depth16.pzp $(OUTDIR)/depth16Recode.ppm 
	./$(PZP) pack samples/rgb8.pnm $(OUTDIR)/rgb8.pzp
	./$(PZP) decompress $(OUTDIR)/rgb8.pzp $(OUTDIR)/rgb8Recode.ppm 
	./$(PZP) pack samples/segment.ppm $(OUTDIR)/segment.pzp
	./$(PZP) decompress $(OUTDIR)/segment.pzp $(OUTDIR)/segmentRecode.ppm 

stest: all $(OUTDIR)
	./$(SPZP) compress samples/sample.ppm $(OUTDIR)/sample.pzp
	./$(SPZP) decompress $(OUTDIR)/sample.pzp $(OUTDIR)/sampleRecode.ppm
	./$(SPZP) compress samples/depth16.pnm $(OUTDIR)/depth16.pzp
	./$(SPZP) decompress $(OUTDIR)/depth16.pzp $(OUTDIR)/depth16Recode.ppm 
	./$(SPZP) compress samples/rgb8.pnm $(OUTDIR)/rgb8.pzp
	./$(SPZP) decompress $(OUTDIR)/rgb8.pzp $(OUTDIR)/rgb8Recode.ppm 
	./$(SPZP) compress samples/segment.ppm $(OUTDIR)/segment.pzp
	./$(SPZP) decompress $(OUTDIR)/segment.pzp $(OUTDIR)/segmentRecode.ppm 

install: $(PZP) $(LIBPZP)
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(INCDIR)
	install -m 755 $(PZP) $(DESTDIR)$(BINDIR)/$(PZP)
	install -m 644 $(LIBPZP) $(DESTDIR)$(LIBDIR)/$(LIBPZP)
	install -m 644 pzp.h $(DESTDIR)$(INCDIR)/pzp.h
	ldconfig $(DESTDIR)$(LIBDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PZP)
	rm -f $(DESTDIR)$(LIBDIR)/$(LIBPZP)
	rm -f $(DESTDIR)$(INCDIR)/pzp.h
	ldconfig $(DESTDIR)$(LIBDIR)

debug: all $(OUTDIR)
	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) compress samples/sample.ppm $(OUTDIR)/sample.pzp 2>log1.txt
	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) decompress $(OUTDIR)/sample.pzp $(OUTDIR)/sampleRecode.ppm 2>log2.txt

	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) compress samples/depth16.pnm $(OUTDIR)/depth16.pzp 2>log3.txt
	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) decompress $(OUTDIR)/depth16.pzp $(OUTDIR)/depth16Recode.ppm 2>log4.txt

	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) compress samples/rgb8.pnm $(OUTDIR)/rgb8.pzp 2>log5.txt
	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) decompress $(OUTDIR)/rgb8.pzp $(OUTDIR)/rgb8Recode.ppm 2>log6.txt

	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) compress samples/segment.ppm $(OUTDIR)/segment.pzp 2>log7.txt
	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) decompress $(OUTDIR)/segment.pzp $(OUTDIR)/segmentRecode.ppm 2>log8.txt

