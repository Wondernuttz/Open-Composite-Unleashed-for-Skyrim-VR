using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using Microsoft.Win32;

namespace OpenCompositeConfigurator
{
    // ═══════════════════════════════════════════════════════════════════════
    // CONTROLLER TEXTURES TAB
    //
    // Swaps the Quest 3 controller render-model skin used by SteamVR.
    // Ported from the standalone "Steam Controller Texture Swapper" tool.
    //
    // Two independent bits of state per controller side:
    //   - APPLIED (teal border)  = what's actually on the controllers now
    //   - STAGED  (purple border) = what's queued for the next Apply click
    // Staging never touches real files; only Apply commits them. This lets
    // you mix a different texture on the left vs right and review before
    // committing.
    //
    // Textures live in "Controller Textures\ready to swap" beside the .exe,
    // one subfolder per texture, each holding a left_color.png and a
    // right_color.png.
    // ═══════════════════════════════════════════════════════════════════════
    public partial class MainForm
    {
        private Button _btnTabTextures = null!;
        private Panel _tabTextures = null!;

        private const int TextureSize = 1899;
        private const string LeftColorFile = "oculus_quest_plus_controller_left_color.png";
        private const string RightColorFile = "oculus_quest_plus_controller_right_color.png";
        private const string BackupSuffix = " SWAPPER";
        private const string StockLabel = "Original (stock)";
        private const string TexturesFolderName = "Controller Textures";

        // Applied = teal, Staged = purple. Deliberately distinct hues so the
        // two states are never visually ambiguous.
        private static readonly Color AppliedBorderColor = Color.FromArgb(40, 230, 210);
        private static readonly Color StagedBorderColor = Color.FromArgb(175, 90, 255);

        private string _texLibRoot = "";
        private string _texThumbDir = "";
        private string _texStateFile = "";
        private string _texLeftLive = "";
        private string _texRightLive = "";
        private string _texLeftBackup = "";
        private string _texRightBackup = "";

        private string _appliedLeft = StockLabel;
        private string _appliedRight = StockLabel;
        private string _stagedLeft = StockLabel;
        private string _stagedRight = StockLabel;

        private readonly List<(string Name, string Folder)> _textureList = new();

        private FlowLayoutPanel _texGallery = null!;
        private Label _texAppliedLabel = null!;
        private Label _texStagedLabel = null!;
        private Label _texStatusLabel = null!;

        private void BuildTexturesTab()
        {
            var container = _tabTextures;
            int y = 10;

            // Everything this feature owns lives inside one folder, so it adds
            // a single tidy item to the mod directory rather than scattering
            // the library, thumbnails and state file loose alongside the .exe.
            var texRoot = Path.Combine(AppContext.BaseDirectory, TexturesFolderName);
            _texLibRoot = Path.Combine(texRoot, "ready to swap");
            _texThumbDir = Path.Combine(texRoot, "thumbnails");
            _texStateFile = Path.Combine(texRoot, "current_texture.txt");

            container.Controls.Add(MakeSectionLabel("Quest 3 Controller Textures", 0, y));
            y += 30;

            container.Controls.Add(MakeLabel(
                "Left-click a texture to select it for both controllers  |  Right-click for Left Only / Right Only  |  Drag PNGs or folders here to add",
                0, y, 900));
            y += 24;

            _texAppliedLabel = new Label
            {
                Location = new Point(0, y),
                Size = new Size(700, 20),
                ForeColor = AppliedBorderColor,
                Font = new Font("Segoe UI", 10f, FontStyle.Bold),
                AutoEllipsis = true,
            };
            container.Controls.Add(_texAppliedLabel);
            y += 22;

            _texStagedLabel = new Label
            {
                Location = new Point(0, y),
                Size = new Size(700, 20),
                ForeColor = StagedBorderColor,
                Font = new Font("Segoe UI", 10f, FontStyle.Bold),
                AutoEllipsis = true,
            };
            container.Controls.Add(_texStagedLabel);
            y += 26;

            var btnApply = MakeButton("Apply", 0, y, 110, 28);
            btnApply.Click += (s, e) => CommitStagedTextures();
            container.Controls.Add(btnApply);

            var btnRestore = MakeButton("Restore Original", 118, y, 140, 28);
            btnRestore.Click += (s, e) => RestoreOriginalTextures();
            container.Controls.Add(btnRestore);

            var btnRefresh = MakeButton("Refresh Library", 266, y, 140, 28);
            btnRefresh.Click += (s, e) => { SyncTextureLibrary(); BuildTextureGallery(); ShowTextureStatus("Library refreshed."); };
            container.Controls.Add(btnRefresh);

            _texStatusLabel = new Label
            {
                Location = new Point(416, y + 5),
                Size = new Size(400, 20),
                ForeColor = Color.FromArgb(130, 130, 130),
                AutoEllipsis = true,
            };
            container.Controls.Add(_texStatusLabel);
            y += 36;

            // Anchored bottom so it grows into whatever height the tab ends up
            // with once all tabs are normalised to the tallest one.
            const int galleryHeight = 560;
            _texGallery = new FlowLayoutPanel
            {
                Location = new Point(0, y),
                Size = new Size(container.Width - 10, galleryHeight),
                AutoScroll = true,
                BackColor = Color.FromArgb(25, 25, 30),
                AllowDrop = true,
                Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right,
            };
            _texGallery.DragEnter += TexGallery_DragEnter;
            _texGallery.DragDrop += TexGallery_DragDrop;
            container.Controls.Add(_texGallery);

            // Size the panel to its content, as the other tabs do.
            container.Size = new Size(container.Width, y + galleryHeight + 6);

            InitializeTextureLibrary();
        }

