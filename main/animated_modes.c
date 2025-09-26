#include "animated_modes.h"

#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"

#include "framebuffer.h"
#include "flip_dot_driver.h"

#define FIREFLY_MAX_COUNT 7
#define FIREFLY_MIN_TTL 4
#define FIREFLY_MAX_TTL 8
#define FIREFLY_SPAWN_CHANCE_PERCENT 20
#define FIREFLY_AFTERGLOW_MAX 4

#define LANGTON_STEPS_PER_TICK 4
#define LANGTON_RESET_AFTER_STEPS 5000

#define MATRIX_TAIL_MIN 2
#define MATRIX_TAIL_MAX 4
#define MATRIX_SPAWN_CHANCE_PERCENT 25
#define MATRIX_COOLDOWN_MIN 4
#define MATRIX_COOLDOWN_MAX 18

typedef struct {
    bool active;
    int8_t x;
    int8_t y;
    uint8_t ttl;
    int8_t dx;
    int8_t dy;
} firefly_t;

typedef struct {
    bool initialized;
    uint8_t grid[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];
    int8_t ant_x;
    int8_t ant_y;
    uint8_t direction;
    uint32_t steps;
} langton_state_t;

typedef struct {
    bool active;
    int8_t head_row;
    uint8_t tail_length;
    uint8_t cooldown;
} matrix_column_t;

static firefly_t fireflies[FIREFLY_MAX_COUNT];
static uint8_t firefly_afterglow[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];
static bool fireflies_initialized;

static langton_state_t langton_state;

static matrix_column_t matrix_columns[FRAMEBUFFER_WIDTH];
static uint8_t matrix_intensity[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];
static bool matrix_initialized;

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int8_t random_step_component(void)
{
    return (int8_t)((int)(esp_random() % 3) - 1);
}

static int random_range(int min_value, int max_value)
{
    if (max_value <= min_value) {
        return min_value;
    }
    uint32_t span = (uint32_t)(max_value - min_value + 1);
    return (int)(min_value + (esp_random() % span));
}

static void reset_fireflies_state(void)
{
    memset(fireflies, 0, sizeof(fireflies));
    memset(firefly_afterglow, 0, sizeof(firefly_afterglow));
    fireflies_initialized = true;
}

static void spawn_firefly(void)
{
    for (int i = 0; i < FIREFLY_MAX_COUNT; i++) {
        firefly_t* firefly = &fireflies[i];
        if (!firefly->active) {
            firefly->active = true;
            firefly->ttl = (uint8_t)(FIREFLY_MIN_TTL + (esp_random() % (FIREFLY_MAX_TTL - FIREFLY_MIN_TTL + 1)));
            firefly->x = (int8_t)(esp_random() % FRAMEBUFFER_WIDTH);
            firefly->y = (int8_t)(esp_random() % FRAMEBUFFER_HEIGHT);
            int8_t dx = 0;
            int8_t dy = 0;
            while (dx == 0 && dy == 0) {
                dx = random_step_component();
                dy = random_step_component();
            }
            firefly->dx = dx;
            firefly->dy = dy;
            firefly_afterglow[firefly->y][firefly->x] = FIREFLY_AFTERGLOW_MAX;
            break;
        }
    }
}

static void reset_langton_state(void)
{
    memset(&langton_state, 0, sizeof(langton_state));
    langton_state.ant_x = (int8_t)random_range(0, FRAMEBUFFER_WIDTH - FRAMEBUFFER_WIDTH / 4);
    langton_state.ant_y = (int8_t)random_range(0, FRAMEBUFFER_HEIGHT - FRAMEBUFFER_HEIGHT / 4);
    langton_state.direction = (uint8_t)(esp_random() % 4);
    langton_state.initialized = true;
}

static void reset_matrix_state(void)
{
    memset(matrix_columns, 0, sizeof(matrix_columns));
    memset(matrix_intensity, 0, sizeof(matrix_intensity));
    for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
        matrix_columns[x].cooldown = (uint8_t)random_range(MATRIX_COOLDOWN_MIN, MATRIX_COOLDOWN_MAX);
    }
    matrix_initialized = true;
}

