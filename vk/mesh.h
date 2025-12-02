#pragma once
#include "header.h"

extern const unsigned char LOD[];
extern const unsigned char LOD_end[];
#define LOD_len ((size_t)(LOD_end - LOD))

extern const unsigned char HEAD[];
extern const unsigned char HEAD_end[];
#define HEAD_len ((size_t)(HEAD_end - HEAD))

#define LOD_LEVELS      5
#define MAX_ANIMATIONS  16
#define MAX_FRAMES      16

// todo: every animation frame is conceptually a different mesh like how the lod is a different mesh
// -> base mesh (#lod) -> lod level (#animation) -> animation (#frame) -> frame within animation
struct MeshFrame {
    const uint32_t *positions;  // [num_vertices]
    const uint32_t *normals;    // [num_vertices]
};

struct MeshAnimation {
    uint32_t num_frames;                      // <= MAX_FRAMES
    struct MeshFrame frames[MAX_FRAMES];
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
    // 2. Metadata: per LOD, per anim frame counts
    // -----------------------------
    uint32_t frame_counts[LOD_LEVELS][MAX_ANIMATIONS] = {0};

    for (uint32_t lo = 0; lo < LOD_LEVELS; ++lo)
    {
        NEED(3 * sizeof(uint32_t));

        struct MeshLod *lod = &mesh->lods[lo];

        lod->num_vertices   = *(const uint32_t *)ptr; ptr += 4;
        lod->num_indices    = *(const uint32_t *)ptr; ptr += 4;
        mesh->num_animations = *(const uint32_t *)ptr; ptr += 4;

        if (mesh->num_animations > MAX_ANIMATIONS)
            return 0;

        NEED(mesh->num_animations * sizeof(uint32_t));
        for (uint32_t ai = 0; ai < mesh->num_animations; ++ai)
        {
            uint32_t nf = *(const uint32_t *)ptr;
            ptr += 4;

            if (nf > MAX_FRAMES)
                return 0;

            frame_counts[lo][ai] = nf;
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
            uint32_t nf = frame_counts[lo][ai];
            anim->num_frames = nf;

            // Skip frame_numbers[num_frames] (we don't store them)
            NEED((size_t)nf * sizeof(uint32_t));
            ptr += (size_t)nf * sizeof(uint32_t);

            // For each frame: positions[nv], normals[nv]
            for (uint32_t fi = 0; fi < nf; ++fi)
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