from pathlib import Path
from uefi_firmware.uefi import FirmwareVolume,FirmwareFile,FirmwareFileSystemSection
import uuid,re,json
import argparse
ap=argparse.ArgumentParser(description='Extract local UMI XBL modules using the pinned Kona INF GUIDs')
ap.add_argument('xbl', type=Path)
args=ap.parse_args()
r=Path(__file__).resolve().parents[3]
b=args.xbl.read_bytes();fv=FirmwareVolume(b[b.index(b'_FVH')-40:]);assert fv.process()
files={}
def walk(o):
 if isinstance(o,FirmwareFile):files[str(uuid.UUID(bytes_le=o.guid))]=o
 for c in getattr(o,'objects',[]):
  if hasattr(c,'__dict__'):walk(c)
walk(fv)
def sections(o):
 out=[]
 for c in getattr(o,'objects',[]):
  if not hasattr(c,'__dict__'):continue
  if isinstance(c,FirmwareFileSystemSection) and c.type in (0x10,0x13,0x15,0x19):out.append(c)
  else:out.extend(sections(c))
 return out
out=r/'work/umi-firmware/extracted';out.mkdir(exist_ok=True)
index={}
for guid,f in files.items():
 ss=sections(f);name=next((x.name for x in ss if x.type==0x15),guid)
 index[name]={'guid':guid,'type':f.type,'sections':[(x.type,len(x.data)) for x in ss]}
 for sec in ss:
  if sec.type==0x19:
   d=out/'RawFiles';d.mkdir(exist_ok=True);(d/name).write_bytes(sec.data)
  elif sec.type==0x10:
   d=out/'Drivers';d.mkdir(exist_ok=True);(d/(name+'.efi')).write_bytes(sec.data)
(out/'index.json').write_text(json.dumps(index,indent=2))
missing=[]
for inf in (r/'Mu-Silicium/Binaries/alioth').rglob('*.inf'):
 text=inf.read_text();m=re.search(r'FILE_GUID\s*=\s*([\w-]+)',text)
 if not m:continue
 guid=m[1].lower()
 if guid not in files:missing.append(str(inf.relative_to(r/'Mu-Silicium/Binaries/alioth')));continue
 ss=sections(files[guid]);target=r/'Mu-Silicium/Binaries/umi'/inf.relative_to(r/'Mu-Silicium/Binaries/alioth');target.parent.mkdir(parents=True,exist_ok=True)
 target.write_text(text)
 for kind,filename in re.findall(r'^\s*(PE32|DXE_DEPEX|RAW)\|([^|]+)',text,re.M):
  code={'PE32':0x10,'DXE_DEPEX':0x13,'RAW':0x19}[kind];matches=[s for s in ss if s.type==code]
  if len(matches)!=1:missing.append(f'{target.name}: {kind}={len(matches)}');continue
  (target.parent/filename).write_bytes(matches[0].data)
print('files',len(files),'modules',len(index),'missing',missing)
if missing: raise SystemExit('Firmware modules do not match the pinned Kona templates')
# Verified against the same Kona TzDxeLA revision: only this flag and
# a build-letter byte differ from Silicium alioth. Set the TZ app-loaded flag.
p=r/'Mu-Silicium/Binaries/umi/QcomPkg/Drivers/TzDxe/TzDxeLA.patched.efi'
b=bytearray(p.read_bytes())
import hashlib
if hashlib.sha256(b).hexdigest() != 'a87cb38bfa7708ae9c801abf63e9c824fd873d9dfabeab2d2456aac265c7a272':
 raise SystemExit('Unexpected TZ driver revision; no patch applied')
b[0xd469]=1
p.write_bytes(b)
import shutil
shutil.copytree(out/'RawFiles',r/'Mu-Silicium/Binaries/umi/RawFiles',dirs_exist_ok=True)
