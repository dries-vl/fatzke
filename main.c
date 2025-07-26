// tcc main.c -lwayland-client -run
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>

#include "wayland/wayland.c" // sudo apt install libwayland-dev

// todo: this needs to match the data in the tga files, but needs to be in scenario/save data (and also not global in code but passed via eg. a struct)
#define GRID_W 50
#define GRID_H 50
#define TILE_SIZE 32
#define ATLAS_SIZE 8

// todo: these hardcoded globals need to be configurable in data instead and not global
#define MAX_UNITS 128 // max number of units per player
#define BUCKET_COUNT 8 // number of buckets for resolve order
#define BUCKET_SIZE MAX_UNITS // number of steps per bucket

#define BLACK 0xFF000000
#define WHITE 0xFFFFFFFF
#define RED 0xFFFF0000
#define GREEN 0xFF00FF00
#define YELLOW 0xFFFFFF00

#define MATRIX(name, type, rows, cols, ...) \
    static const type name##_flat[] = { __VA_ARGS__ }; \
    _Static_assert(sizeof(name##_flat)/sizeof(type) == (rows)*(cols), #name " : need " #rows " x " #cols " entries (edit the list)"); \
    static const type (*const name)[cols] = (const type (*)[cols])name##_flat

struct tga { 
    int w, h; // dimensions
    uint32_t *pix; // pointer to pixel data
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
uint32_t tile_colors[TILE_COUNT] = {
    [SEA] = 0xFF90ccd4,
    [CITY] = 0xFFff1a1a,
    [MOUNTAINS] = 0xFF674616,
    [CROPLAND] = 0xFFc79720,
    [PLAINS] = 0xFF307118,
    [FOREST] = 0xFF21480e
};
int get_tile(int x, int y) {
    uint32_t tile_color = map.pix[y * map.w + x];
    for (int i = 0; i < TILE_COUNT; i++)
        if (tile_colors[i] == tile_color)
            return i;
    printf("Tile not found for color: 0x%08X, tile: %d, %d\n", tile_color, x, y);
    return -1;
}; 
uint32_t tile_income[TILE_COUNT] = {
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
uint32_t player_colors[PLAYER_COUNT] = {
    [GERMANY] = 0xFF6a3e0d,
    [SOVIET] = 0xFF6a0d33
};
int get_player(int x, int y) {
    uint32_t player_color = players.pix[y * players.w + x];
    for (int i = 0; i < PLAYER_COUNT; i++)
        if (player_colors[i] == player_color)
            return i;
    printf("Player not found\n");
    return -1;
}; 
uint32_t player_cities[PLAYER_COUNT] = {0};
uint32_t player_money[PLAYER_COUNT] = {0};
#pragma endregion

#pragma region UNITS
struct tga units;
enum units {
    INFANTRY,
    MOTORIZED,
    ARMOR,
    UNIT_COUNT
};
uint32_t unit_colors[UNIT_COUNT] = {
    [INFANTRY] = 0xFF000000,
    [MOTORIZED] = 0xFF383838,
    [ARMOR] = 0xFF6f6f6f,
};
int get_unit(int x, int y) {
    uint32_t unit_color = units.pix[y * units.w + x];
    for (int i = 0; i < UNIT_COUNT; i++)
        if (unit_colors[i] == unit_color)
            return i;
    printf("Unit not found\n");
    return -1;
}; 
uint32_t unit_cost[UNIT_COUNT] = {
    [INFANTRY] = 100,
    [MOTORIZED] = 200,
    [ARMOR] = 400,
};
MATRIX(movement_cost, uint32_t, UNIT_COUNT, TILE_COUNT,
/*               SEA,    CITY,  MNT,  CRO,  PLN,  FST  */
/*INFANTRY */ UINT32_MAX,  100,   2,    1,    1,    2,
/*MOTORIZED*/ UINT32_MAX,  200,     4,    1,    1,    3,
/*ARMOR    */ UINT32_MAX,  200,     4,    1,    1,    4
);

#pragma endregion

typedef struct {
    int x, y;
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
    int x, y; // position on grid
    int type;
};
struct path {
    uint8_t steps[GRID_H + GRID_W]; // array of steps
    uint8_t length; // number of steps
    // uint8_t id; // unit id
};
struct step {
    uint8_t dir;
    uint16_t cost;
    uint16_t id; // unit id + player id
};

struct path player_paths[PLAYER_COUNT][MAX_UNITS] = {0};
struct step resolve_order[BUCKET_COUNT][BUCKET_SIZE] = {0};
int bucket_size[BUCKET_COUNT] = {0}; // number of steps in each bucket

int player_unit_count[PLAYER_COUNT] = {0}; // size of array NOTE NOT ALWAYS NUMBER OF UNITS
struct unit player_units[PLAYER_COUNT][MAX_UNITS] = {0};

struct unit player_target[PLAYER_COUNT] = {{0, 0, 0}, {0, 0, 0}};

static void key_input_callback(void *ud, uint32_t key, uint32_t state)
{
    if (key == 1) exit(0);
    if (state) printf("key %u down\n", key);
    else printf("key %u up\n", key);
}
static void mouse_input_callback(void *ud, int32_t x, int32_t y, uint32_t b)
{
    printf("button %u\n", b);
    printf("pointer at %d,%d\n", x, y);
}

int battle(int attacker, int defender, int unit_att, int unit_def);

int pathing(int from_x, int from_y, int to_x, int to_y, uint8_t *path, int *pathlength, int unit_type, int player) { // no cost yet, BITshift versie??
    if (!path || !pathlength) return -1; // Invalid arguments
    if (from_x < 0 || from_x >= GRID_W || from_y < 0 || from_y >= GRID_H ||
        to_x < 0 || to_x >= GRID_W || to_y < 0 || to_y >= GRID_H) {
        return -1; // Invalid coordinates
    }
    int paths_count = 1;
    uint8_t paths[GRID_W * GRID_H][GRID_H + GRID_W] = {0}; // STACK OVERFLOW ?????
    paths[0][0] = 0; // path length
    paths[0][1] = UP;
    pos visited[GRID_W * GRID_H] = {0}; // visited tiles
    visited[0] = (pos){from_x, from_y}; // mark starting tile as visiteds
    for (int i = 0; i < paths_count; i++) {
        //printf("Checking path %d: (%d, %d)\n", i + 1, visited[i].x, visited[i].y);
        int pass = 0;
        if (paths_count >= GRID_W * GRID_H) {
            printf("Too many paths, aborting\n");
            return 0; // ran out of memory
        }
        for (int dir = UP; dir <= DOWN_RIGHT; dir++){
            pass = 0; // reset pass for each direction
            // Generic
            int x = from_x;
            int y = from_y;
            for (int step = 0; step < paths[i][0]; step++) {
                x += dir_offsets[paths[i][step + 1]].x;
                y += dir_offsets[paths[i][step + 1]].y;
            }
            x += dir_offsets[dir].x;
            y += dir_offsets[dir].y;
            //printf("Step %d: (%d, %d) ", dir, x, y);
            if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) continue; // out of bounds
            int terrain = get_tile(x, y);
            if (SEA == terrain || terrain == MOUNTAINS) {continue;} // check for impassable tile
            if (units.pix[y * units.w + x] == player_colors[player]) {continue;} // check for player unit
            if (to_x == x && to_y == y) { // found target
                paths[paths_count][0] = paths[i][0] + 1;
                paths[paths_count][1] = dir;
                for (int j = 2; j < paths[i][0] + 2; j++) {
                    paths[paths_count][j] = paths[i][j-1];
                }
                *pathlength = paths[paths_count][0]; // set path length
                memcpy(path, paths[paths_count] + 1, sizeof(uint8_t) * (*pathlength)); // copy path to output
                return 1; // found path
            }
            if (units.pix[y * units.w + x] != 0 && units.pix[y * units.w + x] != player_colors[1 - player]) {continue;} // if enemy on path path around
            for (int j = 0; j < paths_count; j++) {
                if (visited[j].x == x && visited[j].y == y) {
                    pass = 1;
                    //printf("Tile (%d, %d) already visited\n", x, y);
                    break;
                }
            }
            if (pass == 0) {
                paths[paths_count][0] = paths[i][0] + 1;;
                paths[paths_count][1] = dir;
                for (int j = 2; j < paths[i][0] + 2; j++) {
                    paths[paths_count][j] = paths[i][j-1];
                }
                visited[paths_count] = (pos){x, y}; // mark tile as visited
                paths_count++;
                //printf("Path %d: ", paths_count);
            }
        }
    }
    return 0; // no path found
}

static inline uint32_t mix_colors(uint32_t a, uint32_t b) {
    return (((a ^ b) & 0xFEFEFEFEU) >> 1U) + (a & b);
}

void inline draw_unit(enum players player, int unit_y, int unit_x, int w, int h, uint32_t buffer[h][w]) {
    for (int y = 0; y < TILE_SIZE; ++y) {
        for (int x = 0; x < TILE_SIZE; ++x) {
            buffer[(unit_y * TILE_SIZE) + y][(unit_x * TILE_SIZE) + x] = player_colors[player];
        }
    }
}

void draw_units(int w, int h, uint32_t buffer[h][w]) {
    for (int player = 0; player < PLAYER_COUNT; player++) {
        if (player_unit_count[player] == 0) continue; // skip empty players
        for (int unit = 0; unit < player_unit_count[player]; ++unit) {
            if (player_units[player][unit].type == 0) continue; // skip empty unit slots
            int x = player_units[player][unit].x;
            int y = player_units[player][unit].y;
            if ((x+1) * TILE_SIZE > w || (y+1) * TILE_SIZE > h) continue; // don't write beyond the visible grid
            draw_unit(player, y, x, w, h, buffer);
        }
    }
}

// todo: this is same as draw unit, we can merge the functions
void inline draw_tile(struct tga map_atlas, int tile_y, int tile_x, int w, int h, uint32_t terrainbuffer[h][w]) {
    enum tiles tile = get_tile(tile_x, tile_y);
    int start_x = (tile % ATLAS_SIZE) * TILE_SIZE; // todo: hardcoded for now assumes 8 by 8 atlas
    int start_y = (tile / ATLAS_SIZE) * TILE_SIZE;
    for (int y = 0; y < TILE_SIZE; ++y) {
        for (int x = 0; x < TILE_SIZE; ++x) {
            int atlas_x = start_x + x;
            int atlas_y = start_y + y;
            terrainbuffer[(tile_y * TILE_SIZE) + y][(tile_x * TILE_SIZE) + x] = map_atlas.pix[atlas_y * map_atlas.w + atlas_x];
        }
    }
}

void draw_terrain(struct tga map_atlas, int w, int h, uint32_t terrainbuffer[h][w]) {
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            if ((x+1) * TILE_SIZE > w || (y+1) * TILE_SIZE > h) continue; // don't write beyond visible; TODO: use per-frame visible grid size instead of 'GRID_H' etc.
            draw_tile(map_atlas, y, x, w, h, terrainbuffer);
        }
    }
}

void draw_step(enum players player, int step_y, int step_x, int w, int h, uint32_t buffer[h][w])
{
    for (int y = 0; y < TILE_SIZE; ++y) {
        for (int x = 0; x < TILE_SIZE; ++x) {
            buffer[(step_y * TILE_SIZE) + y][(step_x * TILE_SIZE) + x] = mix_colors(player_colors[player], 0xFFFFFFFF);
        }
    }
}

void draw_turn(int w, int h, uint32_t buffer[h][w]){
    pos unit_locations[PLAYER_COUNT][MAX_UNITS] = {0}; // keep track of unit locations
    for (int bucket = 0; bucket < BUCKET_COUNT; bucket ++) {
        if (bucket_size[bucket] == 0) {continue;} // skip empty buckets
        for (int step = 0; step < bucket_size[bucket]; step++) {
            enum players player = resolve_order[bucket][step].id >> 8; // get player from id
            int unit = resolve_order[bucket][step].id % 256; // get unit from id
            if (player_units[player][unit].type == 0) continue; // skip empty unit slots
            pos loc;
            if (unit_locations[player][unit].x != 0 || unit_locations[player][unit].y != 0) {
                loc = unit_locations[player][unit]; // use cached location
            }
            else {
                loc = (pos){player_units[player][unit].x, player_units[player][unit].y}; // start location
            }
            uint8_t dir = resolve_order[bucket][step].dir; // get direction from path
            int x = loc.x + dir_offsets[dir].x; // calculate x position
            int y = loc.y + dir_offsets[dir].y; // calculate y position
            if ((x+1) * TILE_SIZE > w || (y+1) * TILE_SIZE > h) continue; // don't draw beyond the visible grid
            draw_step(player, y, x, w, h, buffer);
            unit_locations[player][unit] = (pos){x, y}; // update location
        }
    }
}


int resolve_turn(){
    printf("Resolving turn...\n");
    int blocked_units[PLAYER_COUNT][MAX_UNITS] = {0}; // keep track of blocked units
    for (int bucket = 0; bucket < BUCKET_COUNT; bucket++) {
        if (bucket_size[bucket] == 0) continue; // skip empty buckets
        // Sort the steps in the bucket by cost MAYBE NEEDED
        // Execute the steps in the bucket
        for (int step = 0; step < bucket_size[bucket]; step++) {
            
            enum players player = resolve_order[bucket][step].id >> 8; // get player from id
            int unit = resolve_order[bucket][step].id % 256; // get unit from id
            if (blocked_units[player][unit] || player_units[player][unit].type == 0) { continue; } // skip blocked and empty units
            uint8_t dir = resolve_order[bucket][step].dir; // get direction from path
            int x = player_units[player][unit].x + dir_offsets[dir].x; // calculate x position
            int y = player_units[player][unit].y + dir_offsets[dir].y; // calculate y position
            int result = move_unit(player, unit, x, y); // move unit
            if (result != 0) {
                blocked_units[player][unit] = 1; // mark unit as blocked
            }
        }
        bucket_size[bucket] = 0; // reset bucket size
        memset(resolve_order[bucket], 0, sizeof(struct step) * BUCKET_SIZE); // clear resolve order
    }

    return 0;
}

int move_unit(int player, int unit, int to_x, int to_y) {
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
    if (units.pix[to_y * units.w + to_x] == player_colors[player]) {
        //printf("Tile (%d, %d) already occupied\n", to_x, to_y);
        return -3; // Tile already occupied
    }
    else if (units.pix[to_y * units.w + to_x] != 0 && units.pix[to_y * units.w + to_x] != player_colors[player]) { // BATTLE!
        //printf("Tile (%d, %d) occupied by enemy\n", to_x, to_y);
        printf("Battle at (%d, %d)!\n", to_x, to_y);
        for (int defender = 0; defender < player_unit_count[1 - player]; defender++) {
            if (player_units[1 - player][defender].x == to_x && player_units[1 - player][defender].y == to_y) {
                return battle(player, 1 - player, unit, defender);
            }
        }
    }
    int from_x = player_units[player][unit].x;
    int from_y = player_units[player][unit].y;
    // move unit from old location to new location
    units.pix[to_y * units.w + to_x] = units.pix[from_y * units.w + from_x];
    units.pix[from_y * units.w + from_x] = 0;
    // set the value of the unit in the player units array
    player_units[player][unit].x = to_x;
    player_units[player][unit].y = to_y;
    // change tile ownership to this player
    enum players to_player = get_player(to_x, to_y);
    if (to_player != player) {
        players.pix[to_y * players.w + to_x] = player_colors[player]; // Update country color
        uint32_t income = tile_income[get_tile(to_x, to_y)];
        if (income > 0) {
            printf("player %d conquered city from player %d\n", player, to_player);
            player_cities[to_player] -= income;
            player_cities[player] += income;
        }
    }
    return 0; // Move successful
}

int add_unit(int player, int x, int y) {
    if (player < 0 || player >= PLAYER_COUNT) {
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
    for (int unit = 0; unit < player_unit_count[player]; ++unit) { // Reuse empty slot in player_units
        if (player_units[player][unit].type == 0) {
            player_units[player][unit] = (struct unit){x, y, 1};
            units.pix[y * units.w + x] = player_colors[player]; // add the unit to the map
            return 0; // Unit added successfully
        }
    }
    player_units[player][player_unit_count[player]] = (struct unit){x, y, 1};
    units.pix[y * units.w + x] = player_colors[player]; // add the unit to the map
    player_unit_count[player]++;
    return 0; // Unit added successfully
}

int remove_unit(int player, int unit) {
    if (player < 0 || player >= PLAYER_COUNT || unit < 0 || unit >= player_unit_count[player]) {
        printf("Invalid player or unit index\n");
        return -1; // Invalid player or unit
    }
    if (player_units[player][unit].type == 0) {
        printf("Unit %d of player %d is empty\n", unit, player);
        return -2; // Unit is not active
    }
    int x = player_units[player][unit].x;
    int y = player_units[player][unit].y;
    units.pix[y * units.w + x] = 0; // Remove unit from grid
    player_units[player][unit] = (struct unit){0, 0, 0}; // Clear unit data NO REORDERING
    return 0; // Unit removed successfully
}

int battle(int attacker, int defender, int unit_att, int unit_def) {
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
    int x_att = player_units[attacker][unit_att].x;
    int y_att = player_units[attacker][unit_att].y;
    int x_def = player_units[defender][unit_def].x;
    int y_def = player_units[defender][unit_def].y;
    if (abs(x_att - x_def) > 1 || abs(y_att - y_def) > 1) {
        printf("Units not adjacent\n");
        return -3; // Units not adjacent
    }
    int random = rand() % 20; // Random number between 0 and 5
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

int commit_turn(enum players player) {
    if (player < 0 || player >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return -1; // Invalid player
    }
    // Resolve all paths for the player
    for (int unit = 0; unit < player_unit_count[player]; ++unit) {
        if (player_paths[player][unit].length == 0) continue; // skip empty paths
        int cost = 0; // cost is cummulative
        for (int step = 0; step < player_paths[player][unit].length; ++step) {
            // calculate cost of the step
            cost += 100; // default cost
            if (cost > 400) { break; } // max cost
            int bucket = cost / 100; // bucket for the step
            resolve_order[bucket][bucket_size[bucket]] = (struct step){
                .dir = player_paths[player][unit].steps[step] , // direction of the step
                .cost = cost, // cost of the step
                .id = (uint16_t)(unit + (player << 8)) // unit id + player id
            };
            bucket_size[bucket]++; // increment bucket size
        }
    }
    printf("Player %d committed turn with %d units\n", player, player_unit_count[player]);
    return 0; // Turn committed successfully

}



int move_towards(int player, int unit, int target_x, int target_y) {
    if (player < 0 || player >= PLAYER_COUNT || unit < 0 || unit >= player_unit_count[player]) {
        printf("Invalid player or unit index\n");
        return -1; // Invalid player or unit
    }
    if (target_x < 0 || target_x >= GRID_W || target_y < 0 || target_y >= GRID_H) {
        printf("Invalid target position (%d, %d)\n", target_x, target_y);
        return -2; // Invalid target position
    }
    int x = player_units[player][unit].x;
    int y = player_units[player][unit].y;
    int dir_x = (target_x - x) < 0 ? -1 : (target_x - x) > 0 ? 1 : 0; // get direction x-axis
    int dir_y = (target_y - y) < 0 ? -1 : (target_y - y) > 0 ? 1 : 0; // get direction y-axis
    int result =  move_unit(player, unit, x + dir_x, y + dir_y);
    if (result == -3) { // Tile already occupied
        // Try to move in the other random direction
        int random = rand() % 2; 
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

void find_front(int player, int center_x, int center_y, struct unit *front_units, int *count) {
    if (player < 0 || player >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return; // Invalid player
    }
    for (int unit = 0; unit < player_unit_count[player]; unit++) {
        if (player_units[player][unit].type == 0) continue; // skip empty unit slots
        int x = player_units[player][unit].x;
        int y = player_units[player][unit].y;
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

int find_target(int player, int unit, struct unit *target) {
    if (player < 0 || player >= PLAYER_COUNT || unit < 0 || unit >= player_unit_count[player]) {
        printf("Invalid player or unit index\n");
        return -1; // Invalid player or unit
    }
    int x = player_units[player][unit].x;
    int y = player_units[player][unit].y;
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

int spawn_unit(enum players player) {
    // try to spawn around a unit
    for (int unit_id = 0; unit_id < player_unit_count[player]; unit_id++) {
        if (player_units[player][unit_id].type == 0) continue; // skip empty unit slots
        struct unit unit = player_units[player][unit_id];
        for (int dir = 0; dir < DIRECTIONS_COUNT; dir++) {
            int spawn_x = unit.x + dir_offsets[dir].x;
            int spawn_y = unit.y + dir_offsets[dir].y;
            if (spawn_y >= 0 && spawn_x >= 0 && spawn_y < units.w && spawn_x < units.w) {
                bool has_unit = units.pix[spawn_y * units.w + spawn_x] != 0;
                bool is_sea = map.pix[spawn_y * map.w + spawn_x] == SEA;
                if (!has_unit && !is_sea && get_player(spawn_x, spawn_y) == player) {
                    int result = add_unit(player, spawn_x, spawn_y);
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

int ai_unit_movement(enum players player) {
    // AI logic to move units towards enemy units
    if (player < 0 || player >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return -1; // Invalid player
    }
    // unit movement
    struct unit front_units[MAX_UNITS];
    int count = 0;
    find_front(1 - player, 0, 0, front_units, &count); // Get front units for other player
    for (int unit = 0; unit < player_unit_count[player]; unit++) {
        memset(player_paths[player][unit].steps, 0, sizeof(player_paths[player][unit].steps)); // clear array
        player_paths[player][unit].length = 0;
        if (player_units[player][unit].type == 0) continue; // skip empty unit slots
        int x = player_units[player][unit].x;
        int y = player_units[player][unit].y;
        int distance = 2500;
        struct unit target = {0};
        bool found_target = false;
        for (int enemy = 0; enemy < count; enemy++) {
            int dis = (front_units[enemy].x - x) * (front_units[enemy].x - x) +
                        (front_units[enemy].y - y) * (front_units[enemy].y - y);
            if (dis < distance) {
                distance = dis;
                target = front_units[enemy];
                found_target = true;
            }
        }
        uint8_t path[GRID_W + GRID_H];
        int path_length = 0;
        int result = 0;
        if (found_target) {
            result = pathing(x, y, target.x, target.y, path, &path_length, 1, player); // Get path to target UNIT_TYPE
            if (result == 1) {
                player_paths[player][unit].length = path_length;
                for (int step = 0; step < path_length; step++) {
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
    printf("Cities (player %d): %d\n", player, player_cities[player]);
    player_money[player] += player_cities[player];
    // buy units for every 10 money
    static const int UNIT_COST = 100;
    int units_to_buy = player_money[player] / UNIT_COST;
    int money_to_use = units_to_buy * UNIT_COST;
    player_money[player] -= money_to_use;
    for (int i = 0; i<units_to_buy; i++) { 
        if (spawn_unit(player) == -1) 
            player_money[player] += UNIT_COST; // refund if cannot be spawned anywhere
    }
    
    ai_unit_movement(player); // BIK
    /*
    // unit movement
    struct unit front_units[MAX_UNITS];
    int count = 0;
    find_front(1 - player, 0, 0, front_units, &count); // Get front units for other player
    if (count == 0) {
        printf("No front units found for player %d\n", player);
        // No enemy unit, move randomly
        for (int unit = 0; unit < player_unit_count[player]; unit++) {
            int x = player_units[player][unit].x;
            int y = player_units[player][unit].y;
            // Randomly move unit
            int dir_x = rand() % 3 - 1; // -1, 0 or 1
            int dir_y = rand() % 3 - 1; // -1, 0 or 1
            move_unit(player, unit, x + dir_x, y + dir_y);
        }
    }
    for (int unit = 0; unit < player_unit_count[player]; unit++) {
        int x = player_units[player][unit].x;
        int y = player_units[player][unit].y;
        int distance = 2500;
        struct unit target = {0};
        for (int enemy = 0; enemy < count; enemy++) {
            int dis = (front_units[enemy].x - x) * (front_units[enemy].x - x) +
                        (front_units[enemy].y - y) * (front_units[enemy].y - y);
            if (dis < distance) {
                distance = dis;
                target = front_units[enemy];
            }
        }
        int result = move_towards(player, unit, target.x, target.y);
    }
    */
}

void script(int frame) {
    if (frame % 20 == 0) {
        player_turn(0);
    }
    if (frame % 20 == 5) {
        player_turn(1);
    }
    if (frame % 20 == 15) {
        resolve_turn();
    }
    //    for (int bucket = 0; bucket < BUCKET_COUNT; bucket++) {
    //        for (int step = 0; step < bucket_size[bucket]; step++) {
    //            printf("Bucket %d, step %d: Player %d, Unit %d, Direction %d\n",
    //                   bucket, step, resolve_order[bucket][step].id / 256,
    //                   resolve_order[bucket][step].id % 256,
    //                   resolve_order[bucket][step].dir);
    //        }
    //   }
    //}
    /*
    if (frame == 2){
        printf("player paths:\n");
        for (int player = 0; player < PLAYER_COUNT; player++) {
            printf("Player %d:\n", player);
            for (int unit = 0; unit < player_unit_count[player]; unit++) {
                printf("Unit %d: ", unit);
                printf("Steps: %d ", player_paths[player][unit].length);
                for (int step = 0; step < player_paths[player][unit].length; step++) {
                    printf("(%d) ", player_paths[player][unit].steps[step]);
                }
                printf("\n");
            }
        }
    }
    */
}

static inline struct tga tga_load(const char *path)
{
    int fd = open(path, O_RDONLY | 02000000);
    if (fd < 0) {fprintf(stderr, "File not found: %s\n", path); exit(1);}

    uint8_t h18[18];
    assert(read(fd, h18, 18) == 18);

    /* ─── validate header ────────────────────────────────────────── */
    if (h18[2]  != 2)   { fprintf(stderr,"TGA type ≠ 2 for %s\n", path);          exit(1); }
    if (h18[16] != 32){ fprintf(stderr,"TGA bpp ≠ 32 (got %u) for %s\n",h18[16], path);exit(1);}
    if ((h18[17] & 0x20) == 0){ fprintf(stderr,"TGA not top-left for %s\n", path); exit(1); }

    int w = h18[12] | h18[13]<<8;
    int h = h18[14] | h18[15]<<8;

    size_t off   = 18 + h18[0];              /* header + ID-field      */
    size_t bytes = (size_t)w * h * 4;    /* 4 bytes per pixel      */

    struct stat st;
    fstat(fd,&st);
    assert(off + bytes <= (size_t)st.st_size);

    void *map = mmap(0, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    assert(map != MAP_FAILED);
    close(fd);

    return (struct tga){ w, h, (uint32_t*)((uint8_t *)map + off), map, st.st_size };
}

static inline void tga_free(struct tga img)
{
    munmap((void*)img.map, img.map_len);
}

#define MAX_BUFFER_WIDTH (32 * 32)
#define MAX_BUFFER_HEIGHT (32 * 24)
static int display_w;
static int display_h;
static int buffer_w;
static int buffer_h;
static int need_scaling;
void resize_window_callback(void *userdata, int new_w, int new_h) {
    display_w = new_w;
    display_h = new_h;
    need_scaling = new_w > MAX_BUFFER_WIDTH || new_h > MAX_BUFFER_HEIGHT;
    buffer_w = need_scaling ? MAX_BUFFER_WIDTH : new_w;
    buffer_h = need_scaling ? MAX_BUFFER_HEIGHT : new_h;
    printf("Window resized to %dx%d, buffer resized to %dx%d\n", display_w, display_h, buffer_w, buffer_h);
}

void copy_centered(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh) {
    assert(sw <= dw && sh <= dh); // sanity check
    int xoff = (dw - sw) / 2;
    int yoff = (dh - sh) / 2;
    for (int y = 0; y < sh; ++y) {
        memcpy(dst + (y + yoff) * dw + xoff, src + y * sw, sw * sizeof(uint32_t));
    }
}

int main(void)
{
    map = tga_load("map.tga");
    units = tga_load("units.tga");
    players = tga_load("players.tga");
    
    struct tga map_atlas = tga_load("map_atlas.tga");

    // find the cities on the map
    for (int y = 0; y < map.h; ++y) {
        for (int x = 0; x < map.w; ++x) {
            uint32_t income = tile_income[get_tile(x, y)];
            if (income > 0) {
                int player_id = get_player(x, y);
                if (player_id != -1) player_cities[player_id] += income;
            }
        }
    }
    
    // loop over units in units tga and use the add_unit function to add them to the grid
    for (int y = 0; y < units.h; ++y) {
        for (int x = 0; x < units.w; ++x) {
            uint32_t pixel = units.pix[y * units.w + x];
            if (pixel != 0) { // unit is not empty pixel
                units.pix[y * units.w + x] = 0;
                add_unit(get_player(x, y), x, y);
            }
        }
    }

    int camera_zoom = 1;
    int camera_x = GRID_W / 2;
    int camera_y = GRID_H / 2;
    uint32_t frame = 0;

    struct ctx *window = create_window(0, 0, "<<Fatzke>>", key_input_callback, mouse_input_callback, resize_window_callback, NULL);

    while(window_poll(window)) {// poll for events and break if compositor connection is lost
        if (frame > 0) script(frame);

        static uint32_t terrainbuffer[MAX_BUFFER_HEIGHT][MAX_BUFFER_WIDTH];
        if (frame == 0) draw_terrain(map_atlas, buffer_w, buffer_h, terrainbuffer);

        static uint32_t scalingbuffer[MAX_BUFFER_HEIGHT][MAX_BUFFER_WIDTH];

        uint64_t buffer_size = buffer_w * buffer_h * sizeof(uint32_t);
        uint32_t *buffer = need_scaling ? scalingbuffer : get_buffer(window);
        memcpy(buffer, terrainbuffer, buffer_size);

        draw_units(buffer_w, buffer_h, buffer);
        draw_turn(buffer_w, buffer_h, buffer);
        
        // copy the rendered buffer into the actual framebuffer if scaling was needed
        if (need_scaling) copy_centered(get_buffer(window), display_w, display_h, scalingbuffer, buffer_w, buffer_h); 
        
        window_wait_vsync(window); // wait for vsync (and keep processing events) before next frame
        commit(window); // tell compositor it can read from the buffer

        frame ++;
    }
    destroy(window);
}
