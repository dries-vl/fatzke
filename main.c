// cc main.c -lwayland-client
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>
#include <pthread.h>

#define _DEBUG_FPS 0

// todo: separate header for all common c stuff
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef float     f32;
typedef double    f64;
typedef intptr_t  isize;
typedef uintptr_t usize;

enum result {
    OK,
    ERROR
};

#define time_us() ({ struct timespec ts; clock_gettime(1, &ts); (long)(ts.tv_sec * 1000000L + ts.tv_nsec / 1000L); })
#define elapsed_us(start) (time_us() - (start))

#include "wayland/wayland.c" // sudo apt install libwayland-dev

#pragma region CAMERA
// todo: this needs to match the data in the tga files, but needs to be in scenario/save data (and also not global in code but passed via eg. a struct)
#define GRID_W 50
#define GRID_H 50
#define TILE_SIZE 32
#define ATLAS_SIZE 8

struct camera {
    i32 tile_x, tile_y; // (top left) camera position on the map
    u32 end_x, end_y; // (down right) furthest visible tile on the map
    u32 buffer_w, buffer_h; // the size of the frame buffer (in pixels)
    u32 display_w, display_h; // size of the original display (in pixels), to keep track of the size we need to scale to (off by one stride means we cannot just x2)
    u32 zoom;
    u32 update; // the camera has moved this frame
};

void move_camera(struct camera *camera, i32 delta_x, i32 delta_y) {
    u32 legal_delta_x = delta_x;
    u32 legal_delta_y = delta_y;
    if (camera->tile_x + delta_x < 0) legal_delta_x = 0;
    if (camera->end_x + delta_x > (GRID_W - 1)) legal_delta_x = 0;
    if (camera->tile_y + delta_y < 0) legal_delta_y = 0;
    if (camera->end_y + delta_y > (GRID_H - 1)) legal_delta_y = 0;
    camera->tile_x += legal_delta_x;
    camera->end_x += legal_delta_x;
    camera->tile_y += legal_delta_y;
    camera->end_y += legal_delta_y;
    camera->update = 1;
}

// todo: pass to callbacks instead of global
#pragma endregion

// todo: these hardcoded globals need to be configurable in data instead and not global
#define MAX_UNITS 128 // max number of units per player
#define BUCKET_COUNT 8 // number of buckets for resolve order
#define BUCKET_SIZE MAX_UNITS // number of steps per bucket

#define WHITE 0xFFFFFFFF
#define RED 0xFFFF0000
#define GREEN 0xFF00FF00
#define YELLOW 0xFFFFFF00

