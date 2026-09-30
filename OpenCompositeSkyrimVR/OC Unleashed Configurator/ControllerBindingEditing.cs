using System;
using System.Linq;

namespace OpenCompositeConfigurator
{
    public partial class MainForm
    {
        private void RefreshControllerActionChoices()
        {
            if (_cmbCtrlAction == null) return;
            _suppressCtrlActionChange = true;
            try
            {
                _cmbCtrlAction.Items.Clear();
                _cmbCtrlAction.Items.Add("(none)");
                string? context = GetSelectedContextName(_cmbCtrlType);
                if (context != null && _contextBindings.TryGetValue(context, out var rows))
                    foreach (string action in rows.Where(row => row.Length > 9)
                        .Select(row => row[0]).Distinct(StringComparer.OrdinalIgnoreCase)
                        .OrderBy(action => action, StringComparer.CurrentCultureIgnoreCase))
                        _cmbCtrlAction.Items.Add(action);
                _cmbCtrlAction.SelectedIndex = 0;
            }
            finally { _suppressCtrlActionChange = false; }
        }

        // This explicit repair changes only the Item Menus context. Keep the
        // existing Drop buttons; add the left face button as an alternative.
        private void BindInventoryDropToLeftFaceButton()
        {
            const string context = "Item Menus";
            if (!_contextBindings.TryGetValue(context, out var rows)
                || rows.FirstOrDefault(row => row.Length > 9 && row[0] == "XButton") is not { } drop)
            {
                _lblKbStatus.Text = "This controlmap has no Item Menus / XButton row to repair.";
                return;
            }
            foreach (int column in new[] { 5, 7, 9 })
            {
                foreach (var row in rows.Where(row => row.Length > 9 && row != drop))
                {
                    if (TryRemoveControllerHex(row[column], "0x07", out string remaining))
                    {
                        row[column] = remaining;
                        RecordControllerChange(context, row[0], column, remaining);
                    }
                }
                string value = AppendTrackpadHex(drop[column], "0x07");
                if (value != drop[column])
                {
                    drop[column] = value;
                    RecordControllerChange(context, "XButton", column, value);
                }
            }
            _cmbCtrlType.SelectedIndex = _contextNames.IndexOf(context);
            _selectedCtrlButton = "x_button";
            RefreshSelectedControllerBinding(updateStatus: false);
            _lblKbStatus.Text = "Drop assigned to left A/X. Existing Drop buttons retained. Save settings or Save Custom, then restart Skyrim.";
            MarkDirty();
        }
    }
}
