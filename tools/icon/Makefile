# Regenerates the nodeTrix icon: sequencer/icon.png (48x48) and a large preview.
OUT := ../../sequencer/icon.png
all:
	cc -O1 make_icon.c -framework ApplicationServices -framework ImageIO -o /tmp/make_icon_$$$$ && /tmp/make_icon_$$$$ $(OUT) ../../docs/screenshots/icon-nodetrix.png; rm -f /tmp/make_icon_*