#define MATRIX(name, type, rows, cols, ...) \
    static const type name##_flat[] = { __VA_ARGS__ }; \
    _Static_assert(sizeof(name##_flat)/sizeof(type) == (rows)*(cols), #name " : need " #rows " x " #cols " entries (edit the list)"); \
    static const type (*const name)[cols] = (const type (*)[cols])name##_flat

struct tga { 
    u32 w, h; // dimensions
    u32 *pix; // pointer to pixel data
    const void *map; // handle
    size_t map_len; // keep track of length for unmapping later
};

#pragma region TILES
struct tga map;
enum tiles {
    SEA,
    CITY,
    MOUNTAINS,
    CROPLAND,
    PLAINS,
    FOREST,
    TILE_COUNT    
};
u32 tile_colors[TILE_COUNT] = {
    [SEA] = 0xFF90ccd4,
    [CITY] = 0xFFff1a1a,
    [MOUNTAINS] = 0xFF674616,
    [CROPLAND] = 0xFFc79720,
    [PLAINS] = 0xFF307118,
    [FOREST] = 0xFF21480e
};
u32 get_tile(u32 x, u32 y) {
    assert(x >= 0 && x < GRID_W && "x is out of bounds");
    assert(y >= 0 && y < GRID_H && "y is out of bounds");
    u32 tile_color = map.pix[y * map.w + x];
    for (u32 i = 0; i < TILE_COUNT; i++)
        if (tile_colors[i] == tile_color)
            return i;
    printf("Tile not found for color: 0x%08X, tile: %d, %d\n", tile_color, x, y);
    return UINT32_MAX;
}; 
u32 tile_income[TILE_COUNT] = {
    [SEA] = 0,
    [CITY] = 1,
    [MOUNTAINS] = 0,
    [CROPLAND] = 0,
    [PLAINS] = 0,
    [FOREST] = 0
};
#pragma endregion

#pragma region PLAYERS
struct tga players;
enum players {
    GERMANY,
    SOVIET,
    PLAYER_COUNT
};
u32 player_colors[PLAYER_COUNT] = {
    [GERMANY] = 0xFF6a3e0d,
    [SOVIET] = 0xFF6a0d33
};
u32 get_player(u32 x, u32 y) {
    u32 player_color = players.pix[y * players.w + x];
    for (u32 i = 0; i < PLAYER_COUNT; i++)
        if (player_colors[i] == player_color)
            return i;
    printf("Player not found\n");
    return UINT32_MAX;
}; 
u32 player_cities[PLAYER_COUNT] = {0};
u32 player_money[PLAYER_COUNT] = {0};
#pragma endregion

#pragma region UNITS
struct tga units;
enum units {
    INFANTRY,
    MOTORIZED,
    ARMOR,
    UNIT_COUNT
};
u32 unit_colors[UNIT_COUNT] = {
    [INFANTRY] = 0xFF000000,
    [MOTORIZED] = 0xFF383838,
    [ARMOR] = 0xFF6f6f6f,
};
u32 get_unit(u32 x, u32 y) {
    u32 unit_color = units.pix[y * units.w + x];
    for (u32 i = 0; i < UNIT_COUNT; i++)
        if (unit_colors[i] == unit_color)
            return i;
    printf("Unit not found for color: 0x%08X, unit: %d, %d\n", unit_color, x, y);
    printf("Unit not found\n");
    return UINT32_MAX;
}; 
u32 unit_cost[UNIT_COUNT] = {
    [INFANTRY] = 100,
    [MOTORIZED] = 200,
    [ARMOR] = 400,
};
MATRIX(movement_cost, u32, UNIT_COUNT, TILE_COUNT,
/*               SEA,    CITY,  MNT,  CRO,  PLN,  FST  */
/*INFANTRY */ UINT32_MAX,  100,   UINT32_MAX,    100,    100,    200,
/*MOTORIZED*/ UINT32_MAX,  100,     UINT32_MAX,    100,    100,    200,
/*ARMOR    */ UINT32_MAX,  100,     UINT32_MAX,    100,    100,    200
);

#pragma endregion

typedef struct {
    u32 x, y;
} pos;

enum directions {
    UP,
    DOWN,
    LEFT,
    RIGHT,
    UP_LEFT,
    UP_RIGHT,
    DOWN_LEFT,
    DOWN_RIGHT,
    DIRECTIONS_COUNT
};
pos dir_offsets[] = {
    [UP] = {0, -1},
    [DOWN] = {0, 1},
    [LEFT] = {-1, 0},
    [RIGHT] = {1, 0},
    [UP_LEFT] = {-1, -1},
    [UP_RIGHT] = {1, -1},
    [DOWN_LEFT] = {-1, 1},
    [DOWN_RIGHT] = {1, 1}
};

struct unit {
    u32 x, y; // position on grid
    u32 type;
};
struct path {
    u8 steps[GRID_H + GRID_W]; // array of steps
    u8 length; // number of steps
    // u8 id; // unit id
};
struct step {
    u8  dir;
    u16 cost;
    u16 id; // unit id + player id
};

struct path player_paths[PLAYER_COUNT][MAX_UNITS] = {0};
struct step resolve_order[BUCKET_COUNT][BUCKET_SIZE] = {0};
u32 bucket_size[BUCKET_COUNT] = {0}; // number of steps in each bucket

u32 player_unit_count[PLAYER_COUNT] = {0}; // size of array NOTE NOT ALWAYS NUMBER OF UNITS
struct unit player_units[PLAYER_COUNT][MAX_UNITS] = {0};

struct unit player_target[PLAYER_COUNT] = {{0, 0, 0}, {0, 0, 0}};

#pragma region INPUT
#define KEY_COUNT 256
u32 pressed_keys[KEY_COUNT];
static void key_input_callback(void *ud, u32 key, u32 state) {
    if (state) {
        pressed_keys[key] = 1;
        printf("KEY PRESSED: %d\n", key);
    } else {
        pressed_keys[key] = 0;
        printf("KEY RELEASED: %d\n", key);
    }
}

static void mouse_input_callback(void *ud, i32 x, i32 y, u32 b) {
    printf("button %u\n", b);
    printf("pointer at %d,%d\n", x, y);
}

void process_input(struct camera *camera) {
    if (pressed_keys[1]) { // esc
        exit(0);
    }
    if (pressed_keys[35]) { // h
        move_camera(&camera, -1, 0);
    }
    if (pressed_keys[36]) { // j
        move_camera(&camera, 0, 1);
    }
    if (pressed_keys[37]) { // k
        move_camera(&camera, 0, -1);
    }
    if (pressed_keys[38]) { // l
        move_camera(&camera, 1, 0);
    }
}
#pragma endregion

i32 battle(u32 attacker, u32 defender, u32 unit_att, u32 unit_def);

i32 pathing(u32 from_x, u32 from_y, u32 to_x, u32 to_y, u8 *path, u32 *pathlength, u32 unit_type, u32 player) { // no cost yet, BITshift versie??
    if (!path || !pathlength) return -1; // Invalid arguments
    if (from_x < 0 || from_x >= GRID_W || from_y < 0 || from_y >= GRID_H ||
        to_x < 0 || to_x >= GRID_W || to_y < 0 || to_y >= GRID_H) {
        return -1; // Invalid coordinates
    }
    u32 paths_count = 1;
    u8 paths[GRID_W * GRID_H][GRID_H + GRID_W * 2] = {0}; // STACK OVERFLOW ?????
    paths[0][0] = 0; // path length
    paths[0][1] = 0; // start direction is never used
    u32 costs[GRID_W * GRID_H] = {0}; // costs of paths
    costs[0] = 0; // start cost is 0
    pos visited[GRID_W * GRID_H] = {0}; // visited tiles
    visited[0] = (pos){from_x, from_y}; // mark starting tile as visiteds
    for (u32 i = 0; i < paths_count; i++) {
        //printf("Checking path %d: (%d, %d)\n", i + 1, visited[i].x, visited[i].y);
        u32 pass = 0;
        if (paths_count >= GRID_W * GRID_H) {
            printf("Too many paths, aborting\n");
            return 0; // ran out of memory
        }
        for (u32 dir = UP; dir <= DOWN_RIGHT; dir++){
            pass = 0; // reset pass for each direction
            // Generic
            u32 x = from_x;
            u32 y = from_y;
            u32 total_cost = 0;
            for (i32 step = paths[i][0] - 1; step >= 0; step--) {
                x += dir_offsets[paths[i][step + 1]].x;
                y += dir_offsets[paths[i][step + 1]].y;
                if (movement_cost[unit_type][get_tile(x, y)] >= 10000000) {printf("step: %d", step); break;} // check for impassable tile
                total_cost += movement_cost[unit_type][get_tile(x, y)];
            }
            x += dir_offsets[dir].x;
            y += dir_offsets[dir].y;
            //printf("Step %d: (%d, %d) ", dir, x, y);
            if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) continue; // out of bounds
            u32 terrain = get_tile(x, y);
            u32 cost = movement_cost[unit_type][terrain];
            if (cost == UINT32_MAX) {continue;} // check for impassable tile
            total_cost += cost;
            if (units.pix[y * units.w + x] == player_colors[player]) {continue;} // check for player unit
            if (to_x == x && to_y == y) { // found target
                paths[paths_count][0] = paths[i][0] + 1; // technicly ruins path order
                paths[paths_count][1] = dir;
                costs[paths_count] = total_cost; // set cost of path
                for (u32 j = 2; j < paths[i][0] + 2; j++) { // copy path from previous tile
                    paths[paths_count][j] = paths[i][j-1];
                }
                *pathlength = paths[paths_count][0]; // set path length
                memcpy(path, paths[paths_count] + 1, sizeof(u8) * (*pathlength)); // copy path to output
                // printf("Found path to (%d, %d) with cost %d\n", x, y, total_cost);
                return 1; // found path
            }
            if (units.pix[y * units.w + x] != 0 && units.pix[y * units.w + x] != player_colors[1 - player]) {continue;} // if enemy on path path around
            for (u32 j = 0; j < paths_count; j++) {
                if (visited[j].x == x && visited[j].y == y) {
                    pass = 1;
                    //printf("Tile (%d, %d) already visited\n", x, y);
                    break;
                }
            }
            if (pass == 0) {
                bool found = false;
                for (u32 tile = i; tile < paths_count; tile++) {
                    //printf("Checking tile %d: (%d, %d)\n", tile, x, y);
                    if (paths[tile][0] == 0) continue; // skip empty paths
                    if (costs[tile] > total_cost) { // higher cost than existing path
                        found = true;
                        for (u32 k = paths_count - 1; k >= tile; k--) { // shift paths up AND COSTS
                            memcpy(paths[k + 1], paths[k], sizeof(uint8_t) * 150);
                            costs[k + 1] = costs[k];
                        }
                        paths[tile][0] = paths[i][0] + 1; // set path length
                        if (paths[tile][0] == 255) {printf("too long path.");return 0;}
                        paths[tile][1] = dir; // add tile to path
                        costs[tile] = total_cost; // set cost of path
                        for (u32 j = 2; j < paths[i][0] + 2; j++) {
                            paths[tile][j] = paths[i][j-1];
                        }
                        visited[paths_count] = (pos){x, y}; // mark tile as visited
                        paths_count++;
                        break;
                    }
                }
                if (!found) {
                    paths[paths_count][0] = paths[i][0] + 1; // set path length
                    if (paths[paths_count][0] == 255) {printf("too long path.");return 0;}
                    paths[paths_count][1] = dir; // add tile to path
                    costs[paths_count] = total_cost; // set cost of path
                    for (u32 j = 2; j < paths[i][0] + 2; j++) {
                        paths[paths_count][j] = paths[i][j-1];
                    }
                    visited[paths_count] = (pos){x, y}; // mark tile as visited
                    paths_count++;
                    //printf("Path %d: ", paths_count);
                }
            }
        }
    }
    return 0; // no path found
}

static inline u32 mix_colors(u32 a, u32 b) {
    return (((a ^ b) & 0xFEFEFEFEU) >> 1U) + (a & b);
}

static inline void draw_unit(struct camera camera, struct tga units_atlas, u32 tile_y, u32 tile_x, u32 *restrict buffer) {
    const u32 start_y = (tile_y - camera.tile_y) * TILE_SIZE + 5;
    const u32 start_x = (tile_x - camera.tile_x) * TILE_SIZE + 5;
    const enum units unit = get_unit(tile_x, tile_y);
    const u32 atlas_start_x = (unit % ATLAS_SIZE) * TILE_SIZE;
    const u32 atlas_start_y = (unit / ATLAS_SIZE) * TILE_SIZE;

    for (u32 i = 0; i < TILE_SIZE-6; ++i) {
        u32 *buffer_location = buffer + (start_y + i) * camera.buffer_w + start_x;
        const u32 *atlas_location = units_atlas.pix + (atlas_start_y + i) * units_atlas.w + atlas_start_x;
        memcpy(buffer_location, atlas_location, (TILE_SIZE-6) * sizeof(u32));
    }
}

void draw_units(struct camera camera, struct tga units_atlas, u32 *buffer) {
    for (u32 player = 0; player < PLAYER_COUNT; player++) {
        if (player_unit_count[player] == 0) continue; // skip empty players
        for (u32 unit = 0; unit < player_unit_count[player]; ++unit) {
            if (player_units[player][unit].type == UINT32_MAX) continue; // skip empty unit slots
            u32 tile_x = player_units[player][unit].x;
            u32 tile_y = player_units[player][unit].y;
            if (tile_x < camera.tile_x || tile_y < camera.tile_y) continue;
            if (tile_x > camera.end_x || tile_y > camera.end_y) continue;
            draw_unit(camera, units_atlas, tile_y, tile_x, buffer);
        }
    }
}

static inline void draw_tile(struct camera camera, struct tga map_atlas, u32 tile_y, u32 tile_x, u32 *restrict terrainbuffer) {
    const u32 start_y = (tile_y - camera.tile_y) * TILE_SIZE;
    const u32 start_x = (tile_x - camera.tile_x) * TILE_SIZE;
    const enum tiles tile = get_tile(tile_x, tile_y);
    const u32 atlas_start_x = (tile % ATLAS_SIZE) * TILE_SIZE;
    const u32 atlas_start_y = (tile / ATLAS_SIZE) * TILE_SIZE;

    for (u32 i = 0; i < TILE_SIZE; ++i) {
        u32 *dest = terrainbuffer + (start_y + i) * camera.buffer_w + start_x;
        const u32 *src = map_atlas.pix + (atlas_start_y + i) * map_atlas.w + atlas_start_x;
        memcpy(dest, src, TILE_SIZE * sizeof(u32));
    }
}

void draw_terrain(struct camera camera, struct tga map_atlas, u32 *terrainbuffer) {
    // loop over all the visible tiles
    for (u32 y = camera.tile_y; y <= camera.end_y; ++y) {
        for (u32 x = camera.tile_x; x <= camera.end_x; ++x) {
            draw_tile(camera, map_atlas, y, x, terrainbuffer);
        }
    }
}

void draw_step(struct camera camera, enum directions direction, u32 tile_y, u32 tile_x, u32 w, u32 h, u32 *buffer) {
    // calculate the rect in the buffer that we need to draw this tile in
    i32 start_y = (tile_y * TILE_SIZE) - (camera.tile_y * TILE_SIZE);
    i32 start_x = (tile_x * TILE_SIZE) - (camera.tile_x * TILE_SIZE);
    i32 end_y = start_y + TILE_SIZE;
    i32 end_x = start_x + TILE_SIZE;
    assert(start_x >= 0 && start_y >= 0 && "Negative drawing position");
    // if this tile goes out of bounds (last tile), draw only to the end of the buffer instead
    if (end_y > h) end_y = h;
    if (end_x > w) end_x = w;
    // loop over the range we calculated above
    for (u32 y = start_y; y < end_y; ++y) {
        for (u32 x = start_x; x < end_x; ++x) {
            buffer[y * w + x] = mix_colors(0xFFFFFFFF, player_colors[direction]);
        }
    }
}

void draw_turn(struct camera camera, u32 *buffer){
    pos unit_locations[PLAYER_COUNT][MAX_UNITS] = {0}; // keep track of unit locations
    for (u32 bucket = 0; bucket < BUCKET_COUNT; bucket ++) {
        if (bucket_size[bucket] == 0) {continue;} // skip empty buckets
        for (u32 step = 0; step < bucket_size[bucket]; step++) {
            enum players player = resolve_order[bucket][step].id >> 8; // get player from id
            u32 unit = resolve_order[bucket][step].id % 256; // get unit from id
            if (player_units[player][unit].type == 0) continue; // skip empty unit slots
            pos loc;
            if (unit_locations[player][unit].x != 0 || unit_locations[player][unit].y != 0) {
                loc = unit_locations[player][unit]; // use cached location
            }
            else {
                loc = (pos){player_units[player][unit].x, player_units[player][unit].y}; // start location
            }
            u8 dir = resolve_order[bucket][step].dir; // get direction from path
            u32 x = loc.x + dir_offsets[dir].x; // calculate x position
            u32 y = loc.y + dir_offsets[dir].y; // calculate y position
            if ((x+1) * TILE_SIZE > camera.buffer_w || (y+1) * TILE_SIZE > camera.buffer_h) continue; // don't draw beyond the visible grid
            if (x < camera.tile_x || y < camera.tile_y) continue;
            draw_step(camera, dir, y, x, camera.buffer_w, camera.buffer_h, buffer);
            unit_locations[player][unit] = (pos){x, y}; // update location
        }
    }
}

i32 move_unit(u32 player, u32 unit, u32 to_x, u32 to_y) {
    if (!player_units[player] || unit < 0 || unit >= player_unit_count[player]) {
        printf("Invalid player or unit index\n");
        return -1; // Invalid player or unit
    }
    if (player_units[player][unit].type == 0) {
        printf("Unit %d of player %d is empty\n", unit, player);
        return -5; // Unit is not active
    }
    if (to_x < 0 || to_x >= GRID_W || to_y < 0 || to_y >= GRID_H) {
        printf("Invalid move to (%d, %d)\n", to_x, to_y);
        return -2; // Invalid move
    }
    if (map.pix[to_y * map.w + to_x] == SEA) {
        printf("Cannot move to water tile (%d, %d)\n", to_x, to_y);
        return -4; // Cannot move to water tile
    }
    if (units.pix[to_y * units.w + to_x] != 0 && units.pix[to_y * units.w + to_x] != player_colors[player]) { // BATTLE!
        //printf("Tile (%d, %d) occupied by enemy\n", to_x, to_y);
        // printf("Battle at (%d, %d)!\n", to_x, to_y);
        for (u32 defender = 0; defender < player_unit_count[1 - player]; defender++) {
            if (player_units[1 - player][defender].x == to_x && player_units[1 - player][defender].y == to_y) {
                return battle(player, 1 - player, unit, defender);
            }
        }
    }
    if (units.pix[to_y * units.w + to_x] != 0) {
        //printf("Tile (%d, %d) already occupied\n", to_x, to_y);
        return -3; // Tile already occupied
    }
    u32 from_x = player_units[player][unit].x;
    u32 from_y = player_units[player][unit].y;
    // move unit from old location to new location
    units.pix[to_y * units.w + to_x] = units.pix[from_y * units.w + from_x];
    units.pix[from_y * units.w + from_x] = 0;
    assert(units.pix[to_y * units.w + to_x] != 0 && "Unit moved to same location as before, and created inconsistency\n");
    // set the value of the unit in the player units array
    player_units[player][unit].x = to_x;
    player_units[player][unit].y = to_y;
    // change tile ownership to this player
    enum players to_player = get_player(to_x, to_y);
    if (to_player != player) {
        players.pix[to_y * players.w + to_x] = player_colors[player]; // Update country color
        u32 income = tile_income[get_tile(to_x, to_y)];
        if (income > 0) {
            printf("player %d conquered city from player %d\n", player, to_player);
            player_cities[to_player] -= income;
            player_cities[player] += income;
        }
    }
    return 0; // Move successful
}

i32 resolve_turn(){
    printf("Resolving turn...\n");
    u32 blocked_units[PLAYER_COUNT][MAX_UNITS] = {0}; // keep track of blocked units
    for (u32 bucket = 0; bucket < BUCKET_COUNT; bucket++) {
        if (bucket_size[bucket] == 0) continue; // skip empty buckets
        // Sort the steps in the bucket by cost MAYBE NEEDED
        // Execute the steps in the bucket
        for (u32 step = 0; step < bucket_size[bucket]; step++) {
            
            enum players player = resolve_order[bucket][step].id >> 8; // get player from id
            u32 unit = resolve_order[bucket][step].id % 256; // get unit from id
            if (blocked_units[player][unit] || player_units[player][unit].type == 0) { continue; } // skip blocked and empty units
            u8 dir = resolve_order[bucket][step].dir; // get direction from path
            u32 x = player_units[player][unit].x + dir_offsets[dir].x; // calculate x position
            u32 y = player_units[player][unit].y + dir_offsets[dir].y; // calculate y position
            i32 result = move_unit(player, unit, x, y); // move unit
            if (result != 0) {
                blocked_units[player][unit] = 1; // mark unit as blocked
            }
        }
        bucket_size[bucket] = 0; // reset bucket size
        memset(resolve_order[bucket], 0, sizeof(struct step) * BUCKET_SIZE); // clear resolve order
    }

    return 0;
}

i32 add_unit(u32 player, enum units unit, u32 x, u32 y) {
    if (player >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return -1; // Invalid player
    }
    if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) {
        printf("Invalid position (%d, %d)\n", x, y);
        return -2; // Invalid position
    }
    if (player_unit_count[player] >= MAX_UNITS) {
        // printf("Max units reached for player %d\n", player);
        return -3; // Max units reached
    }
    if (units.pix[y * units.w + x] != 0) {
        // printf("Tile (%d, %d) already occupied\n", x, y);
        return -4; // Tile already occupied
    }
    assert(unit_colors[unit] != 0 && "Unit is invalid, has color & alpha both set to zero");
    for (u32 i = 0; i < player_unit_count[player]; ++i) { // Reuse empty slot in player_units
        if (player_units[player][i].type == UINT32_MAX) {
            player_units[player][i] = (struct unit){x, y, unit};
            units.pix[y * units.w + x] = unit_colors[unit]; // add the unit to the map
            return 0; // Unit added successfully
        }
    }
    player_units[player][player_unit_count[player]] = (struct unit){x, y, unit};
    units.pix[y * units.w + x] = unit_colors[unit]; // add the unit to the map
    player_unit_count[player]++;
    return 0; // Unit added successfully
}

