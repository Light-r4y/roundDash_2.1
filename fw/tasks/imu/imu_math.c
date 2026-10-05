#include "imu_math.h"
#include <math.h>

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float norm3(const float a[3])
{
    return sqrtf(dot3(a, a));
}

void roundGauge_imu_basis(const float g0[3], float u[3], float cand[4][3])
{
    float n = norm3(g0);
    if (n < 0.5f) { // не откалибровано или датчик врёт: считаем плату лежащей ровно
        u[0] = 0; u[1] = 0; u[2] = 1;
    } else {
        for (int i = 0; i < 3; i++) u[i] = g0[i] / n;
    }

    // Горизонтальные проекции осей платы: e_i - (e_i . u) u.
    float h[3][3], len[3];
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) h[i][k] = (k == i ? 1.0f : 0.0f) - u[i] * u[k];
        len[i] = norm3(h[i]);
    }
    // Две оси с наибольшей проекцией; порядок A < B - по индексу оси, а не по длине,
    // чтобы номер варианта не прыгал при небольших изменениях g0.
    int worst = 0;
    for (int i = 1; i < 3; i++) if (len[i] < len[worst]) worst = i;
    int a = -1, b = -1;
    for (int i = 0; i < 3; i++) {
        if (i == worst) continue;
        if (a < 0) a = i; else b = i;
    }
    for (int k = 0; k < 3; k++) {
        cand[0][k] = h[a][k] / len[a];
        cand[1][k] = -cand[0][k];
        cand[2][k] = h[b][k] / len[b];
        cand[3][k] = -cand[2][k];
    }
}

void roundGauge_imu_project(const float g0[3], uint8_t fwd, const float a[3], float *lon, float *lat, float *vert)
{
    float u[3], cand[4][3], r[3];
    roundGauge_imu_basis(g0, u, cand);
    const float *f = cand[fwd & 3];
    cross3(f, u, r); // вправо = вперёд x вверх
    float d[3] = { a[0] - g0[0], a[1] - g0[1], a[2] - g0[2] };
    *lon = dot3(d, f);
    *lat = dot3(d, r);
    *vert = dot3(d, u);
}

float roundGauge_imu_horizontal(const float g0[3], const float d[3], float h[3])
{
    float u[3], cand[4][3];
    roundGauge_imu_basis(g0, u, cand);
    float up = dot3(d, u);
    for (int k = 0; k < 3; k++) h[k] = d[k] - up * u[k];
    return norm3(h);
}

uint8_t roundGauge_imu_pick_forward(const float g0[3], const float d[3])
{
    float u[3], cand[4][3], h[3];
    roundGauge_imu_basis(g0, u, cand);
    roundGauge_imu_horizontal(g0, d, h);
    int best = 0;
    float best_dot = -1e9f;
    for (int i = 0; i < 4; i++) {
        float v = dot3(h, cand[i]);
        if (v > best_dot) { best_dot = v; best = i; }
    }
    return (uint8_t)best;
}
