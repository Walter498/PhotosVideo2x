import sys,struct,pathlib,hashlib
from repack import ar_read
from inspect_deb import open_member
p=sys.argv[1]
for name,body in ar_read(p):
 if name.startswith('data.tar'):
  tf=open_member(name,body)
  entries=[m for m in tf.getmembers() if m.name.endswith('.dylib')]
  for m in entries:
   d=tf.extractfile(m).read()
   pathlib.Path('/tmp/PV2-artifact.dylib').write_bytes(d)
   magic=struct.unpack_from('>I',d)[0]
   if magic==0xcafebabe:
    count=struct.unpack_from('>I',d,4)[0]; ranges=[struct.unpack_from('>IIIII',d,8+i*20)[2:4] for i in range(count)]
   else:ranges=[(0,len(d))]
   for off,size in ranges:
    assert struct.unpack_from('<I',d,off)[0]==0xfeedfacf
    cpu,sub,ncmds=struct.unpack_from('<III',d,off+4)[0],struct.unpack_from('<I',d,off+8)[0],struct.unpack_from('<I',d,off+16)[0]
    pos=off+32; signed=False
    for _ in range(ncmds):
     cmd,sz=struct.unpack_from('<II',d,pos)
     if cmd==0x1d:
      sigoff,sigsize=struct.unpack_from('<II',d,pos+8); signed=bool(sigsize and sigoff+sigsize<=size)
     pos+=sz
    print('slice',hex(cpu),hex(sub),'LC_CODE_SIGNATURE',signed)
    assert signed
    if sub&0xffffff==2:
     assert sub&0x80000000
     pathlib.Path('/tmp/PV2-artifact.arm64e.dylib').write_bytes(d[off:off+size])
     print('PASS: genuine arm64e ABI slice')
print('deb SHA256',hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest())
