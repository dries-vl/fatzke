// obj2mesh: outputs positions (A2B10G10R10_SNORM), normals (A2B10G10R10_SNORM),
// uvs (R16G16_UNORM), indices (uint16). Either as a C header or a single binary.
//
// Usage:
//   obj2mesh input.obj -o out.h            (C header)
//   obj2mesh input.obj -o out.meshbin      (binary blob)
//
// Binary layout (little-endian):
//   struct Header {
//     uint32_t magic;      // 'MSH1' = 0x3148534D
//     uint32_t vertexCount;
//     uint32_t indexCount;
//     uint32_t reserved[5]; // = 0
//   };
//   uint32_t positions[vertexCount];   // A2B10G10R10_SNORM (cm-quantized, A2=0)
//   uint32_t normals  [vertexCount];   // A2B10G10R10_SNORM (unit vectors, A2=0)
//   uint32_t uvs      [vertexCount];   // packed R16G16 (hi=v, lo=u)
//   uint16_t indices  [indexCount];    // uint16
//
// Deduplication policy: by **packed position** ONLY. The first normal/uv seen for a position is kept.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ---------- utils ---------- */
static float clampf(float x, float a, float b){ return x<a?a:(x>b?b:x); }
static uint16_t unorm16(float x){            /* clamp [0,1] then quantize */
    double t = x; if (t<0.0) t=0.0; if (t>1.0) t=1.0;
    unsigned q = (unsigned)lrint(t*65535.0);
    return (uint16_t)q;
}
static inline uint32_t snorm10q(float x){    /* x in [-1,1] → signed 10-bit (two's complement) */
    double t = x;
    if (t < -1.0) t = -1.0;
    if (t >  1.0) t =  1.0;
    int q = (int)lrint(t * 511.0);           /* -511..+511 */
    if (q < -512) q = -512;                  /* allow min code */
    if (q >  511) q =  511;
    return (uint32_t)(q & 0x3FFu);
}
static inline uint32_t pack_pos_cm_101010(float x_m,float y_m,float z_m){
    /* 1 unit = 1m; 1 cm = 0.01. Clamp to ±511 cm and map to [-1,1] for SNORM10 */
    const float sx = clampf((x_m*100.0f)/511.0f, -1.0f, 1.0f);
    const float sy = clampf((y_m*100.0f)/511.0f, -1.0f, 1.0f);
    const float sz = clampf((z_m*100.0f)/511.0f, -1.0f, 1.0f);
    const uint32_t bx = snorm10q(sx);
    const uint32_t by = snorm10q(sy);
    const uint32_t bz = snorm10q(sz);
    return (bz<<20) | (by<<10) | (bx<<0);    /* A2=0 */
}
static inline uint32_t pack_nrm_101010(float nx,float ny,float nz){
    /* normals packed directly in SNORM10 (unit space) */
    float len = sqrtf(nx*nx + ny*ny + nz*nz);
    if (len > 0.0f){ nx/=len; ny/=len; nz/=len; } else { nx=0; ny=0; nz=1; }
    const uint32_t bx = snorm10q(nx);
    const uint32_t by = snorm10q(ny);
    const uint32_t bz = snorm10q(nz);
    return (bz<<20) | (by<<10) | (bx<<0);    /* A2=0 */
}

/* ---------- data types ---------- */
typedef struct { float x,y,z; } Vec3;
typedef struct { float u,v;   } Vec2;
typedef struct { int v,vt,vn; } Triplet;  /* raw OBJ indices (can be negative, 0=missing) */

/* file blob */
typedef struct { char* ptr; size_t len; } Blob;

/* ---------- IO ---------- */
static Blob read_all(const char* path){
    Blob b = {0};
    FILE* f = path? fopen(path,"rb"): stdin;
    if(!f){ perror("fopen"); exit(1); }
    fseek(f, 0, SEEK_END);
    long L = ftell(f);
    if(L < 0) { /* stdin */
        size_t cap=1<<20; b.ptr=(char*)malloc(cap); if(!b.ptr){perror("malloc"); exit(1);}
        size_t used=0, n;
        rewind(f);
        for(;;){
            if(used+65536 > cap){ cap*=2; b.ptr=(char*)realloc(b.ptr,cap); if(!b.ptr){perror("realloc"); exit(1);} }
            n = fread(b.ptr+used,1,65536,f);
            used+=n;
            if(n<65536) break;
        }
        b.len=used;
    } else {
        rewind(f);
        b.len = (size_t)L;
        b.ptr = (char*)malloc(b.len+1);
        if(!b.ptr){perror("malloc"); exit(1);}
        if(fread(b.ptr,1,b.len,f)!=b.len){ perror("fread"); exit(1); }
        b.ptr[b.len]=0;
    }
    if (path) fclose(f);
    return b;
}

