#include <math.h>

static void make_perspective_vk(float fovy_rad, float aspect, float znear, float zfar, float M[16]) {
    float f = 1.0f / tanf(0.5f * fovy_rad);
    // Vulkan NDC: z in [0,1]
    M[0]=f/aspect; M[1]=0; M[2]=0;                    M[3]=0;
    M[4]=0;        M[5]=f; M[6]=0;                    M[7]=0;
    M[8]=0;        M[9]=0; M[10]=zfar/(zfar-znear);   M[11]=1.0f;
    M[12]=0;       M[13]=0; M[14]=(-znear*zfar)/(zfar-znear); M[15]=0;
}

static void make_lookat_rh(const float eye[3], const float at[3], const float up[3], float M[16]) {
    float z[3] = { at[0]-eye[0], at[1]-eye[1], at[2]-eye[2] }; // forward (+Z)
    float len = sqrtf(z[0]*z[0]+z[1]*z[1]+z[2]*z[2]); z[0]/=len; z[1]/=len; z[2]/=len;
    float x[3] = { up[1]*z[2]-up[2]*z[1], up[2]*z[0]-up[0]*z[2], up[0]*z[1]-up[1]*z[0] };
    len = sqrtf(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]); x[0]/=len; x[1]/=len; x[2]/=len;
    float y[3] = { z[1]*x[2]-z[2]*x[1], z[2]*x[0]-z[0]*x[2], z[0]*x[1]-z[1]*x[0] };

    // column-major
    M[0]=x[0]; M[1]=y[0]; M[2]= z[0]; M[3]=0;
    M[4]=x[1]; M[5]=y[1]; M[6]= z[1]; M[7]=0;
    M[8]=x[2]; M[9]=y[2]; M[10]=z[2]; M[11]=0;

    M[12]=-(x[0]*eye[0] + x[1]*eye[1] + x[2]*eye[2]);
    M[13]=-(y[0]*eye[0] + y[1]*eye[1] + y[2]*eye[2]);
    M[14]=-(z[0]*eye[0] + z[1]*eye[1] + z[2]*eye[2]); // note the minus
    M[15]=1;
}

static void print_mat4_row_major(const char* name, const float* m) {
    printf("%s (row-major rows):\n", name);
    for (int r = 0; r < 4; ++r) {
        printf("  [% .6f % .6f % .6f % .6f]\n",
            m[r*4+0], m[r*4+1], m[r*4+2], m[r*4+3]);
    }
}

static void print_mat4_col_major_as_rows(const char* name, const float* m) {
    printf("%s (column-major printed by rows):\n", name);
    for (int r = 0; r < 4; ++r) {
        printf("  [% .6f % .6f % .6f % .6f]\n",
            m[0*4+r], m[1*4+r], m[2*4+r], m[3*4+r]);
    }
}

// v' = M * v (column-major, column-vector)
static void mul_Mv_col_major(const float M[16], const float v[4], float out[4]) {
    for (int r=0;r<4;r++) {
        out[r] = M[r+0]*v[0] + M[r+4]*v[1] + M[r+8]*v[2] + M[r+12]*v[3];
    }
}

// v' = v * M (row-major, row-vector)
static void mul_vM_row_major(const float v[4], const float M[16], float out[4]) {
    for (int c=0;c<4;c++) {
        out[c] = v[0]*M[0+c] + v[1]*M[4+c] + v[2]*M[8+c] + v[3]*M[12+c];
    }
}