i32 remove_unit(u32 player, u32 unit) {
    if (player < 0 || player >= PLAYER_COUNT || unit < 0 || unit >= player_unit_count[player]) {
        printf("Invalid player or unit index\n");
        return -1; // Invalid player or unit
    }
    if (player_units[player][unit].type == 0) {
        printf("Unit %d of player %d is empty\n", unit, player);
        return -2; // Unit is not active
    }
    u32 x = player_units[player][unit].x;
    u32 y = player_units[player][unit].y;
    units.pix[y * units.w + x] = 0; // Remove unit from grid
    player_units[player][unit] = (struct unit){0, 0, 0}; // Clear unit data NO REORDERING
    return 0; // Unit removed successfully
}

i32 battle(u32 attacker, u32 defender, u32 unit_att, u32 unit_def) {
    if (attacker < 0 || attacker >= PLAYER_COUNT || defender < 0 || defender >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return -1; // Invalid player
    }
    if (unit_att < 0 || unit_att >= player_unit_count[attacker] || unit_def < 0 || unit_def >= player_unit_count[defender]) {
        printf("Invalid unit index\n");
        return -2; // Invalid unit
    }
    if (player_units[attacker][unit_att].type == 0 || player_units[defender][unit_def].type == 0) {
        printf("empty units\n");
        return -2; // Unit is not active
    }
    u32 x_att = player_units[attacker][unit_att].x;
    u32 y_att = player_units[attacker][unit_att].y;
    u32 x_def = player_units[defender][unit_def].x;
    u32 y_def = player_units[defender][unit_def].y;
    if (abs(x_att - x_def) > 1 || abs(y_att - y_def) > 1) {
        printf("Units not adjacent\n");
        return -3; // Units not adjacent
    }
    return 3; // force draw
    i32 random = rand() % 20; // Random number between 0 and 5
    if (random == 20) { // Attacker wins 1/3
        //printf("Attacker wins!\n");
        remove_unit(defender, unit_def); // Remove defender unit
        move_unit(attacker, unit_att, x_def, y_def); // Move attacker to defender position
        return 1; // Attacker wins
    } else if (random < 3) { // Defender wins 1/3
        //printf("Defender wins!\n"); // Defender wins 2/3
        remove_unit(attacker, unit_att); // Remove attacker unit
        return 2; // Defender wins
    } else { // Draw 1/3
        //printf("Draw!\n");
        return 3; // Draw
    }
    return 0; // something went wrong
}

