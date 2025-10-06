#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ---------- utils ---------- */
static float clampf(float x, float a, float b){ return x<a?a:(x>b?b:x); }

static uint16_t unorm16(float x){
    double t = x; if (t<0.0) t=0.0; if (t>1.0) t=1.0;
    unsigned q = (unsigned)lrint(t*65535.0);
    return (uint16_t)q;
}
static uint32_t snorm10_unit(float x){           /* expects x in [-1,1] */
    double t=x; if(t<-1.0)t=-1.0; if(t>1.0)t=1.0;
    int q = (int)lrint(t*511.0);
    return (uint32_t)(q & 0x3FFu);
}
static uint32_t pack1010102(float x,float y,float z){
    /* Fixed mapping: OBJ units are centimeters; clamp to ±511 cm, then /511 to SNORM */
    const float cmx = clampf(x*100.0f, -511.0f, 511.0f);
    const float cmy = clampf(y*100.0f, -511.0f, 511.0f);
    const float cmz = clampf(z*100.0f, -511.0f, 511.0f);
    const float sx = cmx / 511.0f;
    const float sy = cmy / 511.0f;
    const float sz = cmz / 511.0f;
    uint32_t bx=snorm10_unit(sx), by=snorm10_unit(sy), bz=snorm10_unit(sz);
    return (bz<<20) | (by<<10) | bx; /* top 2 bits = 0 */
}
static uint16_t oct_encode_8_8(float nx,float ny,float nz){
    double x=nx,y=ny,z=nz;
    double L = sqrt(x*x+y*y+z*z);
    if(L==0.0){ x=0;y=0;z=1; L=1; }
    x/=L; y/=L; z/=L;
    double ax=fabs(x), ay=fabs(y), az=fabs(z);
    double inv = 1.0/(ax+ay+az+1e-12);
    double ox=x*inv, oy=y*inv;
    if(z<0.0){
        double ox2=(1.0-fabs(oy))*(ox>=0.0?1.0:-1.0);
        double oy2=(1.0-fabs(ox))*(oy>=0.0?1.0:-1.0);
        ox=ox2; oy=oy2;
    }
    unsigned ux=(unsigned)lrint(clampf((float)(ox*0.5+0.5),0,1)*255.0f)&0xFFu;
    unsigned uy=(unsigned)lrint(clampf((float)(oy*0.5+0.5),0,1)*255.0f)&0xFFu;
    return (uint16_t)((uy<<8)|ux);
}

/* ---------- data types ---------- */
typedef struct { float x,y,z; } Vec3;
typedef struct { float u,v;   } Vec2;
typedef struct { int v,vt,vn; } Triplet;  /* raw OBJ indices (can be negative, 0=missing) */

typedef struct {
    uint32_t position;
    uint16_t u, v;
    uint16_t normal;
    uint8_t  bone_1, bone_2;
} Vertex;

/* ---------- file in memory ---------- */
typedef struct { char* ptr; size_t len; } Blob;