void handleModeFirefliesIdle(bool first_run)
{
    if (first_run || !fireflies_initialized) {
        reset_fireflies_state();
    }

    for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
            if (firefly_afterglow[y][x] > 0) {
                firefly_afterglow[y][x]--;
            }
        }
    }

    int active_count = 0;
    for (int i = 0; i < FIREFLY_MAX_COUNT; i++) {
        firefly_t* firefly = &fireflies[i];
        if (!firefly->active) {
            continue;
        }

        int8_t dx = firefly->dx;
        int8_t dy = firefly->dy;

        dx = (int8_t)clamp_int(dx + random_step_component(), -1, 1);
        dy = (int8_t)clamp_int(dy + random_step_component(), -1, 1);

        if (dx == 0 && dy == 0) {
            dx = firefly->dx;
            dy = firefly->dy;
            if (dx == 0 && dy == 0) {
                do {
                    dx = random_step_component();
                    dy = random_step_component();
                } while (dx == 0 && dy == 0);
            }
        }

        int new_x = clamp_int((int)firefly->x + dx, 0, FRAMEBUFFER_WIDTH - 1);
        int new_y = clamp_int((int)firefly->y + dy, 0, FRAMEBUFFER_HEIGHT - 1);

        firefly->x = (int8_t)new_x;
        firefly->y = (int8_t)new_y;
        firefly->dx = dx;
        firefly->dy = dy;

        firefly_afterglow[new_y][new_x] = FIREFLY_AFTERGLOW_MAX;

        if (firefly->ttl > 0) {
            firefly->ttl--;
        }

        if (firefly->ttl == 0) {
            firefly->active = false;
        } else {
            active_count++;
        }
    }

    if (active_count < FIREFLY_MAX_COUNT && (esp_random() % 100) < FIREFLY_SPAWN_CHANCE_PERCENT) {
        spawn_firefly();
    }

    uint8_t* framebuffer = framebuffer_clear();
    for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
            if (firefly_afterglow[y][x] > 0) {
                framebuffer_set_pixel_value((uint8_t)x, (uint8_t)y, 1);
            }
        }
    }

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(150));
}

void handleModeCellularAutomata(bool first_run)
{
    if (first_run || !langton_state.initialized) {
        reset_langton_state();
    }

    int x = langton_state.ant_x;
    int y = langton_state.ant_y;
    uint8_t direction = langton_state.direction;

    for (int step = 0; step < LANGTON_STEPS_PER_TICK; step++) {
        if (langton_state.steps >= LANGTON_RESET_AFTER_STEPS) {
            reset_langton_state();
            x = langton_state.ant_x;
            y = langton_state.ant_y;
            direction = langton_state.direction;
        }

        uint8_t* cell = &langton_state.grid[y][x];
        if (*cell) {
            *cell = 0;
            direction = (uint8_t)((direction + 3) & 0x03);
        } else {
            *cell = 1;
            direction = (uint8_t)((direction + 1) & 0x03);
        }

        switch (direction) {
            case 0:
                y--;
                break;
            case 1:
                x++;
                break;
            case 2:
                y++;
                break;
            default:
                x--;
                break;
        }

        if (x < 0) {
            x = FRAMEBUFFER_WIDTH - 1;
        } else if (x >= FRAMEBUFFER_WIDTH) {
            x = 0;
        }
        if (y < 0) {
            y = FRAMEBUFFER_HEIGHT - 1;
        } else if (y >= FRAMEBUFFER_HEIGHT) {
            y = 0;
        }

        langton_state.steps++;
    }

    langton_state.ant_x = (int8_t)x;
    langton_state.ant_y = (int8_t)y;
    langton_state.direction = direction;

    uint8_t* framebuffer = framebuffer_clear();
    for (int row = 0; row < FRAMEBUFFER_HEIGHT; row++) {
        for (int col = 0; col < FRAMEBUFFER_WIDTH; col++) {
            if (langton_state.grid[row][col]) {
                framebuffer_set_pixel_value((uint8_t)col, (uint8_t)row, 1);
            }
        }
    }
    framebuffer_set_pixel_value((uint8_t)langton_state.ant_x, (uint8_t)langton_state.ant_y, 1);

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(300));
}

void handleModeMatrixRain(bool first_run)
{
    if (first_run || !matrix_initialized) {
        reset_matrix_state();
    }

    for (int row = 0; row < FRAMEBUFFER_HEIGHT; row++) {
        for (int col = 0; col < FRAMEBUFFER_WIDTH; col++) {
            if (matrix_intensity[row][col] > 0) {
                matrix_intensity[row][col]--;
            }
        }
    }

    for (int col = 0; col < FRAMEBUFFER_WIDTH; col++) {
        matrix_column_t* column = &matrix_columns[col];
        if (column->active) {
            int next_head = column->head_row + 1;
            column->head_row = (int8_t)next_head;
            if (next_head >= 0 && next_head < FRAMEBUFFER_HEIGHT) {
                matrix_intensity[next_head][col] = column->tail_length;
            }
            if (next_head >= FRAMEBUFFER_HEIGHT) {
                column->active = false;
                column->cooldown = (uint8_t)random_range(MATRIX_COOLDOWN_MIN, MATRIX_COOLDOWN_MAX);
            }
        } else if (column->cooldown > 0) {
            column->cooldown--;
        } else if ((esp_random() % 100) < MATRIX_SPAWN_CHANCE_PERCENT) {
            column->active = true;
            column->head_row = -1;
            column->tail_length = (uint8_t)random_range(MATRIX_TAIL_MIN, MATRIX_TAIL_MAX);
        }
    }

    uint8_t* framebuffer = framebuffer_clear();
    for (int row = 0; row < FRAMEBUFFER_HEIGHT; row++) {
        for (int col = 0; col < FRAMEBUFFER_WIDTH; col++) {
            if (matrix_intensity[row][col] > 0) {
                framebuffer_set_pixel_value((uint8_t)col, (uint8_t)row, 1);
            }
        }
    }

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(100));
}
