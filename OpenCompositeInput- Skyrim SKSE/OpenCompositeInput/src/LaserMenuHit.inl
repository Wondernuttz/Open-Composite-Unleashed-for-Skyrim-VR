// Crafting owns its control dispatch; probing the full movie separately can
// traverse an invalid display-tree entry while its item preview changes.
bool ProbeLaserPointerTarget(RE::GFxMovieView& movie, const char* menuName,
    float x, float y, float width, float height, bool ready)
{
    if (!ready || !menuName || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        width <= 0.0f || height <= 0.0f ||
        x < 0.0f || y < 0.0f || x > width || y > height)
        return false;

    // The ray already intersects the current, settled menu quad. Only deliberate
    // laser input acquires focus; native mouse dispatch resolves Crafting controls.
    if (std::strcmp(menuName, "Crafting Menu") == 0)
        return true;

    return movie.HitTest(x, y, RE::GFxMovieView::HitTestType::kButtonEvents, 0);
}
