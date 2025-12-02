#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

extern const unsigned char LOD_0[];
extern const unsigned char LOD_0_end[];
#define LOD_0_len ((size_t)(LOD_0_end - LOD_0))

extern const unsigned char HEAD_0[];
extern const unsigned char HEAD_0_end[];
#define HEAD_0_len ((size_t)(HEAD_0_end - HEAD_0))

struct VakfHeader
{
    char magic[4];    // "VAKF"
    uint32_t version; // 2
    uint32_t num_frames;
    uint32_t num_vertices;
    uint32_t num_indices;
};

struct VakfView
{
    const struct VakfHeader *hdr;

    const uint16_t *indices;       // num_indices
    const uint32_t *frame_numbers; // num_frames
    const float *bbox_min;         // 3 floats
    const float *bbox_max;         // 3 floats
    const uint32_t *uvs;           // num_vertices (packed 16-16 UNORM)

    // Frame data is laid out as:
    //  [Frame0 positions][Frame0 normals][Frame1 positions][Frame1 normals]...
    // each of those arrays is num_vertices uint32.
    const uint32_t *frame_data; // start of frame 0 positions
};

int vakf_view_init_from_memory(const void *data, size_t size, struct VakfView *out)
{
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + size;

    if (size < sizeof(struct VakfHeader))
        return 0;

    const struct VakfHeader *hdr = (const struct VakfHeader *)ptr;
    if (memcmp(hdr->magic, "VAKF", 4) != 0)
        return 0;
    if (hdr->version != 2)
        return 0;

    ptr += sizeof(struct VakfHeader);

    uint32_t num_frames = hdr->num_frames;
    uint32_t num_vertices = hdr->num_vertices;
    uint32_t num_indices = hdr->num_indices;

// Bounds checking helper
#define NEED(bytes)                                \
    do                                             \
    {                                              \
        if ((size_t)(end - ptr) < (size_t)(bytes)) \
            return 0;                              \
    } while (0)

    // indices (uint16)
    NEED(num_indices * sizeof(uint16_t));
    const uint16_t *indices = (const uint16_t *)ptr;
    ptr += num_indices * sizeof(uint16_t);

    // frame numbers (uint32)
    NEED(num_frames * sizeof(uint32_t));
    const uint32_t *frame_numbers = (const uint32_t *)ptr;
    ptr += num_frames * sizeof(uint32_t);

    // bbox_min (3 floats)
    NEED(3 * sizeof(float));
    const float *bbox_min = (const float *)ptr;
    ptr += 3 * sizeof(float);

    // bbox_max (3 floats)
    NEED(3 * sizeof(float));
    const float *bbox_max = (const float *)ptr;
    ptr += 3 * sizeof(float);

    // uvs (uint32, one per vertex)
    NEED(num_vertices * sizeof(uint32_t));
    const uint32_t *uvs = (const uint32_t *)ptr;
    ptr += num_vertices * sizeof(uint32_t);

    // frame data: for each frame: positions[num_vertices] + normals[num_vertices]
    size_t per_frame_count = (size_t)num_vertices * 2u;    // pos + nrm
    size_t total_frame_u32 = per_frame_count * num_frames; // in uint32 units

    NEED(total_frame_u32 * sizeof(uint32_t));
    const uint32_t *frame_data = (const uint32_t *)ptr;
    ptr += total_frame_u32 * sizeof(uint32_t);

    // optional: check we exactly consumed the blob
    // assert(ptr == end);

#undef NEED

    out->hdr = hdr;
    out->indices = indices;
    out->frame_numbers = frame_numbers;
    out->bbox_min = bbox_min;
    out->bbox_max = bbox_max;
    out->uvs = uvs;
    out->frame_data = frame_data;

    return 1;
}