        // Creates the folder structure up front so users can drop textures in
        // without having to open the tab first.
        private void InitializeTextureLibrary()
        {
            try
            {
                Directory.CreateDirectory(_texLibRoot);
                Directory.CreateDirectory(_texThumbDir);
            }
            catch (Exception ex)
            {
                ShowTextureStatus("Could not create texture folders: " + ex.Message, true);
                return;
            }

            LoadAppliedState();
            _stagedLeft = _appliedLeft;
            _stagedRight = _appliedRight;

            SyncTextureLibrary();
            BuildTextureGallery();
            UpdateTextureLabels();
        }

        // ── State persistence ────────────────────────────────────────────────
        // Line 1 = left, line 2 = right. A single-line file (from an older
        // build that didn't support mixing) means the same texture on both.

        private void LoadAppliedState()
        {
            _appliedLeft = StockLabel;
            _appliedRight = StockLabel;
            if (!File.Exists(_texStateFile)) return;

            var lines = File.ReadAllLines(_texStateFile);
            if (lines.Length >= 2)
            {
                _appliedLeft = lines[0].Trim();
                _appliedRight = lines[1].Trim();
            }
            else if (lines.Length == 1)
            {
                _appliedLeft = _appliedRight = lines[0].Trim();
            }
        }

        private void SaveAppliedState()
        {
            try
            {
                var dir = Path.GetDirectoryName(_texStateFile);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                File.WriteAllLines(_texStateFile, new[] { _appliedLeft, _appliedRight });
            }
            catch { /* non-fatal: state is a convenience, not required for operation */ }
        }

        // ── SteamVR discovery ────────────────────────────────────────────────
        // Read Steam's install path from the registry, then walk its library
        // folders - no hardcoded drive letters, works on any machine.

        private static string? FindSteamVRRenderModelsDir()
        {
            string? steamPath = null;
            foreach (var (root, key) in new[]
            {
                (Registry.CurrentUser, @"Software\Valve\Steam"),
                (Registry.LocalMachine, @"SOFTWARE\WOW6432Node\Valve\Steam"),
                (Registry.LocalMachine, @"SOFTWARE\Valve\Steam"),
            })
            {
                try
                {
                    using var k = root.OpenSubKey(key);
                    var v = k?.GetValue("SteamPath") ?? k?.GetValue("InstallPath");
                    if (v is string s && !string.IsNullOrWhiteSpace(s)) { steamPath = s.Replace('/', '\\'); break; }
                }
                catch { /* try next location */ }
            }
            if (steamPath == null) return null;

            var libraries = new List<string> { steamPath };
            var vdf = Path.Combine(steamPath, "steamapps", "libraryfolders.vdf");
            if (File.Exists(vdf))
            {
                foreach (System.Text.RegularExpressions.Match m in
                         System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s*\"([^\"]+)\""))
                {
                    var p = m.Groups[1].Value.Replace("\\\\", "\\");
                    if (!libraries.Contains(p)) libraries.Add(p);
                }
            }

