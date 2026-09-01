#include <assert.h>

/* Exercise the private VBO builder; host linking discards PS2 runtime code. */
#include "gfx_api_scene.c"

int main(void)
{
    assert(updateCameraDistance(4.6f, 1.0f) > 4.6f);
    assert(updateCameraDistance(4.6f, -1.0f) < 4.6f);
    assert(updateCameraDistance(6.5f, 1.0f) == 6.5f);
    assert(updateCameraDistance(3.8f, -1.0f) == 3.8f);

    for (int i = 0; i < API_SCENE_DRAW_VERTEX_COUNT; ++i) {
        float *src = &sCubeVbo[i * API_SCENE_TEXTURED_STRIDE];
        for (int j = 0; j < API_SCENE_TEXTURED_STRIDE; ++j) {
            src[j] = (float)(i * 16 + j);
        }
        src[3] = 1.5f + (float)(i % 3) * 3.0f;
    }
    buildFogCubeVbo();
    for (int i = 0; i < API_SCENE_DRAW_VERTEX_COUNT; ++i) {
        const float *src = &sCubeVbo[i * API_SCENE_TEXTURED_STRIDE];
        const float *dst = &sFogCubeVbo[i * API_SCENE_FOG_STRIDE];
        for (int j = 0; j < 6; ++j) assert(dst[j] == src[j]);
        for (int j = 0; j < 4; ++j) assert(dst[10 + j] == src[6 + j]);
        assert(dst[6] == 0.12f && dst[7] == 0.20f && dst[8] == 0.32f);
        assert(dst[9] == (float)(i % 3) * 0.5f);
    }

    buildCubeVbo(0.0f, 1.0f, 0.0f, 1.0f, 4.0f / 3.0f, 0.0f, 4.6f);
    buildFogCubeVbo();
    bool varying_fog = false;
    for (int i = 0; i < API_SCENE_DRAW_VERTEX_COUNT; ++i) {
        const float *vertex = &sFogCubeVbo[i * API_SCENE_FOG_STRIDE];
        assert(vertex[3] > 0.0f);
        assert(vertex[9] >= 0.0f && vertex[9] <= 1.0f);
        varying_fog |= vertex[9] != sFogCubeVbo[9];
    }
    assert(varying_fog);
    return 0;
}
