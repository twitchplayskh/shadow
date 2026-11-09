import pefile, capstone, struct
p=pefile.PE('work/shadow.exe'); base=p.OPTIONAL_HEADER.ImageBase
text=[s for s in p.sections if s.Name.startswith(b'.text')][0]
code=text.get_data(); tva=base+text.VirtualAddress
img=p.get_memory_mapped_image()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
iat={}
for e in p.DIRECTORY_ENTRY_IMPORT:
  for i in e.imports: iat[i.address]=(e.dll.decode()+'!'+(i.name.decode() if i.name else str(i.ordinal)))
def dis(va,n=60):
  off=va-tva; out=[]
  for ins in md.disasm(code[off:off+n*10],va):
    s=ins.op_str
    for a,nm in iat.items():
      if hex(a) in s: s+='   ; '+nm
    out.append('%x %s %s'%(ins.address,ins.mnemonic,s)); n-=1
    if n<=0: break
  print('\n'.join(out))
def find(b):
  r=[];i=code.find(b)
  while i>=0: r.append(tva+i); i=code.find(b,i+1)
  return r
def rd(va,n): return img[va-base:va-base+n]
def callers(t):
    r=[]
    for i in range(len(code)-5):
        if code[i] in (0xe8,0xe9) and tva+i+5+struct.unpack('<i',code[i+1:i+5])[0]==t: r.append(tva+i)
    return r
def xref(g):
    out=[]
    for h in find(struct.pack('<I',g)):
        for back in range(1,9):
            ins=list(md.disasm(code[h-back-tva:h-back-tva+16],h-back,1))
            if ins and ins[0].address+ins[0].size>=h+4 and hex(g) in ins[0].op_str:
                out.append('%x %s %s'%(ins[0].address,ins[0].mnemonic,ins[0].op_str)); break
    return out
