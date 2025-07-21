// tcc main.c -lwayland-client -run
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>

// sudo apt install libwayland-dev
#include "wayland/wayland.c"

#define HEIGHT 1000
#define WIDTH 1000
#define PIXEL_SIZE 20
#define GRID_W (WIDTH / PIXEL_SIZE) // now just screen size
#define GRID_H (HEIGHT / PIXEL_SIZE)
#define UNIT_SIZE 14 // size of a unit on screen
#define MAX_UNITS 100 // max number of units per player
#define BUCKET_COUNT 10 // number of buckets for resolve order
#define BUCKET_SIZE MAX_UNITS // number of steps per bucket

#define WHITE 0xFFFFFFFF
#define BLACK 0xFF000000
#define RED 0xFFFF0000
#define GREEN 0xFF00FF00
#define YELLOW 0xFFFFFF00

#define SEA 0xFF90ccd4
#define LAND 0xFF21480e
#define CITY 0xFFff1a1a

#define GERMANY_COLOR 0xFF6a3e0d
#define SOVIET_COLOR 0xFF6a0d33

enum players {
    GERMANY,
    SOVIET,
    PLAYER_COUNT
};
uint32_t player_colors[PLAYER_COUNT] = {
    [GERMANY] = GERMANY_COLOR,
    [SOVIET] = SOVIET_COLOR
};

uint32_t player_cities[PLAYER_COUNT] = {0};
uint32_t player_money[PLAYER_COUNT] = {0};

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
    uint8_t id; // unit id + player id
};

struct path player_paths[PLAYER_COUNT][MAX_UNITS] = {0};
struct step resolve_order[BUCKET_COUNT][BUCKET_SIZE] = {0};
int bucket_size[BUCKET_COUNT] = {0}; // number of steps in each bucket

int player_unit_count[PLAYER_COUNT] = {0}; // number of units per player
struct unit player_units[PLAYER_COUNT][MAX_UNITS] = {0};

struct unit player_target[PLAYER_COUNT] = {{0, 0, 0}, {0, 0, 0}};

/* 32-bit uncompressed, top-left-origin TGA */
struct tga { 
    int w, h;                /* dimensions                    */
    uint32_t *pix;       /* BGRA pixel pointer            */
    const void *map;      /* whole mmapped file            */
    size_t map_len;  /* length for munmap             */
};

struct tga map;
struct tga units;
struct tga countries;

int get_player(int x, int y) {
    uint32_t player_color = countries.pix[y * map.w + x];
    for (int i = 0; i < PLAYER_COUNT; i++)
        if (player_colors[i] == player_color)
            return i;
    printf("Player not found\n");
    return -1;
}; 

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

static inline uint8_t clamp_mul(uint8_t ch, float factor) {
    int v = (int)(ch * factor);
    return v > 255 ? 255 : v;
}

#define LIGHTEN(c, f) ( \
    (clamp_mul((c)&0xFF, f)) \
    | (clamp_mul(((c)>>8)&0xFF, f) << 8) \
    | (clamp_mul(((c)>>16)&0xFF, f) << 16) \
    | ((c)&0xFF000000) \
)

int battle(int attacker, int defender, int unit_att, int unit_def);

int pathing(int from_x, int from_y, int to_x, int to_y, uint8_t *path, int *pathlength) { // no cost yet, BITshift versie??
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
            if (to_x == x && to_y == y) { // found target
                paths[paths_count][0] = paths[i][0] + 1;
                paths[paths_count][1] = dir;
                for (int j = 2; j < paths[i][0] + 2; j++) {
                    paths[paths_count][j] = paths[i][j-1];
                }
                *pathlength = paths[paths_count][0]; // set path length
                memcpy(path, paths[paths_count] + 1, sizeof(pos) * (*pathlength)); // copy path to output
                return 1; // found path
            }
            if (map.pix[y*map.w + x] != SEA) { // better terrain handling needed
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
            } else {
                //printf("Tile (%d, %d) is water\n", x, y);
            }
            //printf("\n");
        }
    }
    return 0; // no path found
}