/* ---------- parsing ---------- */
static int starts_with(const char* s, const char* p){ return strncmp(s,p,strlen(p))==0; }
static void trim_leading(const char** s){ while(**s==' '||**s=='\t'||**s=='\r') (*s)++; }
static int resolve(int idx, int count){ if(idx>0) return idx-1; if(idx<0) return count+idx; return -1; }

typedef struct { int nv, nvt, nvn; int ntris; } Counts;

static Counts count_pass(char* s, char* e){
    Counts c={0,0,0,0};
    char* p=s;
    while(p<e){
        char* l=p;
        while(p<e && *p!='\n') ++p;
        size_t len = (size_t)(p-l);
        if(p<e && *p=='\n') *p++ = 0; /* make lines C-strings */
        if(len==0) continue;
        while(len && (*l==' '||*l=='\t'||*l=='\r')){ ++l; --len; }
        if(len==0 || *l=='#') continue;

        if(starts_with(l,"v ")){ c.nv++; }
        else if(starts_with(l,"vt ")){ c.nvt++; }
        else if(starts_with(l,"vn ")){ c.nvn++; }
        else if(starts_with(l,"f ")){
            int verts=0, in=0;
            for(char* r=l+1; r<l+len; ++r){
                if(*r!=' ' && *r!='\t'){ if(!in){ in=1; verts++; } }
                else in=0;
            }
            if(verts>=3) c.ntris += (verts-2);
        }
    }
    return c;
}

static void parse_and_fill(
    char* s, char* e,
    Vec3* V, Vec2* VT, Vec3* VN,
    Triplet* T, int* out_nv, int* out_nvt, int* out_nvn, int* out_ntris)
{
    int iv=0, ivt=0, ivn=0, tt=0;
    for(char* l=s; l<e; l += strlen(l)+1){
        size_t len = strlen(l);
        if(len==0) continue;
        const char* p=l; while(*p==' '||*p=='\t'||*p=='\r') ++p;
        if(*p=='#' || *p==0) continue;

        if(starts_with(p,"v ")){
            double x=0,y=0,z=0; const char* q=p+1; trim_leading(&q);
            x=strtod(q,(char**)&q); y=strtod(q,(char**)&q); z=strtod(q,(char**)&q);
            V[iv++] = (Vec3){(float)x,(float)y,(float)z};
        } else if(starts_with(p,"vt ")){
            double u=0,v=0; const char* q=p+2; trim_leading(&q);
            u=strtod(q,(char**)&q); v=strtod(q,(char**)&q);
            VT[ivt++] = (Vec2){(float)u,(float)v};
        } else if(starts_with(p,"vn ")){
            double x=0,y=0,z=0; const char* q=p+2; trim_leading(&q);
            x=strtod(q,(char**)&q); y=strtod(q,(char**)&q); z=strtod(q,(char**)&q);
            VN[ivn++] = (Vec3){(float)x,(float)y,(float)z};
        } else if(starts_with(p,"f ")){
            const char* q=p+1; trim_leading(&q);
            const char* toks[128]; int nt=0, in=0;
            for(const char* a=q; *a; ++a){
                if(*a!=' ' && *a!='\t'){ if(!in){ in=1; toks[nt++]=a; if(nt>=128) break; } }
                else in=0;
            }
            if(nt>=3){
                Triplet tmp[128]; int m=nt; if(m>128)m=128;
                for(int i=0;i<m;i++){
                    int vi=0, vti=0, vni=0, part=0, sign=1, val=0, seen=0;
                    const char* s2 = toks[i];
                    for(;;){
                        if(*s2=='-'){ sign=-1; ++s2; }
                        val=0; seen=0;
                        while(*s2>='0'&&*s2<='9'){ val=val*10+(*s2-'0'); ++s2; seen=1; }
                        if(seen){
                            if(part==0) vi = sign*val;
                            else if(part==1) vti = sign*val;
                            else if(part==2) vni = sign*val;
                        }
                        if(*s2=='/'){ ++s2; ++part; if(part>2) break; sign=1; continue; }
                        break;
                    }
                    tmp[i].v=vi; tmp[i].vt=vti; tmp[i].vn=vni;
                }
                for(int i=1;i+1<nt;i++){
                    T[tt++] = tmp[0];
                    T[tt++] = tmp[i];
                    T[tt++] = tmp[i+1];
                }
            }
        }
    }
    *out_nv=iv; *out_nvt=ivt; *out_nvn=ivn; *out_ntris=tt/3;
}

/* ---------- hash maps ---------- */
static uint32_t hmix32(uint32_t x){ x ^= x>>16; x *= 0x7feb352d; x ^= x>>15; x *= 0x846ca68b; x ^= x>>16; return x; }