i32 commit_turn(enum players player) {
    if (player < 0 || player >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return -1; // Invalid player
    }
    // Resolve all paths for the player
    for (u32 unit = 0; unit < player_unit_count[player]; ++unit) {
        if (player_paths[player][unit].length == 0) continue; // skip empty paths
        u32 cost = 0; // cost is cummulative
        u32 x = player_units[player][unit].x;
        u32 y = player_units[player][unit].y;
        for (u32 step = 0; step < player_paths[player][unit].length; ++step) {
            // calculate cost of the step
            x = x + dir_offsets[player_paths[player][unit].steps[step]].x;
            y = y + dir_offsets[player_paths[player][unit].steps[step]].y;
            u32 tile = get_tile(x, y);
            cost += movement_cost[player_units[player][unit].type][tile]; // tile cost
            if (cost > 400) { break; } // max cost
            u32 bucket = cost / 100; // bucket for the step
            resolve_order[bucket][bucket_size[bucket]] = (struct step){
                .dir = player_paths[player][unit].steps[step] , // direction of the step
                .cost = cost, // cost of the step
                .id = (u16)(unit + (player << 8)) // unit id + player id
            };
            bucket_size[bucket]++; // increment bucket size
        }
    }
    // printf("Player %d committed turn with %d units\n", player, player_unit_count[player]);
    return 0; // Turn committed successfully

}