void draw_grid(uint32_t *buffer)
{
    for (int x = 0; x < WIDTH; ++x) {
        for (int y = 0; y < HEIGHT; ++y) {
            if (x % PIXEL_SIZE == 0 || y % PIXEL_SIZE == 0) {
                buffer[y * WIDTH + x] = BLACK;
            } else {
                // draw based on tga
                int map_x = x / PIXEL_SIZE;
                int map_y = y / PIXEL_SIZE;
                uint32_t pixel = map.pix[map_y * map.w + map_x];
                if (pixel == SEA)
                    buffer[y * WIDTH + x] = BLUE;
                else if (pixel == LAND) {
                    uint32_t country_pixel = countries.pix[map_y * countries.w + map_x];
                    buffer[y * WIDTH + x] = country_pixel;
                } else
                    buffer[y * WIDTH + x] = RED;
            }
        }
    }
}

void draw_unit(enum players player, int unit_x, int unit_y, uint32_t *buffer)
{
    int x = unit_x * PIXEL_SIZE + PIXEL_SIZE / 2;
    int y = unit_y * PIXEL_SIZE + PIXEL_SIZE / 2;
    for (int dx = -UNIT_SIZE/2; dx <= UNIT_SIZE/2; ++dx) {
        for (int dy = -UNIT_SIZE/2; dy <= UNIT_SIZE/2; ++dy) {
            buffer[(y + dy) * WIDTH + (x + dx)] = LIGHTEN(player_colors[player], 2.0f);
        }
    }
}
void draw_step(enum players player, int step_x, int step_y, uint32_t *buffer)
{
    int x = step_x * PIXEL_SIZE + PIXEL_SIZE / 2;
    int y = step_y * PIXEL_SIZE + PIXEL_SIZE / 2;
    for (int dx = -UNIT_SIZE/2; dx <= UNIT_SIZE/2; dx+=2) {
        for (int dy = -UNIT_SIZE/2; dy <= UNIT_SIZE/2; dy+=2) {
            buffer[(y + dy) * WIDTH + (x + dx)] = LIGHTEN(player_colors[player], 2.0f);
        }
    }
}
void draw_paths(uint32_t *buffer){
    for (int player = 0; player < PLAYER_COUNT; player++) {
        for (int unit = 0; unit < player_unit_count[player]; ++unit) {
            pos loc = (pos){player_units[player][unit].x, player_units[player][unit].y}; // start location
            if (player_paths[player][unit].length == 0) continue; // skip empty paths
            for (int step = 0; step < player_paths[player][unit].length; ++step) {
                uint8_t dir = player_paths[player][unit].steps[step]; // get direction from path
                int x = loc.x + dir_offsets[dir].x; // calculate x position
                int y = loc.y + dir_offsets[dir].y; // calculate y position
                draw_step(player, x, y, buffer);
                loc = (pos){x, y}; // update location
            }
        }
    }
}

void draw_units(uint32_t *buffer) {
    for (int player = 0; player < PLAYER_COUNT; player++) {
        if (player_unit_count[player] == 0) continue; // skip empty players
        for (int unit = 0; unit < player_unit_count[player]; ++unit) {
            int x = player_units[player][unit].x;
            int y = player_units[player][unit].y;
            draw_unit(player, x, y, buffer);
        }
    }
}

int move_unit(int player, int unit, int to_x, int to_y) {
    if (!player_units[player] || unit < 0 || unit >= player_unit_count[player]) {
        //printf("Invalid player or unit index\n");
        return -1; // Invalid player or unit
    }
    if (to_x < 0 || to_x >= GRID_W || to_y < 0 || to_y >= GRID_H) {
        //printf("Invalid move to (%d, %d)\n", to_x, to_y);
        return -2; // Invalid move
    }
    if (map.pix[to_y * map.w + to_x] == SEA) {
        //printf("Cannot move to water tile (%d, %d)\n", to_x, to_y);
        return -4; // Cannot move to water tile
    }
    if (units.pix[to_y * units.w + to_x] == player_colors[player]) {
        //printf("Tile (%d, %d) already occupied\n", to_x, to_y);
        return -3; // Tile already occupied
    }
    else if (units.pix[to_y * units.w + to_x] != 0 && units.pix[to_y * units.w + to_x] != player_colors[player]) { // BATTLE!
        //printf("Tile (%d, %d) occupied by enemy\n", to_x, to_y);
        //printf("Battle at (%d, %d)!\n", to_x, to_y);
        for (int defender = 0; defender < player_unit_count[1 - player]; defender++) {
            if (player_units[1 - player][defender].x == to_x && player_units[1 - player][defender].y == to_y) {
                return battle(player, 1 - player, unit, defender);
            }
        }
    }
    int from_x = player_units[player][unit].x;
    int from_y = player_units[player][unit].y;
    units.pix[to_y * units.w + to_x] = units.pix[from_y * units.w + from_x];
    units.pix[from_y * units.w + from_x] = 0;
    countries.pix[to_y * countries.w + to_x] = player_colors[player]; // Update country color
    player_units[player][unit].x = to_x; // Update player unit position
    player_units[player][unit].y = to_y;
    return 0; // Move successful
}

