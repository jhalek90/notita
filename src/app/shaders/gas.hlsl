// Gas grid kernels for the SDL3 GPU API (compiled once per kernel with
// -D K_<NAME>, entry point "main"). They mirror CpuGasSolver
// (src/sim/GasSolverCpu.cpp) step by step; keep the two in sync.
// Bindings follow SDL's DXIL rules: read-only storage buffers in
// (t[n], space0), read-write ones in (u[n], space1), uniforms in (b0, space2).

cbuffer Params : register(b0, space2)
{
    int W; int H; int NS; int Iters;
    float BuoyHeat; float BuoyDens; float Vort; float Damping;
    float MaxSpeed; float Diffusion; float AirCap; int NFlat;
    float4 Light[2];
    float4 Cap[2];
    int4 Flat[4]; // #483: top-down regions in grid cells (i0, j0, i1, j1): no buoyancy there
};

static const float CellPx = 16.0;
static const float MassOne = 4096.0;

float lightOf(int k) { return Light[k >> 2][k & 3]; }
float capOf(int k) { return Cap[k >> 2][k & 3]; }

#define OPEN_HELPERS                                                              \
    bool openCell(int i, int j)                                                  \
    {                                                                            \
        if (i < 0 || j < 0 || i >= W || j >= H) return false;                    \
        return Open[j * W + i] > 0.0;                                            \
    }                                                                            \
    bool openU(int i, int j) { return i > 0 && i < W && openCell(i - 1, j) && openCell(i, j); } \
    bool openV(int i, int j) { return j > 0 && j < H && openCell(i, j - 1) && openCell(i, j); }

// ---------------------------------------------------------------- 1. curl
#ifdef K_CURL
StructuredBuffer<float> Open : register(t0, space0);
StructuredBuffer<float> U : register(t1, space0);
StructuredBuffer<float> V : register(t2, space0);
RWStructuredBuffer<float> Curl : register(u0, space1);
OPEN_HELPERS
float ucO(int i, int j)
{
    if (!openCell(i, j)) return 0.0;
    return 0.5 * (U[j * (W + 1) + i] + U[j * (W + 1) + i + 1]);
}
float vcO(int i, int j)
{
    if (!openCell(i, j)) return 0.0;
    return 0.5 * (V[j * W + i] + V[(j + 1) * W + i]);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (i >= W || j >= H) return;
    float c = 0.0;
    if (openCell(i, j)) c = 0.5 * (vcO(i + 1, j) - vcO(i - 1, j)) - 0.5 * (ucO(i, j + 1) - ucO(i, j - 1));
    Curl[j * W + i] = c;
}
#endif

// -------------------------------------------------------------- 2. forces
#ifdef K_FORCES
StructuredBuffer<float> Open : register(t0, space0);
StructuredBuffer<float> Curl : register(t1, space0);
StructuredBuffer<int> Mass : register(t2, space0);
StructuredBuffer<float> Heat : register(t3, space0);
RWStructuredBuffer<float> U : register(u0, space1);
RWStructuredBuffer<float> V : register(u1, space1);
OPEN_HELPERS
float capacity(int c)
{
    float cc = max(Open[c], 1.0 / CellPx) * CellPx * AirCap;
    for (int k = 0; k < NS; ++k) cc += (float)Mass[k * W * H + c] * (capOf(k) / MassOne);
    return cc;
}
float buoyancy(int i, int j)
{
    int c = j * W + i;
    for (int f = 0; f < NFlat; ++f)
        if (i >= Flat[f].x && i <= Flat[f].z && j >= Flat[f].y && j <= Flat[f].w) return 0.0;
    float b = BuoyHeat * (Heat[c] / capacity(c));
    float perPx = 1.0 / (max(Open[c], 1.0 / CellPx) * CellPx * MassOne);
    for (int k = 0; k < NS; ++k) b += BuoyDens * lightOf(k) * (float)Mass[k * W * H + c] * perPx;
    return b;
}
float absCurl(int i, int j)
{
    i = clamp(i, 0, W - 1);
    j = clamp(j, 0, H - 1);
    return abs(Curl[j * W + i]);
}
void vortForce(int i, int j, out float fx, out float fy)
{
    float gx = 0.5 * (absCurl(i + 1, j) - absCurl(i - 1, j));
    float gy = 0.5 * (absCurl(i, j + 1) - absCurl(i, j - 1));
    float len = sqrt(gx * gx + gy * gy) + 1.0e-5;
    float wv = Curl[j * W + i];
    fx = Vort * (gy / len) * wv;
    fy = -Vort * (gx / len) * wv;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (j < H && i <= W) {
        int s = j * (W + 1) + i;
        if (!openU(i, j)) U[s] = 0.0;
        else {
            float ax, ay, bx, by;
            vortForce(i - 1, j, ax, ay);
            vortForce(i, j, bx, by);
            U[s] = (U[s] + 0.5 * (ax + bx)) * Damping;
        }
    }
    if (i < W && j <= H) {
        int s = j * W + i;
        if (!openV(i, j)) V[s] = 0.0;
        else {
            float ax, ay, bx, by;
            vortForce(i, j - 1, ax, ay);
            vortForce(i, j, bx, by);
            float b = 0.5 * (buoyancy(i, j - 1) + buoyancy(i, j));
            V[s] = (V[s] - b + 0.5 * (ay + by)) * Damping;
        }
    }
}
#endif

