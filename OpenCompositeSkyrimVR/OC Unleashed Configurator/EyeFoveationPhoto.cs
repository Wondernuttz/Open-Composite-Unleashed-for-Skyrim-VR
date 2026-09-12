using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;

namespace OpenCompositeConfigurator;

// UI illustration only. The renderer consumes the original embedded photo;
// it does not change the asset or imitate VRS/RDM's actual reconstruction.
internal sealed class EyeFoveationPhoto : IDisposable
{
    private static readonly Lazy<Bitmap> Source = new(() => {
        using var stream = typeof(EyeFoveationPhoto).Assembly.GetManifestResourceStream(
            "OpenCompositeConfigurator.Resources.DragonPug.png")
            ?? throw new InvalidOperationException("PugDragon preview resource is missing.");
        using var image = Image.FromStream(stream);
        return new Bitmap(image);
    });
    private readonly Dictionary<string, Bitmap> _rates = new();
    private Bitmap? _full;
    private int _side;

    internal static Rectangle CenterCrop(Size source)
    {
        int side = Math.Min(source.Width, source.Height);
        return new Rectangle((source.Width - side) / 2, (source.Height - side) / 2, side, side);
    }

    internal static RectangleF RingBounds(RectangleF plot, PointF gaze, decimal boundary) => new(
        gaze.X - plot.Width * (float)boundary / 2, gaze.Y - plot.Height * (float)boundary / 2,
        plot.Width * (float)boundary, plot.Height * (float)boundary);

    internal static Size RateSize(string rate) => rate switch {
        "1x2" => new(1, 2), "2x1" => new(2, 1), "2x2" => new(2, 2),
        "2x4" => new(2, 4), "4x2" => new(4, 2), "4x4" => new(4, 4), _ => new(1, 1)
    };

    // Cached per preview size/rate. Moving gaze never moves the photo's sample
    // grid, reloads the asset, or rebuilds the reduced-detail bitmaps.
    internal Bitmap ImageForRate(int side, string rate)
    {
        side = Math.Clamp(side, 64, 1600);
        // Multiples of four keep the illustrated coarse pixel blocks regular.
        side = (side + 3) / 4 * 4;
        if (_side != side) {
            Clear(); _side = side;
            _full = new Bitmap(side, side, PixelFormat.Format32bppPArgb);
            using var g = Graphics.FromImage(_full);
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            using var attributes = new ImageAttributes(); attributes.SetWrapMode(WrapMode.TileFlipXY);
            var crop = CenterCrop(Source.Value.Size);
            g.DrawImage(Source.Value, new Rectangle(0, 0, side, side), crop.X, crop.Y, crop.Width, crop.Height,
                GraphicsUnit.Pixel, attributes);
        }
        var block = RateSize(rate);
        if (block.Width == 1 && block.Height == 1) return _full!;
        if (_rates.TryGetValue(rate, out var cached)) return cached;
        using var small = new Bitmap(side / block.Width, side / block.Height, PixelFormat.Format32bppPArgb);
        using (var g = Graphics.FromImage(small)) {
            g.InterpolationMode = InterpolationMode.HighQualityBilinear;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            using var attributes = new ImageAttributes(); attributes.SetWrapMode(WrapMode.TileFlipXY);
            g.DrawImage(_full!, new Rectangle(0, 0, small.Width, small.Height), 0, 0, side, side, GraphicsUnit.Pixel, attributes);
        }
        var result = new Bitmap(side, side, PixelFormat.Format32bppPArgb);
        using (var g = Graphics.FromImage(result)) {
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.PixelOffsetMode = PixelOffsetMode.Half;
            g.DrawImage(small, new Rectangle(0, 0, side, side), 0, 0, small.Width, small.Height, GraphicsUnit.Pixel);
        }
        _rates.Add(rate, result);
        return result;
    }

    internal void Draw(Graphics g, RectangleF plot, PointF gaze, EyeFoveationSettings settings, bool showEffect)
    {
        int side = (int)Math.Ceiling(plot.Width);
        string[] rates = showEffect && settings.Enabled && settings.Backend != 3
            ? settings.EffectiveRates : new[] { "1x1", "1x1", "1x1" };
        var saved = g.Save();
        try {
            g.SetClip(plot, CombineMode.Intersect);
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.PixelOffsetMode = PixelOffsetMode.Half;
            g.DrawImage(ImageForRate(side, rates[2]), plot);
            void zone(decimal boundary, string rate) {
                var state = g.Save();
                using var clip = new GraphicsPath(); clip.AddEllipse(RingBounds(plot, gaze, boundary));
                g.SetClip(clip, CombineMode.Intersect);
                g.DrawImage(ImageForRate(side, rate), plot);
                g.Restore(state);
            }
            zone(settings.Radii.Mid, rates[1]);
            zone(settings.Radii.Inner, rates[0]);
        } finally { g.Restore(saved); }
    }

    private void Clear()
    {
        foreach (var value in _rates.Values) value.Dispose();
        _rates.Clear(); _full?.Dispose(); _full = null;
    }
    public void Dispose() => Clear();
}
