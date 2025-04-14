# rtsp-proxy
RTSP proxy for flaky (wifi) cameras

This code uses FFmpeg libraries to read an RTSP source feed and
transcode its video stream (no audio!) to a destination feed.

The destination server is provided by `mediamtx` or `rtsp-simple-server`
which must be running in advance.

Alternatively, the decoded frame data can be put into shared memory.
This way, the consumer of image data can use it directly.

Connection to some cameras may be flaky which can result in
a terminated stream, which in turn makes the viewer application stop.

In this case, this proxy keeps the last correctly decoded frame
from the source and continuously serves it to the destination
feed and reconnects to the source as soon as possible.

The viewer application does not notice anything.

The INI configuration for `rtsp-proxy` is `/etc/rtsp-proxy.ini` by default:
```
[feed1]
SourceURL=rtsp://192.168.1.150/Main
DecodeHWAccel=vaapi
SourceDelay=3
DestURL=rtsp://127.0.0.1:8554/my_feed
DestResolution=800x600
DestFPS=24
```

The configuration file usage is activated with command line option
`-i <section>` or `--ini <section>` where `[section]` is in the
ini file.

The configuration file path can be specified with `-c <file>` or
`--config <file>`.

`SourceURL` is the URL for the source feed. It may also be a path
to a video file which is decoded at its own frame rate. It is
equivalent to command line option `-s <URL>` and `--src <URL>`.

`DecodeHWAccel` is optional and indicates the hardware acceleration
type to be used to decode the source video. It is equivalent to
the command line option `-a <accel>` or `--src-accel <accel>`.

`SourceDelay` is a delay in seconds. The decoding thread waits for
this number of seconds before trying to connect to the source feed.
It is equivalent to the command line option `-w <N>` or
`--src-delay <N>`.

`DestURL` is the URL for the destination feed. It is equivalent
to the command line option `-d <URL>` or `--dst <URL>`.

When `DestURL` is prefixed with `nng:`, then a valid NNG URI has
to be specified for the PUB-SUB protocol. See
https://github.com/nanomsg/nng. In this case, the frame data is
decoded into POSIX shared memory, and an NNG message is sent to
the subscribers in this format:
```
shared-memory-segment-name width height stride src-connected
```

The shared memory segment name is something like `/image-data-XXXX-Y`.
The frame data parameters (width, height, stride) are followed by
a boolean flag indicating that the `rtsp-proxy` is connected to the
source stream.

`DestResolution` is the frame resolution for the destination feed.
It is equivalent to the command line option `-r WxH` or `--res WxH`.

`DestFPS` is the frame rate for the destination feed. It is
equivalent to the command line option `-f <FPS>` or `--fps <FPS>`

The resolution and the frame rate of the source feed may not be
known in advance, so the destination feed parameters have to be
set manually.

Build tested with FFmpeg 7.0.x and 7.1.

(C) 2025 Zoltán Böszörményi <zboszor@gmail.com>
