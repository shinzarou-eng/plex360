from ftplib import FTP
import os

SRC = r'C:\Users\fbdyl\Downloads\xbox\build\Release\bin'
DEST = '/Hdd1/Homebrew/Plex360'

f = FTP('192.168.129.243', timeout=30)
f.login('xboxftp', 'aaaaaa')

# cree les dossiers un par un (mkd echoue si deja present)
path = ''
for part in DEST.split('/'):
    if not part:
        continue
    path += '/' + part
    try:
        f.mkd(path)
    except Exception:
        pass
f.cwd(DEST)

for fn in ['Plex360.xex', 'config.ini']:
    local = os.path.join(SRC, fn)
    with open(local, 'rb') as fp:
        f.storbinary('STOR ' + fn, fp)
    print('uploaded', fn, os.path.getsize(local), 'bytes')

f.retrlines('LIST')
f.quit()
