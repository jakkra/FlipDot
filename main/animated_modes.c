#include "animated_modes.h"

#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

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

#define RIPPLE_DISTURBANCE_INTERVAL 48
#define RIPPLE_DISTURBANCE_CHANCE_PERCENT 4
#define RIPPLE_DISTURBANCE_MAGNITUDE 768

#define TUNNEL_SWIRL_FACTOR 2.7f
#define TUNNEL_TIME_STEP 0.15f

#define BOUNCING_BALL_COUNT 1
#define BOUNCING_TRAIL_MAX 2

#define TELEPORT_TRAIL_MAX 7

#define LISSAJOUS_TRAIL_MAX 8
#define LISSAJOUS_PATTERN_COUNT 5

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

static int16_t ripple_current[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];
static int16_t ripple_previous[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];
static int16_t ripple_next[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];
static bool ripple_initialized;
static uint32_t ripple_tick;

static bool tunnel_precomputed;
static float tunnel_radius[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];
static float tunnel_angle[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];
static float tunnel_time;

typedef struct {
    float x;
    float y;
    float vx;
    float vy;
} bouncing_ball_t;

static bouncing_ball_t bouncing_balls[BOUNCING_BALL_COUNT];
static uint8_t bouncing_trails[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];
static bool bouncing_initialized;
static uint32_t bouncing_tick;

typedef struct {
    float x;
    float y;
    float vx;
    float vy;
} star_t;

static uint8_t teleport_trail[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];
static uint8_t teleport_x;
static uint8_t teleport_y;
static uint8_t teleport_cooldown;
static bool teleport_initialized;
static uint32_t teleport_tick;

static uint8_t lissajous_trail[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];
static float lissajous_time;
static uint8_t lissajous_pattern_index;
static uint32_t lissajous_pattern_timer;
static bool lissajous_initialized;

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

static float random_unit_float(void)
{
    return (float)esp_random() / (float)UINT32_MAX;
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

void handleModeRipple(bool first_run)
{
    if (first_run || !ripple_initialized) {
        memset(ripple_current, 0, sizeof(ripple_current));
        memset(ripple_previous, 0, sizeof(ripple_previous));
        memset(ripple_next, 0, sizeof(ripple_next));
        ripple_tick = 0;
        ripple_initialized = true;
    }

    ripple_tick++;

    memset(ripple_next, 0, sizeof(ripple_next));

    for (int y = 1; y < FRAMEBUFFER_HEIGHT - 1; y++) {
        for (int x = 1; x < FRAMEBUFFER_WIDTH - 1; x++) {
            int sum = ripple_current[y - 1][x] + ripple_current[y + 1][x] +
                      ripple_current[y][x - 1] + ripple_current[y][x + 1];
            int value = (sum >> 1) - ripple_previous[y][x];
            value -= value / 18;
            value = clamp_int(value, -2048, 2048);
            ripple_next[y][x] = (int16_t)value;
        }
    }

    if ((ripple_tick % RIPPLE_DISTURBANCE_INTERVAL) == 0 || (esp_random() % 100) < RIPPLE_DISTURBANCE_CHANCE_PERCENT) {
        int x = random_range(1, FRAMEBUFFER_WIDTH - 2);
        int y = random_range(1, FRAMEBUFFER_HEIGHT - 2);
        ripple_next[y][x] = RIPPLE_DISTURBANCE_MAGNITUDE;
    }

    memcpy(ripple_previous, ripple_current, sizeof(ripple_current));
    memcpy(ripple_current, ripple_next, sizeof(ripple_current));

    uint8_t* framebuffer = framebuffer_clear();
    for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
            int value = ripple_current[y][x];
            bool on = false;
            if (value > 80) {
                on = true;
            } else if (value < -80) {
                on = (((x + y + (int)(ripple_tick >> 1)) & 1) == 0);
            }
            if (on) {
                framebuffer_set_pixel_value((uint8_t)x, (uint8_t)y, 1);
            }
        }
    }

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(90));
}

