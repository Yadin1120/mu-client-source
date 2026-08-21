#include "stdafx.h"
#include "Scenes/LoginBackdrop.h"

#include "Render/Textures/ZzzOpenglUtil.h"   // RenderColorBitmap, blend helpers, WorldTime
#include "Render/Textures/ZzzTexture.h"      // LoadBitmap, DeleteBitmap

namespace Scenes::Login::Backdrop
{

namespace
{
// The still ships as two 1024x1024 tiles: CGlobalBitmap caps a texture at
// 1024x1024 and pads anything that is not a power of two up to the next one,
// which would leave the drawn quad sampling the padding.
constexpr int TEX_TILE_LEFT = BITMAP_LOGIN_BACKDROP;
constexpr int TEX_TILE_RIGHT = BITMAP_LOGIN_BACKDROP + 1;
constexpr int TEX_SHAFT = BITMAP_LOGIN_BACKDROP + 2;

// The wall glow is baked once per phase of a wave travelling up the cracks;
// Render() cross-fades between consecutive frames, which is what turns a set of
// stills into flow. Must match GLOW_FRAMES in scripts/make-login-backdrop.py.
constexpr int GLOW_FRAMES = 12;
constexpr int TEX_GLOW_FIRST = BITMAP_LOGIN_BACKDROP + 3;

static_assert(TEX_GLOW_FIRST + GLOW_FRAMES - 1 <= BITMAP_LOGIN_BACKDROP_END,
    "the backdrop needs more slots than its texture range holds");
static_assert(BITMAP_LOGIN_BACKDROP_END < BITMAP_EFFECT_TEXTURE_END,
    "the backdrop range has run past the end of the effect textures");

constexpr float TWO_PI = 6.28318531f;

// The still does not move. A slow zoom in and out was tried on 17/08/2026 and
// taken out again: on a screen the player sits in front of, a picture that
// creeps towards them reads as drift, not as life. All the motion belongs to
// the light.
constexpr float GLOW_FLOW_CYCLE_MS = 5000.f;   // one trip of the wave up the walls
constexpr float GLOW_CYCLE_MS = 11000.f;
constexpr float GLOW_BASE = 0.85f;             // the still gave its crack light away
constexpr float GLOW_SWING = 0.10f;            // ...so this layer must carry it

constexpr float SHAFT_CYCLE_MS = 15000.f;
constexpr float SHAFT_BASE = 0.45f;
constexpr float SHAFT_SWING = 0.28f;
constexpr float SHAFT_SWAY_CYCLE_MS = 23000.f;
constexpr float SHAFT_SWAY = 0.010f;
constexpr float SHAFT_INSET = 0.012f;          // headroom the sway moves inside

constexpr unsigned int COLOUR_WHITE = 0xFFFFFFFF;

bool s_ready = false;

// Position inside a cycle, 0..1.
float Cycle(float periodMs)
{
    return (float)(fmod(WorldTime, (double)periodMs) / periodMs);
}

// A cosine so the ends meet: the value at the end of a cycle equals the value at
// its start, and nothing steps when the cycle wraps around.
float Wave(float periodMs)
{
    return cosf(Cycle(periodMs) * TWO_PI);
}

// RenderColorBitmap takes its colour as 0xAABBGGRR. An even grey scales the
// texture uniformly, which is how an additive layer gets dimmed.
unsigned int GreyLevel(float level)
{
    if (level < 0.f) level = 0.f;
    if (level > 1.f) level = 1.f;
    const unsigned int v = (unsigned int)(level * 255.f + 0.5f);
    return 0xFF000000u | (v << 16) | (v << 8) | v;
}

struct Layer
{
    int index;
    const wchar_t* file;
};

constexpr int LAYER_COUNT = 3 + GLOW_FRAMES;

// Every texture the backdrop owns, in load order.
Layer LayerAt(int slot)
{
    static const wchar_t* const GLOW_FILES[GLOW_FRAMES] = {
        L"Interface\\mu_login_glow0.jpg",  L"Interface\\mu_login_glow1.jpg",
        L"Interface\\mu_login_glow2.jpg",  L"Interface\\mu_login_glow3.jpg",
        L"Interface\\mu_login_glow4.jpg",  L"Interface\\mu_login_glow5.jpg",
        L"Interface\\mu_login_glow6.jpg",  L"Interface\\mu_login_glow7.jpg",
        L"Interface\\mu_login_glow8.jpg",  L"Interface\\mu_login_glow9.jpg",
        L"Interface\\mu_login_glow10.jpg", L"Interface\\mu_login_glow11.jpg",
    };

    switch (slot)
    {
    case 0:  return { TEX_TILE_LEFT,  L"Interface\\mu_login_bg01.jpg" };
    case 1:  return { TEX_TILE_RIGHT, L"Interface\\mu_login_bg02.jpg" };
    case 2:  return { TEX_SHAFT,      L"Interface\\mu_login_shaft.jpg" };
    default: return { TEX_GLOW_FIRST + (slot - 3), GLOW_FILES[slot - 3] };
    }
}

// The still, as two tiles side by side, each filling its half of the screen.
//
// Drawn through RenderColorBitmap rather than RenderBitmap, which sets no colour
// of its own: the still would then be modulated by whatever colour the 3D pass
// happened to leave current, so it came out at full brightness on some frames
// and nearly black on others - a flicker between a picture and a black screen.
void RenderStill()
{
    const float half = (float)REFERENCE_WIDTH * 0.5f;

    DisableAlphaBlend();
    RenderColorBitmap(TEX_TILE_LEFT, 0.f, 0.f, half, (float)REFERENCE_HEIGHT,
        0.f, 0.f, 1.f, 1.f, COLOUR_WHITE);
    RenderColorBitmap(TEX_TILE_RIGHT, half, 0.f, half, (float)REFERENCE_HEIGHT,
        0.f, 0.f, 1.f, 1.f, COLOUR_WHITE);
}

// The gold cracks in the walls. Two frames of the baked wave are drawn over each
// other with complementary weights: the blend is additive, so that sums to a
// straight interpolation between them and the bright band slides along the
// cracks instead of jumping from one frame to the next.
void RenderFlowingGlow()
{
    const float brightness = GLOW_BASE + GLOW_SWING * Wave(GLOW_CYCLE_MS);
    const float position = Cycle(GLOW_FLOW_CYCLE_MS) * GLOW_FRAMES;
    const int frame = (int)position;
    const float mix = position - (float)frame;

    const int current = TEX_GLOW_FIRST + (frame % GLOW_FRAMES);
    const int next = TEX_GLOW_FIRST + ((frame + 1) % GLOW_FRAMES);

    RenderColorBitmap(current, 0.f, 0.f, (float)REFERENCE_WIDTH, (float)REFERENCE_HEIGHT,
        0.f, 0.f, 1.f, 1.f, GreyLevel(brightness * (1.f - mix)));
    RenderColorBitmap(next, 0.f, 0.f, (float)REFERENCE_WIDTH, (float)REFERENCE_HEIGHT,
        0.f, 0.f, 1.f, 1.f, GreyLevel(brightness * mix));
}

// The light coming through the ceiling. It is the one layer that moves in space,
// and only by about a percent of the width - enough for the beam to feel alive,
// far too little to read as the picture sliding.
void RenderShaft()
{
    const float brightness = SHAFT_BASE + SHAFT_SWING * Wave(SHAFT_CYCLE_MS);
    const float sway = SHAFT_SWAY * Wave(SHAFT_SWAY_CYCLE_MS);

    RenderColorBitmap(TEX_SHAFT, 0.f, 0.f, (float)REFERENCE_WIDTH, (float)REFERENCE_HEIGHT,
        SHAFT_INSET + sway, 0.f, 1.f - SHAFT_INSET * 2.f, 1.f, GreyLevel(brightness));
}

}  // namespace

bool Load()
{
    // bCheck=false on purpose: a client running a new Main.exe without the new
    // textures must fall back to the 3D fly-through, not pop an error box and
    // quit, so the default bCheck=true would be exactly the wrong thing here.
    s_ready = true;
    for (int slot = 0; slot < LAYER_COUNT; ++slot)
    {
        const Layer layer = LayerAt(slot);
        if (!::LoadBitmap(layer.file, layer.index, GL_LINEAR, GL_CLAMP_TO_EDGE, false))
        {
            s_ready = false;
            break;
        }
    }

    if (!s_ready)
    {
        Release();
    }

    return s_ready;
}

void Release()
{
    // These live outside BITMAP_LOG_IN, so ReleaseLogoSceneData()'s sweep of that
    // range does not reach them and they have to be freed here. Loading over a
    // slot that still holds a texture leaks the one already in it.
    for (int slot = 0; slot < LAYER_COUNT; ++slot)
    {
        ::DeleteBitmap(LayerAt(slot).index);
    }

    s_ready = false;
}

bool IsReady()
{
    return s_ready;
}

void Render()
{
    if (!s_ready)
    {
        return;
    }

    RenderStill();

    EnableAlphaBlend();
    RenderFlowingGlow();
    RenderShaft();

    // Leave the state as the UI drawn after this expects to find it.
    glColor4f(1.f, 1.f, 1.f, 1.f);
    EnableAlphaTest();
}

}  // namespace Scenes::Login::Backdrop
