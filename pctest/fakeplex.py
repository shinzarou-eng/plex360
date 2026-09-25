# Faux serveur Plex sur :8080 — test de connectivite console -> PC
# Sert /library/sections avec une fausse bibliotheque "TEST-FTP-OK"
import socket

xml = (b'<?xml version="1.0" encoding="UTF-8"?>'
       b'<MediaContainer size="1" title1="Plex Library">'
       b'<Directory key="1" title="TEST-CONSOLE-OK" type="movie"/>'
       b'</MediaContainer>')

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('0.0.0.0', 8080))
srv.listen(5)
print('FakePlex ecoute sur 0.0.0.0:8080 — en attente de la console...', flush=True)
while True:
    c, addr = srv.accept()
    print('Connexion de', addr, flush=True)
    try:
        req = c.recv(4096)
        print('  requete:', req.split(b'\r\n')[0], flush=True)
        body = xml
        resp = (b'HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\n'
                b'Content-Length: %d\r\nConnection: close\r\n\r\n' % len(body)) + body
        c.sendall(resp)
    except Exception as e:
        print('  err', e)
    c.close()
