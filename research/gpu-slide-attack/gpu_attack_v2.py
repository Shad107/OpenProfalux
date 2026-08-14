#!/usr/bin/env python3
# v2 : kernel BLOC-COOPERATIF. 1 bloc = 1 alpha (boucle), 256 threads cooperent :
# table de hachage partagee PAR BLOC (pas par thread), lectures pool coalescees,
# build par atomicCAS. Objectif : arreter de gaspiller le GPU.
import numpy as np, cupy as cp, struct, subprocess, time
PROG="/tmp/claude-1000/-mnt-c-Users-delafosse/1a4b94cf-f6ab-4ece-a8c1-0bc79ba9b944/scratchpad/lasec_prog/prog"

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

extern "C" __global__ void precompute_xy(const unsigned* p,const unsigned* c,unsigned k1,int N,unsigned* x,unsigned* y){
  int i=blockIdx.x*blockDim.x+threadIdx.x; if(i>=N) return; x[i]=enc16(p[i],k1); y[i]=dec16(c[i],k1);}

extern "C" __global__ void attack_v2(
    const unsigned* p,const unsigned* c,const unsigned* x,const unsigned* y,int N,unsigned k1,int HTS,unsigned maxalpha,
    unsigned* g_ys,unsigned* g_k4,unsigned* g_ps,int* g_ht,unsigned* found,unsigned long long* outkey)
{
  int blk=blockIdx.x, tid=threadIdx.x, nt=blockDim.x, mask=HTS-1;
  unsigned* ys=g_ys+(size_t)blk*N; unsigned* k4=g_k4+(size_t)blk*N; unsigned* ps=g_ps+(size_t)blk*N;
  int* ht=g_ht+(size_t)blk*HTS;
  for(unsigned alpha=blk; alpha<=maxalpha; alpha+=gridDim.x){
    if(*found) return;
    for(int s=tid;s<HTS;s+=nt) ht[s]=-1;
    __syncthreads();
    for(int j=tid;j<N;j+=nt){                          // build coalesce + atomicCAS
      unsigned long long dk4=((unsigned long long)p[j]<<16)|alpha;
      unsigned kk4=extract16(dk4); unsigned ystar=dec16(y[j],kk4);
      ys[j]=ystar; k4[j]=kk4; ps[j]=(alpha|(p[j]<<16));
      int slot=(int)(((ystar&0xFFFF)*2654435761u)&mask);
      while(true){ int old=atomicCAS(&ht[slot],-1,j); if(old==-1) break; slot=(slot+1)&mask; }
    }
    __syncthreads();
    for(int i=tid;i<N;i+=nt){                           // probe coalesce
      if(*found) break;
      unsigned long long dk2=((unsigned long long)x[i])|(((unsigned long long)alpha)<<32);
      unsigned k2=extract16(dk2); unsigned xMSW=(x[i]>>16)&0xFFFF;
      unsigned cstari=enc16(c[i],k2); unsigned pkey=(cstari>>16)&0xFFFF;
      int slot=(int)((pkey*2654435761u)&mask);
      while(ht[slot]!=-1){
        int j=ht[slot];
        if((ys[j]&0xFFFF)==pkey && is_equal(xMSW,ps[j],cstari,ys[j])){
          unsigned long long dk3=((unsigned long long)xMSW)|(((unsigned long long)ps[j])<<16);
          unsigned long long key=((unsigned long long)k1)|(((unsigned long long)k2)<<16)
                |(((unsigned long long)extract16(dk3))<<32)|(((unsigned long long)k4[j])<<48);
          bool ok=true; for(int m=0;m<4&&m<N;m++) if(c[m]!=enc528(p[m],key)){ok=false;break;}
          if(ok){ if(atomicCAS(found,0u,1u)==0)*outkey=key; return; }
        }
        slot=(slot+1)&mask;
      }
    }
    __syncthreads();
  }
}
'''
mod=cp.RawModule(code=SRC); kprec=mod.get_function("precompute_xy"); katk=mod.get_function("attack_v2")
def load_pt(p):
    raw=open(p,'rb').read(); n=len(raw)//8; P=np.empty(n,np.uint32); C=np.empty(n,np.uint32)
    for i in range(n): P[i],C[i]=struct.unpack_from('<II',raw,i*8)
    return P,C
def npow2(x):
    p=1
    while p<x: p<<=1
    return p
def run(P,C,k1,GRID=1024,BLK=256,maxalpha=0xFFFF):
    N=len(P); HTS=npow2(N+1)   # charge ~50%, moitie moins que npow2(2N)
    dP,dC=cp.asarray(P),cp.asarray(C); dX=cp.zeros(N,cp.uint32); dY=cp.zeros(N,cp.uint32)
    kprec((N//256+1,),(256,),(dP,dC,np.uint32(k1),np.int32(N),dX,dY)); cp.cuda.Stream.null.synchronize()
    g_ys=cp.zeros(GRID*N,cp.uint32); g_k4=cp.zeros(GRID*N,cp.uint32); g_ps=cp.zeros(GRID*N,cp.uint32)
    g_ht=cp.zeros(GRID*HTS,cp.int32); found=cp.zeros(1,cp.uint32); outkey=cp.zeros(1,cp.uint64)
    mem=(3*GRID*N+GRID*HTS)*4/1e9
    t=time.time()
    katk((GRID,),(BLK,),(dP,dC,dX,dY,np.int32(N),np.uint32(k1),np.int32(HTS),np.uint32(maxalpha),
         g_ys,g_k4,g_ps,g_ht,found,outkey)); cp.cuda.Stream.null.synchronize()
    return int(found.get()[0]),int(outkey.get()[0]),time.time()-t,mem,HTS

P,C=load_pt('/tmp/gk.pt')
f,k,dt,mem,_=run(P,C,0x3906,GRID=256,maxalpha=0xFFFF)
ok=(f and k==0xE25AD14098DA3906)
print(f"[valid v2] N={len(P)} : {'OK cle=0x%016X'%k if ok else 'ECHEC f=%d k=%X'%(f,k)} ({dt*1000:.0f} ms)",flush=True)
if not ok: raise SystemExit("v2 buggee")

subprocess.run([f"{PROG}/genkey","-k","/tmp/big.key"],cwd=PROG,capture_output=True)
realkey=int.from_bytes(open("/tmp/big.key","rb").read(),'little'); k1big=realkey&0xFFFF
subprocess.run([f"{PROG}/genkp","-k","/tmp/big.key","-c","65536","-x","-o","/tmp/big.pt"],cwd=PROG,capture_output=True)
Pb,Cb=load_pt('/tmp/big.pt')
SLICE=4095; best=None
print(f"[bench v2] N={len(Pb)}, balayage de config sur GPU libre :",flush=True)
for GRID,BLK in [(1024,512),(1024,1024),(2048,512),(768,512),(1536,512),(2048,1024)]:
    cp.get_default_memory_pool().free_all_blocks()
    try:
        f,k,dt,mem,HTS=run(Pb,Cb,k1big,GRID=GRID,BLK=BLK,maxalpha=SLICE)
        per_k1=dt*(65536/(SLICE+1)); full=per_k1*65536
        print(f"  GRID={GRID} BLK={BLK}: 1 k1={per_k1:.1f}s -> {full/86400:.1f} j (scratch {mem:.1f}Go)",flush=True)
        if best is None or full<best[0]: best=(full,GRID,BLK,per_k1)
    except Exception as e:
        print(f"  GRID={GRID} BLK={BLK}: {str(e)[:50]}",flush=True)
full,GRID,BLK,per_k1=best
print(f"\n>>> MEILLEUR sur ta carte: GRID={GRID} BLK={BLK} -> 1 k1={per_k1:.1f}s",flush=True)
print(f">>> ATTAQUE COMPLETE ~{full/86400:.1f} jours (~{full/3600:.0f} h)  |  gain x{262/(full/86400):.0f} vs v1",flush=True)
