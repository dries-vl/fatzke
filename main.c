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

#define SEA 0xFF90ccd4
#define LAND 0xFF21480e

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

struct tile {
    int terrain;
    int x, y;
    int unit[2]; // 0 = player, 1 = enemy
    int building; // wa random shit
};
struct unit {
    int x, y; // position on grid
    int type;
};
struct tile grid[GRID_W][GRID_H];
int player_unit_count[] = {0, 0}; // number of units per player
struct unit player_units[PLAYER_COUNT][MAX_UNITS] = {0};

struct unit player_target[PLAYER_COUNT] = {{0, 0, 0}, {0, 0, 0}};

/* 32-bit uncompressed, top-left-origin TGA */
struct tga { 
    int w, h;                /* dimensions                    */
    const uint8_t *pix;       /* BGRA pixel pointer            */
    const void *map;      /* whole mmapped file            */
    size_t map_len;  /* length for munmap             */
};

struct tga map;
struct tga units;

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
                uint8_t blue = map.pix[(map_y*map.w + map_x)*4+0];
                uint8_t green= map.pix[(map_y*map.w + map_x)*4+1];
                uint8_t red = map.pix[(map_y*map.w + map_x)*4+2];
                uint8_t alpha = map.pix[(map_y*map.w + map_x)*4+3];
                uint32_t pixel = (alpha << 24) | (red << 16) | (green << 8) | blue;
                if (pixel == SEA)
                    buffer[y * WIDTH + x] = BLUE;
                else if (pixel == LAND)
                    buffer[y * WIDTH + x] = WHITE;
                else
                    buffer[y * WIDTH + x] = RED;
            }
        }
    }
}

