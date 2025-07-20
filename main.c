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

#define WHITE 0xFFFFFFFF
#define BLACK 0xFF000000
#define RED 0xFFFF0000
#define GREEN 0xFF00FF00
#define YELLOW 0xFFFFFF00

#define SEA 0xFF90ccd4
#define LAND 0xFF21480e

struct tile {
    int terrain;
    int x, y;
    int unit[2]; // 0 = player, 1 = enemy
    int building; // wa random shit
};
struct tile grid[GRID_W][GRID_H];
int player_units[][2] = {{5, 5}};
int player_unit_count = sizeof(player_units) / sizeof(player_units[0]);
int enemy_units[][2] = {{10, 10}};
int enemy_unit_count = sizeof(enemy_units) / sizeof(enemy_units[0]);

#include <fcntl.h>
struct tga {
    int            w, h;        /* pixels                    */
    const uint8_t *pix;         /* BGR… rows (top-left 0,0)  */
    const void    *map;         /* ptr to whole mmap         */
    size_t         map_len;     /* exact mmap length         */
};

struct tga map;

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
                uint8_t blue = map.pix[(map_y*map.w + map_x)*3+0];
                uint8_t green= map.pix[(map_y*map.w + map_x)*3+1];
                uint8_t red = map.pix[(map_y*map.w + map_x)*3+2];
                uint32_t pixel = (0xFF << 24) | (red << 16) | (green << 8) | blue;
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
    for (int unit = 0; unit < player_unit_count; ++unit) {
        int x = player_units[unit][0];
        int y = player_units[unit][1];
        draw_unit(&grid[x][y], buffer);
    }
    for (int unit = 0; unit < enemy_unit_count; ++unit) {
        int x = enemy_units[unit][0];
        int y = enemy_units[unit][1];
        draw_unit(&grid[x][y], buffer);
    }
}

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
}

void script(int frame) {
    if (frame == 60){move_unit(5, 5, 6, 6);}
    if (frame == 80){move_unit(10, 10, 9, 9);}
}

static inline struct tga tga_load(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);

    uint8_t h18[18];
    assert(read(fd, h18, 18) == 18);

    // Only accept type-2, 24bpp, top-left origin (bit 5)
    if (h18[2] != 2) {printf("TGA type is not 2\n"); exit(1);}
    if (h18[16] != 24) {printf("TGA is not 24bit but: %d\n", h18[26]); exit(1);}
    if (h18[17] & 0x20 == 0) {printf("TGA is not top-left origin\n"); exit(1);}

    int w = h18[12] | h18[13] << 8;
    int h = h18[14] | h18[15] << 8;

    size_t off   = 18 + h18[0];         /* header + ID field length */
    size_t bytes = (size_t)w * h * 3;   /* pixel payload */

    struct stat st;
    fstat(fd, &st);
    assert(off + bytes <= (size_t)st.st_size);

    size_t len   = (size_t)st.st_size;
    const void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(map != MAP_FAILED);
    close(fd);

    struct tga img = { w, h, (const uint8_t*)map + off, map, len };
    return img;
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

    struct timespec ts = {0};
    int stride;
    uint32_t frame = 0;
    // Initialize grid with some random tiles
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
    for (int unit = 0; unit < player_unit_count; unit++) {
        grid[player_units[unit][0]][player_units[unit][1]].unit[0] = 1; // player unit
    }
    for (int unit = 0; unit < enemy_unit_count; unit++) {
        grid[enemy_units[unit][0]][enemy_units[unit][1]].unit[1] = 1; // enemy unit
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