static Blob read_all(const char* path){
    Blob b = {0};
    FILE* f = path? fopen(path,"rb"): stdin;
    if(!f){ perror("fopen"); exit(1); }
    fseek(f, 0, SEEK_END);
    long L = ftell(f);
    if(L < 0) { /* stdin likely */
        size_t cap=1<<20; b.ptr=(char*)malloc(cap); if(!b.ptr){perror("malloc"); exit(1);}
        size_t used=0, n;
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

static Counts count_pass(const char* s, const char* e){
    Counts c={0,0,0,0};
    const char* p=s;
    while(p<e){
        const char* l=p;
        while(p<e && *p!='\n') ++p;
        size_t len = (size_t)(p-l);
        if(p<e && *p=='\n') ++p;
        if(len==0) continue;
        while(len && (*l==' '||*l=='\t'||*l=='\r')){ ++l; --len; }
        if(len==0 || *l=='#') continue;

        if(starts_with(l,"v ")){ c.nv++; }
        else if(starts_with(l,"vt ")){ c.nvt++; }
        else if(starts_with(l,"vn ")){ c.nvn++; }
        else if(starts_with(l,"f ")){
            int verts=0, in=0;
            const char* q=l+1;
            while(q<l+len && (*q==' '||*q=='\t')) ++q;
            for(const char* r=q; r<l+len; ++r){
                if(*r!=' ' && *r!='\t'){ if(!in){ in=1; verts++; } }
                else in=0;
            }
            if(verts>=3) c.ntris += (verts-2);
        }
    }
    return c;
}

static void parse_and_fill(
    const char* s, const char* e,
    Vec3* V, Vec2* VT, Vec3* VN, int nv, int nvt, int nvn,
    Triplet* T, int ntris)
{
    int iv=0, ivt=0, ivn=0, tt=0;
    const char* p=s;
    while(p<e){
        const char* l=p;
        while(p<e && *p!='\n') ++p;
        size_t len = (size_t)(p-l);
        if(p<e && *p=='\n') ++p;
        if(len==0) continue;
        while(len && (*l==' '||*l=='\t'||*l=='\r')){ ++l; --len; }
        if(len==0 || *l=='#') continue;

        if(starts_with(l,"v ")){
            double x=0,y=0,z=0; const char* q=l+1; trim_leading(&q);
            x=strtod(q,(char**)&q); y=strtod(q,(char**)&q); z=strtod(q,(char**)&q);
            V[iv].x=(float)x; V[iv].y=(float)y; V[iv].z=(float)z; iv++;
        } else if(starts_with(l,"vt ")){
            double u=0,v=0; const char* q=l+2; trim_leading(&q);
            u=strtod(q,(char**)&q); v=strtod(q,(char**)&q);
            VT[ivt].u=(float)u; VT[ivt].v=(float)v; ivt++;
        } else if(starts_with(l,"vn ")){
            double x=0,y=0,z=0; const char* q=l+2; trim_leading(&q);
            x=strtod(q,(char**)&q); y=strtod(q,(char**)&q); z=strtod(q,(char**)&q);
            VN[ivn].x=(float)x; VN[ivn].y=(float)y; VN[ivn].z=(float)z; ivn++;
        } else if(starts_with(l,"f ")){
            const char* q=l+1; trim_leading(&q);
            const char* toks[128]; int nt=0, in=0;
            for(const char* a=q; a<l+len; ++a){
                if(*a!=' ' && *a!='\t'){ if(!in){ in=1; toks[nt++]=a; if(nt>=128) break; } }
                else { if(in){ *((char*)a)='\0'; } in=0; }
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
    (void)nv; (void)nvt; (void)nvn; (void)ntris;
}

/* open-addressing hash map for dedup: key=(vi,vt,vn) -> uint32 index */
typedef struct { Triplet key; uint32_t val; int used; } HEnt;
typedef struct { HEnt* e; size_t cap, count; } HMap;

static uint32_t hmix(uint32_t x){ x ^= x>>16; x *= 0x7feb352d; x ^= x>>15; x *= 0x846ca68b; x ^= x>>16; return x; }
static uint32_t hkey(Triplet k){ return hmix((uint32_t)k.v) ^ (hmix((uint32_t)k.vt)<<1) ^ (hmix((uint32_t)k.vn)<<2); }

static void hmap_init(HMap* m, size_t want){
    size_t cap=1; while(cap < want*2) cap<<=1;
    m->e = (HEnt*)calloc(cap, sizeof(HEnt)); if(!m->e){perror("calloc"); exit(1);}
    m->cap=cap; m->count=0;
}
static int hmap_get_or_put(HMap* m, Triplet k, uint32_t* io){
    size_t mask = m->cap-1;
    size_t idx = (hkey(k) & mask);
    for(;;){
        if(!m->e[idx].used){ m->e[idx].used=1; m->e[idx].key=k; m->e[idx].val=*io; m->count++; return 1; }
        if(m->e[idx].key.v==k.v && m->e[idx].key.vt==k.vt && m->e[idx].key.vn==k.vn){ *io=m->e[idx].val; return 0; }
        idx=(idx+1)&mask;
    }
}

/* ---------- build & emit ---------- */
static void build_and_emit(
    const char* outpath, const char* name,
    const Vec3* V, int nv, const Vec2* VT, int nvt, const Vec3* VN, int nvn,
    const Triplet* T, int ntris)
{
    size_t maxVerts = (size_t)3*ntris;
    Vertex*   VB = (Vertex*)malloc(maxVerts*sizeof(Vertex)); if(!VB){perror("malloc VB"); exit(1);}
    uint16_t* IB = (uint16_t*)malloc((size_t)3*ntris*sizeof(uint16_t)); if(!IB){perror("malloc IB"); exit(1);}

    HMap map; hmap_init(&map, (size_t)3*ntris);

    size_t vcount=0, icount=0;

    for(int i=0;i<ntris*3;i++){
        Triplet tr = T[i];
        int vi  = resolve(tr.v,  nv);
        int vti = (tr.vt!=0)? resolve(tr.vt, nvt) : -1;
        int vni = (tr.vn!=0)? resolve(tr.vn, nvn) : -1;
        if(vi<0 || vi>=nv){ fprintf(stderr,"Invalid vertex index in face.\n"); exit(1); }

        Triplet key = { vi, vti, vni };
        uint32_t idx = (uint32_t)vcount;
        int inserted = hmap_get_or_put(&map, key, &idx);

        if(inserted){
            Vec3 p = V[vi];
            uint32_t P = pack1010102(p.x, p.y, p.z);

            float uu=0.f, vv=0.f;
            if(vti>=0){ uu=VT[vti].u; vv=VT[vti].v; }
            uint16_t U=unorm16(uu), Vv=unorm16(vv);

            float nx=0.f, ny=0.f, nz=1.f;
            if(vni>=0){ nx=VN[vni].x; ny=VN[vni].y; nz=VN[vni].z; }
            uint16_t N = oct_encode_8_8(nx,ny,nz);

            VB[vcount].position=P;
            VB[vcount].u=U; VB[vcount].v=Vv;
            VB[vcount].normal=N;
            VB[vcount].bone_1=0; VB[vcount].bone_2=0;

            if(vcount>=65536){ fprintf(stderr,"Too many unique vertices for uint16 indices.\n"); exit(1); }
            vcount++;
        }
        IB[icount++] = (uint16_t)idx;
    }

    FILE* f = outpath? fopen(outpath,"wb"): stdout;
    if(!f){ perror("fopen out"); exit(1); }

    fprintf(f, "// (1 unit = 1 cm, SNORM10 range [-511, +511] cm)\n#include <stdint.h>\n\n");
    fprintf(f, "typedef struct {\n"
               "    uint32_t position;   // 10/10/10 SNORM; decode -> [-1,1]*511 cm\n"
               "    uint16_t u, v;       // UNORM16 UV\n"
               "    uint16_t normal;     // octahedral 8:8 (high=Y, low=X)\n"
               "    uint8_t  bone_1, bone_2; // unused\n"
               "} Vertex;\n\n");

    fprintf(f, "static const Vertex g_vertices_%s[%zu] = {\n", name, vcount);
    for(size_t i=0;i<vcount;i++){
        const Vertex* v=&VB[i];
        fprintf(f, "    { 0x%08Xu, %5uu, %5uu, 0x%04Xu, %3u, %3u }%s\n",
            (unsigned)v->position, (unsigned)v->u, (unsigned)v->v,
            (unsigned)v->normal, (unsigned)v->bone_1, (unsigned)v->bone_2,
            (i+1<vcount)?",":"");
    }
    fprintf(f, "};\n\n");

    fprintf(f, "static const uint16_t g_indices_%s[%zu] = {\n", name, icount);
    size_t col=0;
    for(size_t i=0;i<icount;i++){
        if(col==0) fputs("    ",f);
        fprintf(f, "%u", (unsigned)IB[i]);
        if(i+1<icount) fputs(", ",f);
        if(++col>=24){ fputc('\n',f); col=0; }
    }
    if(col) fputc('\n',f);
    fprintf(f, "};\n");

    if(outpath) fclose(f);
    free(VB); free(IB); free(map.e);
}

/* ---------- cli ---------- */
typedef struct { char* ptr; size_t len; } Dummy; /* silence if needed */

static void usage(const char* prog){
    fprintf(stderr,"Usage: %s [input.obj] [-o out.h] [--name basename]\n", prog);
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
    const char* s=b.ptr; const char* e=b.ptr + b.len;

    /* count & parse */
    Counts c = count_pass(s,e);
    if(c.nv==0 || c.ntris==0){ fprintf(stderr,"Empty/invalid OBJ.\n"); return 1; }

    Vec3* V  = (Vec3*)malloc((size_t)c.nv * sizeof(Vec3));
    Vec2* VT = (c.nvt>0)? (Vec2*)malloc((size_t)c.nvt*sizeof(Vec2)) : NULL;
    Vec3* VN = (c.nvn>0)? (Vec3*)malloc((size_t)c.nvn*sizeof(Vec3)) : NULL;
    Triplet* T = (Triplet*)malloc((size_t)c.ntris*3*sizeof(Triplet));
    if(!V || (c.nvt && !VT) || (c.nvn && !VN) || !T){ perror("malloc"); return 1; }

    parse_and_fill(s,e, V, VT, VN, c.nv, c.nvt, c.nvn, T, c.ntris);
    build_and_emit(outpath, name, V, c.nv, VT, c.nvt, VN, c.nvn, T, c.ntris);

    free(V); if(VT) free(VT); if(VN) free(VN); free(T); free(b.ptr);
    return 0;
}
