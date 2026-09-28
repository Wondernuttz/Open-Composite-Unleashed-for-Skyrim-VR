using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static class Program
{
    static int checks;
    const BindingFlags Flags = BindingFlags.Instance | BindingFlags.NonPublic;
    static void Check(bool value, string message) { ++checks; if (!value) throw new Exception(message); }
    static T Field<T>(object obj, string name) => (T)typeof(MainForm).GetField(name, Flags)!.GetValue(obj)!;
    sealed class PreviewMain : MainForm { protected override void OnShown(EventArgs e) { FitWindowToArea(Screen.FromControl(this).WorkingArea, DeviceDpi / 96f); } }
    static IEnumerable<Control> All(Control parent)
    {
        foreach(Control c in parent.Controls) { yield return c; if(c is UpDownBase or ComboBox or TextBoxBase) continue; foreach(var n in All(c)) yield return n; }
    }
    static void AssertAccessible(Control c, string context)
    {
        Check(c.Visible, context + " visible");
        Check(c.Width >= 20 && c.Height >= 10, context + " usable size " + c.Bounds);
        var rect = c.ClientRectangle;
        Control child = c;
        for(Control? parent=c.Parent; parent != null; parent=parent.Parent)
        {
            rect.Offset(child.Location);
            var bounds = parent.ClientRectangle; bounds.Inflate(2,2);
            Check(bounds.Contains(rect), context + $" clipped by {parent.GetType().Name} {parent.Name}: {rect} / {bounds}");
            child=parent;
        }
        var center = new Point(c.Left + c.Width/2, c.Top + c.Height/2);
        Check(c.Parent!.GetChildAtPoint(center, GetChildAtPointSkip.Invisible)==c, context + " covered by sibling " + c.Parent.GetChildAtPoint(center, GetChildAtPointSkip.Invisible)?.Text);
        if(c is NumericUpDown number)
        {
            var edit=number.Controls.OfType<TextBox>().Single();
            Check(edit.Visible && edit.Width>8, context + " native edit remains visible");
            Check(number.ClientRectangle.Contains(edit.Bounds), context + " native edit fits");
        }
    }
    static void CheckMain(PreviewMain f, Rectangle area, float dpi)
    {
        Application.DoEvents(); f.FitWindowToArea(area,dpi); f.FitWindowToArea(area,dpi); f.PerformLayout();
        Check(area.Contains(f.Bounds), "main window fits " + area + " / " + f.Bounds);
        Check(!All(f).OfType<ScrollableControl>().Any(c=>c.AutoScroll), "main has no scrolling");
        for(int tab=0;tab<8;tab++)
        {
            typeof(MainForm).GetMethod("SwitchTab",Flags)!.Invoke(f,new object[]{tab});
            foreach(var c in All(f).Where(c=>c.Visible && c is NumericUpDown or ComboBox or TextBox))
                AssertAccessible(c,$"dpi={dpi} area={area.Size} tab={tab} input={c.AccessibleName} {c.Bounds}");
        }
        typeof(MainForm).GetMethod("SwitchTab",Flags)!.Invoke(f,new object[]{0});
        AssertAccessible(Field<NumericUpDown>(f,"_nudLeftDeadZone"),"left deadzone");
        AssertAccessible(Field<NumericUpDown>(f,"_nudRightDeadZone"),"right deadzone");
        foreach(var (text, name) in new[]{("L dead zone:","_nudLeftDeadZone"), ("R:","_nudRightDeadZone"),
            ("Keyboard haptics:","_nudKbHapticStrength"), ("Press sound:","_nudPressVolume")})
        {
            var input=Field<NumericUpDown>(f,name);
            var label=input.Parent!.Controls.OfType<Label>().Single(c=>c.Text==text);
            Check(label.Right<input.Left,text+" has space before input");
            Check(label.PreferredWidth<=label.Width,text+" text fits label");
            foreach(Control sibling in input.Parent.Controls)
                if(sibling != label && sibling.Visible)
                    Check(!label.Bounds.IntersectsWith(sibling.Bounds),text+" overlaps " + sibling.Text);
        }
        typeof(MainForm).GetMethod("SwitchTab",Flags)!.Invoke(f,new object[]{7});
        var cable=Field<CheckBox>(f,"_chkCableTracking");
        AssertAccessible(cable,"cable tracking toggle");
        foreach(Control c in cable.Parent!.Controls) if(c is Button or Label) AssertAccessible(c,"cable panel "+c.Text);
        typeof(MainForm).GetMethod("SwitchTab",Flags)!.Invoke(f,new object[]{0});
        var haptics=Field<NumericUpDown>(f,"_nudKbHapticStrength");
        var press=Field<NumericUpDown>(f,"_nudPressVolume");
        AssertAccessible(haptics,"keyboard haptics on Settings");
        Check(haptics.Left==press.Left && haptics.Width==press.Width && haptics.Top>press.Bottom,
            "keyboard feedback rows align vertically without overlap");
        Check(!All(f).Any(c=>c.Text=="Hover sound:"),"inactive hover audio control removed");
        Check(!All(f).Any(c=>c.Text.StartsWith("Studio imports and saved designs")),"obsolete keyboard hint removed");
        typeof(MainForm).GetMethod("SwitchTab",Flags)!.Invoke(f,new object[]{3});
        var camera=Field<CheckBox>(f,"_chkFsr3CameraMV");
        var advanced=camera.Parent!;
        advanced.Visible=true; Application.DoEvents(); DpiLayout.Refresh(f);
        var mv=Field<CheckBox>(f,"_chkMotionVectorsEnabled");
        var actor=Field<CheckBox>(f,"_chkActorMV");
        Check(mv.Right<actor.Left && actor.Right<camera.Left,$"motion vector checkboxes separated {dpi}: {mv.Bounds} {actor.Bounds} {camera.Bounds}");
        var desc=advanced.Controls.OfType<Label>().Single(c=>c.Text.StartsWith("Motion vectors feed"));
        Check(camera.Right<desc.Left,"camera MV separated from description");
        AssertAccessible(camera,"advanced Camera MV");
        advanced.Visible=false;
        typeof(MainForm).GetMethod("SwitchTab",Flags)!.Invoke(f,new object[]{0});
        var grip=Field<IndexGripControl>(f,"_indexGrip");
        grip.SetIndexSelected(true); grip.Mode.SelectedIndex=1;
        AssertAccessible(grip.Grab,"Index grab"); AssertAccessible(grip.Release,"Index release");
        var footer=All(f).Single(c=>c.Name=="SupportFooter");
        Check(footer.Parent!.ClientRectangle.Contains(footer.Bounds),"footer fits");
    }
    static void CheckUserResize(PreviewMain f)
    {
        typeof(MainForm).GetMethod("SwitchTab",Flags)!.Invoke(f,new object[]{0});
        f.WindowState=FormWindowState.Normal;
        f.FitWindowToArea(Screen.FromControl(f).WorkingArea,1f);
        Application.DoEvents();
        var input=Field<NumericUpDown>(f,"_nudLeftDeadZone");
        var original=f.ClientSize;
        var originalBounds=input.Bounds;
        // Exercise the native resize event, not the layout helper. Dragging must
        // update controls before ResizeEnd, and restoring must keep the chosen size.
        f.ClientSize=new Size(original.Width*4/5,original.Height*4/5);
        var chosen=f.Size;
        Application.DoEvents();
        Check(input.Left<originalBounds.Left,"ordinary resize updates input position before ResizeEnd");
        Check(f.Size==chosen,"ordinary resize keeps the user-selected window size");
        foreach(var c in All(f).Where(c=>c.Visible && c is NumericUpDown or ComboBox or TextBox))
            AssertAccessible(c,"user-shrunken window "+c.Bounds);
        f.WindowState=FormWindowState.Maximized; Application.DoEvents();
        f.WindowState=FormWindowState.Normal; Application.DoEvents();
        Check(f.Size==chosen,"maximize/restore keeps the user-selected window size");
        f.ClientSize=original; Application.DoEvents();
        Check(input.Bounds==originalBounds,"resize roundtrip preserves numeric alignment");
    }
    static void CheckGestureLibrary(PreviewMain f)
    {
        string dir=Path.Combine(AppContext.BaseDirectory,"Gestures"); Directory.CreateDirectory(dir);
        var fixtures=Enumerable.Range(0,5).Select(i=>Path.Combine(dir,$"dpi-fixture-{Guid.NewGuid():N}.json")).ToArray();
        try
        {
            foreach(string path in fixtures) File.WriteAllText(path,"{\"Name\":\"DPI fixture\",\"Left\":[],\"Right\":[]}");
            typeof(MainForm).GetMethod("SwitchTab",Flags)!.Invoke(f,new object[]{2});
            foreach(float scale in new[]{1f,2f,1.25f,1f})
            {
                f.FitWindowToArea(Screen.FromControl(f).WorkingArea,scale);
                typeof(MainForm).GetField("_gestureLibraryPage",Flags)!.SetValue(f,0);
                typeof(MainForm).GetMethod("RefreshGestureLibrary",Flags)!.Invoke(f,null);
                var library=Field<FlowLayoutPanel>(f,"_pnlGestureLibrary");
                int pages=(Directory.GetFiles(dir,"*.json").Length+1)/2;
                for(int page=0;page<pages;page++)
                {
                    library.PerformLayout();
                    Check(library.Controls.Count is >=1 and <=2,"gesture card page size");
                    foreach(Control card in library.Controls) Check(library.ClientRectangle.Contains(card.Bounds),"late-created gesture card fits");
                    if(page+1<pages) typeof(Button).GetMethod("OnClick",Flags)!.Invoke(Field<Button>(f,"_gestureLibraryNext"),new object[]{EventArgs.Empty});
                }
            }
        }
        finally { foreach(string path in fixtures) File.Delete(path); }
    }
    static void CheckCableSettings()
    {
        using var f=new PreviewMain();
        var ini=new IniFile();
        typeof(MainForm).GetField("_ini",Flags)!.SetValue(f,ini);
        void Load()=>typeof(MainForm).GetMethod("LoadCableTrackingSettings",Flags)!.Invoke(f,null);
        Load();
        Check(!Field<CheckBox>(f,"_chkCableTracking").Checked,"cable tracking defaults off");
        Check(Field<Keys>(f,"_cableShowKey")==Keys.F8,"default show shortcut");
        Check(Field<Keys>(f,"_cableResetKey")== (Keys.Control|Keys.Shift|Keys.F8),"reset requires deliberate chord");
        ini.Set("cabletracking","enabled","true");ini.Set("cabletracking","showKey","120");
        ini.Set("cabletracking","showModifiers","4");ini.Set("cabletracking","warningTurns","3.5");
        Load();
        Check(Field<CheckBox>(f,"_chkCableTracking").Checked,"enabled persisted");
        Check(Field<Keys>(f,"_cableShowKey")== (Keys.Alt|Keys.F9),"custom key/modifier loaded");
        Check(Field<decimal>(f,"_cableWarningTurns")==3.5m,"warning threshold loaded");
        typeof(MainForm).GetMethod("SaveCableTrackingSettings",Flags)!.Invoke(f,null);
        Check(ini.Get("cabletracking","showKey","")=="120"&&ini.Get("cabletracking","showModifiers","")=="4","keyboard shortcut save roundtrip");
        Check(ini.Get("cabletracking","warningTurns","")=="3.5","invariant-culture threshold serialization");
        ini.Set("cabletracking","displaySeconds","999");ini.Set("cabletracking","warningTurns","NaN");Load();
        Check(Field<int>(f,"_cableDisplaySeconds")==30&&Field<decimal>(f,"_cableWarningTurns")==2,"invalid/out-of-range config repaired");
        foreach(var action in new[]{0x7f01,0x7f02}) {
            var combo=new ComboEntry {ButtonString="left_grip+x",Mode="long_press",TimingMs=1000,Scancode=action};
            var reloaded=ComboEntry.FromIniValue(combo.ToIniValue());
            Check(reloaded!=null&&reloaded.Scancode==action&&reloaded.ButtonString==combo.ButtonString,"controller action serialization retains code and chord");
            Check(combo.GetDisplaySummary(new()).Contains("Cable tracker:"),"controller action human-readable summary");
        }
        using var dialog=(Form)typeof(MainForm).GetMethod("CreateCableTrackingDialog",Flags)!.Invoke(f,null)!;
        Check(!dialog.AutoScroll,"cable popup has no scrolling");
        var capture=All(dialog).OfType<Button>().Single(b=>b.Text=="Alt+F9");
        typeof(Button).GetMethod("OnClick",Flags)!.Invoke(capture,new object[]{EventArgs.Empty});
        Check(capture.Text.StartsWith("Press key"),"shortcut capture arms on click");
        typeof(Control).GetMethod("OnKeyDown",Flags)!.Invoke(capture,new object[]{new KeyEventArgs(Keys.ControlKey)});
        Check(capture.Text.StartsWith("Press key"),"modifier alone does not finish capture");
        typeof(Control).GetMethod("OnKeyDown",Flags)!.Invoke(capture,new object[]{new KeyEventArgs(Keys.Control|Keys.F7)});
        Check(capture.Text=="Ctrl+F7","shortcut captures key with modifiers");
        typeof(Button).GetMethod("OnClick",Flags)!.Invoke(capture,new object[]{EventArgs.Empty});
        typeof(Control).GetMethod("OnKeyDown",Flags)!.Invoke(capture,new object[]{new KeyEventArgs(Keys.Escape)});
        Check(capture.Text=="Ctrl+F7","Escape preserves previous shortcut");

        Console.WriteLine($"PASS: {checks} cable settings and controller serialization checks.");
    }
    [STAThread] static void Main(string[] args)
    {
        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2); Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        if(args.Contains("--cable")) { CheckCableSettings(); return; }
        if(args.Length==0 || args.Contains("--interactive")) { using var f=new PreviewMain(); f.Text="OCU DPI layout preview"; Application.Run(f); return; }
        if(args.Length==2 && args[0]=="--startup")
        {
            // Fit before first show, then verify native handle initialization
            // preserves editability. This models scale without changing the desktop.
            int dpi=int.Parse(args[1]);
            using var startup=new PreviewMain {Opacity=0,ShowInTaskbar=false};
            Check(Field<NumericUpDown>(startup,"_nudLeftDeadZone").Left==492,"startup preserves logical coordinates");
            startup.FitWindowToArea(new Rectangle(0,0,3840,2120),dpi/96f);
            startup.Show(); Application.DoEvents();
            CheckMain(startup,Screen.FromControl(startup).WorkingArea,dpi/96f);
            Console.WriteLine($"PASS: {checks} pre-show scale checks at {dpi/96f:P0}; native window uses attached monitor DPI."); return;
        }
        using var main=new PreviewMain {Opacity=0,ShowInTaskbar=false}; main.Show();
        CheckUserResize(main);
        foreach(float dpi in new[]{1f,1.25f,1.5f,1.75f,2f,2.5f,3f})
            foreach(var size in new[]{new Size(1366,728),new Size(1920,1040),new Size(2560,1400),new Size(3840,2120),new Size(3440,1400)})
                CheckMain(main,new Rectangle(new Point(0,0),size),dpi);
        var area=Screen.FromControl(main).WorkingArea;
        CheckMain(main,area,1);
        var baseline=All(main).OfType<NumericUpDown>().Select(c=>(c,c.Bounds,c.Font.SizeInPoints)).ToArray();
        for(int i=0;i<5;i++) { CheckMain(main,area,2); CheckMain(main,area,1.25f); CheckMain(main,area,1); }
        foreach(var (c,bounds,points) in baseline) { Check(c.Bounds==bounds,"DPI roundtrip preserves exact input bounds"); Check(Math.Abs(c.Font.SizeInPoints-points)<.001f,"DPI roundtrip preserves font"); }
        main.WindowState=FormWindowState.Maximized; Application.DoEvents();
        var outer=main.Bounds; var restore=main.RestoreBounds; main.FitWindowToArea(area,1);
        Check(main.Bounds==outer && main.RestoreBounds==restore,"maximized bounds and restore bounds preserved");
        main.WindowState=FormWindowState.Minimized; Application.DoEvents();
        var before=main.Controls["DpiContent"]!.Bounds; main.FitWindowToArea(area,2);
        Check(main.Controls["DpiContent"]!.Bounds==before,"minimized layout unchanged");
        main.WindowState=FormWindowState.Normal; Application.DoEvents();
        foreach(var screen in Screen.AllScreens)
        {
            main.Location=screen.WorkingArea.Location; Application.DoEvents();
            CheckMain(main,screen.WorkingArea,main.DeviceDpi/96f);
            Console.WriteLine($"Native display checked: {screen.Bounds.Size}, DPI {main.DeviceDpi}.");
        }
        CheckGestureLibrary(main);
        foreach(string type in new[]{"eye","combo","help","kat","prompt","cable"})
        {
            using Form form=type switch {
                "eye"=>new EyeFoveationEditor(new EyeFoveationSettings()),
                "combo"=>new ComboEditForm(new Dictionary<string,int>{{"A",30}}),
                "help"=>new IndexSpellWheelHelpDialog(),
                "cable"=>(Form)typeof(MainForm).GetMethod("CreateCableTrackingDialog",Flags)!.Invoke(main,null)!,
                _=>new Form {ClientSize=type=="kat"?new Size(780,396):new Size(444,121)} };
            if(type is "kat" or "prompt") { form.Controls.Add(new Button {Text="Close",Location=new Point(form.ClientSize.Width-100,form.ClientSize.Height-40),Size=new Size(90,30)}); DpiLayout.Popup(form); }
            var content=(Panel)form.Controls["DpiContent"]!;
            Size design=type=="eye"?new Size(880,720):content.Size;
            form.Opacity=0; form.ShowInTaskbar=false; form.Show();
            foreach(float dpi in new[]{1f,2f,1.25f,3f,1f})
            {
                DpiLayout.FitPopup(form,content,design,area,dpi);
                Check(area.Contains(form.Bounds),type+" fits display");
                Check(form.AutoScroll==(type=="eye"),type+" scroll policy");
                if(type!="eye") foreach(var number in All(form).OfType<NumericUpDown>().Where(c=>c.Visible)) AssertAccessible(number,type+" number");
                else {
                    var root=content.Controls[0]; foreach(Control child in root.Controls)
                        Check(root.ClientRectangle.Contains(child.Bounds),"eye header/body/footer fit");
                }
            }
        }
        Console.WriteLine($"PASS: {checks} control visibility, editability, containment, DPI roundtrip and popup checks. 100–300%, 1366–3840 wide. Physical mixed-monitor testing remains separate.");
    }
}
