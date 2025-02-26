# rtsp-proxy
RTSP proxy for flaky (wifi) cameras

This code uses FFmpeg libraries to read an RTSP source feed and
transcode its video stream (no audio!) to a destination feed.
The destination server is provided by `mediamtx` or `rtsp-simple-server`
which must be running in advance.

Connection to some cameras may be flaky which can result in
a terminated stream, which in turn makes the viewer application stop.

In this case, this proxy keeps the last correctly decoded frame
from the source and continuously serves it to the destination
feed and reconnects to the source as soon as possible.

The viewer application does not notice anything.

The configuration for `rtsp-proxy` is `/etc/rtsp-proxy.ini`:
```
[feed1]
SourceURL=rtsp://192.168.1.150/Main
SourceDelay=3
DestURL=rtsp://127.0.0.1:8554/my_feed
DestResolution=800x600
DestFPS=24
```

`SourceURL` is the URL for the source feed. It may also be a path
to a video file which is decoded at its own frame rate.

`SourceDelay` is a delay in seconds. The decoding thread waits for
this number of seconds before trying to connect to the source feed.

`DestURL` is the URL for the destination feed.

`DestResolution` is the frame resolution for the destination feed.

`DestFPS` is the frame rate for the destination feed.

The resolution and the frame rate of the source feed may not be
known in advance, so the destination feed parameters have to be
set manually.

(C) 2025 Zoltán Böszörményi <zboszor@gmail.com>
