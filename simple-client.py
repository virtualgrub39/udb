import socket

s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect("/tmp/udb.sock")

try:
    while True:
        data = (input("> ") + "\r\n").encode("ASCII")
        s.send(data)
        response = str(s.recv(256))
        print("< " + response)
except KeyboardInterrupt:
    pass
finally:
    s.close()
