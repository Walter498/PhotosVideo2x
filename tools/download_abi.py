import struct,pathlib,json
root=pathlib.Path('/var/minis/workspace/dsc');out={}
for image,classes in {'PhotosUICore':['PXDisplayAssetVideoContentDeliveryStrategy','PXDisplayAssetVideoContentProviderRequest','PXDisplayAssetVideoContentProvider','PXVideoContentProvider','PXVideoContentProviderLoadingResult','PXVideoSession'],'PhotosUIPrivate':['PUBrowsingVideoPlayer','PUVideoTileViewController']}.items():
 d=(root/'extracted'/image).read_bytes();segs=[];pos=32
 for _ in range(struct.unpack_from('<I',d,16)[0]):
  cmd,sz=struct.unpack_from('<II',d,pos)
  if cmd==25:segs.append(struct.unpack_from('<QQQQ',d,pos+24))
  pos+=sz
 def read(a,n):
  for vm,vs,off,fs in segs:
   if vm<=a and a+n<=vm+fs:return d[off+a-vm:off+a-vm+n]
  raise ValueError(hex(a))
 def u(a):return struct.unpack('<Q',read(a,8))[0]
 def s(a):return struct.unpack('<i',read(a,4))[0]
 def text(a):return read(a,128).split(b'\0')[0].decode(errors='replace')
 syms={}
 for line in (root/'index'/f'{image}.nm').read_text().splitlines():
  parts=line.split(maxsplit=2)
  if len(parts)==3:
   try:syms[parts[2]]=int(parts[0],16)
   except:pass
 reverse={v:k for k,v in syms.items() if k.startswith('-[')}
 for c in classes:
  a=syms['__OBJC_$_INSTANCE_METHODS_'+c];f,n=struct.unpack('<II',read(a,8));size=(f&0xffff)&~3;rows=[]
  for i in range(n):
   e=a+8+i*size
   if f&0x80000000:
    nv=e+s(e);tp=e+4+s(e+4);imp=e+8+s(e+8)
    if not f&0x40000000:nv=u(nv)
   else:nv=u(e);tp=u(e+8);imp=u(e+16)
   k=reverse.get(imp,'');names=[k.split(' ',1)[1][:-1]] if k.startswith('-['+c+' ') else []
   try:rows.append({'sel':names[0] if names else 'unknown','type':text(tp),'imp':hex(imp)})
   except:pass
  out[c]=rows
 if image=='PhotosUICore':
  print('priority-intents',struct.unpack('<5Q',read(0x1ac365b08,40)))
  print('priority-download',struct.unpack('<5Q',read(0x1ac365b30,40)))
print(json.dumps(out,indent=2))