// -------------------------------------------------------------- 3. advect
#ifdef K_ADVECT
StructuredBuffer<float> Open : register(t0, space0);
StructuredBuffer<float> U : register(t1, space0);
StructuredBuffer<float> V : register(t2, space0);
RWStructuredBuffer<float> U2 : register(u0, space1);
RWStructuredBuffer<float> V2 : register(u1, space1);
OPEN_HELPERS
float sampleU(float x, float y)
{
    y -= 0.5;
    x = clamp(x, 0.0, (float)W);
    y = clamp(y, 0.0, (float)(H - 1));
    int i0 = min((int)x, W - 1), j0 = min((int)y, max(H - 2, 0));
    int i1 = min(i0 + 1, W), j1 = min(j0 + 1, H - 1);
    float fx = x - (float)i0, fy = y - (float)j0;
    int s = W + 1;
    float a = U[j0 * s + i0] * (1 - fx) + U[j0 * s + i1] * fx;
    float b = U[j1 * s + i0] * (1 - fx) + U[j1 * s + i1] * fx;
    return a * (1 - fy) + b * fy;
}
float sampleV(float x, float y)
{
    x -= 0.5;
    x = clamp(x, 0.0, (float)(W - 1));
    y = clamp(y, 0.0, (float)H);
    int i0 = min((int)x, max(W - 2, 0)), j0 = min((int)y, H - 1);
    int i1 = min(i0 + 1, W - 1), j1 = min(j0 + 1, H);
    float fx = x - (float)i0, fy = y - (float)j0;
    float a = V[j0 * W + i0] * (1 - fx) + V[j0 * W + i1] * fx;
    float b = V[j1 * W + i0] * (1 - fx) + V[j1 * W + i1] * fx;
    return a * (1 - fy) + b * fy;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (j < H && i <= W) {
        float r = 0.0;
        if (openU(i, j)) {
            float x = (float)i, y = (float)j + 0.5;
            float vx = U[j * (W + 1) + i], vy = sampleV(x, y);
            r = clamp(sampleU(x - vx, y - vy), -MaxSpeed, MaxSpeed);
        }
        U2[j * (W + 1) + i] = r;
    }
    if (i < W && j <= H) {
        float r = 0.0;
        if (openV(i, j)) {
            float x = (float)i + 0.5, y = (float)j;
            float vx = sampleU(x, y), vy = V[j * W + i];
            r = clamp(sampleV(x - vx, y - vy), -MaxSpeed, MaxSpeed);
        }
        V2[j * W + i] = r;
    }
}
#endif

// ---------------------------------------------------------- 4. divergence
#ifdef K_DIVERGENCE
StructuredBuffer<float> Open : register(t0, space0);
StructuredBuffer<float> U2 : register(t1, space0);
StructuredBuffer<float> V2 : register(t2, space0);
StructuredBuffer<float> Src : register(t3, space0);
RWStructuredBuffer<float> Div : register(u0, space1);
OPEN_HELPERS
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (i >= W || j >= H) return;
    int c = j * W + i;
    float d = 0.0;
    if (openCell(i, j))
        d = (U2[j * (W + 1) + i + 1] - U2[j * (W + 1) + i] + V2[(j + 1) * W + i] - V2[j * W + i]) - Src[c];
    Div[c] = d;
}
#endif

// -------------------------------------------------------------- 5. jacobi
#ifdef K_JACOBI
StructuredBuffer<float> Open : register(t0, space0);
StructuredBuffer<float> Div : register(t1, space0);
StructuredBuffer<float> PIn : register(t2, space0);
RWStructuredBuffer<float> POut : register(u0, space1);
OPEN_HELPERS
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (i >= W || j >= H) return;
    int c = j * W + i;
    float r = 0.0;
    if (openCell(i, j)) {
        float sum = 0.0;
        int cnt = 0;
        if (openCell(i - 1, j)) { sum += PIn[c - 1]; ++cnt; }
        if (openCell(i + 1, j)) { sum += PIn[c + 1]; ++cnt; }
        if (openCell(i, j - 1)) { sum += PIn[c - W]; ++cnt; }
        if (openCell(i, j + 1)) { sum += PIn[c + W]; ++cnt; }
        r = cnt > 0 ? (sum - Div[c]) / (float)cnt : 0.0;
    }
    POut[c] = r;
}
#endif

