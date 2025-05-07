#!/usr/bin/python3

from pynng import Sub0
import posix_ipc
import mmap
import os

sock = Sub0()
sock.dial('tcp://127.0.0.1:9999')
sock.subscribe("")

shmaps = {}
mmaps = {}

frames = 0

while frames < 50:
    frames += 1
    msg = sock.recv()
    msg = msg.decode('ascii')
    x = msg.split()
    print(frames)
    print(x)
    size = int(x[2]) * int(x[3])
    print("picture data size: %d" % size)

    shmname = x[0]
    if shmname not in shmaps.keys():
        shmaps[shmname] = posix_ipc.SharedMemory(shmname, flags=0, read_only=True)
        mmaps[shmname] = mmap.mmap(shmaps[x[0]].fd, size, mmap.MAP_SHARED, mmap.PROT_READ)

    mmaps[shmname].seek(0, os.SEEK_SET)

    print("picture data size: %d" % shmaps[shmname].size)

    filename = "raw-%03d.rgba" % frames
    if os.path.exists(filename):
        os.remove(filename)
    f = open(filename, "wb")

    print("mmap size: %d" % mmaps[shmname].size())
    f.write(mmaps[shmname].read(size))
    f.close()

print(shmaps)

for map in shmaps.keys():
    mmaps[map].close()

print("Generated 50 RGBA raw image data.")
print("Execute e.g. the below to convert to viewable image:")
print("magick -size %sx%s -depth 8 raw-050.rgba raw-050.png" % (x[1], x[2]))
