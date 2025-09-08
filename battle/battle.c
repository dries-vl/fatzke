#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

#define MAX_UNIT_SIZE 256
#define MAX_UNITS 256
#define u32 uint32_t

typedef struct {
    u32 x, y, z;
} pos;

struct soldier { // 16 bytes assumes uniform units
    pos pos;
    u32 health;
};

struct unit {
    u32 size;
    u32 type;
    struct soldier soldier_list[MAX_UNIT_SIZE];
};

void load_units(struct unit units[MAX_UNITS]) {
    units[1].size = 12;
    units[1].type = 1; // infantry
    for (u32 i = 0; i < units[1].size; i++) {
        units[1].soldier_list[i].pos.x = 5;
        units[1].soldier_list[i].pos.y = 3+i;
        units[1].soldier_list[i].pos.z = 0;
        units[1].soldier_list[i].health = 10;
    }
    units[2].size = 12;
    units[2].type = 1;
    for (u32 i = 0; i < units[2].size; i++) {
        units[2].soldier_list[i].pos.x = 144;
        units[2].soldier_list[i].pos.y = 3+i;
        units[2].soldier_list[i].pos.z = 0;
        units[2].soldier_list[i].health = 10;
    }
}
void draw_battlefield(struct unit units[MAX_UNITS]) {
    // lees units
    char battlefield[20][150] = {0}; // TEMP battlefield size
    for (u32 i = 0; i < 2; i++) { // laad 2 units
        for (u32 j = 0; j < units[i+1].size; j++) {
            battlefield[units[i+1].soldier_list[j].pos.y][units[i+1].soldier_list[j].pos.x] = (i == 0) ? 'A' : 'B';
        }
    }
    // draw battlefield TEMP
    printf("\n========================================================================================================================================================");
    for (u32 row = 0; row < 20; row++) {
        printf("\n|");
        for (u32 col = 0; col < 150; col++) {
            printf("%c", battlefield[row][col] ? battlefield[row][col] : ' ');
        }
        printf("|");
    }
    printf("\n========================================================================================================================================================");
}


void simulate_battle() {
    printf("Simulating battle...\n");
    struct unit units[MAX_UNITS];
    load_units(units);

    while (true) {
        draw_battlefield(units);
        sleep(1000);
    }
}