// ------------------------------------------------------------- 6. project
#ifdef K_PROJECT
StructuredBuffer<float> Open : register(t0, space0);
StructuredBuffer<float> U2 : register(t1, space0);
StructuredBuffer<float> V2 : register(t2, space0);
StructuredBuffer<float> P : register(t3, space0);
RWStructuredBuffer<float> U : register(u0, space1);
RWStructuredBuffer<float> V : register(u1, space1);
OPEN_HELPERS
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (j < H && i <= W) {
        int s = j * (W + 1) + i;
        U[s] = openU(i, j) ? clamp(U2[s] - (P[j * W + i] - P[j * W + i - 1]), -MaxSpeed, MaxSpeed) : 0.0;
    }
    if (i < W && j <= H) {
        int s = j * W + i;
        V[s] = openV(i, j) ? clamp(V2[s] - (P[s] - P[s - W]), -MaxSpeed, MaxSpeed) : 0.0;
    }
}
#endif

// --------------------------------------------------------------- 7. scale
#ifdef K_SCALE
StructuredBuffer<float> U : register(t0, space0);
StructuredBuffer<float> V : register(t1, space0);
RWStructuredBuffer<float> Scale : register(u0, space1);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (i >= W || j >= H) return;
    float o = max(U[j * (W + 1) + i + 1], 0.0) + max(-U[j * (W + 1) + i], 0.0) + max(V[(j + 1) * W + i], 0.0) +
              max(-V[j * W + i], 0.0);
    Scale[j * W + i] = o > 0.8 ? 0.8 / o : 1.0;
}
#endif

// ------------------------------------------------------------- 8. scalars
#ifdef K_SCALARS
StructuredBuffer<float> Open : register(t0, space0);
StructuredBuffer<float> U : register(t1, space0);
StructuredBuffer<float> V : register(t2, space0);
StructuredBuffer<float> Scale : register(t3, space0);
StructuredBuffer<int> Mass : register(t4, space0);
StructuredBuffer<float> Heat : register(t5, space0);
RWStructuredBuffer<int> Mass2 : register(u0, space1);
RWStructuredBuffer<float> Heat2 : register(u1, space1);
OPEN_HELPERS
// the same expressions as gasAdvectFlux / gasDiffuseFlux in GasGrid.h
int advectFlux(int donorMass, float speed, float donorScale)
{
    precise float f = (float)donorMass * (speed * donorScale);
    return (int)f;
}
int diffuseFlux(int a, int b, float d)
{
    precise float f = (float)(a - b) * d;
    return (int)f;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    int i = (int)id.x, j = (int)id.y;
    if (i >= W || j >= H) return;
    int c = j * W + i;
    int n = W * H;
    if (!openCell(i, j)) {
        for (int k = 0; k < NS; ++k) Mass2[k * n + c] = Mass[k * n + c];
        Heat2[c] = Heat[c];
        return;
    }
    float d = clamp(Diffusion, 0.0, 0.05);
    int nb[4] = { c - 1, c + 1, c - W, c + W };
    bool ok[4] = { openCell(i - 1, j), openCell(i + 1, j), openCell(i, j - 1), openCell(i, j + 1) };
    float vel[4] = { U[j * (W + 1) + i], U[j * (W + 1) + i + 1], V[j * W + i], V[(j + 1) * W + i] };
    float outSign[4] = { -1.0, 1.0, -1.0, 1.0 };
    float sc = Scale[c];

    float hv = Heat[c];
    [unroll] for (int q = 0; q < 4; ++q) {
        if (!ok[q]) continue;
        float sv = vel[q] * outSign[q];
        if (sv > 0.0) hv -= Heat[c] * (sv * sc);
        else if (sv < 0.0) hv += Heat[nb[q]] * (-sv * Scale[nb[q]]);
        if (q == 0 || q == 2) hv += (Heat[nb[q]] - Heat[c]) * d;
        else hv -= (Heat[c] - Heat[nb[q]]) * d;
    }
    Heat2[c] = hv;

    for (int k = 0; k < NS; ++k) {
        int me = Mass[k * n + c];
        int mv = me;
        [unroll] for (int q2 = 0; q2 < 4; ++q2) {
            if (!ok[q2]) continue;
            int other = Mass[k * n + nb[q2]];
            float sv = vel[q2] * outSign[q2];
            if (sv > 0.0) mv -= advectFlux(me, sv, sc);
            else if (sv < 0.0) mv += advectFlux(other, -sv, Scale[nb[q2]]);
            if (q2 == 0 || q2 == 2) mv += diffuseFlux(other, me, d);
            else mv -= diffuseFlux(me, other, d);
        }
        Mass2[k * n + c] = mv;
    }
}
#endif
