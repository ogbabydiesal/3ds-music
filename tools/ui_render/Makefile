# Render SYNTH page screenshots on macOS from the app's real UI code.
#   make -C tools/ui_render
SRC  := ../../sequencer/source
OUT  := ../../docs/screenshots
BIN  := build/render

$(OUT)/synth-1-starter.png: $(BIN)
	@mkdir -p $(OUT)
	./$(BIN) $(OUT)

$(BIN): render.c $(SRC)/page_synth.c $(SRC)/synth.c $(SRC)/settings.c $(SRC)/*.h shim/*.h
	@mkdir -p build
	cc -O2 -w -DSYN_HOST_AUDIO_OK=1 -Ishim -I$(SRC) -I../../common render.c $(SRC)/page_synth.c $(SRC)/synth.c $(SRC)/settings.c \
	   -framework CoreGraphics -framework CoreText -framework ImageIO -framework CoreFoundation -lm -o $@

clean:
	rm -rf build

.PHONY: clean
