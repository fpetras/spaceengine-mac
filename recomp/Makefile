# Builds the machine-translated functions and checks them against the original.
#
# -ffp-contract=off: the original has no fused multiply-add, so none may be
# introduced. -fno-strict-aliasing and -fwrapv: translated code reinterprets
# memory freely and relies on wrap-around arithmetic.
PY     := ../tools/.venv/bin/python
# The player's own copy of the game, found where Steam puts it (or named in
# ~/Library/Application Support/SpaceEngine-native/game-path.txt).
EXE    ?= $(shell python3 ../tools/find_game.py)
# Tables about the executable, derived from it on the first build: functions,
# imports, the FFmpeg functions it asks for. Nothing about the game is kept in
# the repository.
ANALYSIS := ../analysis
CFLAGS := -O2 -ffp-contract=off -fno-strict-aliasing -fwrapv -Wall -Wextra \
          -Wno-unused-label -Wno-unused-but-set-variable -Wno-parentheses-equality -Iruntime

SPIKE := 0x140440660 0x140440790 0x1401e3be0 0x1401e3210 0x1401eba10 0x1401eeb30 0x1401ef090 \
         0x1401eecc0 0x1401eea40 0x1401ee8d0 0x1401ef260 0x1401eefa0 0x1401eee30

all: build/driver

gen/spike.c: translate.py $(ANALYSIS)/functions.tsv $(ANALYSIS)/imports.tsv
	@mkdir -p gen
	$(PY) translate.py emit $@ $(SPIKE)

build/%.o: runtime/%.c runtime/x86.h runtime/runtime.h
	@mkdir -p build
	$(CC) -std=c11 $(CFLAGS) -c $< -o $@

build/spike.o: gen/spike.c runtime/x86.h
	@mkdir -p build
	$(CC) -std=c11 $(CFLAGS) -c $< -o $@

# reserve.s holds the image's address range inside this program's own image.
RESERVE := -Wl,-segaddr,__SEIMAGE,0x130000000

build/driver: tests/driver.cpp build/spike.o build/runtime.o build/hle.o runtime/reserve.s
	$(CXX) -std=c++17 $(CFLAGS) $(RESERVE) $^ -o $@

# The same 10,102 cases the hand-written reconstruction is checked with.
$(ANALYSIS)/functions.tsv $(ANALYSIS)/imports.tsv: ../tools/pe_analyze.py
	@mkdir -p $(ANALYSIS)
	python3 ../tools/pe_analyze.py "$(EXE)" - $(ANALYSIS)

test: build/driver
	SE_DRIVER="$(CURDIR)/build/driver" $(PY) tests/difftest.py

