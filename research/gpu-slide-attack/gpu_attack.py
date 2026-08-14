#!/usr/bin/env python3
# ETAPE B : attaque slide-MITM sur GPU. Given k1, scanne tous les alpha en
# parallele (1 thread = 1 alpha), build/match fidele au preneel.cpp d'Enderlein,
# et doit retrouver la cle connue E25AD14098DA3906 sur /tmp/gk.pt.
import numpy as np, cupy as cp, struct, sys

NLF=0x3A5C742E; M32=0xFFFFFFFF
def g5(x,a,b,c,d,e): return ((x>>a)&1)|(((x>>b)&1)<<1)|(((x>>c)&1)<<2)|(((x>>d)&1)<<3)|(((x>>e)&1)<<4)
def enc16(data,key):
    x=data&M32
    for r in range(16):
        f=((x>>0)&1)^((x>>16)&1)^((key>>r)&1)^((NLF>>g5(x,1,9,20,26,31))&1); x=((x>>1)^(f<<31))&M32
    return x
def dec16(data,key):
    x=data&M32
    for r in range(16):
        f=((x>>31)&1)^((x>>15)&1)^((key>>(15-r))&1)^((NLF>>g5(x,0,8,19,25,30))&1); x=((x<<1)^f)&M32
    return x

SRC=r'''
#define NLF 0x3A5C742Eu
#define MAXN 300
__device__ __forceinline__ unsigned g5u(unsigned long long x,int a,int b,int c,int d,int e){
  return ((unsigned)((x>>a)&1))|(((unsigned)((x>>b)&1))<<1)|(((unsigned)((x>>c)&1))<<2)
        |(((unsigned)((x>>d)&1))<<3)|(((unsigned)((x>>e)&1))<<4);
}
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
    if(a!=b) return false;
  } return true;}

extern "C" __global__ void attack_k1(
    const unsigned* p,const unsigned* c,const unsigned* x,const unsigned* y,int N,unsigned k1,
    unsigned* found,unsigned long long* outkey)
{
  unsigned alpha=blockIdx.x*blockDim.x+threadIdx.x;
  if(alpha>0xFFFF) return;
  if(*found) return;
  unsigned ystarj[MAXN], k4arr[MAXN], pstarj[MAXN];
  // build side (P[j])
  for(int j=0;j<N;j++){
    unsigned long long dk4=((unsigned long long)p[j]<<16)|alpha;   // Extract(alpha,p_j)
    unsigned k4=extract16(dk4);
    unsigned ps=(alpha|(p[j]<<16));                                // pstarj (u32)
    unsigned ys=dec16(y[j],k4);
    k4arr[j]=k4; pstarj[j]=ps; ystarj[j]=ys;
  }
  // match side (P[i])
  for(int i=0;i<N;i++){
    unsigned long long dk2=((unsigned long long)x[i])|(((unsigned long long)alpha)<<32); // Extract(x_i,alpha)
    unsigned k2=extract16(dk2);
    unsigned xMSW=(x[i]>>16)&0xFFFF;
    unsigned cstari=enc16(c[i],k2);
    unsigned collision=(cstari>>16)&0xFFFF;
    for(int j=0;j<N;j++){
      if((ystarj[j]&0xFFFF)!=collision) continue;
      if(!is_equal(xMSW,pstarj[j],cstari,ystarj[j])) continue;
      unsigned long long dk3=((unsigned long long)xMSW)|(((unsigned long long)pstarj[j])<<16);
      unsigned k3=extract16(dk3);
      unsigned k4=k4arr[j];
      unsigned long long key=((unsigned long long)k1)|(((unsigned long long)k2)<<16)|(((unsigned long long)k3)<<32)|(((unsigned long long)k4)<<48);
      bool ok=true;
      for(int m=0;m<4&&m<N;m++) if(c[m]!=enc528(p[m],key)){ ok=false; break; }
      if(ok){ if(atomicCAS(found,0u,1u)==0){ *outkey=key; } return; }
    }
  }
}
'''

def main():
    raw=open('/tmp/gk.pt','rb').read()
    n=len(raw)//8
    P=np.empty(n,np.uint32); C=np.empty(n,np.uint32)
    for i in range(n):
        pt,ct=struct.unpack_from('<II',raw,i*8); P[i]=pt; C[i]=ct
    k1=0x3906
    X=np.array([enc16(int(P[i]),k1) for i in range(n)],np.uint32)
    Y=np.array([dec16(int(C[i]),k1) for i in range(n)],np.uint32)
    print(f"jeu: {n} paires, k1=0x{k1:04X}, cible=E25AD14098DA3906")

    mod=cp.RawModule(code=SRC); kern=mod.get_function("attack_k1")
    dP,dC,dX,dY=[cp.asarray(a) for a in (P,C,X,Y)]
    found=cp.zeros(1,cp.uint32); outkey=cp.zeros(1,cp.uint64)
    import time; t=time.time()
    kern((256,),(256,),(dP,dC,dX,dY,np.int32(n),np.uint32(k1),found,outkey))
    cp.cuda.Stream.null.synchronize()
    dt=time.time()-t
    f=int(found.get()[0]); k=int(outkey.get()[0])
    print(f"scan 65536 alpha sur GPU en {dt*1000:.1f} ms")
    if f and k==0xE25AD14098DA3906:
        print(f">>> ETAPE B VALIDEE : GPU a retrouve la cle 0x{k:016X} !")
    elif f:
        print(f">>> cle trouvee mais differente: 0x{k:016X} (bug de portage)")
    else:
        print(">>> AUCUNE cle -> le kernel ne matche pas encore (a debugger)")

if __name__=="__main__": main()
