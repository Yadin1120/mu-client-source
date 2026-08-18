#pragma once

namespace Scenes::Login::Backdrop
{
    // Loads the still and its two additive layers into the LOG_IN texture range.
    // Returns false when any of them is missing, in which case Render() must not
    // be called and the scene keeps the 3D camera fly-through it always had.
    bool Load();

    // Marks the textures gone. Called from ReleaseLogoSceneData(), which frees
    // the whole LOG_IN range in one sweep.
    void Release();

    bool IsReady();

    // Draws the whole login backdrop for this frame. Expects to be called inside
    // BeginBitmap()/EndBitmap(), and leaves the current colour white.
    void Render();
}
