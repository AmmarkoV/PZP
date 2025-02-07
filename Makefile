CC = gcc
CFLAGS = -lzstd -lm
DEBUG_FLAGS = -D_GNU_SOURCE -O0 -g3 -fno-omit-frame-pointer -Wstrict-overflow -fPIE -fPIC

SRC = src/pzp.c
OUTDIR = output
PZP = pzp
DPZP = dpzp

.PHONY: all clean test

all: $(PZP) $(DPZP)

$(PZP): $(SRC)
	$(CC) $(SRC) $(CFLAGS) -o $(PZP)

$(DPZP): $(SRC)
	$(CC) $(SRC) $(DEBUG_FLAGS) $(CFLAGS) -o $(DPZP)

clean:
	rm -rf $(PZP) $(DPZP) $(OUTDIR)/*.pzp $(OUTDIR)/*.ppm log*.txt

$(OUTDIR):
	mkdir -p $(OUTDIR)

test: all $(OUTDIR)
	./$(PZP) compress samples/sample.ppm $(OUTDIR)/sample.pzp
	./$(PZP) decompress $(OUTDIR)/sample.pzp $(OUTDIR)/sampleRecode.ppm
	#diff samples/sample.ppm $(OUTDIR)/sampleRecode.ppm 
	./$(PZP) compress samples/depth16.pnm $(OUTDIR)/depth16.pzp
	./$(PZP) decompress $(OUTDIR)/depth16.pzp $(OUTDIR)/depth16Recode.ppm
	#diff samples/depth16.pnm $(OUTDIR)/depth16Recode.ppm 
	./$(PZP) compress samples/rgb8.pnm $(OUTDIR)/rgb8.pzp
	./$(PZP) decompress $(OUTDIR)/rgb8.pzp $(OUTDIR)/rgb8Recode.ppm
	#diff samples/rgb8.pnm $(OUTDIR)/rgb8Recode.ppm
	./$(PZP) compress samples/segment.ppm $(OUTDIR)/segment.pzp
	./$(PZP) decompress $(OUTDIR)/segment.pzp $(OUTDIR)/segmentRecode.ppm
	#diff samples/segment.pnm $(OUTDIR)/segmentRecode.ppm


debug: all $(OUTDIR)
	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) compress samples/sample.ppm $(OUTDIR)/sample.pzp 2>log1.txt
	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) decompress $(OUTDIR)/sample.pzp $(OUTDIR)/sampleRecode.ppm 2>log2.txt

	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) compress samples/depth16.pnm $(OUTDIR)/depth16.pzp 2>log3.txt
	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) decompress $(OUTDIR)/depth16.pzp $(OUTDIR)/depth16Recode.ppm 2>log4.txt

	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) compress samples/rgb8.pnm $(OUTDIR)/rgb8.pzp 2>log5.txt
	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) decompress $(OUTDIR)/rgb8.pzp $(OUTDIR)/rgb8Recode.ppm 2>log6.txt

	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) compress samples/segment.ppm $(OUTDIR)/segment.pzp 2>log7.txt
	valgrind --tool=memcheck --leak-check=yes --show-reachable=yes --track-origins=yes --num-callers=20 --track-fds=yes ./$(DPZP) decompress $(OUTDIR)/segment.pzp $(OUTDIR)/segmentRecode.ppm 2>log8.txt