int add_unit(int player, int x, int y) {
    if (player < 0 || player >= PLAYER_COUNT) {
        //printf("Invalid player index\n");
        return -1; // Invalid player
    }
    if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) {
        //printf("Invalid position (%d, %d)\n", x, y);
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
    int x = player_units[player][unit].x;
    int y = player_units[player][unit].y;
    units.pix[y * units.w + x] = 0; // Remove unit from grid
    player_units[player][unit] = (struct unit){0, 0, 0}; // Clear unit data
    for (int unit_iterator = unit; unit_iterator < player_unit_count[player] - 1; ++unit_iterator) {
        player_units[player][unit_iterator] = player_units[player][unit_iterator + 1]; // Shift units left
    }
    player_units[player][player_unit_count[player] - 1] = (struct unit){0, 0, 0}; // Clear last unit
    player_unit_count[player]--;
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
        pos prev_step = {player_units[player][unit].x, player_units[player][unit].y}; // previous step position
        for (int step = 0; step < player_paths[player][unit].length; ++step) {
            int x = 1;
            int y = 1;
            // calculate cost of the step
            cost += 100; // default cost
            resolve_order[cost/100][bucket_size[cost/100]] = (struct step){
                .dir = 1, // direction of the step
                .cost = cost, // cost of the step
                .id = (uint8_t)(unit + (player << 8)) // unit id + player id
            };
            prev_step = (pos){x, y}; // update previous step
        }
    }
    return 0; // Turn committed successfully

}


