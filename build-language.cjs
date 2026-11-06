// Rebuild only the language resource from the user's verified original DLL.
const fs=require('fs'),path=require('path'),crypto=require('crypto');
const sha=b=>crypto.createHash('sha256').update(b).digest('hex');
if(!process.argv[2]) throw Error('Usage: node build-language.cjs <original English ENU.DLL>');
const b=Buffer.from(fs.readFileSync(process.argv[2]));
if(sha(b)!=='2309d5b30560103c7a8b3b97e626877fb89fd85164f82186082dc1c665466e70') throw Error('Original DLL hash mismatch');
const tr=JSON.parse(fs.readFileSync(path.join(__dirname,'translations/zh.json'),'utf8'));
const pe=b.readUInt32LE(60),opt=pe+24,st=opt+b.readUInt16LE(pe+20),n=b.readUInt16LE(pe+6);
const sections=Array.from({length:n},(_,i)=>{const p=st+i*40;return {va:b.readUInt32LE(p+12),size:Math.max(b.readUInt32LE(p+8),b.readUInt32LE(p+16)),raw:b.readUInt32LE(p+20)};});
function offset(rva){const s=sections.find(s=>rva>=s.va&&rva<s.va+s.size);if(!s)throw Error('Unmapped RVA');return s.raw+rva-s.va;}
const resource=offset(b.readUInt32LE(opt+112));
const entries=p=>Array.from({length:b.readUInt16LE(p+12)+b.readUInt16LE(p+14)},(_,i)=>{const at=p+16+i*8;return {id:b.readUInt32LE(at),value:b.readUInt32LE(at+4)};});
const type=entries(resource).find(e=>e.id===6); if(!type||!(type.value&0x80000000))throw Error('String resource directory missing');
const sa=b.readUInt32LE(opt+32),fa=b.readUInt32LE(opt+36),align=(v,a)=>Math.ceil(v/a)*a;
const last=st+40*(n-1),va=align(b.readUInt32LE(last+12)+b.readUInt32LE(last+8),sa),raw=align(b.length,fa);
const chunks=[];let length=0,count=0;
for(const name of entries(resource+(type.value&0x7fffffff))) for(const lang of entries(resource+(name.value&0x7fffffff))) {
 const dataEntry=resource+lang.value,old=offset(b.readUInt32LE(dataEntry)); let cursor=old;const parts=[];
 for(let j=0;j<16;j++) {
  const id=(name.id-1)*16+j,size=b.readUInt16LE(cursor);cursor+=2;
  const en=b.subarray(cursor,cursor+size*2).toString('utf16le');cursor+=size*2;
  const s=tr[id]??en,bytes=Buffer.from(s,'utf16le'),len=Buffer.alloc(2);
  if(bytes.length/2>65535)throw Error('String too long '+id);
  len.writeUInt16LE(bytes.length/2);parts.push(len,bytes);count++;
 }
 const data=Buffer.concat(parts);b.writeUInt32LE(va+length,dataEntry);b.writeUInt32LE(data.length,dataEntry+4);chunks.push(data);length+=data.length;
 const pad=align(length,4)-length;chunks.push(Buffer.alloc(pad));length+=pad;
}
const header=st+40*n;if(header+40>b.readUInt32LE(opt+60))throw Error('No section header space');
b.fill(0,header,header+40);b.write('.zhdata',header,'ascii');b.writeUInt32LE(length,header+8);b.writeUInt32LE(va,header+12);b.writeUInt32LE(align(length,fa),header+16);b.writeUInt32LE(raw,header+20);b.writeUInt32LE(0x40000040,header+36);b.writeUInt16LE(n+1,pe+6);b.writeUInt32LE(align(va+length,sa),opt+56);b.writeUInt32LE(0,opt+64);
const result=Buffer.concat([b,Buffer.alloc(raw-b.length),...chunks,Buffer.alloc(align(length,fa)-length)]);
fs.writeFileSync(path.join(__dirname,'payload/ENU.DLL'),result);
console.log(JSON.stringify({sha256:sha(result),bytes:result.length,resource_slots:count,translated:Object.keys(tr).length}));
