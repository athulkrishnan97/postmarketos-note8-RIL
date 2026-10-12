#!/bin/sh
# MFC (hardware video codec) checks, run as a user in the video group.
#   mfctest.sh info               - driver, formats, GStreamer elements
#   mfctest.sh dec <file>         - H.264 decode through GStreamer to fakesink
#   mfctest.sh speed <file>       - H.264 decode through ffmpeg h264_v4l2m2m
#   mfctest.sh md5 <file> <out>   - decode to raw I420 and md5 it (compare
#                                   with: ffmpeg -i <file> -f rawvideo
#                                   -pix_fmt yuv420p - | md5sum)
#   mfctest.sh enc <out.mp4>      - 5 s 720p H.264 hardware encode (input NV21)
# GStreamer copies every decoded frame out of the (uncached) MFC buffers when
# nothing downstream takes DMA-BUFs, so "dec" is CPU-bound; "speed" is not.
set -u
cmd=${1:-}; shift || true
case "$cmd" in
info)
	v4l2-ctl -d /dev/video0 --info --list-formats-out --list-formats
	gst-inspect-1.0 video4linux2 | grep -i dec ;;
dec)
	time gst-launch-1.0 -q filesrc location="$1" ! parsebin ! v4l2h264dec ! fakesink sync=false ;;
speed)
	ffmpeg -hide_banner -nostdin -c:v h264_v4l2m2m -i "$1" -f null - 2>&1 |
		tr '\r' '\n' | grep frame= | tail -1 ;;
enc)
	ffmpeg -hide_banner -nostdin -loglevel warning -f lavfi \
		-i testsrc2=size=1280x720:rate=30 -t 5 -pix_fmt nv21 \
		-c:v h264_v4l2m2m -b:v 4M -y "$1" ;;
md5)
	gst-launch-1.0 -q filesrc location="$1" ! parsebin ! v4l2h264dec ! videoconvert ! \
		video/x-raw,format=I420 ! filesink location="$2" && md5sum "$2" ;;
*) echo "usage: $0 info|dec|speed|enc|md5"; exit 1 ;;
esac