i32 move_towards(u32 player, u32 unit, u32 target_x, u32 target_y) {
    if (player < 0 || player >= PLAYER_COUNT || unit < 0 || unit >= player_unit_count[player]) {
        printf("Invalid player or unit index\n");
        return -1; // Invalid player or unit
    }
    if (target_x < 0 || target_x >= GRID_W || target_y < 0 || target_y >= GRID_H) {
        printf("Invalid target position (%d, %d)\n", target_x, target_y);
        return -2; // Invalid target position
    }
    u32 x = player_units[player][unit].x;
    u32 y = player_units[player][unit].y;
    u32 dir_x = (target_x - x) < 0 ? -1 : (target_x - x) > 0 ? 1 : 0; // get direction x-axis
    u32 dir_y = (target_y - y) < 0 ? -1 : (target_y - y) > 0 ? 1 : 0; // get direction y-axis
    i32 result =  move_unit(player, unit, x + dir_x, y + dir_y);
    if (result == -3) { // Tile already occupied
        // Try to move in the other random direction
        i32 random = rand() % 2; 
        if (dir_x == 0) {
            result = move_unit(player, unit, x + 1 - random*2, y + dir_y);
        } else if (dir_y == 0) {
            result = move_unit(player, unit, x + dir_x, y + 1 - random*2);
        } else if (dir_x != 0 && dir_y != 0) {
            // If both directions are available, try to move in the other direction
            result = move_unit(player, unit, x + dir_x*random, y + dir_y*(1 - random));
        }
    }
    return result;
}