void draw_unit(struct tile *t, uint32_t *buffer)
{
    int x = t->x * PIXEL_SIZE + PIXEL_SIZE / 2;
    int y = t->y * PIXEL_SIZE + PIXEL_SIZE / 2;
    if (t->unit[0]) {
        // Draw player unit
        for (int dx = -UNIT_SIZE/2; dx <= UNIT_SIZE/2; ++dx) {
            for (int dy = -UNIT_SIZE/2; dy <= UNIT_SIZE/2; ++dy) {
                buffer[(y + dy) * WIDTH + (x + dx)] = player_colors[0];
            }
        }
    }
    if (t->unit[1]) {
        // Draw enemy unit
        for (int dx = -UNIT_SIZE/2; dx <= UNIT_SIZE/2; ++dx) {
            for (int dy = -UNIT_SIZE/2; dy <= UNIT_SIZE/2; ++dy) {
                buffer[(y + dy) * WIDTH + (x + dx)] = player_colors[1];
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
            draw_unit(&grid[x][y], buffer);
        }
    }
}

int move_unit(int player, int unit, int to_x, int to_y) {
    if (!player_units[player] || unit < 0 || unit >= player_unit_count[player]) {
        printf("Invalid player or unit index\n");
        return -1; // Invalid player or unit
    }
    if (to_x < 0 || to_x >= GRID_W || to_y < 0 || to_y >= GRID_H) {
        printf("Invalid move to (%d, %d)\n", to_x, to_y);
        return -2; // Invalid move
    }
    if (grid[to_x][to_y].terrain == 1) {
        //printf("Cannot move to water tile (%d, %d)\n", to_x, to_y);
        return -4; // Cannot move to water tile
    }
    if (grid[to_x][to_y].unit[player]) {
        //printf("Tile (%d, %d) already occupied\n", to_x, to_y);
        return -3; // Tile already occupied
    }
    else if (grid[to_x][to_y].unit[1 - player]) { // BATTLE!
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
    grid[to_x][to_y].unit[player] = grid[from_x][from_y].unit[player];
    grid[from_x][from_y].unit[player] = 0;
    player_units[player][unit].x = to_x; // Update player unit position
    player_units[player][unit].y = to_y;
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
        printf("Max units reached for player %d\n", player);
        return -3; // Max units reached
    }
    if (grid[x][y].unit[0] || grid[x][y].unit[1]) {
        printf("Tile (%d, %d) already occupied\n", x, y);
        return -4; // Tile already occupied
    }
    player_units[player][player_unit_count[player]] = (struct unit){x, y, 1};
    grid[x][y].unit[player] = 1; // Mark unit on grid
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
    grid[x][y].unit[player] = 0; // Remove unit from grid
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
        printf("Attacker wins!\n");
        remove_unit(defender, unit_def); // Remove defender unit
        move_unit(attacker, unit_att, x_def, y_def); // Move attacker to defender position
        return 1; // Attacker wins
    } else if (random < 3) { // Defender wins 1/3
        printf("Defender wins!\n"); // Defender wins 2/3
        remove_unit(attacker, unit_att); // Remove attacker unit
        return 2; // Defender wins
    } else { // Draw 1/3
        printf("Draw!\n");
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
    return move_unit(player, unit, x + dir_x, y + dir_y);
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
    if (grid[x+1][y].unit[1 - player] && x+1 < GRID_W) {
        target->type = 1; target->x = x+1; target->y = y;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    if (grid[x-1][y].unit[1 - player] && x-1 >= 0) {
        target->type = 1; target->x = x-1; target->y = y;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    if (grid[x][y+1].unit[1 - player] && y+1 < GRID_H) {
        target->type = 1; target->x = x; target->y = y+1;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    if (grid[x][y-1].unit[1 - player] && y-1 >= 0) {
        target->type = 1; target->x = x; target->y = y-1;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    return 0; // No target found
}

void player_turn(int player) {
    if (player < 0 || player >= PLAYER_COUNT) {
        printf("Invalid player index\n");
        return; // Invalid player
    }
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
        printf("Unit %d at (%d, %d) targeting (%d, %d)\n", unit, x, y, target.x, target.y);
        move_towards(player, unit, target.x, target.y);
    }
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
    /*
    if (frame == 100){move_unit(0, 0, 6, 6);}
    if (frame == 140){move_unit(1, 0, 9, 7);}
    if (frame == 180){add_unit(1, 11, 9);}
    if (frame == 220){move_unit(1, 0, 9, 6);}
    if (frame == 260){move_unit(1, 2, 10, 9);}
    */
    if (frame % 20 == 0) {
        player_turn(0); // Player 0's turn every 60 frames
    }
    if (frame % 20 == 6) {
        player_turn(1); // Player 1's turn every 120 frames
    }
}

static inline struct tga tga_load(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);                        /* 1 */
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

    const void *map = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(map != MAP_FAILED);
    close(fd);

    return (struct tga){ w, h, (const uint8_t*)map + off, map, st.st_size };
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

    struct timespec ts = {0};
    int stride;
    uint32_t frame = 0;
    // Initialize grid
    for (int x = 0; x < GRID_W; ++x) {
        for (int y = 0; y < GRID_H; ++y) {
            grid[x][y].terrain = 0; // 0, 1, or 2
            grid[x][y].x = x;
            grid[x][y].y = y;
            grid[x][y].unit[0] = 0; // no player unit
            grid[x][y].unit[1] = 0; // no enemy unit
            grid[x][y].building = 0; // random building presence
        }
    }

    // loop over map tga and fill the grid with terrain
    for (int y = 0; y < map.h; ++y) {
        for (int x = 0; x < map.w; ++x) {
            uint8_t blue = map.pix[(y*map.w + x)*4+0];
            uint8_t green= map.pix[(y*map.w + x)*4+1];
            uint8_t red = map.pix[(y*map.w + x)*4+2];
            uint8_t alpha = map.pix[(y*map.w + x)*4+3];
            uint32_t pixel = (alpha << 24) | (red << 16) | (green << 8) | blue;
            if (pixel == SEA) {
                grid[x][y].terrain = 1; // water
            } else if (pixel == LAND) {
                grid[x][y].terrain = 0; // land
            } else {
                grid[x][y].terrain = 2; // random terrain
            }
        }
    }
    
    // loop over units in units tga and use the add_unit function to add them to the grid
    for (int y = 0; y < units.h; ++y) {
        for (int x = 0; x < units.w; ++x) {
            uint8_t blue = units.pix[(y*units.w + x)*4+0];
            uint8_t green= units.pix[(y*units.w + x)*4+1];
            uint8_t red = units.pix[(y*units.w + x)*4+2];
            uint8_t alpha = map.pix[(y*map.w + x)*4+3];
            uint32_t pixel = (alpha << 24) | (red << 16) | (green << 8) | blue;
            if (pixel == GERMANY_COLOR) {
                add_unit(GERMANY, x, y);
            } else if (pixel == SOVIET_COLOR) {
                add_unit(SOVIET, x, y);
            }
        }
    }
    for (int unit = 0; unit < player_unit_count[0]; unit++) {
        grid[player_units[0][unit].x][player_units[0][unit].y].unit[0] = 1; // player unit
    }
    for (int unit = 0; unit < player_unit_count[1]; unit++) {
        grid[player_units[1][unit].x][player_units[1][unit].y].unit[1] = 1; // enemy unit
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
