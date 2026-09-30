using System;

namespace OpenCompositeConfigurator
{
    /// <summary>
    /// Isotropic image-space helpers for the monocular legacy tracker path.
    /// Normalized X spans the camera width while normalized Y spans its height,
    /// so X must carry the source aspect ratio before either axis is interpreted
    /// as body-space distance.
    /// </summary>
    internal static class Legacy2DTrackerGeometry
    {
        internal static float ResolveImageAspect(int width, int height)
        {
            if (width <= 0 || height <= 0)
                return 1f;

            float aspect = width / (float)height;
            return float.IsFinite(aspect)
                ? Math.Clamp(aspect, 0.5f, 3.0f)
                : 1f;
        }

        internal static float ToIsotropicImageX(float normalizedX, float imageAspect) =>
            0.5f + (normalizedX - 0.5f) * imageAspect;

        internal static float MetricX(
            float normalizedX,
            float metersPerVerticalUnit,
            float imageAspect) =>
            (0.5f - normalizedX) * metersPerVerticalUnit * imageAspect;

        internal static float ImageDeltaToMeters(
            float normalizedDeltaX,
            float metersPerVerticalUnit,
            float imageAspect) =>
            normalizedDeltaX * metersPerVerticalUnit * imageAspect;

        internal static float MetersToImageDelta(
            float meters,
            float metersPerVerticalUnit,
            float imageAspect)
        {
            float denominator = metersPerVerticalUnit * imageAspect;
            return float.IsFinite(denominator) && MathF.Abs(denominator) > 0.000001f
                ? meters / denominator
                : 0f;
        }

        internal static void PinGroundedDepth(
            bool grounded,
            ref float kneeDepth,
            ref float footDepth)
        {
            if (!grounded)
                return;

            // Monocular foreshortening cannot distinguish a planted fore/aft
            // stance from ordinary per-leg model noise. A grounded tracker is
            // therefore anchored to the shared body plane immediately instead
            // of letting stale, independently filtered depth cross the feet.
            kneeDepth = 0f;
            footDepth = 0f;
        }
    }
}