void find_front(u32 player, u32 center_x, u32 center_y, struct unit *front_units, u32 *count) {
    if (player < 0 || player >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return; // Invalid player
    }
    for (u32 unit = 0; unit < player_unit_count[player]; unit++) {
        if (player_units[player][unit].type == 0) continue; // skip empty unit slots
        u32 x = player_units[player][unit].x;
        u32 y = player_units[player][unit].y;
        if (*count >= MAX_UNITS) {
            printf("Front units array full\n");
            return; // Front units array full
        }
        else {
            front_units[*count] = player_units[player][unit];
            (*count)++;
        }
    }
    return;
}

i32 find_target(u32 player, u32 unit, struct unit *target) {
    if (player < 0 || player >= PLAYER_COUNT || unit < 0 || unit >= player_unit_count[player]) {
        printf("Invalid player or unit index\n");
        return -1; // Invalid player or unit
    }
    u32 x = player_units[player][unit].x;
    u32 y = player_units[player][unit].y;
    if ((units.pix[y * units.w + x+1] != 0 && units.pix[y * units.w + x+1] != player_colors[player]) && x+1 < GRID_W) {
        target->type = 1; target->x = x+1; target->y = y;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    if ((units.pix[y * units.w + x-1] != 0 && units.pix[y * units.w + x-1] != player_colors[player]) && x-1 >= 0) {
        target->type = 1; target->x = x-1; target->y = y;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    if ((units.pix[(y+1) * units.w + x] != 0 && units.pix[(y+1) * units.w + x] != player_colors[player]) && y+1 < GRID_H) {
        target->type = 1; target->x = x; target->y = y+1;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    if ((units.pix[(y-1) * units.w + x] != 0 && units.pix[(y-1) * units.w + x] != player_colors[player]) && y-1 >= 0) {
        target->type = 1; target->x = x; target->y = y-1;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    return 0; // No target found
}

i32 spawn_unit(enum players player) {
    // try to spawn around a unit
    for (u32 unit_id = 0; unit_id < player_unit_count[player]; unit_id++) {
        if (player_units[player][unit_id].type == 0) continue; // skip empty unit slots
        struct unit unit = player_units[player][unit_id];
        for (u32 dir = 0; dir < DIRECTIONS_COUNT; dir++) {
            u32 spawn_x = unit.x + dir_offsets[dir].x;
            u32 spawn_y = unit.y + dir_offsets[dir].y;
            if (spawn_y >= 0 && spawn_x >= 0 && spawn_y < units.w && spawn_x < units.w) {
                bool has_unit = units.pix[spawn_y * units.w + spawn_x] != 0;
                bool is_sea = map.pix[spawn_y * map.w + spawn_x] == SEA;
                if (!has_unit && !is_sea && get_player(spawn_x, spawn_y) == player) {
                    i32 result = add_unit(player, INFANTRY, spawn_x, spawn_y);
                    if (result == 0) {
                        return 1;
                    } else if (result == -3) {
                        return -1;
                    }
                }
            }
        }
    }
    return -1;
}

i32 ai_unit_movement(enum players player) {
    // AI logic to move units towards enemy units
    if (player < 0 || player >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return -1; // Invalid player
    }
    // unit movement
    struct unit front_units[MAX_UNITS];
    u32 count = 0;
    find_front(1 - player, 0, 0, front_units, &count); // Get front units for other player
    for (u32 unit = 0; unit < player_unit_count[player]; unit++) {
        memset(player_paths[player][unit].steps, 0, sizeof(player_paths[player][unit].steps)); // clear array
        player_paths[player][unit].length = 0;
        if (player_units[player][unit].type == 0) continue; // skip empty unit slots
        u32 x = player_units[player][unit].x;
        u32 y = player_units[player][unit].y;
        u32 distance = 2500;
        struct unit target = {0};
        bool found_target = false;
        for (u32 enemy = 0; enemy < count; enemy++) {
            u32 dis = (front_units[enemy].x - x) * (front_units[enemy].x - x) +
                        (front_units[enemy].y - y) * (front_units[enemy].y - y);
            if (dis < distance) {
                distance = dis;
                target = front_units[enemy];
                found_target = true;
            }
        }
        u8 path[GRID_W + GRID_H];
        u32 path_length = 0;
        i32 result = 0;
        if (found_target) {
            result = pathing(x, y, target.x, target.y, path, &path_length, 1, player); // Get path to target UNIT_TYPE
            if (result == 1) {
                player_paths[player][unit].length = path_length;
                for (u32 step = 0; step < path_length; step++) {
                    player_paths[player][unit].steps[step] = path[path_length - step - 1];
                    //printf("Step added %d: (%d, %d)\n", step, player_paths[player][unit].steps[step].x, player_paths[player][unit].steps[step].y);
                }
            }
            else {
                player_paths[player][unit].length = 0; // No path found
                //printf("No steps: (%d, %d)\n", player_paths[player][unit].steps[0].x, player_paths[player][unit].steps[0].y);
            }
        }
        else {
            player_paths[player][unit].length = 0; // No path found
            //printf("No steps: (%d, %d)\n", player_paths[player][unit].steps[0].x, player_paths[player][unit].steps[0].y);
        }
    }
    commit_turn(player);
    return 0; // AI movement done
}

void player_turn(enum players player) {
    // verify that the player exists in the player enum
    if (player < 0 || player >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return; // Invalid player
    }
    // add 1 money for every city
    //printf("Cities (player %d): %d\n", player, player_cities[player]);
    player_money[player] += player_cities[player];
    // buy units for every 10 money
    static const u32 UNIT_COST = 100;
    u32 units_to_buy = player_money[player] / UNIT_COST;
    u32 money_to_use = units_to_buy * UNIT_COST;
    player_money[player] -= money_to_use;
    for (u32 i = 0; i<units_to_buy; i++) { 
        if (spawn_unit(player) == -1) 
            player_money[player] += UNIT_COST; // refund if cannot be spawned anywhere
    }
    
    ai_unit_movement(player); // BIK
}

void *script(void *arg) {
    u32 scrpt_frame = 0;
    while (true) {
        u64 us_scrpt = time_us();
        if (scrpt_frame % 20 == 1) {
            player_turn(0);
        }
        if (scrpt_frame % 20 == 5) {
            player_turn(1);
        }
        if (scrpt_frame % 20 == 15) {
            resolve_turn();
        }
        struct timespec ts = {0, 16 * 1000000};
        if (elapsed_us(us_scrpt) >= 0) {
            printf("%ld ms_scrpt\n", elapsed_us(us_scrpt));
        }
        nanosleep(&ts, NULL);
        scrpt_frame++;
    }
}

static inline struct tga tga_load(const char *path)
{
    u32 fd = open(path, O_RDONLY | 02000000);
    if (fd < 0) {fprintf(stderr, "File not found: %s\n", path); exit(1);}

    u8 h18[18];
    assert(read(fd, h18, 18) == 18);

    /* ─── validate header ────────────────────────────────────────── */
    if (h18[2]  != 2)   { fprintf(stderr,"TGA type ≠ 2 for %s\n", path);          exit(1); }
    if (h18[16] != 32){ fprintf(stderr,"TGA bpp ≠ 32 (got %u) for %s\n",h18[16], path);exit(1);}
    if ((h18[17] & 0x20) == 0){ fprintf(stderr,"TGA not top-left for %s\n", path); exit(1); }

    u32 w = h18[12] | h18[13]<<8;
    u32 h = h18[14] | h18[15]<<8;

    size_t off   = 18 + h18[0];              /* header + ID-field      */
    size_t bytes = (size_t)w * h * 4;    /* 4 bytes per pixel      */

    struct stat st;
    fstat(fd,&st);
    assert(off + bytes <= (size_t)st.st_size);

    void *map = mmap(0, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    assert(map != MAP_FAILED);
    close(fd);

    return (struct tga){ w, h, (u32*)((u8 *)map + off), map, st.st_size };
}

static inline void tga_free(struct tga img)
{
    munmap((void*)img.map, img.map_len);
}

#define MAX_BUFFER_WIDTH (1920)
#define MAX_BUFFER_HEIGHT (1200)
void resize_window_callback(void *userdata, u32 new_w, u32 new_h) {
    struct camera *camera = (struct camera *)userdata;
    u32 need_scaling = new_w > MAX_BUFFER_WIDTH || new_h > MAX_BUFFER_HEIGHT;
    camera->display_w = new_w;
    camera->display_h = new_h;
    camera->buffer_w = need_scaling ? (new_w / 2) : new_w;
    camera->buffer_h = need_scaling ? (new_h / 2) : new_h;
    camera->end_x = camera->tile_x + (camera->buffer_w / TILE_SIZE) - 1;
    camera->end_y = camera->tile_y + (camera->buffer_h / TILE_SIZE) - 1;
    if (camera->end_x >= GRID_W) camera->end_x = GRID_W - 1;
    if (camera->end_y >= GRID_H) camera->end_y = GRID_H - 1;
    printf("Frame buffer resized: buffer(%d, %d), tile(%d, %d), end_tile(%d, %d)\n", camera->buffer_w, camera->buffer_h, camera->tile_x, camera->tile_y, camera->end_x, camera->end_y);
}

u32 main(void) {
    map = tga_load("map.tga");
    units = tga_load("units.tga");
    players = tga_load("players.tga");
    
    struct tga map_atlas = tga_load("map_atlas.tga");
    struct tga units_atlas = tga_load("units_atlas.tga");

    // find the cities on the map
    for (u32 y = 0; y < map.h; ++y) {
        for (u32 x = 0; x < map.w; ++x) {
            u32 income = tile_income[get_tile(x, y)];
            if (income > 0) {
                u32 player_id = get_player(x, y);
                if (player_id != UINT32_MAX) player_cities[player_id] += income;
            }
        }
    }
    
    // loop over units in units tga and use the add_unit function to add them to the grid
    for (u32 y = 0; y < units.h; ++y) {
        for (u32 x = 0; x < units.w; ++x) {
            u32 pixel = units.pix[y * units.w + x];
            if (pixel != 0) { // unit is not empty pixel
                enum units unit = get_unit(x, y);
                units.pix[y * units.w + x] = 0;
                i32 result = add_unit(get_player(x, y), unit, x, y);
                assert(result == 0 && "Init unit map went wrong\n");
            }
        }
    }

    u32 frame = 0;

    struct camera camera = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1};
    struct ctx *window = create_window(MAX_BUFFER_WIDTH, MAX_BUFFER_HEIGHT, "<<Fatzke>>", key_input_callback, mouse_input_callback, resize_window_callback, &camera);

    u64 start_us = time_us();
    while(poll_events(window)) {// poll for events
        if (!window->vsync_ready) continue; // wait for next event until vsync is not done

        u64 frame_us = time_us();
        if (frame == 0) {
            pthread_t tid;
            pthread_create(&tid, NULL, script, NULL);
            printf("Script thread started\n");
        }
        u64 us_thread = elapsed_us(frame_us);

        static u32 terrainbuffer[MAX_BUFFER_HEIGHT][MAX_BUFFER_WIDTH];
        u32 *buffer = get_buffer(window);
        u64 us_get_buffer = elapsed_us(frame_us);
        
        process_input(&camera);
        u64 us_process_inputs = elapsed_us(frame_us);

        if (camera.update == 1) {
            draw_terrain(camera, map_atlas, (u32 *)terrainbuffer);
            camera.update = 0;
        }
        u64 us_draw_terrain = elapsed_us(frame_us);
        memcpy(buffer, terrainbuffer, camera.buffer_w * camera.buffer_h * sizeof(u32));
        u64 us_cpy_terrain = elapsed_us(frame_us);
        draw_units(camera, units_atlas, buffer);
        u64 us_draw_units = elapsed_us(frame_us);
        draw_turn(camera, buffer);
        u64 us_draw_steps = elapsed_us(frame_us);
        
        commit(window); // tell compositor it can read from the buffer
        frame ++;

        #if _DEBUG_FPS
        printf("total: %ld us\n", elapsed_us(frame_us));
        printf("get thread: %ld us\n", us_thread);
        printf("process inputs: %ld us\n", us_process_inputs);
        printf("get buffer: %ld us\n", us_get_buffer);
        printf("draw terrain: %ld us\n", us_draw_terrain);
        printf("cpy terrain: %ld us\n", us_cpy_terrain);
        printf("draw units: %ld us\n", us_draw_units);
        printf("draw steps: %ld us\n", us_draw_steps);
        u64 us_per_frame = elapsed_us(start_us) / frame;
        printf("TIME: %ld, FRAME: %d, US PER FRAME: %ld\n", elapsed_us(start_us), frame, us_per_frame);
        #endif
    }
    destroy(window);
}
