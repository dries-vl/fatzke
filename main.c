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

#define WHITE 0xFFFFFFFF
#define BLACK 0xFF000000
#define RED 0xFFFF0000
#define GREEN 0xFF00FF00
#define YELLOW 0xFFFFFF00

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
    printf("Tile not found for color: 0x%08X\n", tile_color);
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
int player_unit_count[PLAYER_COUNT] = {0}; // number of units per player
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

#define LIGHTEN_DESATURATE(c, lf, sf) ({ \
    uint8_t r = (c)&0xFF, g = ((c)>>8)&0xFF, b = ((c)>>16)&0xFF, a = ((c)>>24)&0xFF; \
    uint8_t gray = (r + g + b) / 3; \
    r = (1 - sf) * gray + sf * r; \
    g = (1 - sf) * gray + sf * g; \
    b = (1 - sf) * gray + sf * b; \
    r = r + (255 - r) * (lf - 1); \
    g = g + (255 - g) * (lf - 1); \
    b = b + (255 - b) * (lf - 1); \
    (a << 24) | ((uint8_t)(b) << 16) | ((uint8_t)(g) << 8) | (uint8_t)(r); \
})

int battle(int attacker, int defender, int unit_att, int unit_def);

int pathing(int from_x, int from_y, int to_x, int to_y, pos *path, int *pathlength) { // no cost yet, BITshift versie??
    if (!path || !pathlength) return -1; // Invalid arguments
    if (from_x < 0 || from_x >= GRID_W || from_y < 0 || from_y >= GRID_H ||
        to_x < 0 || to_x >= GRID_W || to_y < 0 || to_y >= GRID_H) {
        return -1; // Invalid coordinates
    }
    int paths_count = 1;
    pos paths[GRID_W * GRID_H][GRID_H + GRID_W] = {0}; // STACK OVERFLOW ?????
    paths[0][0] = (pos){1, 0}; // path length
    paths[0][1] = (pos){from_x, from_y};
    for (int i = 0; i < paths_count; i++) {
        int pass = 0;
        if (paths_count >= GRID_W * GRID_H) {
            printf("Too many paths, aborting\n");
            return 0; // ran out of memory
        }
        int x;
        int y;
        for (int dir = UP; dir <= DOWN_RIGHT; dir++){
            pass = 0; // reset pass for each direction
            // Generic
            x = paths[i][1].x + dir_offsets[dir].x;
            y = paths[i][1].y + dir_offsets[dir].y;
            if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) continue; // out of bounds
            if (to_x == x && to_y == y) { // found target
                paths[paths_count][0] = (pos){paths[i][0].x + 1, 0};
                paths[paths_count][1] = (pos){x, y};
                for (int j = 2; j < paths[i][0].x + 2; j++) {
                    paths[paths_count][j] = paths[i][j-1];
                }
                *pathlength = paths[paths_count][0].x; // set path length
                memcpy(path, paths[paths_count] + 1, sizeof(pos) * (*pathlength)); // copy path to output
                return 1; // found path
            }
            if (map.pix[y*map.w + x] != SEA) { // better terrain handling needed
                for (int j = 0; j < paths_count; j++) {
                    if (paths[j][1].x == x && paths[j][1].y == y) {
                        pass = 1;
                        break;
                    }
                }
                if (pass == 0) {
                    paths[paths_count][0] = (pos){paths[i][0].x + 1, 0};
                    paths[paths_count][1] = (pos){x, y};
                    for (int j = 2; j < paths[i][0].x + 2; j++) {
                        paths[paths_count][j] = paths[i][j-1];
                    }
                    paths_count++;
                }
            }
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
                // todo: fix
                if (tile_income[get_tile(map_x, map_y)] > 0) {
                    uint32_t country_pixel = players.pix[map_y * players.w + map_x];
                    buffer[y * WIDTH + x] = country_pixel;
                } else
                    buffer[y * WIDTH + x] = LIGHTEN_DESATURATE(pixel, 1.2, 0.6);
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
            buffer[(y + dy) * WIDTH + (x + dx)] = player_colors[player];
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
}

void script(int frame) {
    if (frame % 2 == 1) {
        player_turn(0);
    }
    if (frame % 2 == 0) {
        player_turn(1);
    }
}

static inline struct tga tga_load(const char *path)
{
    int fd = open(path, O_RDONLY | 02000000);
    if (fd < 0) {fprintf(stderr, "File not found: %s\n", path); exit(1);}

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
    players = tga_load("players.tga");

    struct timespec ts = {0};
    int stride;
    uint32_t frame = 0;

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

    // test Pathing
    pos path[GRID_W + GRID_H];
    int path_length = 0;
    int result = pathing(20, 0, 20, 5, path, &path_length); // path from (0,0) to (10,10)
    printf("Pathing result: %d\n", result);
    for (int i = 0; i < path_length; i++) {
        printf("Path step %d: (%d, %d)\n", i, path[i].x, path[i].y);
    }

    while(window_poll(window)) {// poll for events and break if compositor connection is lost
        uint32_t *buffer = get_pixels(window, &stride);
        script(frame);

        // Clear the buffer
        memset(buffer, 255, WIDTH * HEIGHT * sizeof(uint32_t));
        draw_grid(buffer);
        draw_units(buffer);
        frame ++;

        window_wait_vsync(window); // wait for vsync (and keep processing events) before next frame
        commit(window); // tell compositor it can read from the buffer
    }
    destroy(window);
}
