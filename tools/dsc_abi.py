import struct,json,pathlib
root=pathlib.Path('/var/minis/workspace/dsc')
result={}
for image,classes in {'PhotosUIPrivate':['PUTileViewController','PUVideoTileViewController'],'PhotosPlayer':['ISWrappedAVPlayer']}.items():
 data=(root/'extracted'/image).read_bytes(); segs=[]; pos=32
 for _ in range(struct.unpack_from('<I',data,16)[0]):
  cmd,size=struct.unpack_from('<II',data,pos)
  if cmd==0x19:
   vm,vs,off,fs=struct.unpack_from('<QQQQ',data,pos+24); segs.append((vm,fs,off))
  pos+=size
 def read(a,n):
  for vm,fs,off in segs:
   if vm<=a and a+n<=vm+fs: return data[off+a-vm:off+a-vm+n]
  raise ValueError(hex(a))
 def u(a): return struct.unpack('<Q',read(a,8))[0]
 def s(a): return struct.unpack('<i',read(a,4))[0]
 def text(a): return read(a,160).split(b'\0')[0].decode(errors='replace')
 symbols={}
 for line in (root/'index'/f'{image}.nm').read_text().splitlines():
  parts=line.split(' ',2)
  if len(parts)==3:
   try: symbols[parts[2]]=int(parts[0],16)
   except ValueError: pass
 for cls in classes:
  a=symbols[f'__OBJC_$_INSTANCE_METHODS_{cls}']; flags,count=struct.unpack('<II',read(a,8)); stride=flags&0xffff
  methods=[]
  for i in range(count):
   e=a+8+i*stride
   if flags&0x80000000:
    typ=e+4+s(e+4); imp=e+8+s(e+8)
   else: typ=u(e+8); imp=u(e+16)
   names=[k for k,v in symbols.items() if v==imp and k.startswith('-[')]
   if names:
    try: methods.append({'name':names[0],'type':text(typ),'imp':hex(imp)})
    except ValueError: pass
  result[cls]=methods
print(json.dumps(result,indent=2))
