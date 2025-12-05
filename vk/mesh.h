#pragma once
#include "header.h"

extern const unsigned char BODY[];
extern const unsigned char BODY_end[];
#define BODY_len ((size_t)(BODY_end - BODY))

extern const unsigned char HEAD[];
extern const unsigned char HEAD_end[];
#define HEAD_len ((size_t)(HEAD_end - HEAD))

#define PLANE_MESHES     5
#define LOD_LEVELS       5  // hardcoded to have easy offsets in shader code
#define MAX_ANIMATIONS   8  // not hardcoded
#define ANIMATION_FRAMES 4  // hardcoded to have easy offsets in shader code

// todo: every animation frame is conceptually a different mesh like how the lod is a different mesh
// -> base mesh (#lod) -> lod level (#animation) -> animation (#frame) -> frame within animation
struct MeshFrame {
    const uint32_t *positions;  // [num_vertices]
    const uint32_t *normals;    // [num_vertices]
};

struct MeshAnimation {
    struct MeshFrame frames[ANIMATION_FRAMES];
};

struct MeshLod{
    uint32_t       num_vertices;
    uint32_t       num_indices;
    const uint16_t *indices;                        // [num_indices]
    const uint32_t *uvs;                            // [num_vertices]
    struct MeshAnimation animations[MAX_ANIMATIONS];
};

struct Mesh {
    const float    *bbox_min;                       // [3] (X,Z,Y)
    const float    *bbox_max;                       // [3] (X,Z,Y)
    const float    *radius;                         // [1]
    uint32_t       num_animations;                  // <= MAX_ANIMATIONS
    struct MeshLod lods[LOD_LEVELS];
};

static int load_mesh_blob(const void *data, size_t size, struct Mesh *mesh) {
    if (!data || !mesh) return 0;

    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + size;

#define NEED(bytes)                                \
    do {                                           \
        if ((size_t)(end - ptr) < (size_t)(bytes)) \
            return 0;                              \
    } while (0)

    // -----------------------------
    // 1. Header: "VAML", version, num_lods
    // -----------------------------
    NEED(4 + 4 + 4);
    const char *magic = (const char *)ptr;
    if (magic[0] != 'V' || magic[1] != 'A' || magic[2] != 'M' || magic[3] != 'L')
        return 0;
    ptr += 4;

    uint32_t version  = *(const uint32_t *)ptr; ptr += 4;
    uint32_t num_lods = *(const uint32_t *)ptr; ptr += 4;

    if (version != 1)            return 0;
    if (num_lods != LOD_LEVELS) return 0;

    // -----------------------------
    // 2. Metadata: per LOD (no per-animation frame counts anymore)
    // -----------------------------
    for (uint32_t lo = 0; lo < LOD_LEVELS; ++lo)
    {
        NEED(3 * sizeof(uint32_t));

        struct MeshLod *lod = &mesh->lods[lo];

        lod->num_vertices = *(const uint32_t *)ptr; ptr += 4;
        lod->num_indices  = *(const uint32_t *)ptr; ptr += 4;
        uint32_t na       = *(const uint32_t *)ptr; ptr += 4; // num_animations for this LOD

        if (na > MAX_ANIMATIONS)
            return 0;

        if (lo == 0) {
            mesh->num_animations = na;
        } else {
            // All LODs must agree on number of animations
            if (na != mesh->num_animations)
                return 0;
        }
    }

    // -----------------------------
    // 3. Body: per LOD
    // -----------------------------
    for (uint32_t lo = 0; lo < LOD_LEVELS; ++lo)
    {
        struct MeshLod *lod = &mesh->lods[lo];

        uint32_t nv = lod->num_vertices;
        uint32_t ni = lod->num_indices;
        uint32_t na = mesh->num_animations;

        // Indices
        NEED((size_t)ni * sizeof(uint16_t));
        lod->indices = (const uint16_t *)ptr;
        ptr += (size_t)ni * sizeof(uint16_t);

        // UVs
        NEED((size_t)nv * sizeof(uint32_t));
        lod->uvs = (const uint32_t *)ptr;
        ptr += (size_t)nv * sizeof(uint32_t);

        // Per-LOD bbox + radius
        NEED(3 * sizeof(float));
        mesh->bbox_min = (const float *)ptr;
        ptr += 3 * sizeof(float);

        NEED(3 * sizeof(float));
        mesh->bbox_max = (const float *)ptr;
        ptr += 3 * sizeof(float);

        NEED(sizeof(float));
        mesh->radius = (const float *)ptr;
        ptr += sizeof(float);

        // Animations
        for (uint32_t ai = 0; ai < na; ++ai)
        {
            struct MeshAnimation *anim = &lod->animations[ai];

            // New format: every animation always has exactly 4 frames.
            // Skip frame_numbers[4] (we don't store them)
            ptr += (size_t)ANIMATION_FRAMES * sizeof(uint32_t);

            // For each frame: positions[nv], normals[nv]
            for (uint32_t fi = 0; fi < ANIMATION_FRAMES; ++fi)
            {
                struct MeshFrame *frame = &anim->frames[fi];

                // positions
                NEED((size_t)nv * sizeof(uint32_t));
                frame->positions = (const uint32_t *)ptr;
                ptr += (size_t)nv * sizeof(uint32_t);

                // normals
                NEED((size_t)nv * sizeof(uint32_t));
                frame->normals = (const uint32_t *)ptr;
                ptr += (size_t)nv * sizeof(uint32_t);
            }
        }
    }

#undef NEED
    return 1;
}