void handleModeTunnel(bool first_run)
{
    if (!tunnel_precomputed) {
        float center_x = (float)(FRAMEBUFFER_WIDTH - 1) / 2.0f;
        float center_y = (float)(FRAMEBUFFER_HEIGHT - 1) / 2.0f;
        for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
            for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
                float dx = (float)x - center_x;
                float dy = (float)y - center_y;
                float radius = sqrtf(dx * dx + dy * dy);
                if (radius < 0.2f) {
                    radius = 0.2f;
                }
                tunnel_radius[y][x] = radius;
                tunnel_angle[y][x] = atan2f(dy, dx);
            }
        }
        tunnel_precomputed = true;
    }

    if (first_run) {
        tunnel_time = 0.0f;
    }

    tunnel_time += TUNNEL_TIME_STEP;

    uint8_t* framebuffer = framebuffer_clear();
    for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
            float radius = tunnel_radius[y][x];
            float angle = tunnel_angle[y][x];
            float spiral = angle * TUNNEL_SWIRL_FACTOR + tunnel_time;
            float radial = tunnel_time * 0.6f + (2.2f / (radius + 0.6f));
            float wave = sinf(spiral + radial);
            float bands = sinf(radius * 0.6f - tunnel_time * 0.5f);

            bool on = false;
            if (wave > 0.3f) {
                on = true;
            } else if (bands > 0.45f) {
                on = (((x + y + (int)(tunnel_time * 1.0f)) & 1) == 0);
            } else if (radius < 1.3f) {
                on = (((int)(tunnel_time * 1.0f)) & 1) == 0;
            }

            if (on) {
                framebuffer_set_pixel_value((uint8_t)x, (uint8_t)y, 1);
            }
        }
    }

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(100));
}

void handleModeBouncingBalls(bool first_run)
{
    if (first_run || !bouncing_initialized) {
        memset(bouncing_trails, 0, sizeof(bouncing_trails));
        // Initialize single ball at center with random velocity
        bouncing_balls[0].x = (float)(FRAMEBUFFER_WIDTH - 1) / 2.0f;
        bouncing_balls[0].y = (float)(FRAMEBUFFER_HEIGHT - 1) / 2.0f;
        bouncing_balls[0].vx = (random_unit_float() - 0.5f) > 0 ? 0.7f : -0.7f;
        bouncing_balls[0].vy = (random_unit_float() - 0.5f) > 0 ? 0.5f : -0.5f;
        bouncing_tick = 0;
        bouncing_initialized = true;
    }

    bouncing_tick++;

    // Fade trail every other frame
    bool fade_step = ((bouncing_tick & 1U) != 0);
    for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
            if (fade_step && bouncing_trails[y][x] > 0) {
                bouncing_trails[y][x]--;
            }
        }
    }

    bouncing_ball_t* ball = &bouncing_balls[0];
    
    // Move ball - no gravity or friction, constant velocity
    ball->x += ball->vx;
    ball->y += ball->vy;

    // Bounce off edges with perfect reflection (no energy loss)
    if (ball->x <= 0.0f) {
        ball->x = 0.0f;
        ball->vx = -ball->vx;
    } else if (ball->x >= (float)(FRAMEBUFFER_WIDTH - 1)) {
        ball->x = (float)(FRAMEBUFFER_WIDTH - 1);
        ball->vx = -ball->vx;
    }
    
    if (ball->y <= 0.0f) {
        ball->y = 0.0f;
        ball->vy = -ball->vy;
    } else if (ball->y >= (float)(FRAMEBUFFER_HEIGHT - 1)) {
        ball->y = (float)(FRAMEBUFFER_HEIGHT - 1);
        ball->vy = -ball->vy;
    }

    // Draw ball position and small trail
    int xi = clamp_int((int)(ball->x + 0.5f), 0, FRAMEBUFFER_WIDTH - 1);
    int yi = clamp_int((int)(ball->y + 0.5f), 0, FRAMEBUFFER_HEIGHT - 1);
    bouncing_trails[yi][xi] = BOUNCING_TRAIL_MAX;
    
    // Add smaller trail around ball
    if (yi > 0 && bouncing_trails[yi - 1][xi] < 2) {
        bouncing_trails[yi - 1][xi] = 2;
    }
    if (yi + 1 < FRAMEBUFFER_HEIGHT && bouncing_trails[yi + 1][xi] < 2) {
        bouncing_trails[yi + 1][xi] = 2;
    }
    if (xi > 0 && bouncing_trails[yi][xi - 1] < 2) {
        bouncing_trails[yi][xi - 1] = 2;
    }
    if (xi + 1 < FRAMEBUFFER_WIDTH && bouncing_trails[yi][xi + 1] < 2) {
        bouncing_trails[yi][xi + 1] = 2;
    }

    uint8_t* framebuffer = framebuffer_clear();
    for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
            uint8_t value = bouncing_trails[y][x];
            if (value == 0) {
                continue;
            }
            if (value >= 2 || (((x + y + (int)bouncing_tick) & 1) == 0)) {
                framebuffer_set_pixel_value((uint8_t)x, (uint8_t)y, 1);
            }
        }
    }

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(80));  // Slightly slower for DVD screensaver feel
}