/* (A) Position map: packed position -> vertex index */
typedef struct { uint32_t key; uint32_t val; int used; } PosEnt;
typedef struct { PosEnt* e; size_t cap; } PosMap;
static void posmap_init(PosMap* m, size_t want){
    size_t cap=1; while(cap < want*2) cap<<=1;
    m->e = (PosEnt*)calloc(cap, sizeof(PosEnt)); if(!m->e){perror("calloc"); exit(1);}
    m->cap=cap;
}
static int posmap_get_or_put(PosMap* m, uint32_t key, uint32_t* ioVal){
    size_t mask = m->cap - 1;
    size_t idx = (hmix32(key) & mask);
    for(;;){
        if(!m->e[idx].used){
            m->e[idx].used=1; m->e[idx].key=key; m->e[idx].val=*ioVal;
            return 1; /* inserted */
        }
        if(m->e[idx].key == key){
            *ioVal = m->e[idx].val;
            return 0; /* found */
        }
        idx = (idx + 1) & mask;
    }
}

/* ---------- emit ---------- */
static int ends_with(const char* s, const char* suf){
    size_t ls=strlen(s), lt=strlen(suf);
    return (ls>=lt && memcmp(s+ls-lt,suf,lt)==0);
}

static void emit_header(FILE* f,
    const uint32_t* pos_u32, const uint32_t* nrm_u32, const uint32_t* uv_u32, size_t vcount,
    const uint16_t* idx_u16, size_t icount, const char* name)
{
    fprintf(f, "// auto-generated mesh: %s\n#include <stdint.h>\n\n", name);
    fprintf(f, "static const uint32_t g_positions_%s[%zu] = {\n", name, vcount);
    for(size_t i=0;i<vcount;i++){ fprintf(f, "  0x%08Xu,%s", pos_u32[i], (i+1<vcount)?"\n":"\n"); }
    fprintf(f, "};\n\n");

    fprintf(f, "static const uint32_t g_normals_%s[%zu] = {\n", name, vcount);
    for(size_t i=0;i<vcount;i++){ fprintf(f, "  0x%08Xu,%s", nrm_u32[i], (i+1<vcount)?"\n":"\n"); }
    fprintf(f, "};\n\n");

    fprintf(f, "static const uint32_t g_uvs_%s[%zu] = {\n", name, vcount);
    for(size_t i=0;i<vcount;i++){ fprintf(f, "  0x%08Xu,%s", uv_u32[i], (i+1<vcount)?"\n":"\n"); }
    fprintf(f, "};\n\n");

    fprintf(f, "static const uint16_t g_indices_%s[%zu] = {\n", name, icount);
    for(size_t i=0;i<icount;i++){ fprintf(f, "  %uu,%s", idx_u16[i], (i+1<icount)?"\n":"\n"); }
    fprintf(f, "};\n\n");

    fprintf(f, "static const uint32_t g_vertex_count_%s = %zu;\n", name, vcount);
    fprintf(f, "static const uint32_t g_index_count_%s  = %zu;\n", name, icount);
}

static void emit_binary(FILE* f,
    const uint32_t* pos_u32, const uint32_t* nrm_u32, const uint32_t* uv_u32, size_t vcount,
    const uint16_t* idx_u16, size_t icount)
{
    struct Header { uint32_t magic, vc, ic, reserved[5]; } hdr;
    hdr.magic = 0x3148534D; /* 'MSH1' */
    hdr.vc = (uint32_t)vcount;
    hdr.ic = (uint32_t)icount;
    memset(hdr.reserved, 0, sizeof hdr.reserved);
    fwrite(&hdr, sizeof hdr, 1, f);
    fwrite(pos_u32, sizeof(uint32_t), vcount, f);
    fwrite(nrm_u32, sizeof(uint32_t), vcount, f);
    fwrite(uv_u32,  sizeof(uint32_t), vcount, f);
    fwrite(idx_u16, sizeof(uint16_t), icount, f);
}

