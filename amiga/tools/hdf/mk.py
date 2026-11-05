import struct, sys, subprocess
stub = open('/tmp/hdfw/stub.bin','rb').read()
pay = open('/home/user/excitebike/amiga/platform/build/exb.pkg','rb').read()
pay += b'\0' * (-len(pay) % 4)
stub = stub[:-4] + struct.pack('>I', len(pay))          # payload_len is the last long of the stub
code = stub + pay
words = len(code) // 4
hunk = struct.pack('>IIIIII', 0x3F3, 0, 1, 0, 0, words) + struct.pack('>II', 0x3E9, words) + code + struct.pack('>I', 0x3F2)
open('/tmp/hdfw/Excitebike','wb').write(hunk)
open('/tmp/hdfw/Startup-Sequence','w').write('Excitebike\n')