void handleModeTeleport(bool first_run)
{
    if (first_run || !teleport_initialized) {
        memset(teleport_trail, 0, sizeof(teleport_trail));
        teleport_x = (uint8_t)random_range(0, FRAMEBUFFER_WIDTH - 1);
        teleport_y = (uint8_t)random_range(0, FRAMEBUFFER_HEIGHT - 1);
        teleport_cooldown = 0;
        teleport_tick = 0;
        teleport_initialized = true;
    }

    teleport_tick++;

    bool fade_trail = ((teleport_tick & 1U) != 0);
    for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
            if (fade_trail && teleport_trail[y][x] > 0) {
                teleport_trail[y][x]--;
            }
        }
    }

    if (teleport_cooldown > 0) {
        teleport_cooldown--;
    }

    bool trigger = false;
    if (teleport_cooldown == 0) {
        if ((teleport_tick % 48) == 0 || (esp_random() % 100) < 10) {
            trigger = true;
        }
    }

    if (trigger) {
        teleport_x = (uint8_t)random_range(0, FRAMEBUFFER_WIDTH - 1);
        teleport_y = (uint8_t)random_range(0, FRAMEBUFFER_HEIGHT - 1);
        teleport_cooldown = (uint8_t)random_range(16, 36);

        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                if (dx * dx + dy * dy <= 4) {
                    int px = (int)teleport_x + dx;
                    int py = (int)teleport_y + dy;
                    if (px >= 0 && px < FRAMEBUFFER_WIDTH && py >= 0 && py < FRAMEBUFFER_HEIGHT) {
                        uint8_t* cell = &teleport_trail[py][px];
                        if (*cell < TELEPORT_TRAIL_MAX - 1) {
                            *cell = (uint8_t)(TELEPORT_TRAIL_MAX - 1);
                        }
                    }
                }
            }
        }
    }

    teleport_trail[teleport_y][teleport_x] = TELEPORT_TRAIL_MAX;

    uint8_t* framebuffer = framebuffer_clear();
    for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
            uint8_t value = teleport_trail[y][x];
            if (value == 0) {
                continue;
            }
            if (value >= 2 || (((x + y + (int)teleport_tick) & 1) == 0)) {
                framebuffer_set_pixel_value((uint8_t)x, (uint8_t)y, 1);
            }
        }
    }

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(70));
}

