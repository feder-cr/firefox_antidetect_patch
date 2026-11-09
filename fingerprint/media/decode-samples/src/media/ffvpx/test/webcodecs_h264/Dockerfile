# What Firefox needs to start under Xvfb and nothing that decodes media: the
# test is that H.264 decodes with the ffvpx inside the build and no libavcodec
# on the system.
FROM debian:12
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates python3 xvfb xauth \
      libgtk-3-0 libdbus-glib-1-2 libx11-xcb1 libxtst6 libasound2 \
 && rm -rf /var/lib/apt/lists/* \
 && ! ldconfig -p | grep -q 'libavcodec\|libavutil'
