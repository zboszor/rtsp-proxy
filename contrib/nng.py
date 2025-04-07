#!/usr/bin/python3

from pynng import Sub0
# This is for using the POSIX shared memory segment
# for the image data. The segment name to attach to is
# sent by rtsp-proxy
from multiprocessing import shared_memory as shm
import os

sock = Sub0()
sock.dial('tcp://127.0.0.1:9999')
sock.subscribe("")

shmaps = {}

frames = 0

while frames < 50:
    frames += 1
    msg = sock.recv()
    msg = msg.decode('ascii')
    x = msg.split()
    print(frames)
    print(x)
    if x[0] not in shmaps:
        shmname = x[0].lstrip('/')
        # Use track=False to let the server remove its own memory block
        # when it exits. Otherwise this client will remove the shared
        # memory block, # making the previously shared memory private
        # to the server.
        shmaps[x[0]] = shm.SharedMemory(name=shmname, track=False)

    filename = "raw-%03d.rgba" % frames
    if os.path.exists(filename):
        os.remove(filename)
    f = open(filename, "wb")
    f.write(shmaps[x[0]].buf)
    f.close()

print(shmaps)

for map in shmaps.keys():
    shmaps[map].close()

print("Generated 50 RGBA raw image data.")
print("Execute e.g. the below to convert to viewable image:")
print("magick -size %sx%s -depth 8 raw-050.rgba raw-050.png" % (x[1], x[2]))
