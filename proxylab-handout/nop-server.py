#!/usr/bin/env python3

# nop-server.py - This is a server that we use to create head-of-line
#                 blocking for the concurrency test. It accepts a
#                 connection, and then spins forever.
#
# usage: nop-server.py <port>                
#
import socket
import sys
import time

if len(sys.argv) < 2:
    sys.exit(1)

# create an INET, STREAMing socket
serversocket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
serversocket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
serversocket.bind(('', int(sys.argv[1])))
serversocket.listen(5)

while True:
    channel, details = serversocket.accept()
    while True:
        time.sleep(1)