int resolve_turn(){

    return 0;
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


/*
int move_unit(int from_x, int from_y, int to_x, int to_y)
{
    int moved = 0;
    if (from_x < 0 || from_x >= GRID_W || from_y < 0 || from_y >= GRID_H ||
        to_x < 0 || to_x >= GRID_W || to_y < 0 || to_y >= GRID_H) {
        printf("Invalid move from (%d, %d) to (%d, %d)\n", from_x, from_y, to_x, to_y);
        return -1; // Invalid move
    }
    if (grid[from_x][from_y].unit[0] == 0 && grid[from_x][from_y].unit[1] == 0) {
        printf("No unit to move from (%d, %d)\n", from_x, from_y);
        return -2; // No unit to move
    }
    if (grid[to_x][to_y].unit[0] || grid[to_x][to_y].unit[1]) {
        printf("Target tile (%d, %d) already occupied\n", to_x, to_y);
        return -3; // Target tile already occupied
    }
    for (int unit = 0; unit < player_unit_count; ++unit) {
        if (player_units[unit][0] == from_x && player_units[unit][1] == from_y) {
            grid[to_x][to_y].unit[0] = grid[from_x][from_y].unit[0];
            grid[from_x][from_y].unit[0] = 0;
            player_units[unit][0] = to_x; // Update player unit position
            player_units[unit][1] = to_y;
            moved = 1; // Player unit moved
            break;
        }
    }
    if (moved) return 0; // Player unit moved successfully
    for (int unit = 0; unit < enemy_unit_count; ++unit) {
        if (enemy_units[unit][0] == from_x && enemy_units[unit][1] == from_y) {
            grid[to_x][to_y].unit[1] = grid[from_x][from_y].unit[1]; // Move the unit
            grid[from_x][from_y].unit[1] = 0;
            enemy_units[unit][0] = to_x; // Update enemy unit position
            enemy_units[unit][1] = to_y;
            moved = 2; // Enemy unit moved
            break;
        }
    }
    if (!moved) {
        printf("No unit found at (%d, %d)\n", from_x, from_y);
        return -4; // No unit found at source
    }
    return 0; // Move successful
}*/

void find_front(int player, int center_x, int center_y, struct unit *front_units, int *count) {
    if (player < 0 || player >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return; // Invalid player
    }
    for (int unit = 0; unit < player_unit_count[player]; unit++) {
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
        struct unit unit = player_units[player][unit_id];
        for (int dir = 0; dir < DIRECTIONS_COUNT; dir++) {
            int spawn_x = unit.x + dir_offsets[dir].x;
            int spawn_y = unit.y + dir_offsets[dir].y;
            if (spawn_y >= 0 && spawn_x >= 0 && spawn_y < units.w && spawn_x < units.w) {
                bool has_unit = units.pix[spawn_y * units.w + spawn_x] != 0;
                bool is_sea = map.pix[spawn_y * map.w + spawn_x] == SEA;
                if (!has_unit && !is_sea && get_player(spawn_x, spawn_y) == player) {
                    if (add_unit(player, spawn_x, spawn_y) == 0) {
                        // printf("Spawned a new unit!\n");
                        return 1;
                    }
                }
            }
        }
    }
    //printf("Could not find location to spawn the unit\n");
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
    printf("count %d\n", count);
    for (int unit = 0; unit < player_unit_count[player]; unit++) {
        printf("(count=%d)", count);
        printf("AI unit %d, %d at (%d, %d)  ", unit, player, player_units[player][unit].x, player_units[player][unit].y);
        memset(player_paths[player][unit].steps, 0, sizeof(player_paths[player][unit].steps)); // clear array
        printf("(count=%d)", count);
        int x = player_units[player][unit].x;
        int y = player_units[player][unit].y;
        int distance = 2500;
        struct unit target = {0};
        bool found_target = false;
        printf("(count=%d)", count);
        for (int enemy = 0; enemy < count; enemy++) {
            printf("#");
            int dis = (front_units[enemy].x - x) * (front_units[enemy].x - x) +
                        (front_units[enemy].y - y) * (front_units[enemy].y - y);
            if (dis < distance) {
                distance = dis;
                target = front_units[enemy];
                found_target = true;
            }
        }
        printf("(count=%d)", count);
        uint8_t path[GRID_W + GRID_H];
        int path_length = 0;
        int result = 0;
        printf(" to (%d, %d) with path length %d", target.x, target.y, path_length);
        if (found_target) {
        printf("(count=%d)", count);
            result = pathing(x, y, target.x, target.y, path, &path_length); // Get path to target
        printf("(count=%d)", count);
            if (result == 1) {
                printf(" - Path found");
                player_paths[player][unit].length = path_length;
                for (int step = 0; step < path_length; step++) {
                    player_paths[player][unit].steps[step] = path[path_length - step - 1];
                    //printf("Step added %d: (%d, %d)\n", step, player_paths[player][unit].steps[step].x, player_paths[player][unit].steps[step].y);
                }
            }
            else {
                printf(" - No path found1");
                player_paths[player][unit].length = 0; // No path found
                //printf("No steps: (%d, %d)\n", player_paths[player][unit].steps[0].x, player_paths[player][unit].steps[0].y);
            }
        }
        else {
            printf(" - No path found2");
            player_paths[player][unit].length = 0; // No path found
            //printf("No steps: (%d, %d)\n", player_paths[player][unit].steps[0].x, player_paths[player][unit].steps[0].y);
        }
        printf("(count=%d)\n", count);
    }
    return 0; // AI movement done
}

void player_turn(enum players player) {
    // verify that the player exists in the player enum
    if (player < 0 || player >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return; // Invalid player
    }
    // add 1 money for every city
    player_money[player] += player_cities[player];
    // buy units for every 10 money
    static const int UNIT_COST = 10;
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
/*
void player_turn(int player) {
    if (player_target[player].type > 0 && player_target[player].type < 8) {
        player_target[player].type ++; // reset target
    }
    else {
        player_target[player].type = 0; // reset target
        for (int unit = 0; unit < player_unit_count[player]; unit++) {
            int result = find_target(player, unit, &player_target[player]);
            if (result == 1) {break;}
        }
    }
    if (player_target[player].type == 0) {
        for (int unit = 0; unit < player_unit_count[player]; unit++) {
            int dir = rand() % 4;
            int x = player_units[player][unit].x;
            int y = player_units[player][unit].y;
            move_unit(player, unit, x + (1-dir%2*2)*(1-dir/2), y + dir/2*(dir/2 - dir%2*2));
        }
    }
    else {
        for (int unit = 0; unit < player_unit_count[player]; unit++) {
            int x = player_units[player][unit].x;
            int y = player_units[player][unit].y;
            if (abs(player_target[player].x - x) > abs(player_target[player].y - y)) {
                int dir_x = (player_target[player].x - x) < 0 ? -1 : (player_target[player].x - x) > 0 ? 1 : 0; // get direction x-axis
                int result = move_unit(player, unit, x + dir_x, y);
                if (result == -3) { // Tile already occupied
                    int dir_y = (player_target[player].y - y) < 0 ? -1 : (player_target[player].y - y) > 0 ? 1 : 0; // get direction y-axis
                    move_unit(player, unit, x, y + dir_y);
                }
            }
            else {
                int dir_y = (player_target[player].y - y) < 0 ? -1 : (player_target[player].y - y) > 0 ? 1 : 0; // get direction y-axis
                int result = move_unit(player, unit, x, y + dir_y);
                if (result == -3) { // Tile already occupied
                    int dir_x = (player_target[player].x - x) < 0 ? -1 : (player_target[player].x - x) > 0 ? 1 : 0; // get direction x-axis
                    move_unit(player, unit, x + dir_x, y);
                }
            }
            //move_unit(player, unit, x + dir_x, y + dir_y);
        }
    }
}
*/

void script(int frame) {
    if (frame == 0) {
        player_turn(0);
    }
    if (frame == 1) {
        player_turn(1);
    }
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
}

static inline struct tga tga_load(const char *path)
{
    int fd = open(path, O_RDONLY | 02000000);                        /* 1 */
    assert(fd >= 0);

    uint8_t h18[18];
    assert(read(fd, h18, 18) == 18);

    /* ─── validate header ────────────────────────────────────────── */
    if (h18[2]  != 2)   { fprintf(stderr,"TGA type ≠ 2\n");          exit(1); }
    if (h18[16] != 32){ fprintf(stderr,"TGA bpp ≠ 32 (got %u)\n",h18[16]);exit(1);}
    if ((h18[17] & 0x20) == 0){ fprintf(stderr,"TGA not top-left\n"); exit(1); }

    int w = h18[12] | h18[13]<<8;
    int h = h18[14] | h18[15]<<8;

    size_t off   = 18 + h18[0];              /* header + ID-field      */
    size_t bytes = (size_t)w * h * 4;    /* 4 bytes per pixel      */

    struct stat st;
    fstat(fd,&st);
    assert(off + bytes <= (size_t)st.st_size);

    const void *map = mmap(0, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    assert(map != MAP_FAILED);
    close(fd);

    return (struct tga){ w, h, (const uint32_t*)((uint8_t *)map + off), map, st.st_size };
}

static inline void tga_free(struct tga img)
{
    munmap((void*)img.map, img.map_len);
}

int main(void)
{
    struct ctx *window = create_window(WIDTH, HEIGHT, "<<Fatzke>>");
    set_input_cb(window, key_input_callback, mouse_input_callback, NULL);
    
    map = tga_load("map.tga");
    units = tga_load("units.tga");
    countries = tga_load("countries.tga");

    struct timespec ts = {0};
    int stride;
    uint32_t frame = 0;

    // find the cities on the map
    for (int y = 0; y < map.h; ++y) {
        for (int x = 0; x < map.w; ++x) {
            uint32_t pixel = map.pix[y * map.w + x];
            if (pixel == CITY) {
                int player_id = get_player(x, y);
                if (player_id != -1) player_cities[player_id]++;
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
    // test pathing
    uint8_t path[GRID_W + GRID_H];
    int pathlength = 0;
    int result = pathing(20, 0, 20, 5, path, &pathlength);
    pos loc = {20, 0};
    if (result == 1) {
        printf("Path found: ");
        for (int i = 1; i < pathlength + 1; i++) {
            loc.x += dir_offsets[path[pathlength - i]].x; // update x position
            loc.y += dir_offsets[path[pathlength - i]].y; // update y position
            printf("%d: (%d, %d) ", path[pathlength - i], loc.x, loc.y);
        }
        printf("\n");
    } else {
        printf("No path found\n");
    }

    while(window_poll(window)) {// poll for events and break if compositor connection is lost
        uint32_t *buffer = get_pixels(window, &stride);
        script(frame);

        // Clear the buffer
        memset(buffer, 255, WIDTH * HEIGHT * sizeof(uint32_t));
        draw_grid(buffer);
        draw_units(buffer);
        draw_paths(buffer);
        frame ++;

        window_wait_vsync(window); // wait for vsync (and keep processing events) before next frame
        commit(window); // tell compositor it can read from the buffer
    }
    destroy(window);
}
