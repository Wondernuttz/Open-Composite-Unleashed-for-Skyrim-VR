using System;
using System.Drawing;
using System.Globalization;
using System.Windows.Forms;

namespace OpenCompositeConfigurator;

public partial class MainForm
{
    private CheckBox _chkCableTracking = null!;
    private Label _lblCableTracking = null!;
    private Keys _cableShowKey = Keys.F8, _cableResetKey = Keys.Control | Keys.Shift | Keys.F8;
    private int _cableDisplaySeconds = 5;
    private decimal _cableWarningTurns = 2;

    private void BuildCableTrackingPanel(Control parent, int x, int y, int width)
    {
        var panel = new Panel { Location = new Point(x,y), Size = new Size(width,148),
            BackColor = ModernUiTheme.SurfaceRaised, AccessibleName = "Cable tracking" };
        panel.Controls.Add(MakeSectionLabel("Cable tracking",12,10));
        _chkCableTracking = MakeCheckBox("Track headset turns",12,38);
        _chkCableTracking.AutoSize = true;
        panel.Controls.Add(_chkCableTracking);
        _lblCableTracking = new Label { Location = new Point(12,66), Size = new Size(width-24,32),
            ForeColor = ModernUiTheme.TextSecondary };
        panel.Controls.Add(_lblCableTracking);
        var setup = MakeButton("Shortcuts & display...",12,106,width-24,30);
        setup.Click += (_,_) => ShowCableTrackingSettings();
        panel.Controls.Add(setup);
        parent.Controls.Add(panel);
    }
    private static string CableKeyName(Keys key) => key == Keys.None ? "Unassigned" : new KeysConverter().ConvertToString(key) ?? key.ToString();
    private void RefreshCableTrackingSummary() => _lblCableTracking.Text = $"Show: {CableKeyName(_cableShowKey)}  |  Physical turns\nWorks without KAT VR or body trackers.";
    private void LoadCableTrackingSettings()
    {
        int Number(string name,int fallback,int min,int max) => int.TryParse(_ini.Get("cabletracking",name,fallback.ToString()),out var n) ? Math.Clamp(n,min,max) : fallback;
        Keys Key(string name,int defaultMods) {
            var key=(Keys)Number(name+"Key",119,0,254);
            int mods=Number(name+"Modifiers",defaultMods,0,7);
            return key | ((mods&1)!=0?Keys.Control:0) | ((mods&2)!=0?Keys.Shift:0) | ((mods&4)!=0?Keys.Alt:0);
        }
        _chkCableTracking.Checked=ParseBool(_ini.Get("cabletracking","enabled","false"));
        _cableShowKey=Key("show",0); _cableResetKey=Key("reset",3);
        _cableDisplaySeconds=Number("displaySeconds",5,1,30);
        _cableWarningTurns=decimal.TryParse(_ini.Get("cabletracking","warningTurns","2"),NumberStyles.Float,CultureInfo.InvariantCulture,out var turns)?Math.Clamp(turns,0,20):2;
        RefreshCableTrackingSummary();
    }
    private void SaveCableTrackingSettings()
    {
        void Put(string name,object value) => _ini.Set("cabletracking",name,Convert.ToString(value,CultureInfo.InvariantCulture) ?? "");
        void Key(string name,Keys key) {
            Put(name+"Key",(int)(key&Keys.KeyCode));
            Put(name+"Modifiers",((key&Keys.Control)!=0?1:0)|((key&Keys.Shift)!=0?2:0)|((key&Keys.Alt)!=0?4:0));
        }
        Put("enabled",_chkCableTracking.Checked?"true":"false");
        Key("show",_cableShowKey); Key("reset",_cableResetKey);
        Put("displaySeconds",_cableDisplaySeconds); Put("warningTurns",_cableWarningTurns);
    }
    private void ShowCableTrackingSettings()
    {
        using var dialog = CreateCableTrackingDialog();
        dialog.ShowDialog(this);
    }
    private Form CreateCableTrackingDialog()
    {
        var dialog = new Form { Text = "Cable tracking", ClientSize = new Size(660,430),
            StartPosition = FormStartPosition.CenterParent, FormBorderStyle = FormBorderStyle.FixedDialog,
            MaximizeBox = false, MinimizeBox = false, BackColor = ModernUiTheme.Window,
            ForeColor = ModernUiTheme.TextPrimary, Font = new Font("Segoe UI",9f) };
        dialog.Controls.Add(MakeSectionLabel("Cable tracking",20,16));
        dialog.Controls.Add(new Label { Location=new Point(20,48),Size=new Size(620,44),
            Text="Counts physical headset turns and shows which way to unwind.\nStart with the cable untwisted. Tracking gaps are flagged in amber.",ForeColor=ModernUiTheme.TextSecondary });
        Keys showKey=_cableShowKey, resetKey=_cableResetKey;
        Button Capture(string label,int y,Keys initial,Action<Keys> changed) {
            dialog.Controls.Add(MakeLabel(label,20,y+5,190));
            var button=MakeButton(CableKeyName(initial),216,y,300,30);
            bool waiting=false;
            button.Click+=(_,_)=> { waiting=true;button.Text="Press key or chord... (Esc cancels)"; };
            button.PreviewKeyDown+=(_,e)=> { if(waiting)e.IsInputKey=true; };
            button.KeyDown+=(_,e)=> {
                if(!waiting)return;
                e.Handled=true;e.SuppressKeyPress=true;
                if(e.KeyCode is Keys.ControlKey or Keys.ShiftKey or Keys.Menu)return;
                if(e.KeyCode!=Keys.Escape) { initial=e.KeyData;changed(initial); }
                waiting=false;button.Text=CableKeyName(initial);
            };
            button.LostFocus+=(_,_)=> {waiting=false;button.Text=CableKeyName(initial);};
            var clear=MakeButton("Clear",530,y,108,30);
            clear.Click+=(_,_)=> { initial=Keys.None;changed(initial);button.Text="Unassigned"; };
            dialog.Controls.Add(button);dialog.Controls.Add(clear);return button;
        }
        Capture("Show counter",106,showKey,k=>showKey=k);
        Capture("Reset zero (after untwisting)",146,resetKey,k=>resetKey=k);
        dialog.Controls.Add(MakeLabel("Display time (seconds)",20,198,190));
        var duration=new NumericUpDown {Location=new Point(216,194),Size=new Size(86,28),Minimum=1,Maximum=30,Value=_cableDisplaySeconds};
        dialog.Controls.Add(duration);
        dialog.Controls.Add(MakeLabel("Warn at turns (0 = off)",20,238,190));
        var threshold=new NumericUpDown {Location=new Point(216,234),Size=new Size(86,28),Minimum=0,Maximum=20,DecimalPlaces=1,Increment=.5m,Value=_cableWarningTurns};
        dialog.Controls.Add(threshold);
        dialog.Controls.Add(new Label {Location=new Point(20,278),Size=new Size(620,66),ForeColor=ModernUiTheme.TextSecondary,
            Text="Controller: Bindings → Button combos → Cable tracker: Show / Reset zero.\nUse a deliberate chord or long press for reset. Existing button actions still work.\nThe counter runs while hidden; reset zero after untwisting, including each new session."});
        dialog.Controls.Add(new Label {Location=new Point(20,352),Size=new Size(620,24),ForeColor=ModernUiTheme.AccentText,
            Text="Apply, save settings, then restart Skyrim. No SteamVR overlay app required."});
        var apply=MakeButton("Apply",414,390,108,30);
        var cancel=MakeButton("Cancel",530,390,108,30);cancel.DialogResult=DialogResult.Cancel;
        apply.Click+=(_,_)=> {
            if(showKey!=Keys.None && showKey==resetKey) { MessageBox.Show(dialog,"Use different shortcuts for Show and Reset zero.","Cable tracking",MessageBoxButtons.OK,MessageBoxIcon.Information);return; }
            _cableShowKey=showKey;_cableResetKey=resetKey;_cableDisplaySeconds=(int)duration.Value;_cableWarningTurns=threshold.Value;
            RefreshCableTrackingSummary();MarkDirty();dialog.DialogResult=DialogResult.OK;
        };
        dialog.Controls.Add(apply);dialog.Controls.Add(cancel);
        ModernUiTheme.Apply(dialog,windowBands:false);DpiLayout.Popup(dialog);return dialog;
    }
}
