#!/usr/bin/env python3
# ETAPE A du portage GPU de l'attaque slide-MITM (preneel) :
# porter les primitives KeeLoq_16 + Extract + Is_Equal + Encrypt(528) en CUDA,
# et les VALIDER bit-a-bit contre une reference Python fidele au C d'Enderlein.
import numpy as np, cupy as cp

NLF = 0x3A5C742E
M32 = 0xFFFFFFFF

# ---------------- reference Python (copie exacte du C keeloq.cpp) ----------------
def g5(x,a,b,c,d,e):
    return ((x>>a)&1)|(((x>>b)&1)<<1)|(((x>>c)&1)<<2)|(((x>>d)&1)<<3)|(((x>>e)&1)<<4)

def enc16(data,key):
    x=data&M32
    for r in range(16):
        f=((x>>0)&1)^((x>>16)&1)^((key>>r)&1)^((NLF>>g5(x,1,9,20,26,31))&1)
        x=((x>>1)^(f<<31))&M32
    return x

def dec16(data,key):
    x=data&M32
    for r in range(16):
        f=((x>>31)&1)^((x>>15)&1)^((key>>(15-r))&1)^((NLF>>g5(x,0,8,19,25,30))&1)
        x=((x<<1)^f)&M32
    return x

def extract16(data):           # data = u64
    key=0
    for r in range(16):
        b=((data>>r)&1)^((data>>(r+32))&1)^((data>>(r+16))&1)^((NLF>>g5(data,1+r,9+r,20+r,26+r,31+r))&1)
        key|=(b<<r)
    return key

def enc528(data,key):          # key = u64
    x=data&M32
    for r in range(528):
        f=((x>>0)&1)^((x>>16)&1)^((key>>(r&63))&1)^((NLF>>g5(x,1,9,20,26,31))&1)
        x=((x>>1)^(f<<31))&M32
    return x

# ---------------- kernel CUDA (device funcs identiques) ----------------
SRC = r'''
#define NLF 0x3A5C742Eu
__device__ __forceinline__ unsigned g5u(unsigned long long x,int a,int b,int c,int d,int e){
  return ((unsigned)((x>>a)&1))|(((unsigned)((x>>b)&1))<<1)|(((unsigned)((x>>c)&1))<<2)
        |(((unsigned)((x>>d)&1))<<3)|(((unsigned)((x>>e)&1))<<4);
}
__device__ unsigned enc16(unsigned data, unsigned key){
  unsigned x=data;
  for(int r=0;r<16;r++){
    unsigned f=((x>>0)&1)^((x>>16)&1)^((key>>r)&1)^((NLF>>g5u(x,1,9,20,26,31))&1);
    x=(x>>1)^(f<<31);
  } return x;
}
__device__ unsigned dec16(unsigned data, unsigned key){
  unsigned x=data;
  for(int r=0;r<16;r++){
    unsigned f=((x>>31)&1)^((x>>15)&1)^((key>>(15-r))&1)^((NLF>>g5u(x,0,8,19,25,30))&1);
    x=(x<<1)^f;
  } return x;
}
__device__ unsigned extract16(unsigned long long data){
  unsigned key=0;
  for(int r=0;r<16;r++){
    unsigned b=((unsigned)((data>>r)&1))^((unsigned)((data>>(r+32))&1))^((unsigned)((data>>(r+16))&1))
              ^((NLF>>g5u(data,1+r,9+r,20+r,26+r,31+r))&1);
    key|=(b<<r);
  } return key;
}
__device__ unsigned enc528(unsigned data, unsigned long long key){
  unsigned x=data;
  for(int r=0;r<528;r++){
    unsigned f=((x>>0)&1)^((x>>16)&1)^((unsigned)((key>>(r&63))&1))^((NLF>>g5u(x,1,9,20,26,31))&1);
    x=(x>>1)^(f<<31);
  } return x;
}
extern "C" __global__ void test_prim(
    const unsigned* data, const unsigned* key16, const unsigned long long* data64,
    const unsigned* d528, const unsigned long long* k64, int n,
    unsigned* o_enc16, unsigned* o_dec16, unsigned* o_ext16, unsigned* o_enc528)
{
  int i=blockIdx.x*blockDim.x+threadIdx.x; if(i>=n) return;
  o_enc16[i]=enc16(data[i],key16[i]);
  o_dec16[i]=dec16(data[i],key16[i]);
  o_ext16[i]=extract16(data64[i]);
  o_enc528[i]=enc528(d528[i],k64[i]);
}
'''

def main():
    rng=np.random.default_rng(1234)
    n=20000
    data=rng.integers(0,1<<32,n,dtype=np.uint64).astype(np.uint32)
    key16=rng.integers(0,1<<16,n,dtype=np.uint32)
    data64=rng.integers(0,1<<48,n,dtype=np.uint64)
    d528=rng.integers(0,1<<32,n,dtype=np.uint64).astype(np.uint32)
    k64=rng.integers(0,1<<64,n,dtype=np.uint64)

    mod=cp.RawModule(code=SRC); kern=mod.get_function("test_prim")
    dv=[cp.asarray(a) for a in (data,key16,data64,d528,k64)]
    o=[cp.zeros(n,dtype=cp.uint32) for _ in range(4)]
    kern((n//256+1,),(256,),(dv[0],dv[1],dv[2],dv[3],dv[4],np.int32(n),o[0],o[1],o[2],o[3]))
    cp.cuda.Stream.null.synchronize()
    g_enc16,g_dec16,g_ext16,g_enc528=[x.get() for x in o]

    # reference Python sur un echantillon (528-tours est lent, on en teste 400)
    print(f"validation GPU vs reference C-fidele sur {n} vecteurs...")
    ok=True
    # enc16/dec16/ext16 : tous
    for i in range(n):
        if enc16(int(data[i]),int(key16[i]))!=g_enc16[i]: print("MISMATCH enc16",i); ok=False; break
        if dec16(int(data[i]),int(key16[i]))!=g_dec16[i]: print("MISMATCH dec16",i); ok=False; break
        if extract16(int(data64[i]))!=g_ext16[i]:         print("MISMATCH ext16",i); ok=False; break
    # enc528 : echantillon (lent en python)
    for i in range(400):
        if enc528(int(d528[i]),int(k64[i]))!=g_enc528[i]: print("MISMATCH enc528",i); ok=False; break

    # sanity : roundtrip enc16/dec16 doit tenir sur GPU
    rt=all(dec16(int(g_enc16[i]),int(key16[i]))==int(data[i]) for i in range(200))
    print(f"  enc16 GPU==C : {'OK' if ok else 'FAIL'}")
    print(f"  roundtrip dec16(enc16)==data : {'OK' if rt else 'FAIL'}")
    print("\n>>> ETAPE A", "VALIDEE : primitives KeeLoq correctes sur ton GPU" if (ok and rt) else "ECHOUEE")

if __name__=="__main__": main()
