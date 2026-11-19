using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using RuntimeSwitcher.Core;

namespace RuntimeSwitcher
{
    /// <summary>
    /// The options of the openlamina.ini next to the libraries that a player would change (<see cref="Settings"/>),
    /// each with its name in the file and its default. Save writes only the options changed here, into the file as it
    /// is then; everything else in it stays.
    /// </summary>
    public sealed class SettingsForm : Form
    {
        private readonly string _file;
        private readonly ToolTip _tips = new ToolTip { AutoPopDelay = 20000 };

        // Each option's value now, as the file would have it, and the one the window opened with.
        private readonly List<(Setting setting, Func<string> value, string opened)> _rows =
            new List<(Setting, Func<string>, string)>();

        public SettingsForm(string file)
        {
            _file = file;
            IniFile ini = IniFile.Load(file);

            SuspendLayout();
            Text = "OpenLamina: settings";
            Icon = AppIcon.Get();
            Font = SystemFonts.MessageBoxFont;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            ShowInTaskbar = false;
            StartPosition = FormStartPosition.CenterParent;
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;

            // Two groups beside each other, the next two below them.
            var groups = new TableLayoutPanel
            {
                AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 2, Margin = Padding.Empty,
            };
            foreach (IGrouping<string, Setting> group in Settings.All.GroupBy(s => s.Group))
            {
                var table = new TableLayoutPanel
                {
                    AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 2, Dock = DockStyle.Fill,
                };
                foreach (Setting setting in group)
                    AddRow(table, setting, ini.Get(setting.Name));
                // The last row takes the height a taller group beside this one leaves over.
                table.Controls.Add(new Label { Size = Size.Empty, Margin = Padding.Empty });
                var box = new GroupBox
                {
                    Text = group.Key, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
                    Dock = DockStyle.Fill, Padding = new Padding(8, 4, 8, 6), Margin = new Padding(0, 0, 8, 8),
                };
                box.Controls.Add(table);
                groups.Controls.Add(box);
            }

            var save = new Button
            {
                Text = "Save", AutoSize = true, MinimumSize = new Size(100, 30), UseVisualStyleBackColor = true,
            };
            var cancel = new Button
            {
                Text = "Cancel", AutoSize = true, MinimumSize = new Size(100, 30), UseVisualStyleBackColor = true,
                DialogResult = DialogResult.Cancel,
            };
            save.Click += (s, e) => Save();
            var buttons = new FlowLayoutPanel
            {
                AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false,
                Margin = new Padding(0, 6, 0, 0),
            };
            buttons.Controls.AddRange(new Control[] { save, cancel });

            var root = new TableLayoutPanel
            {
                AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 1, Padding = new Padding(12),
            };
            root.Controls.Add(groups);
            root.Controls.Add(Note("Changes apply to games started afterwards. An openlamina.ini in a game's folder " +
                                   "overrides these for that game; more options are in the README.",
                SystemColors.ControlText));
            root.Controls.Add(Note($"Saved in {file}", SystemColors.GrayText));
            root.Controls.Add(buttons);
            Controls.Add(root);
            AcceptButton = save;
            CancelButton = cancel;

            AutoScaleDimensions = new SizeF(96F, 96F);
            AutoScaleMode = AutoScaleMode.Dpi;
            ResumeLayout(false);
            PerformLayout();
        }

        private static Label Note(string text, Color colour) =>
            new Label { AutoSize = true, Text = text, ForeColor = colour, Margin = new Padding(0, 3, 0, 3) };

        /// <summary>A row: the option's control, then its name in the file and its default.</summary>
        private void AddRow(TableLayoutPanel table, Setting setting, string raw)
        {
            string current = setting.Read(raw);
            Control row;
            Func<string> value;
            if (setting.Kind == SettingKind.Bool)
            {
                var box = new CheckBox
                {
                    Text = setting.Label, AutoSize = true, Checked = current == "true", Anchor = AnchorStyles.Left,
                    Margin = new Padding(3, 4, 3, 3),
                };
                value = () => box.Checked ? "true" : "false";
                row = box;
            }
            else
            {
                Control input;
                if (setting.Kind == SettingKind.Choice)
                {
                    var list = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 190 };
                    list.Items.AddRange(setting.Choices.Select(c => (object)c.Value).ToArray());
                    list.SelectedIndex = setting.Choices.ToList().FindIndex(c => c.Key == current);
                    value = () => setting.Choices[list.SelectedIndex].Key;
                    input = list;
                }
                else
                {
                    var number = new NumericUpDown
                    {
                        DecimalPlaces = 2, Increment = 0.05m, Minimum = 0.1m, Maximum = 5m, Width = 70,
                    };
                    number.Value = Math.Min(number.Maximum, Math.Max(number.Minimum, decimal.Parse(current,
                        System.Globalization.CultureInfo.InvariantCulture)));
                    value = () => Setting.Format(number.Value);
                    input = number;
                }

                var label = new Label
                {
                    Text = setting.Label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 0, 4, 0),
                };
                var flow = new FlowLayoutPanel
                {
                    AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false,
                    Anchor = AnchorStyles.Left, Margin = new Padding(3, 0, 3, 0),
                };
                flow.Controls.AddRange(new[] { label, input });
                _tips.SetToolTip(label, setting.Help);
                _tips.SetToolTip(input, setting.Help);
                row = flow;
            }

            var name = new Label
            {
                Text = $"{setting.Name}, default {setting.DefaultText}", AutoSize = true, Anchor = AnchorStyles.Left,
                ForeColor = SystemColors.GrayText, Margin = new Padding(8, 0, 0, 0),
            };
            _tips.SetToolTip(row, setting.Help);
            _tips.SetToolTip(name, setting.Help);
            table.Controls.Add(row);
            table.Controls.Add(name);
            _rows.Add((setting, value, value()));
        }

        private void Save()
        {
            // Finish a number still being typed.
            ValidateChildren();
            try
            {
                // Read again, so what was changed in the file while the window was open stays.
                IniFile ini = IniFile.Load(_file);
                bool changed = false;
                foreach ((Setting setting, Func<string> value, string opened) in _rows)
                {
                    string now = value();
                    if (now == opened)
                        continue;
                    if (now.Length == 0)
                        ini.Remove(setting.Name);
                    else
                        ini.Set(setting.Name, now);
                    changed = true;
                }

                if (changed)
                    ini.Save(_file);
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
            {
                MessageBox.Show(this, $"{_file} can't be written: {e.Message}", "OpenLamina", MessageBoxButtons.OK,
                    MessageBoxIcon.Error);
                return;
            }

            DialogResult = DialogResult.OK;
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
                _tips.Dispose();
            base.Dispose(disposing);
        }
    }
}