# ---------------------------------------------------------------- whole program
# `make program` translates every function and compiles the result. Run it
# with -j; the parts are independent. PROG_OPT trades build time for speed of
# the translated code.
PROG_OPT ?= -O1
PARTS = $(patsubst gen/prog/%.c,build/prog/%.o,$(wildcard gen/prog/*.c))

gen/prog/functions.h: translate.py hle.txt $(ANALYSIS)/functions.tsv $(ANALYSIS)/imports.tsv
	$(PY) translate.py program gen/prog

build/prog/%.o: gen/prog/%.c gen/prog/functions.h runtime/x86.h
	@mkdir -p build/prog
	$(CC) -std=c11 $(PROG_OPT) $(filter-out -O2,$(CFLAGS)) -Igen/prog -c $< -o $@

build/libprogram.a: $(PARTS)
	rm -f $@
	ar rcs $@ $^

program: gen/prog/functions.h
	$(MAKE) build/libprogram.a

# The spike's test driver linked against the whole translated program.
build/driver_full: tests/driver.cpp build/libprogram.a build/runtime.o build/hle.o runtime/reserve.s
	$(CXX) -std=c++17 $(CFLAGS) $(RESERVE) $^ -o $@

# ------------------------------------------------------------------ the game
# build/se is the translated program plus the native host that stands in for
# Windows. SE_TRACE=1 prints every Windows call; SE_VERBOSE=1 prints how many
# imports are implemented.
HOST_SRC := $(wildcard host/*.c)
HOST_OBJ := $(HOST_SRC:host/%.c=build/host/%.o) build/host/gl_thunks.o build/host/ff_thunks.o
# SDL3 (window, input, OpenGL context) comes from Homebrew.
SDL_PREFIX ?= /opt/homebrew
HOST_CFLAGS := -std=c11 -D_DARWIN_C_SOURCE $(CFLAGS) -Igen -Ihost -I$(SDL_PREFIX)/include
HOST_LIBS := -L$(SDL_PREFIX)/lib -lSDL3 -lpng16 -ltiff -lvorbisfile -lvorbis -logg -lz -lobjc -framework OpenGL

gen/import_slots.h: $(ANALYSIS)/imports.tsv
	@mkdir -p gen
	awk -F'\t' 'BEGIN { print "// Generated from analysis/imports.tsv, sorted by slot address."; \
	    print "static const struct { uint64_t slot; const char *dll, *name; } import_slots[] = {" } \
	    { split($$2, p, "!"); printf "    {%sull, \"%s\", \"%s\"},\n", $$1, p[1], p[2] } \
	    END { print "};" }' $< > $@

gen/gl_thunks.c: gen_gl.py
	@mkdir -p gen
	$(PY) gen_gl.py $@

build/host/%.o: host/%.c $(wildcard host/*.h) runtime/x86.h runtime/runtime.h gen/import_slots.h
	@mkdir -p build/host
	$(CC) $(HOST_CFLAGS) -c $< -o $@

build/host/gl_thunks.o: gen/gl_thunks.c host/gl.h host/host.h runtime/x86.h
	@mkdir -p build/host
	$(CC) $(HOST_CFLAGS) -c $< -o $@

# FFmpeg for the video recorder: built into third_party/ffmpeg by
# ../tools/build_ffmpeg.sh, loaded with dlopen when the game asks for it.
FFMPEG := $(CURDIR)/third_party/ffmpeg
FF_CFLAGS := -I$(FFMPEG)/include -DFFMPEG_LIB_DIR='"$(FFMPEG)/lib"' -Wno-deprecated-declarations

$(FFMPEG)/lib/libavcodec.62.dylib:
	../tools/build_ffmpeg.sh

$(ANALYSIS)/ffmpeg_imports.tsv: ../tools/ffmpeg_names.py $(FFMPEG)/lib/libavcodec.62.dylib
	@mkdir -p $(ANALYSIS)
	$(PY) ../tools/ffmpeg_names.py "$(EXE)" $(FFMPEG)/lib $@

gen/ff_thunks.c: gen_ff.py $(ANALYSIS)/ffmpeg_imports.tsv $(FFMPEG)/lib/libavcodec.62.dylib
	@mkdir -p gen
	$(PY) gen_ff.py $(FFMPEG)/include $@

build/host/ff_thunks.o: gen/ff_thunks.c host/ffmpeg.h host/gl.h host/host.h runtime/x86.h
	@mkdir -p build/host
	$(CC) $(FF_CFLAGS) $(HOST_CFLAGS) -c $< -o $@

build/host/ffmpeg.o: host/ffmpeg.c host/ffmpeg.h host/host.h $(FFMPEG)/lib/libavcodec.62.dylib
	@mkdir -p build/host
	$(CC) $(FF_CFLAGS) $(HOST_CFLAGS) -c $< -o $@

build/se: $(HOST_OBJ) build/libprogram.a build/runtime.o build/hle.o runtime/reserve.s
	$(CC) $(RESERVE) $^ $(HOST_LIBS) -o $@

run: build/se
	./build/se "$(EXE)"

# ------------------------------------------------------------------- the app
# build/SpaceEngine.app is build/se in a bundle that can be double-clicked. It
# finds the game in Steam's library by itself and logs to
# ~/Library/Logs/SpaceEngine-native.log. `make install` puts it in
# ~/Applications. Like everything built here it is for this machine only: it
# is the game's translated code.
APP := build/SpaceEngine.app

app: build/se app/Info.plist ../tools/icon.py
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	cp build/se $(APP)/Contents/MacOS/SpaceEngine
	cp app/Info.plist $(APP)/Contents/Info.plist
	$(PY) ../tools/icon.py "$(EXE)" $(APP)/Contents/Resources/SpaceEngine.icns
	codesign --force --sign - $(APP)

install: app
	mkdir -p "$(HOME)/Applications"
	rm -rf "$(HOME)/Applications/SpaceEngine.app"
	cp -R $(APP) "$(HOME)/Applications/SpaceEngine.app"

test-full: build/driver_full
	SE_DRIVER="$(CURDIR)/build/driver_full" $(PY) tests/difftest.py

coverage:
	$(PY) translate.py coverage

clean:
	rm -rf build gen

.PHONY: all test test-full program run app install coverage clean
