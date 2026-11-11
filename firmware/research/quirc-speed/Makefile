# Host bench for quirc's identify stages (bench.c). The quirc here is the
# firmware's vendored copy, built with the firmware's maths flags.
QUIRC   := ../../components/quirc/lib
QSRC    := $(QUIRC)/quirc.c $(QUIRC)/identify.c $(QUIRC)/decode.c $(QUIRC)/version_db.c
CFLAGS  ?= -O2 -Wall
QFLAGS  := -I$(QUIRC) -DQUIRC_FLOAT_TYPE=float -DQUIRC_USE_TGMATH -DQUIRC_STAGE_TIMING -DQUIRC_COUNT_MAPS
SETS    := none v5 v6 v8
FRAMES  ?= 60

all: bench bench-fast

bench: bench.c $(QSRC) $(QUIRC)/quirc_stages.h
	$(CC) $(CFLAGS) $(QFLAGS) -o $@ bench.c $(QSRC) -lm

bench-fast: bench.c $(QSRC) $(QUIRC)/quirc_stages.h
	$(CC) $(CFLAGS) $(QFLAGS) -DQUIRC_FAST_PERSPECTIVE -o $@ bench.c $(QSRC) -lm

%.gray: gen-frames.py
	python3 gen-frames.py $* $@ $(FRAMES)

run: all $(SETS:=.gray)
	for s in $(SETS); do ./bench $$s.gray 10; ./bench-fast $$s.gray 10; done
	for s in $(SETS); do ./bench -v $$s.gray 1 > $$s.base.txt; \
	  ./bench-fast -v $$s.gray 1 > $$s.fast.txt; \
	  cmp -s $$s.base.txt $$s.fast.txt && echo "$$s: identical results" || \
	  { echo "$$s: RESULTS DIFFER"; diff $$s.base.txt $$s.fast.txt | head; }; done

clean:
	rm -f bench bench-fast *.gray *.txt

.PHONY: all run clean
