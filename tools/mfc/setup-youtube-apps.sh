#!/bin/sh
# Set up Clapper and Pipeline (YouTube via clapper-enhancers' yt-dlp plugin)
# to play on the MFC hardware decoder. Run as the desktop user; uses sudo for
# the system parts. Takes effect at the next login.
#
# Works around (Alpine, 2026-10):
# - clapper's GL importers link libgstclapper.so, which lives only in the
#   GStreamer plugin directory (no rpath): no video sink, nothing plays.
# - the yt-dlp enhancer is Python: needs libpeas' Python loader.
# - souphttpsrc trips a libsoup thread assertion inside Clapper; prefer curl.
# - Clapper's media-scanner runs a second pipeline on the same item; two
#   adaptivedemux2 libsoup sessions in one process hit the same assertion.
#   Load every enhancer except the media scanner.
set -eu

sudo apk add clapper clapper-enhancers libpeas2-python3 pipeline yt-dlp
sudo ln -sf gstreamer-1.0/libgstclapper.so /usr/lib/libgstclapper.so

d="$HOME/.local/share/clapper-enhancers"
rm -rf "$d"
mkdir -p "$d"
for e in /usr/lib/clapper-0.0/enhancers/*; do
	[ "$(basename "$e")" = media-scanner ] && continue
	ln -s "$e" "$d/"
done

mkdir -p "$HOME/.config/plasma-workspace/env"
cat > "$HOME/.config/plasma-workspace/env/gst-http.sh" <<'EOT'
# YouTube in Clapper / Pipeline: see tools/mfc/setup-youtube-apps.sh
export GST_PLUGIN_FEATURE_RANK="curlhttpsrc:PRIMARY,souphttpsrc:NONE"
export CLAPPER_ENHANCERS_PATH="$HOME/.local/share/clapper-enhancers"
EOT
sync
echo "done; log out and back in"
