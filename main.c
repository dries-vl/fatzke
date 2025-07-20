// tcc main.c -lwayland-client -run
#include <stdio.h>
#include <stdint.h>
#include <time.h>

// sudo apt install libwayland-dev
#include "wayland/wayland.c"

#define HEIGHT 1000
#define WIDTH 1000
#define PIXEL_SIZE 20
#define GRID_W (WIDTH / PIXEL_SIZE) // now just screen size
#define GRID_H (HEIGHT / PIXEL_SIZE)
#define UNIT_SIZE 14 // size of a unit on screen
#define MAX_UNITS 100 // max number of units per player
#define PLAYER_COUNT 2 // number of players

#define WHITE 0xFFFFFFFF
#define BLACK 0xFF000000
#define RED 0xFFFF0000
#define GREEN 0xFF00FF00
#define YELLOW 0xFFFFFF00

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
//player_units[0] = {{5, 5}, {5, 6}, {5, 7}, {5, 8}, {5, 9}, {5, 10}, {5, 11}, {5, 12}, {5, 13}, {5, 14}};
//player_units[1] = {{10, 10}, {11, 10}, {12, 10}, {13, 10}, {14, 10}, {10, 11}, {11, 11}, {12, 11}, {13, 11}, {14, 11}};

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

void draw_grid(uint32_t *buffer)
{
    for (int x = 0; x < WIDTH; ++x) {
        for (int y = 0; y < HEIGHT; ++y) {
            if (x % PIXEL_SIZE == 0 || y % PIXEL_SIZE == 0)
                buffer[y * WIDTH + x] = BLACK;
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
                buffer[(y + dy) * WIDTH + (x + dx)] = GREEN;
            }
        }
    }
    if (t->unit[1]) {
        // Draw enemy unit
        for (int dx = -UNIT_SIZE/2; dx <= UNIT_SIZE/2; ++dx) {
            for (int dy = -UNIT_SIZE/2; dy <= UNIT_SIZE/2; ++dy) {
                buffer[(y + dy) * WIDTH + (x + dx)] = RED;
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
    if (grid[to_x][to_y].unit[0] || grid[to_x][to_y].unit[1]) {
        printf("Tile (%d, %d) already occupied\n", to_x, to_y);
        return -3; // Tile already occupied
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

int find_target(int player, int unit, struct unit *target) {
    if (player < 0 || player >= PLAYER_COUNT || unit < 0 || unit >= player_unit_count[player]) {
        printf("Invalid player or unit index\n");
        return -1; // Invalid player or unit
    }
    int x = player_units[player][unit].x;
    int y = player_units[player][unit].y;
    if (grid[x+1][y].unit[1 - player]) {
        target->type = 1; target->x = x+1; target->y = y;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    if (grid[x-1][y].unit[1 - player]) {
        target->type = 1; target->x = x-1; target->y = y;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    if (grid[x][y+1].unit[1 - player]) {
        target->type = 1; target->x = x; target->y = y+1;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    if (grid[x][y-1].unit[1 - player]) {
        target->type = 1; target->x = x; target->y = y-1;
        printf("Target found at (%d, %d) for player %d\n", x, y, player);
        return 1; // Return target x coordinate
    }
    return 0; // No target found
}

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

void script(int frame) {
    /*
    if (frame == 100){move_unit(0, 0, 6, 6);}
    if (frame == 140){move_unit(1, 0, 9, 7);}
    if (frame == 180){add_unit(1, 11, 9);}
    if (frame == 220){move_unit(1, 0, 9, 6);}
    if (frame == 260){move_unit(1, 2, 10, 9);}
    */
    if (frame % 6 == 0) {
        player_turn(0); // Player 0's turn every 60 frames
    }
    if (frame % 6 == 2) {
        player_turn(1); // Player 1's turn every 120 frames
    }
}

int main(void)
{
    struct ctx *window = create_window(WIDTH, HEIGHT, "<<Fatzke>>");
    set_input_cb(window, key_input_callback, mouse_input_callback, NULL);

    struct timespec ts = {0};
    int stride;
    uint32_t frame = 0;
    // Initialize grid with some random tiles
    
    add_unit(0, 5, 5);
    add_unit(0, 5, 6);
    add_unit(0, 5, 7);
    add_unit(0, 5, 8);
    add_unit(0, 5, 9);
    add_unit(0, 5, 10);
    add_unit(0, 5, 11);
    add_unit(0, 5, 12);
    add_unit(0, 5, 13);
    add_unit(0, 5, 14);
    add_unit(1, 20, 7);
    add_unit(1, 20, 8);
    add_unit(1, 20, 9);
    add_unit(1, 20, 10);
    add_unit(1, 20, 11);
    add_unit(1, 20, 12);
    add_unit(1, 20, 13);
    add_unit(1, 20, 14);
    add_unit(1, 20, 15);
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