void handleModeLissajous(bool first_run)
{
    // Lissajous pattern definitions: {freq_x, freq_y, phase_offset, duration_seconds}
    static const struct {
        float freq_x;
        float freq_y;
        float phase_offset;
        uint32_t duration_ticks;
    } patterns[LISSAJOUS_PATTERN_COUNT] = {
        {3.0f, 2.0f, 0.0f, 400},      // 3:2 ratio - classic figure-8
        {5.0f, 4.0f, 1.57f, 350},     // 5:4 ratio with phase shift - flower-like
        {2.0f, 3.0f, 0.78f, 450},     // 2:3 ratio - three-lobed curve
        {4.0f, 3.0f, 0.0f, 400},      // 4:3 ratio - four petals
        {1.0f, 2.0f, 1.57f, 300}      // 1:2 ratio - simple ellipse
    };

    if (first_run || !lissajous_initialized) {
        memset(lissajous_trail, 0, sizeof(lissajous_trail));
        lissajous_time = 0.0f;
        lissajous_pattern_index = 0;
        lissajous_pattern_timer = 0;
        lissajous_initialized = true;
    }

    lissajous_pattern_timer++;
    
    // Switch to next pattern after duration
    if (lissajous_pattern_timer >= patterns[lissajous_pattern_index].duration_ticks) {
        lissajous_pattern_index = (lissajous_pattern_index + 1) % LISSAJOUS_PATTERN_COUNT;
        lissajous_pattern_timer = 0;
        memset(lissajous_trail, 0, sizeof(lissajous_trail)); // Clear trail for new pattern
    }

    lissajous_time += 0.05f;
    if (lissajous_time > 6.28f * 2.0f) { // 4π - full cycle for most patterns
        lissajous_time = 0.0f;
    }

    // Fade existing trail
    for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
            if (lissajous_trail[y][x] > 0) {
                lissajous_trail[y][x]--;
            }
        }
    }

    // Calculate current pattern parameters
    float freq_x = patterns[lissajous_pattern_index].freq_x;
    float freq_y = patterns[lissajous_pattern_index].freq_y;
    float phase_y = patterns[lissajous_pattern_index].phase_offset;

    // Calculate Lissajous curve point
    float center_x = (float)(FRAMEBUFFER_WIDTH - 1) / 2.0f;
    float center_y = (float)(FRAMEBUFFER_HEIGHT - 1) / 2.0f;
    
    // Scale factors to fit the display nicely (leave some margin)
    float scale_x = (float)(FRAMEBUFFER_WIDTH - 4) / 2.0f;
    float scale_y = (float)(FRAMEBUFFER_HEIGHT - 4) / 2.0f;
    
    // Generate multiple points per frame for smoother curves
    for (int i = 0; i < 3; i++) {
        float t = lissajous_time + (float)i * 0.02f;
        
        float x = center_x + scale_x * cosf(freq_x * t);
        float y = center_y + scale_y * sinf(freq_y * t + phase_y);
        
        int xi = clamp_int((int)(x + 0.5f), 0, FRAMEBUFFER_WIDTH - 1);
        int yi = clamp_int((int)(y + 0.5f), 0, FRAMEBUFFER_HEIGHT - 1);
        
        lissajous_trail[yi][xi] = LISSAJOUS_TRAIL_MAX;
        
        // Add some neighboring points for thicker lines
        if (xi > 0 && lissajous_trail[yi][xi - 1] < LISSAJOUS_TRAIL_MAX - 2) {
            lissajous_trail[yi][xi - 1] = LISSAJOUS_TRAIL_MAX - 2;
        }
        if (xi + 1 < FRAMEBUFFER_WIDTH && lissajous_trail[yi][xi + 1] < LISSAJOUS_TRAIL_MAX - 2) {
            lissajous_trail[yi][xi + 1] = LISSAJOUS_TRAIL_MAX - 2;
        }
        if (yi > 0 && lissajous_trail[yi - 1][xi] < LISSAJOUS_TRAIL_MAX - 2) {
            lissajous_trail[yi - 1][xi] = LISSAJOUS_TRAIL_MAX - 2;
        }
        if (yi + 1 < FRAMEBUFFER_HEIGHT && lissajous_trail[yi + 1][xi] < LISSAJOUS_TRAIL_MAX - 2) {
            lissajous_trail[yi + 1][xi] = LISSAJOUS_TRAIL_MAX - 2;
        }
    }

    uint8_t* framebuffer = framebuffer_clear();
    for (int y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
        for (int x = 0; x < FRAMEBUFFER_WIDTH; x++) {
            uint8_t intensity = lissajous_trail[y][x];
            if (intensity == 0) {
                continue;
            }
            // Show different intensities with different patterns
            if (intensity >= 6 || (intensity >= 3 && ((x + y) & 1) == 0)) {
                framebuffer_set_pixel_value((uint8_t)x, (uint8_t)y, 1);
            }
        }
    }

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(80));
}
