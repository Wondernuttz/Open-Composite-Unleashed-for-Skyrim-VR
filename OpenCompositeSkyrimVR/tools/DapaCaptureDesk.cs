using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Windows.Forms;

// Standalone local diagnostic helper, .NET Framework. No game input injection,
// network, settings edits, or continuous recording.
internal static class DapaCaptureDesk
{
    [STAThread]
    static void Main(string[] args)
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
            "My Games", "Skyrim VR", "DAPA Captures");
        Directory.CreateDirectory(root);
        var form = new Form {
            Text = "DAPA — Desktop Capture", ClientSize = new Size(570, 360),
            StartPosition = FormStartPosition.CenterScreen, BackColor = Color.FromArgb(20,25,32),
            ForeColor = Color.White, Font = new Font("Segoe UI", 10),
            FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false
        };
        var title = new Label { Text = "See real vs reprojection", Font = new Font("Segoe UI",18,FontStyle.Bold),
            Location = new Point(22,18), Size = new Size(525,38) };
        var info = new Label { Text = "Both grips: START (1 pulse); again: STOP (2 pulses).\r\nUp to 2 minutes of movement / 8 photo attempts; not video.\r\n3 pulses = unavailable. Button: one delayed snapshot.",
            Location = new Point(24,65), Size = new Size(520,80) };
        var status = new Label { Text = "Ready. Start Skyrim through MO2 with the capture build.",
            Location = new Point(24,240), Size = new Size(522,60), ForeColor = Color.LightSkyBlue };
        Func<string> latest = () => Directory.GetDirectories(root).Where(p => File.Exists(Path.Combine(p,"index.html"))
            && File.Exists(Path.Combine(p,"README.txt"))).OrderByDescending(p => File.GetLastWriteTimeUtc(Path.Combine(p,"README.txt"))).FirstOrDefault();
        Action<string> open = path => {
            try { Process.Start(new ProcessStartInfo(path) { UseShellExecute=true }); }
            catch (Exception e) { status.Text=e.Message; }
        };
        var capture = new Button { Text = "Capture in 3 seconds", Location = new Point(24,155), Size = new Size(255,44),
            BackColor = Color.FromArgb(226,177,66), ForeColor = Color.Black, FlatStyle=FlatStyle.Flat };
        var report = new Button { Text = "Open latest comparison", Location = new Point(292,155), Size = new Size(255,44),
            FlatStyle=FlatStyle.Flat };
        var folder = new Button { Text = "Open capture folder / PNG photos", Location = new Point(24,208),
            Size = new Size(523,30), FlatStyle=FlatStyle.Flat };
        var hotkey = new Label { Text = "1 pulse = recording. 2 = stopped/saving. 3 = unavailable.",
            Location = new Point(24,310), Size = new Size(525,32), ForeColor = Color.Silver };
        DateTime requestAt=DateTime.MinValue;string priorReport=null;bool pending=false;
        capture.Click += delegate {
            if(Process.GetProcessesByName("SkyrimVR").Length==0) { status.Text="Start Skyrim through MO2 first.";return; }
            try {
                priorReport=latest();File.WriteAllText(Path.Combine(root,"capture.request"),"single-shot\r\n");
                requestAt=DateTime.Now;pending=true;capture.Enabled=false;
                status.Text="Requested. Resume your movement now. Keep the headset rendering for about 5 seconds.";
            } catch(Exception e) { status.Text=e.Message; }
        };
        report.Click += delegate {
            var path=latest();if(path==null)status.Text="No complete capture yet.";
            else open(Path.Combine(path,"index.html"));
        };
        folder.Click += delegate { open(root); };
        var timer=new Timer {Interval=1000};
        timer.Tick += delegate {
            if(!pending)return;
            var path=latest();
            if(path!=null && path!=priorReport) {
                status.Text="Saved: "+Path.GetFileName(path)+"\r\nOpen the comparison, or the PNG photos.";
                capture.Enabled=true;pending=false;
            } else if((DateTime.Now-requestAt).TotalSeconds>30) {
                status.Text="No completed report yet. Check the DAPA CAPTURE lines in OCUnleashedSKSE.log; capture may be unavailable, aborted, or still writing.";
                capture.Enabled=true;pending=false;
            }
        };
        form.Controls.AddRange(new Control[]{title,info,capture,report,folder,status,hotkey});
        if(args.Contains("--self-test")) {
            foreach(Control control in form.Controls) {
                if(control.Right>form.ClientSize.Width || control.Bottom>form.ClientSize.Height)
                    throw new InvalidOperationException("Capture helper control outside client area");
            }
            form.Dispose();timer.Dispose();return;
        }
        timer.Start();Application.Run(form);timer.Dispose();
    }
}
