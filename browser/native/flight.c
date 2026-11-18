#include "stream.h"

enum { FORWARD = 1, LEFT = 2, BACK = 4, RIGHT = 8, RISE = 16, DESCEND = 32,
       REMOVE = 64, PLACE = 128 };

static const double pi = 3.141592653589793;
static double eye[3], yaw, pitch;
static float camera[16];

static double sine(double x) {
    if (x > pi) x -= 2 * pi;
    if (x > pi / 2) x = pi - x;
    if (x < -pi / 2) x = -pi - x;
    double x2 = x * x;
    return x * (1 + x2 * (-1.0/6 + x2 * (1.0/120 + x2 * (-1.0/5040 + x2/362880))));
}

static void basis(double forward[3], double right[3], double up[3]) {
    double cy = sine(yaw + pi / 2), sy = sine(yaw);
    double cp = sine(pitch + pi / 2), sp = sine(pitch);
    forward[0] = sy * cp; forward[1] = sp; forward[2] = cy * cp;
    right[0] = -cy; right[1] = 0; right[2] = sy;
    up[0] = -sy * sp; up[1] = cp; up[2] = -cy * sp;
}

static double dot(const double a[3], const double b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static void update_camera(const double f[3], const double r[3], const double u[3]) {
    /* Unit aspect, 90-degree vertical field of view, WebGPU depth [0, 1].
     * The canvas renderer can scale the first row for its aspect ratio. */
    const double depth = 128.0 / (128.0 - 0.05);
    for (int i = 0; i < 3; i++) {
        camera[4*i] = (float)r[i];
        camera[4*i+1] = (float)u[i];
        camera[4*i+2] = (float)(f[i] * depth);
        camera[4*i+3] = (float)f[i];
    }
    camera[12] = (float)-dot(r, eye);
    camera[13] = (float)-dot(u, eye);
    camera[14] = (float)((-dot(f, eye) - 0.05) * depth);
    camera[15] = (float)-dot(f, eye);
}

void strata_browser_init_camera(void) {
    eye[0] = 16.5; eye[1] = 10; eye[2] = 16.5;
    yaw = 0; pitch = -pi / 2 + 0.01;
    double f[3], r[3], u[3];
    basis(f, r, u);
    update_camera(f, r, u);
}

int strata_browser_eye_cell(int axis) { return (int)__builtin_floor(eye[axis]); }

/* Walk the world grid until a solid voxel is found. Track the preceding
 * loaded air cell for placement, including across a chunk boundary. */
static int pick(const double direction[3], int hit[3], int previous[3], int *has_previous) {
    int cell[3], advance[3];
    double next[3], delta[3];
    for (int axis = 0; axis < 3; axis++) {
        cell[axis] = (int)__builtin_floor(eye[axis]);
        advance[axis] = direction[axis] > 0 ? 1 : -1;
        if (direction[axis] == 0) {
            next[axis] = delta[axis] = 1e30;
        } else {
            double boundary = cell[axis] + (advance[axis] > 0);
            next[axis] = (boundary - eye[axis]) / direction[axis];
            delta[axis] = advance[axis] / direction[axis];
        }
    }
    *has_previous = 0;
    for (;;) {
        int voxel = strata_browser_voxel(cell);
        if (voxel > 0) {
            for (int i = 0; i < 3; i++) hit[i] = cell[i];
            return 1;
        }
        *has_previous = voxel == 0;
        if (*has_previous)
            for (int i = 0; i < 3; i++) previous[i] = cell[i];
        int axis = next[0] <= next[1] ? 0 : 1;
        if (next[2] < next[axis]) axis = 2;
        if (next[axis] > 16) return 0;
        cell[axis] += advance[axis];
        next[axis] += delta[axis];
    }
}

static double clamp(double x, double lo, double hi) {
    if (!__builtin_isfinite(x)) return 0;
    return x < lo ? lo : x > hi ? hi : x;
}

static int direction_bit(uint32_t held, unsigned positive, unsigned negative) {
    return !!(held & positive) - !!(held & negative);
}

__attribute__((export_name("step")))
uint32_t step(double dt_ms, uint32_t held, uint32_t pressed, double dx, double dy) {
    if (!strata_browser_ready()) return 0;
    yaw += clamp(dx, -1000, 1000) * 0.003;
    if (yaw > pi) yaw -= 2 * pi;
    if (yaw < -pi) yaw += 2 * pi;
    pitch = clamp(pitch - clamp(dy, -1000, 1000) * 0.003, -pi/2 + 0.01, pi/2 - 0.01);
    double f[3], r[3], u[3];
    basis(f, r, u);
    double distance = clamp(dt_ms, 0, 100) * 0.008;
    int strafe = direction_bit(held, RIGHT, LEFT);
    int forward = direction_bit(held, FORWARD, BACK);
    int lift = direction_bit(held, RISE, DESCEND);
    for (int i = 0; i < 3; i++)
        eye[i] = clamp(eye[i] + distance * (strafe*r[i] + forward*f[i] + (i == 1 ? lift : 0)), -512, 512);
    update_camera(f, r, u);

    if (!(pressed & (REMOVE | PLACE))) return 0;
    int hit[3], before[3], has_before;
    if (!pick(f, hit, before, &has_before)) return 0;
    if (pressed & REMOVE) return strata_browser_edit(hit, 0);
    return has_before && strata_browser_edit(before, 1);
}

__attribute__((export_name("camera_ptr")))
uint32_t camera_ptr(void) { return (uint32_t)(uintptr_t)camera; }
