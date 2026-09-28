using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using OpenCompositeConfigurator;

static partial class Program
{
    static int AuditFrame()
    {
        string root=AppContext.BaseDirectory;
        Directory.CreateDirectory(Path.Combine(root,"root"));
        Directory.CreateDirectory(Path.Combine(root,"interface","controls","pc"));
        string path=Path.Combine(root,"interface","controls","pc","controlmapvr.txt");
        using (var stream=typeof(MainForm).Assembly.GetManifestResourceStream("OpenCompositeConfigurator.controlmapvr_template.txt")!)
        using (var reader=new StreamReader(stream)) File.WriteAllText(path,reader.ReadToEnd());
        Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
        Application.EnableVisualStyles();
        using (var form=new MainForm()) {
            Call(form,"ApplyControllerModel","frame");
            Check((bool)Call(form,"SaveUiModelChoice")!,"Frame selection save");
            Check(Field<Image>(form,"_frameImage").Width==830,"Frame artwork embedded");
            Check(Field<CheckBox>(form,"_chkLeftX").Text=="D-pad Down","Keyboard left primary mislabeled");
            var dots=Field<Dictionary<string,(string display,PointF pos,bool isStickDir)>>(form,"_activeControllerButtons");
            Check(dots.Count==26,"Missing Frame input targets");
            foreach (var dot in dots) {
                Check((string?)Call(form,"HitTestControllerButton",dot.Value.pos.X,dot.Value.pos.Y)==dot.Key,"Unreachable/overlapping center: "+dot.Key);
                if (!dot.Value.isStickDir) Check((float)Call(form,"DotRadiusFor",dot.Key)! == 14f,"Frame circles differ from Quest size");
            }
            Call(form,"TryLoadControlmapVR");
            var contexts=Field<Dictionary<string,List<string[]>>>(form,"_contextBindings");
            Field<ComboBox>(form,"_cmbCtrlType").SelectedIndex=Field<List<string>>(form,"_contextNames").IndexOf("Main Gameplay");
            var jump=contexts["Main Gameplay"].Single(r=>r[0]=="Jump");
            string prior=jump[6];
            Set(form,"_selectedCtrlButton","frame_r_bumper");
            Call(form,"RefreshSelectedControllerBinding",true);
            var picker=Field<ComboBox>(form,"_cmbCtrlAction");
            Check(picker.Enabled && picker.Items.Contains("Jump"),"Frame bumper unbindable");
            picker.SelectedItem="Jump";
            Check(jump[6].Split(',').Contains("0x03"),"Bumper code missing");
            if (prior!="0xff") Check(jump[6].Contains(prior),"Adding bumper removed existing Jump assignment");
            Call(form,"SaveCurrentBindingEdits");
            Call(form,"TryLoadControlmapVR");
            Check(contexts["Main Gameplay"].Single(r=>r[0]=="Jump")[6].Contains("0x03"),"Bumper assignment lost on reload");
            Call(form,"SwitchTab",1);
            var picture=Field<PictureBox>(form,"_picBindingsController");
            using var bitmap=new Bitmap(picture.Width,picture.Height);
            picture.DrawToBitmap(bitmap,picture.ClientRectangle);
            bitmap.Save(Path.Combine(root,"frame-bindings.png"));
            Call(form,"SyncComboEditorModel");
            using var combo=new ComboEditForm(new Dictionary<string,int>());
            var buttonField=typeof(ComboEditForm).GetField("_buttons",Private)!;
            var buttons=(Dictionary<string,(string display,PointF pos,bool isStickDir)>)buttonField.GetValue(combo)!;
            Check(buttons["x"].display=="D-pad Down" && buttons["y"].display=="D-pad Up","Combo popup labels disagree");
            Call(form,"ApplyControllerModel","touch");
            Check(Field<CheckBox>(form,"_chkLeftX").Text=="X Button","Switching back fails to restore Quest labels");
            Check(!Field<Dictionary<string,(string,PointF,bool)>>(form,"_activeControllerButtons").ContainsKey("frame_x"),"Frame extras leaked into Quest layout");
        }
        using (var restored=new MainForm())
            Check(Field<string>(restored,"_controllerModelKey")=="frame","Frame choice did not survive restart");
        Console.WriteLine("PASS: Frame image, 26 target centers, Quest-size circles, independent bumper save/reload, popup labels, model persistence and Quest switch-back");
        return 0;
    }
}
