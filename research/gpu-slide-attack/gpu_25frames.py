#!/usr/bin/env python3
# Scan COMPLET (tous les 65536 k1 x tous les alpha) sur NOS 25 trames reelles, sur GPU.
# 1 thread = 1 k1, boucle alpha interne, match O(N^2) (N=25 -> trivial). Finit ce que
# le CPU n'a pas termine. Resultat attendu : aucune cle (25 trames << 65536).
import numpy as np, cupy as cp, struct, time
SRC=r'''
#define NLF 0x3A5C742Eu
__device__ __forceinline__ unsigned g5u(unsigned long long x,int a,int b,int c,int d,int e){
  return ((unsigned)((x>>a)&1))|(((unsigned)((x>>b)&1))<<1)|(((unsigned)((x>>c)&1))<<2)
        |(((unsigned)((x>>d)&1))<<3)|(((unsigned)((x>>e)&1))<<4);}
__device__ unsigned enc16(unsigned data,unsigned key){ unsigned x=data;
  for(int r=0;r<16;r++){ unsigned f=((x>>0)&1)^((x>>16)&1)^((key>>r)&1)^((NLF>>g5u(x,1,9,20,26,31))&1); x=(x>>1)^(f<<31);} return x;}
__device__ unsigned dec16(unsigned data,unsigned key){ unsigned x=data;
  for(int r=0;r<16;r++){ unsigned f=((x>>31)&1)^((x>>15)&1)^((key>>(15-r))&1)^((NLF>>g5u(x,0,8,19,25,30))&1); x=(x<<1)^f;} return x;}
__device__ unsigned extract16(unsigned long long data){ unsigned key=0;
  for(int r=0;r<16;r++){ unsigned b=((unsigned)((data>>r)&1))^((unsigned)((data>>(r+32))&1))^((unsigned)((data>>(r+16))&1))
        ^((NLF>>g5u(data,1+r,9+r,20+r,26+r,31+r))&1); key|=(b<<r);} return key;}
__device__ unsigned enc528(unsigned data,unsigned long long key){ unsigned x=data;
  for(int r=0;r<528;r++){ unsigned f=((x>>0)&1)^((x>>16)&1)^((unsigned)((key>>(r&63))&1))^((NLF>>g5u(x,1,9,20,26,31))&1); x=(x>>1)^(f<<31);} return x;}
__device__ bool is_equal(unsigned L,unsigned L16,unsigned M,unsigned M16){
  unsigned long long d=((unsigned long long)L)|(((unsigned long long)L16)<<16);
  unsigned long long d2=((unsigned long long)M)|((((unsigned long long)M16)>>16)<<32);
  for(int r=0;r<16;r++){
    unsigned a=((unsigned)((d>>r)&1))^((unsigned)((d>>(r+32))&1))^((unsigned)((d>>(r+16))&1))^((NLF>>g5u(d,1+r,9+r,20+r,26+r,31+r))&1);
    unsigned b=((unsigned)((d2>>r)&1))^((unsigned)((d2>>(r+32))&1))^((unsigned)((d2>>(r+16))&1))^((NLF>>g5u(d2,1+r,9+r,20+r,26+r,31+r))&1);
    if(a!=b) return false;} return true;}
extern "C" __global__ void full25(const unsigned* p,const unsigned* c,int N,unsigned* found,unsigned long long* outkey,unsigned* progress){
  unsigned k1=blockIdx.x*blockDim.x+threadIdx.x; if(k1>0xFFFF) return;
  unsigned x[32],y[32];
  for(int i=0;i<N;i++){ x[i]=enc16(p[i],k1); y[i]=dec16(c[i],k1); }
  for(unsigned alpha=0; alpha<=0xFFFF; alpha++){
    if((alpha&0x3FF)==0 && *found) return;
    unsigned ys[32],k4[32],ps[32];
    for(int j=0;j<N;j++){ unsigned long long dk4=((unsigned long long)p[j]<<16)|alpha;
      unsigned kk4=extract16(dk4); ps[j]=(alpha|(p[j]<<16)); ys[j]=dec16(y[j],kk4); k4[j]=kk4; }
    for(int i=0;i<N;i++){
      unsigned long long dk2=((unsigned long long)x[i])|(((unsigned long long)alpha)<<32);
      unsigned k2=extract16(dk2); unsigned xMSW=(x[i]>>16)&0xFFFF; unsigned cstari=enc16(c[i],k2);
      unsigned coll=(cstari>>16)&0xFFFF;
      for(int j=0;j<N;j++){
        if((ys[j]&0xFFFF)!=coll) continue;
        if(!is_equal(xMSW,ps[j],cstari,ys[j])) continue;
        unsigned long long dk3=((unsigned long long)xMSW)|(((unsigned long long)ps[j])<<16);
        unsigned long long key=((unsigned long long)k1)|(((unsigned long long)k2)<<16)|(((unsigned long long)extract16(dk3))<<32)|(((unsigned long long)k4[j])<<48);
        bool ok=true; for(int m=0;m<4&&m<N;m++) if(c[m]!=enc528(p[m],key)){ok=false;break;}
        if(ok){ if(atomicCAS(found,0u,1u)==0)*outkey=key; return; }
      }
    }
  }
  atomicAdd(progress,1u);
}
'''
mod=cp.RawModule(code=SRC); k=mod.get_function("full25")
raw=open('/tmp/ours.pt','rb').read(); n=len(raw)//8
P=np.empty(n,np.uint32); C=np.empty(n,np.uint32)
for i in range(n): P[i],C[i]=struct.unpack_from('<II',raw,i*8)
print(f"scan COMPLET GPU : {n} trames reelles, tous les 65536 k1 x 65536 alpha",flush=True)
dP,dC=cp.asarray(P),cp.asarray(C); found=cp.zeros(1,cp.uint32); outkey=cp.zeros(1,cp.uint64); prog=cp.zeros(1,cp.uint32)
t=time.time()
k((256,),(256,),(dP,dC,np.int32(n),found,outkey,prog)); cp.cuda.Stream.null.synchronize()
dt=time.time()-t
f=int(found.get()[0]); key=int(outkey.get()[0]); pr=int(prog.get()[0])
print(f"termine en {dt:.1f}s. k1 completes={pr}/65536",flush=True)
if f: print(f">>> !!! CLE TROUVEE : 0x{key:016X} !!!",flush=True)
else: print(f">>> AUCUNE cle sur les 65536 k1 x 65536 alpha (scan COMPLET, 25 trames)",flush=True)