/* ---------- main build (dedup by packed position only) ---------- */
static void build_and_emit(
    const char* outpath, const char* name,
    const Vec3* V, int nv, const Vec2* VT, int nvt, const Vec3* VN, int nvn,
    Triplet* T, int ntris)
{
    size_t maxVerts = (size_t)3*ntris;

    /* outputs */
    uint32_t* pos_u32 = (uint32_t*)malloc(maxVerts*sizeof(uint32_t));
    uint32_t* nrm_u32 = (uint32_t*)malloc(maxVerts*sizeof(uint32_t));
    uint32_t* uv_u32  = (uint32_t*)malloc(maxVerts*sizeof(uint32_t));
    uint16_t* idx_u16 = (uint16_t*)malloc((size_t)3*ntris*sizeof(uint16_t));
    if(!pos_u32||!nrm_u32||!uv_u32||!idx_u16){ perror("malloc"); exit(1); }

    PosMap pmap; posmap_init(&pmap, (size_t)3*ntris);

    size_t vcount=0, icount=0;

    for(int i=0;i<ntris*3;i++){
        Triplet tr = T[i];
        int vi  = resolve(tr.v,  nv);
        int vti = (tr.vt!=0)? resolve(tr.vt, nvt) : -1;
        int vni = (tr.vn!=0)? resolve(tr.vn, nvn) : -1;
        if(vi<0 || vi>=nv){ fprintf(stderr,"Invalid vertex index in face.\n"); exit(1); }

        /* pack attributes for this corner */
        Vec3 p = V[vi];
        uint32_t pPacked = pack_pos_cm_101010(p.x, p.y, p.z);

        float nx=0.f, ny=0.f, nz=1.f;
        if(vni>=0){ nx=VN[vni].x; ny=VN[vni].y; nz=VN[vni].z; }
        uint32_t nPacked = pack_nrm_101010(nx, ny, nz);

        float uu=0.f, vv=0.f;
        if(vti>=0){ uu=VT[vti].u; vv=VT[vti].v; }
        uint32_t tPacked = ((uint32_t)unorm16(uu)) | ((uint32_t)unorm16(vv) << 16);

        /* dedup by **position only** */
        uint32_t idx = (uint32_t)vcount;
        int inserted = posmap_get_or_put(&pmap, pPacked, &idx);
        if(inserted){
            if(idx > 0xFFFFu){ fprintf(stderr,"Too many vertices (>65535).\n"); exit(1); }
            pos_u32[idx] = pPacked;
            nrm_u32[idx] = nPacked;   /* first normal encountered for this position */
            uv_u32[idx]  = tPacked;   /* first UV encountered for this position     */
            vcount++;
        } else {
            /* already had this position; keep existing normal/uv (first wins) */
        }

        if(idx > 0xFFFFu){ fprintf(stderr,"Index overflow.\n"); exit(1); }
        idx_u16[icount++] = (uint16_t)idx;
    }

    FILE* f = outpath? fopen(outpath, ends_with(outpath,".meshbin")? "wb":"w") : stdout;
    if(!f){ perror("fopen out"); exit(1); }

    if(outpath && ends_with(outpath,".meshbin")){
        emit_binary(f, pos_u32, nrm_u32, uv_u32, vcount, idx_u16, icount);
    }else{
        emit_header(f, pos_u32, nrm_u32, uv_u32, vcount, idx_u16, icount, name?name:"mesh");
    }

    if(outpath) fclose(f);
    free(pos_u32); free(nrm_u32); free(uv_u32); free(idx_u16); free(pmap.e);
}

/* ---------- top-level parse pipeline ---------- */
static void usage(const char* prog){
    fprintf(stderr,"Usage: %s [input.obj] [-o out.{h|meshbin}] [--name basename]\n", prog);
}
int main(int argc, char** argv){
    const char* inpath=NULL, *outpath=NULL, *name="mesh";
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-o")==0 && i+1<argc) outpath=argv[++i];
        else if(strcmp(argv[i],"--name")==0 && i+1<argc) name=argv[++i];
        else if(argv[i][0]=='-'){ usage(argv[0]); return 1; }
        else inpath=argv[i];
    }

    Blob b = read_all(inpath);
    char* s=b.ptr; char* e=b.ptr + b.len;

    /* first pass (also NUL-terminates each line) */
    Counts c = count_pass(s,e);
    if(c.nv==0 || c.ntris==0){ fprintf(stderr,"Empty/invalid OBJ.\n"); return 1; }

    Vec3* V  = (Vec3*)malloc((size_t)c.nv * sizeof(Vec3));
    Vec2* VT = (c.nvt>0)? (Vec2*)malloc((size_t)c.nvt*sizeof(Vec2)) : NULL;
    Vec3* VN = (c.nvn>0)? (Vec3*)malloc((size_t)c.nvn*sizeof(Vec3)) : NULL;
    Triplet* T = (Triplet*)malloc((size_t)c.ntris*3*sizeof(Triplet));
    if(!V || (c.nvt && !VT) || (c.nvn && !VN) || !T){ perror("malloc"); return 1; }

    int nv,nvt,nvn,ntris;
    parse_and_fill(s,e, V, VT, VN, T, &nv,&nvt,&nvn,&ntris);

    build_and_emit(outpath, name, V, nv, VT, nvt, VN, nvn, T, ntris);

    free(V); if(VT) free(VT); if(VN) free(VN); free(T); free(b.ptr);
    return 0;
}