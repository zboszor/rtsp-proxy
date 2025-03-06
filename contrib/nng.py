#!/usr/bin/python3

from pynng import Sub0
# This is for using the POSIX shared memory segment
# for the image data. The segment name to attach to is
# sent by rtsp-proxy
from multiprocessing import shared_memory as shm

sock = Sub0()
sock.dial('tcp://127.0.0.1:9999')
sock.subscribe("")

while True:
    msg = sock.recv()
    print(msg)