            foreach (var lib in libraries)
            {
                var rm = Path.Combine(lib, "steamapps", "common", "SteamVR", "resources", "rendermodels");
                if (Directory.Exists(rm)) return rm;
            }
            return null;
        }

        private bool EnsureSteamVrPaths()
        {
            if (_texLeftLive.Length > 0) return true;

            var rm = FindSteamVRRenderModelsDir();
            if (rm == null)
            {
                ShowTextureStatus("Could not find SteamVR's rendermodels folder.", true);
                return false;
            }

            var leftDir = Path.Combine(rm, "oculus_quest_plus_controller_left");
            var rightDir = Path.Combine(rm, "oculus_quest_plus_controller_right");
            _texLeftLive = Path.Combine(leftDir, LeftColorFile);
            _texRightLive = Path.Combine(rightDir, RightColorFile);
            _texLeftBackup = _texLeftLive + BackupSuffix;
            _texRightBackup = _texRightLive + BackupSuffix;
            return true;
        }

        // ── Image processing ─────────────────────────────────────────────────
        // Uses System.Drawing (built into the Windows .NET runtime) rather
        // than bundling an external image tool - no extra dependency to ship,
        // and nothing that relies on registry keys written by an installer.

        // Loads without keeping the file locked, so the source can be
        // overwritten in place afterwards.
        private static Bitmap LoadUnlocked(string path)
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read);
            using var img = Image.FromStream(fs);
            return new Bitmap(img);
        }

        private static bool ResizeImage(string srcPath, string destPath, int width, int height, bool asJpeg = false)
        {
            try
            {
                using var src = LoadUnlocked(srcPath);
                using var dest = new Bitmap(width, height);
                using (var g = Graphics.FromImage(dest))
                {
                    g.CompositingQuality = CompositingQuality.HighQuality;
                    g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    g.SmoothingMode = SmoothingMode.HighQuality;
                    g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                    g.DrawImage(src, 0, 0, width, height);
                }

                var dir = Path.GetDirectoryName(destPath);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

                dest.Save(destPath, asJpeg
                    ? System.Drawing.Imaging.ImageFormat.Jpeg
                    : System.Drawing.Imaging.ImageFormat.Png);
                return true;
            }
            catch { return false; }
        }

        // Fits within a box while preserving aspect ratio (for thumbnails).
        private static bool MakeThumbnail(string srcPath, string destPath, int maxSize)
        {
            try
            {
                int w, h;
                using (var probe = LoadUnlocked(srcPath))
                {
                    double scale = Math.Min((double)maxSize / probe.Width, (double)maxSize / probe.Height);
                    w = Math.Max(1, (int)Math.Round(probe.Width * scale));
                    h = Math.Max(1, (int)Math.Round(probe.Height * scale));
                }
                return ResizeImage(srcPath, destPath, w, h, asJpeg: true);
            }
            catch { return false; }
        }

        private static bool IsCorrectSize(string file)
        {
            try
            {
                using var fs = new FileStream(file, FileMode.Open, FileAccess.Read);
                using var img = Image.FromStream(fs, false, false);
                return img.Width == TextureSize && img.Height == TextureSize;
            }
            catch { return false; }
        }

        private string UniqueLibraryName(string baseName)
        {
            var name = baseName;
            int suffix = 2;
            while (Directory.Exists(Path.Combine(_texLibRoot, name)))
                name = $"{baseName}_{suffix++}";
            return name;
        }

        // ── Library sync ─────────────────────────────────────────────────────
        // Makes every texture folder valid and thumbnailed: fills in a missing
        // left/right from a lone PNG, resizes anything that isn't 1899x1899,
        // builds missing thumbnails, drops orphaned ones.

        private void SyncTextureLibrary()
        {
            if (!Directory.Exists(_texLibRoot)) return;

            var folders = Directory.GetDirectories(_texLibRoot).OrderBy(f => f).ToList();
            var names = folders.Select(Path.GetFileName).ToHashSet(StringComparer.OrdinalIgnoreCase);

            foreach (var thumb in Directory.GetFiles(_texThumbDir, "*.jpg"))
            {
                if (!names.Contains(Path.GetFileNameWithoutExtension(thumb)))
                {
                    try { File.Delete(thumb); } catch { }
                }
            }

            foreach (var folder in folders)
            {
                var left = Path.Combine(folder, LeftColorFile);
                var right = Path.Combine(folder, RightColorFile);

                if (!File.Exists(left) || !File.Exists(right))
                {
                    var pngs = Directory.GetFiles(folder, "*.png");
                    if (pngs.Length != 1) continue;
                    try
                    {
                        if (!File.Exists(left)) File.Copy(pngs[0], left, true);
                        if (!File.Exists(right)) File.Copy(pngs[0], right, true);
                    }
                    catch { continue; }
                }

                foreach (var f in new[] { left, right })
                {
                    if (!IsCorrectSize(f))
                        ResizeImage(f, f, TextureSize, TextureSize);
                }

                var thumbPath = Path.Combine(_texThumbDir, Path.GetFileName(folder) + ".jpg");
                bool needsThumb = !File.Exists(thumbPath) ||
                                  File.GetLastWriteTime(thumbPath) < File.GetLastWriteTime(left);
                if (needsThumb)
                    MakeThumbnail(left, thumbPath, 200);
            }
        }

        // ── Gallery ──────────────────────────────────────────────────────────

        private void BuildTextureGallery()
        {
            var savedScroll = new Point(-_texGallery.AutoScrollPosition.X, -_texGallery.AutoScrollPosition.Y);

            foreach (Control c in _texGallery.Controls)
            {
                foreach (Control inner in c.Controls)
                    if (inner is PictureBox pb && pb.Image != null) pb.Image.Dispose();
            }
            _texGallery.Controls.Clear();

            _textureList.Clear();
            if (Directory.Exists(_texLibRoot))
            {
                foreach (var folder in Directory.GetDirectories(_texLibRoot).OrderBy(f => Path.GetFileName(f)))
                    _textureList.Add((Path.GetFileName(folder)!, folder));
            }

            if (_textureList.Count == 0)
            {
                _texGallery.Controls.Add(new Label
                {
                    Text = "No textures yet - drag a PNG or a texture folder into this area to add one.",
                    ForeColor = Color.FromArgb(160, 160, 160),
                    AutoSize = true,
                    Margin = new Padding(10),
                });
                return;
            }

            foreach (var (texName, texFolder) in _textureList)
            {
                var thumbPath = Path.Combine(_texThumbDir, texName + ".jpg");

                var tile = new Panel
                {
                    Size = new Size(220, 240),
                    Margin = new Padding(8),
                    BackColor = Color.FromArgb(40, 40, 48),
                };

                // Borders are drawn per-repaint from live state, so a selection
                // change only needs Invalidate() - no teardown/rebuild, which
                // would otherwise flash and lose scroll position.
                string nameForPaint = texName;
                tile.Paint += (s, e) =>
                {
                    var p = (Panel)s!;
                    var appliedSide = HighlightSideFor(nameForPaint, _appliedLeft, _appliedRight);
                    var stagedSide = HighlightSideFor(nameForPaint, _stagedLeft, _stagedRight);
                    e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
                    if (appliedSide != HighlightSide.None)
                        DrawSideBorder(e.Graphics, p.Width, p.Height, appliedSide, false, AppliedBorderColor);
                    if (stagedSide != HighlightSide.None && stagedSide != appliedSide)
                        DrawSideBorder(e.Graphics, p.Width, p.Height, stagedSide, true, StagedBorderColor);
                };

                var pic = new PictureBox
                {
                    Size = new Size(200, 200),
                    Location = new Point(10, 8),
                    SizeMode = PictureBoxSizeMode.Zoom,
                    Cursor = Cursors.Hand,
                    BackColor = Color.FromArgb(25, 25, 30),
                };
                if (File.Exists(thumbPath))
                {
                    // Load through a stream copy so the file isn't left locked -
                    // Sync would fail to overwrite the thumbnail otherwise.
                    try
                    {
                        using var fs = new FileStream(thumbPath, FileMode.Open, FileAccess.Read);
                        using var tmp = Image.FromStream(fs);
                        pic.Image = new Bitmap(tmp);
                    }
                    catch { }
                }
                pic.Click += (s, e) =>
                {
                    if (e is MouseEventArgs me && me.Button == MouseButtons.Left)
                        StageTexture(nameForPaint, HighlightSide.Both);
                };
                tile.Controls.Add(pic);

                var ctx = new ContextMenuStrip();
                ctx.Items.Add("Stage Left Only").Click += (s, e) => StageTexture(nameForPaint, HighlightSide.Left);
                ctx.Items.Add("Stage Right Only").Click += (s, e) => StageTexture(nameForPaint, HighlightSide.Right);
                tile.ContextMenuStrip = ctx;
                pic.ContextMenuStrip = ctx;

                // Small magnifier box - opens the full-size preview. Kept
                // separate from the thumbnail itself, since clicking the
                // thumbnail selects rather than previews.
                int previewIndex = _textureList.FindIndex(t => t.Name == texName);
                var previewBtn = new Panel
                {
                    Size = new Size(20, 20),
                    Location = new Point(10, 212),
                    BackColor = Color.FromArgb(55, 55, 65),
                    Cursor = Cursors.Hand,
                };
                previewBtn.Paint += (s, e) =>
                {
                    e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
                    using var pen = new Pen(Color.White, 2);
                    e.Graphics.DrawEllipse(pen, 3, 3, 10, 10);
                    e.Graphics.DrawLine(pen, 12, 12, 17, 17);
                };
                previewBtn.Click += (s, e) => ShowTexturePreview(previewIndex);
                tile.Controls.Add(previewBtn);

                var lbl = new Label
                {
                    Text = texName,
                    ForeColor = Color.FromArgb(210, 210, 210),
                    TextAlign = ContentAlignment.MiddleLeft,
                    Size = new Size(174, 20),
                    Location = new Point(36, 212),
                    AutoEllipsis = true,
                };
                tile.Controls.Add(lbl);

                _texGallery.Controls.Add(tile);
            }

            // A real spacer control, not Padding.Bottom - FlowLayoutPanel does
            // not reliably count bottom padding in its scrollable height, which
            // leaves the last row's label clipped at full scroll.
            _texGallery.Controls.Add(new Panel
            {
                Size = new Size(_texGallery.Width - 30, 12),
                BackColor = _texGallery.BackColor,
            });

            _texGallery.PerformLayout();
            _texGallery.AutoScrollPosition = savedScroll;
        }

        private void RepaintTextureGallery() => _texGallery.Invalidate(true);

        // ── Preview window ───────────────────────────────────────────────────
        // Full-size view with next/previous navigation. Stays a single window -
        // the arrows swap the displayed image in place rather than opening a
        // new window each time. Selecting works here exactly as in the gallery.

        private void ShowTexturePreview(int startIndex)
        {
            if (_textureList.Count == 0) return;
            int index = ((startIndex % _textureList.Count) + _textureList.Count) % _textureList.Count;

            var previewForm = new Form
            {
                StartPosition = FormStartPosition.CenterParent,
                BackColor = Color.FromArgb(25, 25, 30),
                ForeColor = Color.FromArgb(220, 220, 220),
                MinimizeBox = false,
                MaximizeBox = false,
                ShowInTaskbar = false,
            };

            var wa = Screen.FromControl(this).WorkingArea;
            int side = Math.Min(820, Math.Min(wa.Width - 120, wa.Height - 160));
            previewForm.ClientSize = new Size(side, side);

            var pb = new PictureBox
            {
                Dock = DockStyle.Fill,
                SizeMode = PictureBoxSizeMode.Zoom,
                BackColor = Color.FromArgb(25, 25, 30),
            };
            previewForm.Controls.Add(pb);

            void LoadCurrent()
            {
                var (texName, texFolder) = _textureList[index];
                previewForm.Text = texName;
                var imgPath = Path.Combine(texFolder, LeftColorFile);
                var old = pb.Image;
                try
                {
                    if (File.Exists(imgPath)) pb.Image = LoadUnlocked(imgPath);
                }
                catch { }
                old?.Dispose();
                pb.Invalidate();
            }

            // Same dual indicator as the tiles, re-evaluated each repaint so
            // navigating or selecting from in here updates it immediately.
            pb.Paint += (s, e) =>
            {
                var texName = _textureList[index].Name;
                var appliedSide = HighlightSideFor(texName, _appliedLeft, _appliedRight);
                var stagedSide = HighlightSideFor(texName, _stagedLeft, _stagedRight);
                e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
                if (appliedSide != HighlightSide.None)
                    DrawSideBorder(e.Graphics, pb.Width, pb.Height, appliedSide, false, AppliedBorderColor);
                if (stagedSide != HighlightSide.None && stagedSide != appliedSide)
                    DrawSideBorder(e.Graphics, pb.Width, pb.Height, stagedSide, true, StagedBorderColor);
            };

            pb.Click += (s, e) =>
            {
                if (e is MouseEventArgs me && me.Button == MouseButtons.Left)
                {
                    StageTexture(_textureList[index].Name, HighlightSide.Both);
                    pb.Invalidate();
                }
            };

            var previewCtx = new ContextMenuStrip();
            previewCtx.Items.Add("Stage Left Only").Click += (s, e) =>
            {
                StageTexture(_textureList[index].Name, HighlightSide.Left);
                pb.Invalidate();
            };
            previewCtx.Items.Add("Stage Right Only").Click += (s, e) =>
            {
                StageTexture(_textureList[index].Name, HighlightSide.Right);
                pb.Invalidate();
            };
            pb.ContextMenuStrip = previewCtx;

            Button MakeNavButton(string glyph)
            {
                var b = new Button
                {
                    Text = glyph,
                    Size = new Size(50, 90),
                    Font = new Font("Segoe UI", 14f, FontStyle.Bold),
                    ForeColor = Color.White,
                    BackColor = Color.FromArgb(45, 45, 55),
                    FlatStyle = FlatStyle.Flat,
                    Cursor = Cursors.Hand,
                };
                b.FlatAppearance.BorderSize = 0;
                return b;
            }

            var prevBtn = MakeNavButton("◀");
            var nextBtn = MakeNavButton("▶");
            prevBtn.Click += (s, e) =>
            {
                index = ((index - 1) % _textureList.Count + _textureList.Count) % _textureList.Count;
                LoadCurrent();
            };
            nextBtn.Click += (s, e) =>
            {
                index = ((index + 1) % _textureList.Count + _textureList.Count) % _textureList.Count;
                LoadCurrent();
            };
            previewForm.Controls.Add(prevBtn);
            previewForm.Controls.Add(nextBtn);

            void PositionNav()
            {
                int midY = (previewForm.ClientSize.Height - prevBtn.Height) / 2;
                prevBtn.Location = new Point(12, midY);
                nextBtn.Location = new Point(previewForm.ClientSize.Width - nextBtn.Width - 12, midY);
                prevBtn.BringToFront();
                nextBtn.BringToFront();
            }
            previewForm.Resize += (s, e) => PositionNav();
            previewForm.Shown += (s, e) => PositionNav();

            // Arrow keys navigate too.
            previewForm.KeyPreview = true;
            previewForm.KeyDown += (s, e) =>
            {
                if (e.KeyCode == Keys.Left) prevBtn.PerformClick();
                else if (e.KeyCode == Keys.Right) nextBtn.PerformClick();
                else if (e.KeyCode == Keys.Escape) previewForm.Close();
            };

            previewForm.FormClosed += (s, e) =>
            {
                pb.Image?.Dispose();
                previewForm.Dispose();
                // Selections made in the preview need to show on the tiles.
                RepaintTextureGallery();
            };

            LoadCurrent();
            previewForm.ShowDialog(this);
        }

        // ── Highlight geometry ───────────────────────────────────────────────

        private enum HighlightSide { None, Left, Right, Both }

        private static HighlightSide HighlightSideFor(string texName, string leftVal, string rightVal)
        {
            bool l = string.Equals(texName, leftVal, StringComparison.Ordinal);
            bool r = string.Equals(texName, rightVal, StringComparison.Ordinal);
            if (l && r) return HighlightSide.Both;
            if (l) return HighlightSide.Left;
            if (r) return HighlightSide.Right;
            return HighlightSide.None;
        }

        // "thin" = staged (single crisp line); otherwise a soft multi-ring glow
        // for applied. A half-border means that texture is only on one side.
        private static void DrawSideBorder(Graphics g, int w, int h, HighlightSide side, bool thin, Color color)
        {
            if (side == HighlightSide.None) return;

            var rings = thin
                ? new[] { (Offset: 0, Alpha: 255) }
                : new[] { (0, 70), (1, 115), (2, 165), (3, 215), (4, 255) };

            foreach (var (offset, alpha) in rings)
            {
                using var pen = new Pen(Color.FromArgb(alpha, color), 2);
                int o = offset;
                int midX = w / 2;
                switch (side)
                {
                    case HighlightSide.Both:
                        g.DrawRectangle(pen, o, o, w - 1 - (2 * o), h - 1 - (2 * o));
                        break;
                    case HighlightSide.Left:
                        g.DrawLine(pen, o, o, midX, o);
                        g.DrawLine(pen, o, h - 1 - o, midX, h - 1 - o);
                        g.DrawLine(pen, o, o, o, h - 1 - o);
                        break;
                    case HighlightSide.Right:
                        g.DrawLine(pen, midX, o, w - 1 - o, o);
                        g.DrawLine(pen, midX, h - 1 - o, w - 1 - o, h - 1 - o);
                        g.DrawLine(pen, w - 1 - o, o, w - 1 - o, h - 1 - o);
                        break;
                }
            }
        }

        // ── Staging / applying ───────────────────────────────────────────────

        // Selecting the same thing twice clears it, reverting that side to
        // whatever is currently applied. Only ever touches staged state.
        private void StageTexture(string textureName, HighlightSide side)
        {
            if (side == HighlightSide.Both)
            {
                if (_stagedLeft == textureName && _stagedRight == textureName)
                {
                    _stagedLeft = _appliedLeft;
                    _stagedRight = _appliedRight;
                }
                else
                {
                    _stagedLeft = textureName;
                    _stagedRight = textureName;
                }
            }
            else if (side == HighlightSide.Left)
            {
                _stagedLeft = (_stagedLeft == textureName) ? _appliedLeft : textureName;
            }
            else if (side == HighlightSide.Right)
            {
                _stagedRight = (_stagedRight == textureName) ? _appliedRight : textureName;
            }

            RepaintTextureGallery();
            UpdateTextureLabels();
        }

        private string? FolderForTexture(string name)
        {
            foreach (var (texName, folder) in _textureList)
                if (texName == name) return folder;
            return null;
        }

        // Preserves whatever is live by renaming it aside on first swap, so
        // Restore always has a true original to return to - per side, so
        // swapping only the left still protects the right.
        private void ApplyOneSide(bool isLeft, string folder)
        {
            var srcFile = Path.Combine(folder, isLeft ? LeftColorFile : RightColorFile);
            var liveFile = isLeft ? _texLeftLive : _texRightLive;
            var backupFile = isLeft ? _texLeftBackup : _texRightBackup;

            if (!File.Exists(backupFile) && File.Exists(liveFile))
                File.Move(liveFile, backupFile);

            File.Copy(srcFile, liveFile, true);
        }

        private void CommitStagedTextures()
        {
            if (!EnsureSteamVrPaths()) return;

            var leftFolder = FolderForTexture(_stagedLeft);
            var rightFolder = FolderForTexture(_stagedRight);
            if (leftFolder == null || rightFolder == null)
            {
                ShowTextureStatus("Select a texture first - left-click a thumbnail, or right-click for one side.", true);
                return;
            }

            try
            {
                ApplyOneSide(true, leftFolder);
                ApplyOneSide(false, rightFolder);
            }
            catch (Exception ex)
            {
                ShowTextureStatus("Failed to apply: " + ex.Message, true);
                return;
            }

            _appliedLeft = _stagedLeft;
            _appliedRight = _stagedRight;
            SaveAppliedState();
            RepaintTextureGallery();
            UpdateTextureLabels();
            ShowTextureStatus("Applied. Restart SteamVR to see the change.");
        }

        private void RestoreOriginalTextures()
        {
            if (!EnsureSteamVrPaths()) return;

            if (!File.Exists(_texLeftBackup) || !File.Exists(_texRightBackup))
            {
                ShowTextureStatus("Already at default - no backup to restore from.");
                return;
            }

            try
            {
                if (File.Exists(_texLeftLive)) File.Delete(_texLeftLive);
                if (File.Exists(_texRightLive)) File.Delete(_texRightLive);
                File.Move(_texLeftBackup, _texLeftLive);
                File.Move(_texRightBackup, _texRightLive);
            }
            catch (Exception ex)
            {
                ShowTextureStatus("Failed to restore: " + ex.Message, true);
                return;
            }

            _appliedLeft = _appliedRight = StockLabel;
            _stagedLeft = _stagedRight = StockLabel;
            SaveAppliedState();
            RepaintTextureGallery();
            UpdateTextureLabels();
            ShowTextureStatus("Restored original stock textures.");
        }

        // ── Drag and drop ────────────────────────────────────────────────────
        // A dropped PNG becomes its own texture folder; a dropped folder is
        // taken as an already-built texture set.

        private void TexGallery_DragEnter(object? sender, DragEventArgs e)
        {
            e.Effect = (e.Data != null && e.Data.GetDataPresent(DataFormats.FileDrop))
                ? DragDropEffects.Copy
                : DragDropEffects.None;
        }

        private void TexGallery_DragDrop(object? sender, DragEventArgs e)
        {
            if (e.Data?.GetData(DataFormats.FileDrop) is not string[] paths) return;

            int pngCount = 0, folderCount = 0, skipped = 0;

            foreach (var p in paths)
            {
                try
                {
                    if (Directory.Exists(p))
                    {
                        var dest = Path.Combine(_texLibRoot, UniqueLibraryName(Path.GetFileName(p.TrimEnd('\\'))!));
                        CopyDirectory(p, dest);
                        folderCount++;
                    }
                    else if (File.Exists(p) && p.EndsWith(".png", StringComparison.OrdinalIgnoreCase))
                    {
                        var destFolder = Path.Combine(_texLibRoot, UniqueLibraryName(Path.GetFileNameWithoutExtension(p)));
                        Directory.CreateDirectory(destFolder);
                        var left = Path.Combine(destFolder, LeftColorFile);
                        var right = Path.Combine(destFolder, RightColorFile);
                        if (!ResizeImage(p, left, TextureSize, TextureSize))
                            File.Copy(p, left, true);
                        File.Copy(left, right, true);
                        pngCount++;
                    }
                    else skipped++;
                }
                catch { skipped++; }
            }

            SyncTextureLibrary();
            BuildTextureGallery();
            UpdateTextureLabels();

            var msg = $"Added {pngCount} PNG(s) and {folderCount} folder(s).";
            if (skipped > 0) msg += $" Skipped {skipped}.";
            ShowTextureStatus(msg);
        }

        private static void CopyDirectory(string src, string dest)
        {
            Directory.CreateDirectory(dest);
            foreach (var file in Directory.GetFiles(src))
                File.Copy(file, Path.Combine(dest, Path.GetFileName(file)), true);
            foreach (var dir in Directory.GetDirectories(src))
                CopyDirectory(dir, Path.Combine(dest, Path.GetFileName(dir)));
        }

        // ── Labels ───────────────────────────────────────────────────────────

        private void UpdateTextureLabels()
        {
            _texAppliedLabel.Text = (_appliedLeft == _appliedRight)
                ? $"Current: {_appliedLeft}"
                : $"Current  -  Left: {_appliedLeft}   Right: {_appliedRight}";

            _texStagedLabel.Text = (_stagedLeft == _stagedRight)
                ? $"Staged: {_stagedLeft}"
                : $"Staged  -  Left: {_stagedLeft}   Right: {_stagedRight}";
        }

        private void ShowTextureStatus(string message, bool warn = false)
        {
            _texStatusLabel.ForeColor = warn ? Color.FromArgb(255, 180, 100) : Color.FromArgb(130, 130, 130);
            _texStatusLabel.Text = message;
        }
    }
}
